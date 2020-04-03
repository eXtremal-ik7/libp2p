#include "asyncioextras/zmtp.h"
#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"

static const char *zmtpSocketPool = "zmtpSocket";
static const char *poolId = "zmtp";
static const char *timerPoolId = "zmtpTimer";

static uint8_t localSignature[] = {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7F};
static uint8_t localMajorVersion = 3;
static uint8_t localGreetingOther[] = {
  0,
  'N', 'U', 'L', 'L', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

enum zmtpMsgTy {
  zmtpMsgFlagNone,
  zmtpMsgFlagMore = 1,
  zmtpMsgFlagLong = 2,
  zmtpMsgFlagCommand = 4
};

static const char *socketTypeNames[] = {
  "REQ",
  "REP",
  "DEALER",
  "ROUTER",
  "PUB",
  "XPUB",
  "SUB",
  "XSUB",
  "PUSH",
  "PULL",
  "PAIR"
};

static inline zmtpMsgTy operator|(zmtpMsgTy a, zmtpMsgTy b) {
  return static_cast<zmtpMsgTy>(static_cast<int>(a) | static_cast<int>(b));
}

enum btcOpTy {
  zmtpOpAccept = OPCODE_READ,
  zmtpOpRecv,
  zmtpOpConnect = OPCODE_WRITE,
  zmtpOpSend
};

enum zmtpOpState {
  stInitialize = 0,
  stAcceptWriteLocalSignature,
  stAcceptReadMajorVersion,
  stAcceptWriteLocalMajorVersion,
  stAcceptReadMinorVersion,
  stAcceptWriteLocalGreeting,
  stAcceptReadReadyMsg,
  stAcceptWriteReadyMsg,
  stAcceptWriteReadyMsgWaiting,

  stConnectWriteLocalSignature,
  stConnectReadSignature,
  stConnectWriteLocalMajorVersion,
  stConnectReadMajorVersion,
  stConnectWriteLocalGreeting,
  stConnectReadGreeting,
  stConnectWriteReadyMsg,
  stConnectReadReadyMsg,
  stConnectReadReadyMsgWaiting,

  stRecvReadType,
  stRecvReadSize,
  stRecvReadData,

  stWriteSize,
  stWriteData,

  stFinished
};

__NO_PADDING_BEGIN
struct zmtpSocket {
  aioObjectRoot root;
  aioObject *plainSocket;
  zmtpSocketTy type;
  bool needSendMore;
  uint8_t buffer[256];
};

struct zmtpOp {
  asyncOpRoot root;
  HostAddress address;
  zmtpOpState state;
  zmtpOpState stateRw;
  zmtpStream *stream;
  zmtpMsgTy type;
  void *data;
  size_t size;
  size_t transferred;
};
__NO_PADDING_END

static AsyncOpStatus startZmtpRecv(asyncOpRoot *opptr);

static asyncOpRoot *alloc()
{
  return static_cast<asyncOpRoot*>(malloc(sizeof(zmtpOp)));
}

static void resumeConnectCb(AsyncOpStatus status, aioObject*, void *arg)
{
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static void resumeRwCb(AsyncOpStatus status, aioObject*, size_t, void *arg)
{
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static int cancel(asyncOpRoot *opptr)
{
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  cancelIo(reinterpret_cast<aioObjectRoot*>(socket->plainSocket));
  return 0;
}

static void acceptFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<zmtpAcceptCb*>(opptr->callback)(opGetStatus(opptr),
                                                   reinterpret_cast<zmtpSocket*>(opptr->object),
                                                   opptr->arg);
}

static void connectFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<zmtpConnectCb*>(opptr->callback)(opGetStatus(opptr),
                                                    reinterpret_cast<zmtpSocket*>(opptr->object),
                                                    opptr->arg);
}

static void recvFinish(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpUserMsgTy type = op->type & zmtpMsgFlagCommand ? zmtpCommand : zmtpMessage;
  reinterpret_cast<zmtpRecvCb*>(opptr->callback)(opGetStatus(opptr),
                                                 reinterpret_cast<zmtpSocket*>(opptr->object),
                                                 type,
                                                 op->stream,
                                                 opptr->arg);
}

static void sendFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<zmtpSendCb*>(opptr->callback)(opGetStatus(opptr),
                                                 reinterpret_cast<zmtpSocket*>(opptr->object),
                                                 opptr->arg);
}

