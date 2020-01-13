#include "asyncioextras/btc.h"
#include "asyncio/objectPool.h"
#include "p2putils/xmstream.h"
#include <stdlib.h>
#include <string.h>

#include "openssl/sha.h"

static const char *btcSocketPool = "btcSocket";
static const char *poolId = "btc";
static const char *timerPoolId = "btcTimer";

#pragma pack(push, 1)
struct MessageHeader {
  uint32_t magic;
  char command[12];
  uint32_t length;
  uint32_t checksum;
};
#pragma pack(pop)

constexpr int USERSPACE_BUFFER_SIZE=1472;

enum btcOpTy {
  btcOpRecv = OPCODE_READ,
  btcOpSend = OPCODE_WRITE
};

enum btcOpState {
  stInitialize = 0,
  stReadData,
  stWriteData,
  stFinished
};


struct BTCSocket {
  aioObjectRoot root;
  aioObject *plainSocket;
  uint32_t magic;
  uint8_t sendBuffer[USERSPACE_BUFFER_SIZE];
  uint8_t receiveBuffer[sizeof(MessageHeader)];
};

struct btcOp {
  asyncOpRoot root;
  HostAddress address;
  btcOpState state;
  char *commandPtr;
  char command[12];
  xmstream *stream;
  void *buffer;
  size_t size;
  void *internalBuffer;
  size_t internalBufferSize;
};

static uint32_t calculateCheckSum(void *data, size_t size)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  unsigned char hash2[SHA256_DIGEST_LENGTH];
  SHA256_CTX sha256;
  SHA256_Init(&sha256);
  SHA256_Update(&sha256, data, size);
  SHA256_Final(hash, &sha256);
  SHA256_Init(&sha256);
  SHA256_Update(&sha256, hash, SHA256_DIGEST_LENGTH);
  SHA256_Final(hash2, &sha256);
  return *reinterpret_cast<uint32_t*>(hash2);
}

static void buildMessageHeader(MessageHeader *out, uint32_t magic, char command[12], void *data, uint32_t size)
{
  out->magic = xhtole(magic);
  memcpy(out->command, command, 12);
  out->length = xhtole(size);
  out->checksum = xhtole(calculateCheckSum(data, size));
}

static void buildMessageHeader(MessageHeader *out, uint32_t magic, const char *command, void *data, uint32_t size)
{
  out->magic = xhtole(magic);
  memset(out->command, 0, sizeof(out->command));
  strncpy(out->command, command, sizeof(out->command));
  out->length = xhtole(size);
  out->checksum = xhtole(calculateCheckSum(data, size));
}

static void decodeMessageHeader(MessageHeader *header)
{
  header->magic = xletoh(header->magic);
  header->length = xletoh(header->length);
  header->checksum = xletoh(header->checksum);
}

static asyncOpRoot *alloc()
{
  btcOp *op = static_cast<btcOp*>(__tagged_alloc(sizeof(btcOp)));
  op->internalBuffer = nullptr;
  op->internalBufferSize = 0;
  return &op->root;

//  return static_cast<asyncOpRoot*>();
}

static void resumeRwCb(AsyncOpStatus status, aioObject*, size_t, void *arg)
{
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static int cancel(asyncOpRoot *opptr)
{
  BTCSocket *socket = reinterpret_cast<BTCSocket*>(opptr->object);
  cancelIo(reinterpret_cast<aioObjectRoot*>(socket->plainSocket));
  return 0;
}

static void recvFinish(asyncOpRoot *opptr)
{
  btcOp *op = reinterpret_cast<btcOp*>(opptr);
  reinterpret_cast<btcRecvCb*>(opptr->callback)(opGetStatus(opptr),
                                                      reinterpret_cast<BTCSocket*>(opptr->object),
                                                      op->command,
                                                      op->stream,
                                                      opptr->arg);
}

static void sendFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<btcSendCb*>(opptr->callback)(opGetStatus(opptr),
                                                reinterpret_cast<BTCSocket*>(opptr->object),
                                                opptr->arg);
}

