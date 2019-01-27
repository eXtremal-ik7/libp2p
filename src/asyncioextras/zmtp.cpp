#include "asyncioextras/zmtp.h"
#include "asyncio/coroutine.h"

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

enum zmtpOpTy {
  zmtpOpAccept = OPCODE_READ,
  zmtpOpRecv,
  zmtpOpConnect = OPCODE_WRITE,
  zmtpOpSendCommand,
  zmtpOpSendMessage
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

  stRecvReadSize,
  stRecvReadData,

  stWriteSize,
  stWriteData,

  stFinished
};

struct zmtpSocket {
  aioObjectRoot root;
  aioObject *plainSocket;
  zmtpSocketTy type;
  bool needSendMore;
  uint8_t buffer[128];
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

struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  zmtpUserMsgTy type;
};

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

void coroAcceptCb(AsyncOpStatus status, zmtpSocket*, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;
  coroutineCall(r->coroutine);
}

void coroConnectCb(AsyncOpStatus status, zmtpSocket*, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;
  coroutineCall(r->coroutine);
}

void coroRecvCb(AsyncOpStatus status, zmtpSocket*, zmtpUserMsgTy type, zmtpStream*, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;
  r->type = type;
  coroutineCall(r->coroutine);
}

void coroSendCb(AsyncOpStatus status, zmtpSocket*, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;
  coroutineCall(r->coroutine);
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
        if (op->stream)
          op->stream->reset();
        op->stateRw = stRecvReadSize;
        childOp = implRead(socket->plainSocket, socket->buffer, 1, afNone, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stRecvReadSize : {
        op->stateRw = stRecvReadData;
        op->type = static_cast<zmtpMsgTy>(socket->buffer[0]);
        if (!(op->type & zmtpMsgFlagLong)) {
          childOp = implRead(socket->plainSocket, socket->buffer, 1, afNone, 0, resumeRwCb, opptr, &bytes);
        } else {
          childOp = implRead(socket->plainSocket, socket->buffer, 8, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        }

        break;
      }

      case stRecvReadData : {
        op->stateRw = (op->type & zmtpMsgFlagMore) ? stInitialize : stFinished;
        if (!(op->type & zmtpMsgFlagLong)) {
          op->transferred = static_cast<size_t>(socket->buffer[0]);
        } else {
          op->transferred = static_cast<size_t>(xntoh<uint64_t>(reinterpret_cast<uint64_t*>(socket->buffer)[0]));
        }

        if (op->transferred <= op->size)
          childOp = implRead(socket->plainSocket, op->stream ? op->stream->alloc(op->transferred) : op->data, op->transferred, afWaitAll, 0, resumeRwCb, opptr, &bytes);
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

static AsyncOpStatus startZmtpSend(asyncOpRoot *opptr)
{
  zmtpOp *op = reinterpret_cast<zmtpOp*>(opptr);
  zmtpSocket *socket = reinterpret_cast<zmtpSocket*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->stateRw) {
      case stInitialize : {
        op->stateRw = stWriteSize;
        if (!(op->type & zmtpMsgFlagCommand) &&
            (socket->type == zmtpSocketREQ || socket->type == zmtpSocketREP) &&
            !socket->needSendMore) {
          uint8_t X[2] = {zmtpMsgFlagMore, 0};
          childOp = implWrite(socket->plainSocket, X, 2, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        }

        socket->needSendMore = (op->type & zmtpMsgFlagMore);
        break;
      }
      case stWriteSize : {
        op->stateRw = stWriteData;
        if (!(op->type & zmtpMsgFlagLong)) {
          uint8_t X[2];
          X[0] = static_cast<uint8_t>(op->type);
          X[1] = static_cast<uint8_t>(op->size);
          childOp = implWrite(socket->plainSocket, X, 2, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          uint8_t X[16];
          X[0] = static_cast<uint8_t>(op->type);
          *reinterpret_cast<uint64_t*>(&X[1]) = xhton<uint64_t>(op->size);
          childOp = implWrite(socket->plainSocket, X, 9, afWaitAll, 0, resumeRwCb, opptr, &bytes);
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

void zmtpSocketDestructor(aioObjectRoot *object)
{
  deleteAioObject(reinterpret_cast<zmtpSocket*>(object)->plainSocket);
}

zmtpSocket *zmtpSocketNew(asyncBase *base, aioObject *plainSocket, zmtpSocketTy type)
{
  zmtpSocket *socket = static_cast<zmtpSocket*>(malloc(sizeof(zmtpSocket)));
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
  zmtpOp *op =
    initReadOp(startZmtpRecv, recvFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpRecv, nullptr, &msg, limit);
  opStart(&op->root);
  return -aosPending;
}

ssize_t aioZmtpSend(zmtpSocket *socket, void *data, size_t size, zmtpUserMsgTy type, AsyncFlags flags, uint64_t timeout, zmtpSendCb callback, void *arg)
{
  zmtpOp *op =
    initWriteOp(startZmtpSend, sendFinish, socket, flags, timeout, reinterpret_cast<void*>(callback), arg, zmtpOpConnect, data, size, type);
  opStart(&op->root);
  return -aosPending;
}

int ioZmtpAccept(zmtpSocket *socket, AsyncFlags flags, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosUnknown, zmtpUnknown};
  zmtpOp *op =
    initOp(startZmtpAccept, acceptFinish, socket, flags, timeout, reinterpret_cast<void*>(coroAcceptCb), &r, zmtpOpAccept);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -r.status;
}

int ioZmtpConnect(zmtpSocket *socket, const HostAddress *address, AsyncFlags flags, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosUnknown, zmtpUnknown};
  zmtpOp *op =
    initOp(startZmtpConnect, connectFinish, socket, flags, timeout, reinterpret_cast<void*>(coroConnectCb), &r, zmtpOpConnect);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -r.status;
}

ssize_t ioZmtpRecv(zmtpSocket *socket, zmtpStream &msg, size_t limit, AsyncFlags flags, uint64_t timeout, zmtpUserMsgTy *type)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosUnknown, zmtpUnknown};
  zmtpOp *op =
    initReadOp(startZmtpRecv, recvFinish, socket, flags, timeout, reinterpret_cast<void*>(coroRecvCb), &r, zmtpOpRecv, nullptr, &msg, limit);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  *type = r.type;
  return r.status == aosSuccess ? static_cast<ssize_t>(msg.sizeOf()) : -r.status;
}

ssize_t ioZmtpSend(zmtpSocket *socket, void *data, size_t size, zmtpUserMsgTy type, AsyncFlags flags, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosUnknown, zmtpUnknown};
  zmtpOp *op =
    initWriteOp(startZmtpSend, sendFinish, socket, flags, timeout, reinterpret_cast<void*>(coroSendCb), &r, zmtpOpConnect, data, size, type);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? static_cast<ssize_t>(size) : -r.status;
}