zmtpOp *initOp(aioExecuteProc *start,
               aioFinishProc *finish,
               zmtpSocket *socket,
               AsyncFlags flags,
               uint64_t timeout,
               void *callback,
               void *arg,
               int opCode)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  op->state = stInitialize;
  op->stateRw = stInitialize;
  return op;
}

zmtpOp *initReadOp(aioExecuteProc *start,
                   aioFinishProc *finish,
                   zmtpSocket *socket,
                   AsyncFlags flags,
                   uint64_t timeout,
                   void *callback,
                   void *arg,
                   int opCode,
                   void *,
                   zmtpStream *stream,
                   size_t size)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  op->state = stInitialize;
  op->stateRw = stInitialize;
  op->data = nullptr;
  op->stream = stream;
  op->size = 0;
  op->size = size;
  return op;
}

zmtpOp *initWriteOp(aioExecuteProc *start,
                    aioFinishProc *finish,
                    zmtpSocket *socket,
                    AsyncFlags flags,
                    uint64_t timeout,
                    void *callback,
                    void *arg,
                    int opCode,
                    void *data,
                    size_t size,
                    zmtpUserMsgTy type)
{
  asyncOpRoot *opptr =
    initAsyncOpRoot(poolId, timerPoolId, alloc, start, cancel, finish, &socket->root, reinterpret_cast<void*>(callback), arg, flags, opCode, timeout);
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  op->state = stInitialize;
  op->stateRw = stInitialize;
  op->data = data;
  op->stream = nullptr;
  op->size = size;
  switch (type) {
    case zmtpCommand :
      op->type = (size < 256) ? zmtpMsgFlagCommand : zmtpMsgFlagCommand | zmtpMsgFlagLong;
      break;
    case zmtpMessagePart :
      op->type = (size < 256) ? zmtpMsgFlagMore : zmtpMsgFlagMore | zmtpMsgFlagLong;
      break;
    case zmtpMessage :
    default :
      op->type = (size < 256) ? zmtpMsgFlagNone : zmtpMsgFlagLong;
      break;
  }
  return op;
}