btcOp *initReadOp(aioExecuteProc *start,
                   aioFinishProc *finish,
                   BTCSocket *socket,
                   AsyncFlags flags,
                   uint64_t timeout,
                   void *callback,
                   void *arg,
                   int opCode,
                   xmstream *stream,
                   size_t sizeLimit,
                   char *commandPtr)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  btcOp *op = reinterpret_cast<btcOp*>(opptr);
  op->state = stInitialize;
  op->size = sizeLimit;
  op->stream = stream;
  op->commandPtr = commandPtr;
  return op;
}

btcOp *initWriteOp(aioExecuteProc *start,
                    aioFinishProc *finish,
                    BTCSocket *socket,
                    AsyncFlags flags,
                    uint64_t timeout,
                    void *callback,
                    void *arg,
                    int opCode,
                    const char *command,
                    void *data,
                    size_t size)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  btcOp *op = reinterpret_cast<btcOp*>(opptr);
  op->state = stInitialize;

//  op->buffer = data;
  if (!(flags & afNoCopy)) {
    if (op->internalBuffer == nullptr) {
      op->internalBuffer = malloc(size);
      op->internalBufferSize = size;
    } else if (op->internalBufferSize < size) {
      op->internalBufferSize = size;
      op->internalBuffer = realloc(op->internalBuffer, size);
    }

    memcpy(op->internalBuffer, data, size);
    op->buffer = op->internalBuffer;
  } else {
    op->buffer = (void*)(uintptr_t)data;
  }



  op->size = size;
  op->stream = nullptr;
  strncpy(op->command, command, sizeof(op->command));
  return op;
}

static AsyncOpStatus startBtcRecv(asyncOpRoot *opptr)
{
  btcOp *op = reinterpret_cast<btcOp*>(opptr);
  BTCSocket *socket = reinterpret_cast<BTCSocket*>(opptr->object);
  MessageHeader *header = reinterpret_cast<MessageHeader*>(socket->receiveBuffer);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;

  while (!childOp) {
    switch (op->state) {
      case stInitialize : {
        // Read header first
        op->state = stReadData;
        childOp = implRead(socket->plainSocket, header, sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stReadData : {
        op->state = stFinished;
        decodeMessageHeader(header);
        if (header->magic != socket->magic)
          return btcMakeStatus(btcInvalidMagic);
        if (header->command[11] != 0)
          return btcMakeStatus(btcInvalidCommand);
        if (op->size < header->length)
          return aosBufferTooSmall;

        op->stream->reset();
        childOp = implRead(socket->plainSocket, op->stream->alloc(header->length), header->length, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stFinished : {
        uint32_t checkSum = calculateCheckSum(op->stream->data(), header->length);
        if (header->checksum != checkSum)
          return btcMakeStatus(btcInvalidChecksum);
        memcpy(op->commandPtr, header->command, 12);
        op->stream->seekSet(0);
        return aosSuccess;
      }

      default :
        return aosUnknownError;
    }
  }

  opStart(childOp);
  return aosPending;
}

asyncOpRoot *implBtcRecv(BTCSocket *socket,
                         char command[12],
                         xmstream &stream,
                         size_t sizeLimit,
                         AsyncFlags flags,
                         uint64_t timeout,
                         btcRecvCb callback,
                         void *arg,
                         size_t *bytesTransferred)
{
  size_t bytes;
  asyncOpRoot *childOp = nullptr;
  btcOpState state = stInitialize;
  AsyncOpStatus result = aosSuccess;
  state = stReadData;
  MessageHeader *header = reinterpret_cast<MessageHeader*>(socket->receiveBuffer);
  if ( !(childOp = implRead(socket->plainSocket, header, sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes)) ) {
    state = stFinished;
    decodeMessageHeader(header);
    if (header->magic != socket->magic)
      result = btcMakeStatus(btcInvalidMagic);
    if (header->command[11] != 0)
      result = btcMakeStatus(btcInvalidCommand);
    if (sizeLimit < header->length)
      result = aosBufferTooSmall;

    stream.reset();
    if (result == aosSuccess &&
        ! (childOp = implRead(socket->plainSocket, stream.alloc(header->length), header->length, afWaitAll, 0, resumeRwCb, nullptr, &bytes))) {
      uint32_t checkSum = calculateCheckSum(stream.data(), header->length);
      if (header->checksum != checkSum)
        result = btcMakeStatus(btcInvalidChecksum);
    }
  }

  if (result != aosSuccess) {
    btcOp *op = initReadOp(startBtcRecv, recvFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, btcOpRecv, &stream, sizeLimit, command);
    opForceStatus(&op->root, result);
    return &op->root;
  }

  if (childOp) {
    btcOp *op = initReadOp(startBtcRecv, recvFinish, socket, flags|afRunning, timeout, reinterpret_cast<void*>(callback), arg, btcOpRecv, &stream, sizeLimit, command);
    op->state = state;
    childOp->arg = op;
    opStart(childOp);
    return &op->root;
  }

  stream.seekSet(0);
  memcpy(command, header->command, 12);
  *bytesTransferred = header->length + sizeof(MessageHeader);
  return nullptr;
}

static AsyncOpStatus startBtcSend(asyncOpRoot *opptr)
{
  btcOp *op = reinterpret_cast<btcOp*>(opptr);
  BTCSocket *socket = reinterpret_cast<BTCSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;

  uint8_t buffer[USERSPACE_BUFFER_SIZE];
  MessageHeader *header = reinterpret_cast<MessageHeader*>(buffer);

  while (!childOp) {
    switch (op->state) {
      case stInitialize : {
        buildMessageHeader(header, socket->magic, op->command, op->buffer, op->size);
        if (op->size+sizeof(MessageHeader) <= sizeof(buffer)) {
          op->state = stFinished;
          uint8_t *dataPtr = buffer + sizeof(MessageHeader);
          memcpy(dataPtr, op->buffer, op->size);
          childOp = implWrite(socket->plainSocket, buffer, op->size+sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          op->state = stWriteData;
          childOp = implWrite(socket->plainSocket, buffer, sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        }

        break;
      }

      case stWriteData : {
        op->state = stFinished;
        childOp = implWrite(socket->plainSocket, op->buffer, op->size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stFinished :
        return aosSuccess;

      default :
        return aosUnknownError;
    }
  }

  opStart(childOp);
  return aosPending;
}

asyncOpRoot *implBtcSend(BTCSocket *socket,
                         const char *command,
                         void *data,
                         size_t size,
                         AsyncFlags flags,
                         uint64_t timeout,
                         btcSendCb callback,
                         void *arg,
                         size_t *bytesTransferred)
{
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  btcOpState state = stInitialize;

  uint8_t buffer[USERSPACE_BUFFER_SIZE];
  MessageHeader *header = reinterpret_cast<MessageHeader*>(buffer);

  buildMessageHeader(header, socket->magic, command, data, size);

  if (size+sizeof(MessageHeader) <= sizeof(buffer)) {
    state = stFinished;
    uint8_t *dataPtr = buffer + sizeof(MessageHeader);
    memcpy(dataPtr, data, size);
    childOp = implWrite(socket->plainSocket, buffer, size+sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes);
  } else {
    state = stWriteData;
    childOp = implWrite(socket->plainSocket, buffer, sizeof(MessageHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    if (!childOp) {
      state = stFinished;
      childOp = implWrite(socket->plainSocket, data, size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    }
  }

  if (childOp) {
    btcOp *op = initWriteOp(startBtcSend, sendFinish, socket, flags|afRunning, timeout, reinterpret_cast<void*>(callback), arg, btcOpSend, command, data, size);
    op->state = state;
    childOp->arg = op;
    opStart(childOp);
    return &op->root;
  }

  *bytesTransferred = size + sizeof(MessageHeader);
  return nullptr;
}


void btcSocketDestructor(aioObjectRoot *object)
{
  deleteAioObject(reinterpret_cast<BTCSocket*>(object)->plainSocket);
  objectRelease(object, btcSocketPool);
}

aioObjectRoot *btcSocketHandle(BTCSocket *socket)
{
  return &socket->root;
}

BTCSocket *btcSocketNew(asyncBase *base, aioObject *plainSocket)
{
  BTCSocket *socket = static_cast<BTCSocket*>(objectGet(btcSocketPool));
  if (!socket)
    socket = static_cast<BTCSocket*>(malloc(sizeof(BTCSocket)));
  initObjectRoot(&socket->root, base, ioObjectUserDefined, btcSocketDestructor);

  socket->plainSocket = plainSocket;

  // Set up default magic for BTC mainnet
  socket->magic = 0xD9B4BEF9;
  return socket;
}

void btcSocketDelete(BTCSocket *socket)
{
  objectDelete(&socket->root);
}

aioObject *btcGetPlainSocket(BTCSocket *socket)
{
  return socket->plainSocket;
}

void btcSocketSetMagic(BTCSocket *socket, uint32_t magic)
{
  socket->magic = magic;
}


ssize_t aioBtcRecv(BTCSocket *socket, char command[12], xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout, btcRecvCb callback, void *arg)
{
  ssize_t result = -aosPending;
  size_t bytesTransferred;
  aioMethod([=, &stream]() { return &initReadOp(startBtcRecv, recvFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, btcOpRecv, &stream, sizeLimit, command)->root; },
            [=, &stream, &bytesTransferred]() { return implBtcRecv(socket, command, stream, sizeLimit, flags, timeout, callback, arg, &bytesTransferred); },
            [&bytesTransferred, &result]() { result = static_cast<ssize_t>(bytesTransferred); },
            [](asyncOpRoot*) {},
            &socket->root,
            flags,
            reinterpret_cast<void*>(callback),
            btcOpRecv);
  return result;
}

ssize_t aioBtcSend(BTCSocket *socket, const char *command, void *data, size_t size, AsyncFlags flags, uint64_t timeout, btcSendCb callback, void *arg)
{
  ssize_t result = -aosPending;
  size_t bytesTransferred;
  aioMethod([=]() { return &initWriteOp(startBtcSend, sendFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, btcOpSend, command, data, size)->root; },
            [=, &bytesTransferred]() { return implBtcSend(socket, command, data, size, flags, timeout, callback, arg, &bytesTransferred); },
            [&bytesTransferred, &result]() { result = static_cast<ssize_t>(bytesTransferred); },
            [](asyncOpRoot*) {},
            &socket->root,
            flags,
            reinterpret_cast<void*>(callback),
            btcOpSend);
  return result;
}


ssize_t ioBtcRecv(BTCSocket *socket, char command[12], xmstream &stream, size_t sizeLimit, AsyncFlags flags, uint64_t timeout)
{
  size_t bytesTransferred;
  btcOp *op = reinterpret_cast<btcOp*>(
    ioMethod([=, &stream]() { return &initReadOp(startBtcRecv, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, btcOpRecv, &stream, sizeLimit, command)->root; },
             [=, &stream, &bytesTransferred]() { return implBtcRecv(socket, command, stream, sizeLimit, flags | afCoroutine, timeout, nullptr, nullptr, &bytesTransferred); },
             [](asyncOpRoot*) {},
             &socket->root,
             btcOpRecv));
  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    objectRelease(&op->root, op->root.poolId);
    objectDecrementReference(&socket->root, 1);
    return status == aosSuccess ? static_cast<ssize_t>(stream.sizeOf()) : -status;
  } else {
    return static_cast<ssize_t>(stream.sizeOf());
  }
}

ssize_t ioBtcSend(BTCSocket *socket, const char *command, void *data, size_t size, AsyncFlags flags, uint64_t timeout)
{
  size_t bytesTransferred;
  btcOp *op = reinterpret_cast<btcOp*>(
    ioMethod([=]() { return &initWriteOp(startBtcSend, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, btcOpSend, command, data, size)->root; },
             [=, &bytesTransferred]() { return implBtcSend(socket, command, data, size, flags | afCoroutine, timeout, nullptr, nullptr, &bytesTransferred); },
             [](asyncOpRoot*) {},
             &socket->root,
             btcOpSend));
  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    objectRelease(&op->root, op->root.poolId);
    objectDecrementReference(&socket->root, 1);
    return status == aosSuccess ? static_cast<ssize_t>(size) : -status;
  } else {
    return static_cast<ssize_t>(size);
  }
}