static AsyncOpStatus startZmtpAccept(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->state) {
      case stInitialize : {
        op->state = stAcceptWriteLocalSignature;
        childOp = implRead(socket->plainSocket, socket->buffer, 10, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptWriteLocalSignature : {
        if (socket->buffer[0] != 0xFF || socket->buffer[9] != 0x7F)
          return aosUnknownError;
        op->state = stAcceptReadMajorVersion;
        childOp = implWrite(socket->plainSocket, localSignature, sizeof(localSignature), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptReadMajorVersion : {
        op->state = stAcceptWriteLocalMajorVersion;
        childOp = implRead(socket->plainSocket, socket->buffer, 1, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptWriteLocalMajorVersion : {
        op->state = stAcceptReadMinorVersion;
        childOp = implWrite(socket->plainSocket, &localMajorVersion, 1, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptReadMinorVersion : {
        op->state = stAcceptWriteLocalGreeting;
        childOp = implRead(socket->plainSocket, socket->buffer, 1+20+1+31, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptWriteLocalGreeting : {
        op->state = stAcceptReadReadyMsg;
        childOp = implWrite(socket->plainSocket, localGreetingOther, sizeof(localGreetingOther), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptReadReadyMsg : {
        op->state = stAcceptWriteReadyMsgWaiting;
        op->stateRw = stInitialize;
        op->stream = nullptr;
        op->data = socket->buffer;
        op->size = sizeof(socket->buffer);
        break;
      }

      case stAcceptWriteReadyMsgWaiting : {
        AsyncOpStatus result = startZmtpRecv(&op->root);
        if (result != aosSuccess)
          return result;

        // TODO: check message
        RawData rawSocketType;
        RawData rawIdentity;
        zmtpStream stream(socket->buffer, op->transferred);
        if (!stream.readReadyCmd(&rawSocketType, &rawIdentity))
          return aosUnknownError;

        op->state = stAcceptWriteReadyMsg;
        stream.reset();
        stream.write<uint16_t>(0);
        stream.writeReadyCmd(socketTypeNames[socket->type], nullptr);
        stream.data<uint8_t>()[0] = zmtpMsgFlagCommand;
        stream.data<uint8_t>()[1] = static_cast<uint8_t>(stream.offsetOf() - 2);
        childOp = implWrite(socket->plainSocket, stream.data(), stream.offsetOf(), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stAcceptWriteReadyMsg : {
        return aosSuccess;
      }

      default :
        return aosUnknownError;
    }
  }

  opStart(childOp);
  return aosPending;
}

static AsyncOpStatus startZmtpConnect(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->state) {
      case stInitialize : {
        op->state = stConnectWriteLocalSignature;
        aioConnect(socket->plainSocket, &op->address, 0, resumeConnectCb, op);
        return aosPending;
      }
      case stConnectWriteLocalSignature : {
        op->state = stConnectReadSignature;
        childOp = implWrite(socket->plainSocket, localSignature, sizeof(localSignature), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectReadSignature : {
        op->state = stConnectWriteLocalMajorVersion;
        childOp = implRead(socket->plainSocket, socket->buffer, 10, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectWriteLocalMajorVersion : {
        op->state = stConnectReadMajorVersion;
        childOp = implWrite(socket->plainSocket, &localMajorVersion, sizeof(localMajorVersion), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectReadMajorVersion : {
        op->state = stConnectWriteLocalGreeting;
        childOp = implRead(socket->plainSocket, socket->buffer, 1, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectWriteLocalGreeting : {
        op->state = stConnectReadGreeting;
        childOp = implWrite(socket->plainSocket, localGreetingOther, sizeof(localGreetingOther), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectReadGreeting : {
        op->state = stConnectWriteReadyMsg;
        childOp = implRead(socket->plainSocket, socket->buffer, 1+20+1+31, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectWriteReadyMsg : {
        op->state = stConnectReadReadyMsg;
        zmtpStream stream(socket->buffer, sizeof(socket->buffer));
        stream.reset();
        stream.write<uint16_t>(0);
        stream.writeReadyCmd(socketTypeNames[socket->type], "");
        stream.data<uint8_t>()[0] = zmtpMsgFlagCommand;
        stream.data<uint8_t>()[1] = static_cast<uint8_t>(stream.offsetOf() - 2);
        childOp = implWrite(socket->plainSocket, stream.data(), stream.offsetOf(), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stConnectReadReadyMsg : {
        op->state = stConnectReadReadyMsgWaiting;
        op->stateRw = stInitialize;
        op->stream = nullptr;
        op->data = socket->buffer;
        op->size = sizeof(socket->buffer);
        break;
      }

      case stConnectReadReadyMsgWaiting : {
        AsyncOpStatus result = startZmtpRecv(&op->root);
        if (result != aosSuccess)
          return result;

        // TODO: check message
        return aosSuccess;
      }

      default :
        return aosUnknownError;
    }
  }

  opStart(childOp);
  return aosPending;
}

static AsyncOpStatus startZmtpRecv(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->stateRw) {
      case stInitialize : {
        op->stateRw = stRecvReadType;
        if (op->stream)
          op->stream->reset();
        break;
      }
      case stRecvReadType : {
        op->stateRw = stRecvReadSize;
        childOp = implRead(socket->plainSocket, socket->buffer, 2, afNone, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stRecvReadSize : {
        op->stateRw = stRecvReadData;
        op->type = static_cast<zmtpMsgTy>(socket->buffer[0]);
        if (op->type & zmtpMsgFlagLong)
          childOp = implRead(socket->plainSocket, socket->buffer+2, 7, afNone, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stRecvReadData : {
        op->stateRw = (op->type & zmtpMsgFlagMore) ? stRecvReadType : stFinished;
        if (!(op->type & zmtpMsgFlagLong))
          op->transferred = static_cast<size_t>(socket->buffer[1]);
        else
          op->transferred = static_cast<size_t>(xntoh<uint64_t>(reinterpret_cast<uint64_t*>(socket->buffer+1)[0]));

        if (op->transferred <= op->size)
          childOp = implRead(socket->plainSocket, op->stream ? op->stream->reserve(op->transferred) : op->data, op->transferred, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        else
          return aosBufferTooSmall;
        break;
      }

      case stFinished : {
        if (op->stream)
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

static asyncOpRoot *implZmtpRecvStream(zmtpSocket *socket, zmtpStream &msg, size_t limit, AsyncFlags flags, uint64_t timeout, zmtpRecvCb callback, void *arg, size_t *bytesRead, zmtpMsgTy *msgRead)
{
  size_t bytes;
  asyncOpRoot *childOp = nullptr;
  zmtpMsgTy type = zmtpMsgFlagNone;
  zmtpOpState state = stInitialize;
  size_t transferred = 0;

  bool finish = false;
  msg.reset();
  while (!finish) {
    if ( (childOp = implRead(socket->plainSocket, socket->buffer, 2, afNone, 0, resumeRwCb, nullptr, &bytes)) ) {
      state = stRecvReadSize;
      break;
    }

    type = static_cast<zmtpMsgTy>(socket->buffer[0]);
    if (type & zmtpMsgFlagLong) {
      childOp = implRead(socket->plainSocket, socket->buffer+2, 7, afNone, 0, resumeRwCb, nullptr, &bytes);
      if (childOp) {
        state = stRecvReadData;
        break;
      }
    }

    if (!(type & zmtpMsgFlagLong))
      transferred = static_cast<size_t>(socket->buffer[1]);
    else
      transferred = static_cast<size_t>(xntoh<uint64_t>(reinterpret_cast<uint64_t*>(socket->buffer+1)[0]));

    if (transferred <= limit) {
      if ( (childOp = implRead(socket->plainSocket, msg.reserve(transferred), transferred, afWaitAll, 0, resumeRwCb, nullptr, &bytes)) ) {
        state = (type & zmtpMsgFlagMore) ? stRecvReadType : stFinished;
        break;
      }
    } else {
      zmtpOp *op = initReadOp(startZmtpRecv, recvFinish, socket, flags, 0, reinterpret_cast<void*>(callback), arg, zmtpOpRecv, nullptr, &msg, limit);
      opForceStatus(&op->root, aosBufferTooSmall);
      return &op->root;
    }

    finish = (type & zmtpMsgFlagMore) ? false : true;
  }

  if (childOp) {
    zmtpOp *op = initReadOp(startZmtpRecv, recvFinish, socket, flags|afRunning, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpRecv, nullptr, &msg, limit);
    op->stateRw = state;
    op->type = type;
    op->transferred = transferred;
    childOp->arg = op;
    opStart(childOp);
    return &op->root;
  }

  msg.seekSet(0);
  *msgRead = type;
  *bytesRead = transferred;
  return nullptr;
}

static AsyncOpStatus startZmtpSend(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  unsigned offset = 0;
  while (!childOp) {
    switch (op->stateRw) {
      case stInitialize : {
        op->stateRw = stWriteSize;
        if (!(op->type & zmtpMsgFlagCommand) &&
            (socket->type == zmtpSocketREQ || socket->type == zmtpSocketREP) && !socket->needSendMore) {
          socket->buffer[0] = zmtpMsgFlagMore;
          socket->buffer[1] = 0;
          offset = 2;
        }

        socket->needSendMore = (op->type & zmtpMsgFlagMore);
        break;
      }
      case stWriteSize : {
        op->stateRw = stWriteData;
        if (!(op->type & zmtpMsgFlagLong)) {
          socket->buffer[offset] = static_cast<uint8_t>(op->type);
          socket->buffer[offset+1] = static_cast<uint8_t>(op->size);
          offset += 2;
        } else {
          socket->buffer[offset] = static_cast<uint8_t>(op->type);
          reinterpret_cast<uint64_t*>(socket->buffer+offset+1)[0] = xhton<uint64_t>(op->size);
          offset += 9;
        }

        if (op->size <= (sizeof(socket->buffer) - offset)) {
          op->stateRw = stFinished;
          memcpy(socket->buffer+offset, op->data, op->size);
          childOp = implWrite(socket->plainSocket, socket->buffer, op->size+offset, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          op->stateRw = stWriteData;
          childOp = implWrite(socket->plainSocket, socket->buffer, offset, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        }

        break;
      }

      case stWriteData : {
        op->stateRw = stFinished;
        childOp = implWrite(socket->plainSocket, op->data, op->size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
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

asyncOpRoot *implZmtpSend(zmtpSocket *socket, void *data, size_t size, zmtpUserMsgTy type, AsyncFlags flags, uint64_t timeout, zmtpSendCb callback, void *arg, size_t *bytesTransferred)
{
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  zmtpOpState state = stInitialize;
  unsigned offset = 0;
  zmtpMsgTy msgType;

  switch (type) {
    case zmtpCommand :
      msgType = (size < 256) ? zmtpMsgFlagCommand : zmtpMsgFlagCommand | zmtpMsgFlagLong;
      break;
    case zmtpMessagePart :
      msgType = (size < 256) ? zmtpMsgFlagMore : zmtpMsgFlagMore | zmtpMsgFlagLong;
      break;
    case zmtpMessage :
    default :
      msgType = (size < 256) ? zmtpMsgFlagNone : zmtpMsgFlagLong;
      break;
  }

  if (!(msgType & zmtpMsgFlagCommand) &&
      (socket->type == zmtpSocketREQ || socket->type == zmtpSocketREP) && !socket->needSendMore) {
    socket->buffer[0] = zmtpMsgFlagMore;
    socket->buffer[1] = 0;
    offset = 2;
  }

  socket->needSendMore = (msgType & zmtpMsgFlagMore);

  if (!(msgType & zmtpMsgFlagLong)) {
    socket->buffer[offset] = static_cast<uint8_t>(msgType);
    socket->buffer[offset+1] = static_cast<uint8_t>(size);
    offset += 2;
  } else {
    socket->buffer[offset] = static_cast<uint8_t>(msgType);
    reinterpret_cast<uint64_t*>(socket->buffer+offset+1)[0] = xhton<uint64_t>(size);
    offset += 9;
  }

  if (size <= (sizeof(socket->buffer) - offset)) {
    memcpy(socket->buffer+offset, data, size);
    childOp = implWrite(socket->plainSocket, socket->buffer, size+offset, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    state = stFinished;
  } else {
    childOp = implWrite(socket->plainSocket, socket->buffer, offset, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    if (!childOp) {
      childOp = implWrite(socket->plainSocket, data, size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
      state = stFinished;
    } else {
      state = stWriteData;
    }
  }

  if (childOp) {
    zmtpOp *op = initWriteOp(startZmtpSend, sendFinish, socket, flags|afRunning, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpConnect, data, size, type);
    op->stateRw = state;
    op->type = msgType;
    childOp->arg = op;
    opStart(childOp);
    return &op->root;
  }

  *bytesTransferred = size;
  return nullptr;
}

void zmtpSocketDestructor(aioObjectRoot *object)
{
  deleteAioObject(reinterpret_cast<zmtpSocket*>(object)->plainSocket);
  objectRelease(object, zmtpSocketPool);
}

zmtpSocket *zmtpSocketNew(asyncBase *base, aioObject *plainSocket, zmtpSocketTy type)
{
  zmtpSocket *socket = static_cast<zmtpSocket*>(objectGet(zmtpSocketPool));
  if (!socket)
    socket = static_cast<zmtpSocket*>(malloc(sizeof(zmtpSocket)));
  initObjectRoot(&socket->root, base, ioObjectUserDefined, zmtpSocketDestructor);
  socket->plainSocket = plainSocket;
  socket->type = type;
  socket->needSendMore = false;
  setSocketBuffer(plainSocket, 256);
  return socket;
}

void zmtpSocketDelete(zmtpSocket *socket)
{
  objectDelete(&socket->root);
}

void aioZmtpAccept(zmtpSocket *socket, AsyncFlags flags, uint64_t timeout, zmtpAcceptCb callback, void *arg)
{
  zmtpOp *op =
    initOp(startZmtpAccept, acceptFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpAccept);
  opStart(&op->root);
}

void aioZmtpConnect(zmtpSocket *socket, const HostAddress *address, AsyncFlags flags, uint64_t timeout, zmtpConnectCb callback, void *arg)
{
  zmtpOp *op =
    initOp(startZmtpConnect, connectFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpConnect);
  op->address = *address;
  opStart(&op->root);
}

ssize_t aioZmtpRecv(zmtpSocket *socket, zmtpStream &msg, size_t limit, AsyncFlags flags, uint64_t timeout, zmtpRecvCb callback, void *arg)
{
  ssize_t result = -aosPending;
  size_t bytesTransferred;
  zmtpMsgTy type = zmtpMsgFlagNone;
  aioMethod([=, &msg]() { return &initReadOp(startZmtpRecv, recvFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpRecv, nullptr, &msg, limit)->root; },
            [=, &msg, &bytesTransferred, &type]() { return implZmtpRecvStream(socket, msg, limit, flags, timeout, callback, arg, &bytesTransferred, &type); },
            [&bytesTransferred, &result]() { result = static_cast<ssize_t>(bytesTransferred); },
            [&bytesTransferred, &type](asyncOpRoot *op) {
              reinterpret_cast<zmtpOp*>(op)->type = type;
              reinterpret_cast<zmtpOp*>(op)->transferred = bytesTransferred;
            },
            &socket->root,
            flags,
            reinterpret_cast<void*>(callback),
            zmtpOpRecv);
  return result;
}

ssize_t aioZmtpSend(zmtpSocket *socket, void *data, size_t size, zmtpUserMsgTy type, AsyncFlags flags, uint64_t timeout, zmtpSendCb callback, void *arg)
{
  ssize_t result = -aosPending;
  size_t bytesTransferred;
  aioMethod([=]() { return &initWriteOp(startZmtpSend, sendFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpConnect, data, size, type)->root; },
            [=, &bytesTransferred]() { return implZmtpSend(socket, data, size, type, flags, timeout, callback, arg, &bytesTransferred); },
            [&bytesTransferred, &result]() { result = static_cast<ssize_t>(bytesTransferred); },
            [&bytesTransferred](asyncOpRoot *op) { reinterpret_cast<zmtpOp*>(op)->transferred = bytesTransferred; },
            &socket->root,
            flags,
            reinterpret_cast<void*>(callback),
            zmtpOpSend);
  return result;
}

int ioZmtpAccept(zmtpSocket *socket, AsyncFlags flags, uint64_t timeout)
{
  zmtpOp *op =
    initOp(startZmtpAccept, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, zmtpOpAccept);
  combinerCall(&socket->root, 1, &op->root, aaStart);
  coroutineYield();

  AsyncOpStatus status = opGetStatus(&op->root);
  objectRelease(&op->root, op->root.poolId);
  objectDecrementReference(&socket->root, 1);

  return status == aosSuccess ? 0 : -status;
}

int ioZmtpConnect(zmtpSocket *socket, const HostAddress *address, AsyncFlags flags, uint64_t timeout)
{
  zmtpOp *op =
    initOp(startZmtpConnect, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, zmtpOpConnect);
  op->address = *address;
  combinerCall(&socket->root, 1, &op->root, aaStart);
  coroutineYield();
  AsyncOpStatus status = opGetStatus(&op->root);
  objectRelease(&op->root, op->root.poolId);
  objectDecrementReference(&socket->root, 1);

  return status == aosSuccess ? 0 : -status;
}

ssize_t ioZmtpRecv(zmtpSocket *socket, zmtpStream &msg, size_t limit, AsyncFlags flags, uint64_t timeout, zmtpUserMsgTy *type)
{
  zmtpMsgTy msgType = zmtpMsgFlagNone;
  size_t bytesTransferred;
  zmtpOp *op = reinterpret_cast<zmtpOp*>(
    ioMethod([=, &msg]() { return &initReadOp(startZmtpRecv, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, zmtpOpRecv, nullptr, &msg, limit)->root; },
             [=, &msg, &msgType, &bytesTransferred]() { return implZmtpRecvStream(socket, msg, limit, flags | afCoroutine, timeout, nullptr, nullptr, &bytesTransferred, &msgType); },
             [&bytesTransferred, &msgType](asyncOpRoot *op) {
               reinterpret_cast<zmtpOp*>(op)->type = msgType;
               reinterpret_cast<zmtpOp*>(op)->transferred = bytesTransferred;
             },
             &socket->root,
             zmtpOpSend));
  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    *type = (op->type & zmtpMsgFlagCommand) ? zmtpCommand : zmtpMessage;
    objectRelease(&op->root, op->root.poolId);
    objectDecrementReference(&socket->root, 1);
    return status == aosSuccess ? static_cast<ssize_t>(msg.sizeOf()) : -status;
  } else {
    *type = (msgType & zmtpMsgFlagCommand) ? zmtpCommand : zmtpMessage;
    return static_cast<ssize_t>(msg.sizeOf());
  }
}

ssize_t ioZmtpSend(zmtpSocket *socket, void *data, size_t size, zmtpUserMsgTy type, AsyncFlags flags, uint64_t timeout)
{
  size_t bytesTransferred;
  zmtpOp *op = reinterpret_cast<zmtpOp*>(
    ioMethod([=]() { return &initWriteOp(startZmtpSend, nullptr, socket, flags | afCoroutine, timeout, nullptr, nullptr, zmtpOpConnect, data, size, type)->root; },
             [=, &bytesTransferred]() { return implZmtpSend(socket, data, size, type, flags | afCoroutine, timeout, nullptr, nullptr, &bytesTransferred); },
             [&bytesTransferred](asyncOpRoot *op) { reinterpret_cast<zmtpOp*>(op)->transferred = bytesTransferred; },
             &socket->root,
             zmtpOpSend));

  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    objectRelease(&op->root, op->root.poolId);
    objectDecrementReference(&socket->root, 1);
    return status == aosSuccess ? static_cast<ssize_t>(size) : -status;
  } else {
    return static_cast<ssize_t>(size);
  }
}
