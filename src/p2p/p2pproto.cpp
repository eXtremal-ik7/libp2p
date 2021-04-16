#include "asyncio/coroutine.h"
#include "p2p/p2pproto.h"
#include "p2p/p2pformat.h"
#include <stdlib.h>

static ConcurrentQueue opPool;
static ConcurrentQueue opTimerPool;
static ConcurrentQueue objectPool;

struct Context {
  aioExecuteProc *StartProc;
  aioFinishProc *FinishProc;
  p2pStream *Stream;
  void *Buffer;
  size_t TransactionSize;
  p2pHeader Header;
  Context(aioExecuteProc *startProc,
          aioFinishProc *finishProc,
          p2pStream *stream,
          void *buffer,
          size_t transactionSize,
          p2pHeader header) :
    StartProc(startProc),
    FinishProc(finishProc),
    Stream(stream),
    Buffer(buffer),
    TransactionSize(transactionSize),
    Header(header) {}
};

enum p2pOpTy {
  p2pOpAccept = OPCODE_READ,
  p2pOpRecv,
  p2pOpRecvStream,
  p2pOpConnect = OPCODE_WRITE,
  p2pOpSend
};

enum p2pPeerState {
  stInitialize = 0,

  // connect states
  stConnectConnected,  
  stConnectWaitMsgSend,
  stConnectResponseReceived,
  
  // accept states
  stAcceptAuthReceived,
  stAcceptWaitAnswerSend,

  // other states
  stTransferring,
  stFinished
};

static AsyncOpStatus sendProc(asyncOpRoot *opptr);
static AsyncOpStatus recvStreamProc(asyncOpRoot *opptr);

static inline AsyncOpStatus p2pStatusFromError(p2pErrorTy error)
{
  switch (error) {
    case p2pOk : return aosSuccess;
    case p2pErrorAuthFailed : return p2pMakeStatus(p2pStAuthFailed);
    case p2pErrorAppNotFound : return p2pMakeStatus(p2pStAppNotFound);
  }

  return aosUnknownError;
}

static void resumeRwCb(AsyncOpStatus status, aioObject*, size_t, void *arg)
{
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static int cancel(asyncOpRoot *opptr)
{
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  cancelIo(reinterpret_cast<aioObjectRoot*>(connection->socket));
  return 0;
}

static void acceptFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<p2pAcceptCb*>(opptr->callback)(opGetStatus(opptr),
                                                  reinterpret_cast<p2pConnection*>(opptr->object),
                                                  nullptr,
                                                  opptr->arg);
}

static void connectFinish(asyncOpRoot *opptr)
{
  reinterpret_cast<p2pConnectCb*>(opptr->callback)(opGetStatus(opptr),
                                                   reinterpret_cast<p2pConnection*>(opptr->object),
                                                   opptr->arg);
}

static void recvFinish(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  reinterpret_cast<p2preadCb*>(opptr->callback)(opGetStatus(opptr),
                                                reinterpret_cast<p2pConnection*>(opptr->object),
                                                op->header,
                                                op->buffer,
                                                opptr->arg);
}

static void recvStreamFinish(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  reinterpret_cast<p2preadStreamCb*>(opptr->callback)(opGetStatus(opptr),
                                                      reinterpret_cast<p2pConnection*>(opptr->object),
                                                      op->header,
                                                      op->stream,
                                                      opptr->arg);
}

static void sendFinish(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  reinterpret_cast<p2pwriteCb*>(opptr->callback)(opGetStatus(opptr),
                                                 reinterpret_cast<p2pConnection*>(opptr->object),
                                                 op->header,
                                                 opptr->arg);
}

static void releaseProc(asyncOpRoot*)
{
}

static asyncOpRoot *newAsyncOp(aioObjectRoot *object,
                               AsyncFlags flags,
                               uint64_t usTimeout,
                               void *callback,
                               void *arg,
                               int opCode,
                               void *contextPtr)
{
  struct Context *context = (struct Context*)contextPtr;
  p2pOp *op = 0;
  if (asyncOpAlloc(object->base, sizeof(p2pOp), flags & afRealtime, &opPool, &opTimerPool, (asyncOpRoot**)&op)) {
  }

  initAsyncOpRoot(&op->root, context->StartProc, cancel, context->FinishProc, releaseProc, object, callback, arg, flags, opCode, usTimeout);
  if (context->Stream)
    op->stream = context->Stream;
  else
    op->buffer = context->Buffer;
  op->bufferSize = context->TransactionSize;
  op->state = stInitialize;
  op->rwState = stInitialize;
  if (opCode == p2pOpSend)
    op->header = context->Header;
  return &op->root;
}


static void destructor(aioObjectRoot *root)
{
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(root);
  deleteAioObject(connection->socket);
  concurrentQueuePush(&objectPool, connection);
}

p2pConnection *p2pConnectionNew(aioObject *socket)
{
  p2pConnection *connection = 0;
  if (!concurrentQueuePop(&objectPool, (void**)&connection)) {
    connection = static_cast<p2pConnection*>(malloc(sizeof(p2pConnection)));
    new(&connection->stream) xmstream;
  }

  initObjectRoot(&connection->root, aioGetBase(socket), ioObjectUserDefined, destructor);
  connection->socket = socket;
  setSocketBuffer(socket, 256);
  return connection;
}

void p2pConnectionDelete(p2pConnection *connection)
{
  objectDelete(&connection->root);
}

static AsyncOpStatus acceptProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  AsyncOpStatus result = aosSuccess;
  bool finish = false;
  while (!finish && result == aosSuccess) {
    switch (op->state) {
      case stInitialize : {
        op->state = stAcceptAuthReceived;
        op->rwState = stInitialize;
        op->stream = &connection->stream;
        op->bufferSize = 4096;
        break;
      }

      case stAcceptAuthReceived : {
        AsyncOpStatus recvResult = recvStreamProc(opptr);
        if (recvResult != aosSuccess)
          return recvResult;

        if (connection->stream.readConnectMessage(&op->connectMsg)) {
          op->state = stAcceptWaitAnswerSend;
          p2pErrorTy authStatus = reinterpret_cast<p2pAcceptCb*>(op->root.callback)(aosPending, connection, &op->connectMsg, op->root.arg);
          connection->stream.reset();
          connection->stream.writeStatusMessage(authStatus);
          op->lastError = p2pStatusFromError(authStatus);
          op->rwState = stInitialize;
          op->buffer = connection->stream.data();
          op->header = p2pHeader(p2pMsgStatus, static_cast<uint32_t>(connection->stream.sizeOf()));
        } else {
          result = p2pMakeStatus(p2pStFormatError);
        }

        break;
      }

      case stAcceptWaitAnswerSend : {
        AsyncOpStatus sendResult = sendProc(opptr);
        if (sendResult != aosSuccess)
          return sendResult;
        result = op->lastError;
        finish = true;
        break;
      }
    }
  }

  return result;
}

void aiop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{
  Context context(acceptProc, acceptFinish, nullptr, nullptr, 0, p2pHeader());
  p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afNone, timeout, reinterpret_cast<void*>(callback), arg, p2pOpAccept, &context));
  combinerPushOperation(&op->root, aaStart);
}

static void connectCb(AsyncOpStatus status, aioObject *socket, void *arg)
{
  __UNUSED(socket);
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static AsyncOpStatus connectProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  AsyncOpStatus result = aosSuccess;
  bool finish = false;

  while (!finish && result == aosSuccess) {
    switch (op->state) {
      case stInitialize : {
        op->state = stConnectConnected;
        aioConnect(connection->socket, &op->address, 0, connectCb, op);
        result = aosPending;
        break;
      }
      
      case stConnectConnected : {
        op->state = stConnectWaitMsgSend;
        op->rwState = stInitialize;
        op->buffer = connection->stream.data();
        op->header = p2pHeader(p2pMsgConnect, static_cast<uint32_t>(connection->stream.sizeOf()));
        break;
      }
      case stConnectWaitMsgSend : {
        AsyncOpStatus sendResult = sendProc(opptr);
        if (sendResult != aosSuccess)
          return sendResult;

        op->state = stConnectResponseReceived;
        op->rwState = stInitialize;
        op->stream = &connection->stream;
        op->bufferSize = 4096;
        break;
      }

      case stConnectResponseReceived : {
        p2pErrorTy error;
        AsyncOpStatus recvResult = recvStreamProc(opptr);
        if (recvResult != aosSuccess)
          return recvResult;


        if (connection->stream.readStatusMessage(&error))
          result = p2pStatusFromError(error);
        else
          result = p2pMakeStatus(p2pStFormatError);
        finish = true;
        break;
      }
    }
  }

  return result;
}

void aiop2pConnect(p2pConnection *connection, const HostAddress *address, p2pConnectData *data, uint64_t timeout, p2pConnectCb *callback, void *arg)
{
  Context context(connectProc, connectFinish, nullptr, nullptr, 0, p2pHeader());
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afNone, timeout, reinterpret_cast<void*>(callback), arg, p2pOpConnect, &context));
  op->address = *address;
  combinerPushOperation(&op->root, aaStart);
}

static AsyncOpStatus recvBufferProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->rwState) {
      case stInitialize : {
        op->rwState = stTransferring;
        childOp = implRead(connection->socket, &op->header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stTransferring : {
        op->rwState = stFinished;
        if (op->header.size <= op->bufferSize)  {
          childOp = implRead(connection->socket, op->buffer, op->header.size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          return aosBufferTooSmall;
        }

        break;
      }

      case stFinished :
        return aosSuccess;
    }
  }

  combinerPushOperation(&op->root, aaStart);
  return aosPending;
}

asyncOpRoot *implp2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, void *callback, void *arg, p2pHeader *header)
{
  size_t bytes;
  asyncOpRoot *childOp = nullptr;
  int state = stInitialize;

  while (true) {
    childOp = implRead(connection->socket, header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    if (childOp) {
      state = stTransferring;
      break;
    }

    if (header->size <= bufferSize) {
      childOp = implRead(connection->socket, buffer, header->size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
      if (childOp) {
        state = stFinished;
        break;
      }
    } else {
      Context context(recvBufferProc, recvFinish, nullptr, buffer, bufferSize, p2pHeader());
      p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afNone, timeout, callback, arg, p2pOpRecv, &context));
      opForceStatus(&op->root, aosBufferTooSmall);
      return &op->root;
    }

    break;
  }

  if (childOp) {
    Context context(recvBufferProc, recvFinish, nullptr, buffer, bufferSize, p2pHeader());
    p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, flags|afRunning, timeout, callback, arg, p2pOpRecv, &context));
    op->header = *header;
    op->rwState = state;
    childOp->arg = op;
    if (state == stTransferring)
      implReadModify(childOp, &op->header, sizeof(p2pHeader));
    combinerPushOperation(childOp, aaStart);
    return &op->root;
  }

  return nullptr;
}

static asyncOpRoot *implp2pRecvProxy(aioObjectRoot *object, AsyncFlags flags, uint64_t usTimeout, void *callback, void *arg, void *contextPtr)
{
  struct Context *context = (struct Context*)contextPtr;
  return implp2pRecv(reinterpret_cast<p2pConnection*>(object), context->Buffer, static_cast<uint32_t>(context->TransactionSize), flags, usTimeout, callback, arg, &context->Header);
}

void aiop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2preadCb *callback, void *arg)
{
  Context context(recvBufferProc, recvFinish, nullptr, buffer, bufferSize, p2pHeader());
  auto makeResult = [](void*){};
  auto initOp = [](asyncOpRoot *op, void *contextPtr) { reinterpret_cast<p2pOp*>(op)->header = static_cast<Context*>(contextPtr)->Header; };
  runAioOperation(&connection->root, newAsyncOp, implp2pRecvProxy, makeResult, initOp, flags, timeout, reinterpret_cast<void*>(callback), arg, p2pOpRecv, &context);
}

static AsyncOpStatus recvStreamProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->rwState) {
      case stInitialize : {
        op->rwState = stTransferring;
        op->stream->reset();
        childOp = implRead(connection->socket, &op->header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stTransferring : {
        op->rwState = stFinished;
        if (op->header.size <= op->bufferSize)  {
          childOp = implRead(connection->socket, op->stream->reserve(op->header.size), op->header.size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          return aosBufferTooSmall;
        }

        break;
      }

      case stFinished :
        op->stream->seekSet(0);
        return aosSuccess;
    }
  }

  combinerPushOperation(childOp, aaStart);
  return aosPending;
}

asyncOpRoot *implp2pRecvStream(p2pConnection *connection, p2pStream &stream, size_t maxMsgSize, AsyncFlags flags, uint64_t timeout, void *callback, void *arg, p2pHeader *header)
{
  size_t bytes;
  asyncOpRoot *childOp = nullptr;
  int state = stInitialize;

  while (true) {
    stream.reset();
    childOp = implRead(connection->socket, header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    if (childOp) {
      state = stTransferring;
      break;
    }

    if (header->size <= maxMsgSize) {
      childOp = implRead(connection->socket, stream.reserve(header->size), header->size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
      if (childOp) {
        state = stFinished;
        break;
      }
    } else {
      Context context(recvStreamProc, recvStreamFinish, &stream, nullptr, maxMsgSize, p2pHeader());
      p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afNone, timeout, callback, arg, p2pOpRecvStream, &context));
      opForceStatus(&op->root, aosBufferTooSmall);
      return &op->root;
    }

    break;
  }

  if (childOp) {
    Context context(recvStreamProc, recvStreamFinish, &stream, nullptr, maxMsgSize, p2pHeader());
    p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, flags|afRunning, timeout, callback, arg, p2pOpRecvStream, &context));
    op->header = *header;
    op->rwState = state;
    childOp->arg = op;
    if (state == stTransferring)
      implReadModify(childOp, &op->header, sizeof(p2pHeader));
    combinerPushOperation(childOp, aaStart);
    return &op->root;
  }

  return nullptr;
}

static asyncOpRoot *implp2pRecvStreamProxy(aioObjectRoot *object, AsyncFlags flags, uint64_t usTimeout, void *callback, void *arg, void *contextPtr)
{
  struct Context *context = (struct Context*)contextPtr;
  return implp2pRecvStream(reinterpret_cast<p2pConnection*>(object), *context->Stream, context->TransactionSize, flags, usTimeout, callback, arg, &context->Header);
}

void aiop2pRecvStream(p2pConnection *connection, p2pStream &stream, size_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2preadStreamCb *callback, void *arg)
{
  Context context(recvStreamProc, recvStreamFinish, &stream, nullptr, maxMsgSize, p2pHeader());
  auto makeResult = [](void*){};
  auto initOp = [](asyncOpRoot *op, void *contextPtr) { reinterpret_cast<p2pOp*>(op)->header = static_cast<Context*>(contextPtr)->Header; };
  runAioOperation(&connection->root, newAsyncOp, implp2pRecvStreamProxy, makeResult, initOp, flags, timeout, reinterpret_cast<void*>(callback), arg, p2pOpRecvStream, &context);
}

static AsyncOpStatus sendProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(opptr->object);
  asyncOpRoot *childOp = nullptr;
  size_t bytes;
  while (!childOp) {
    switch (op->rwState) {
      case stInitialize : {
        if (op->header.size < 256) {
          op->rwState = stFinished;
          uint8_t sendBuffer[320];
          memcpy(sendBuffer, &op->header, sizeof(p2pHeader));
          memcpy(sendBuffer+sizeof(p2pHeader), op->buffer, op->header.size);
          childOp = implWrite(connection->socket, sendBuffer, sizeof(p2pHeader)+op->header.size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        } else {
          op->rwState = stTransferring;
          childOp = implWrite(connection->socket, &op->header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, opptr, &bytes);
        }

        break;
      }

      case stTransferring : {
        op->rwState = stFinished;
        childOp = implWrite(connection->socket, op->buffer, op->header.size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
        break;
      }

      case stFinished :
        return aosSuccess;

      default :
        return aosUnknownError;
    }
  }

  combinerPushOperation(childOp, aaStart);
  return aosPending;
}

asyncOpRoot *implp2pSend(p2pConnection *connection, const void *data, p2pHeader header, AsyncFlags flags, uint64_t timeout, void *callback, void *arg)
{
  int state;
  size_t bytes;
  asyncOpRoot *childOp = nullptr;

  if (header.size < 256) {
    state = stFinished;
    uint8_t sendBuffer[320];
    memcpy(sendBuffer, &header, sizeof(p2pHeader));
    memcpy(sendBuffer+sizeof(p2pHeader), data, header.size);
    childOp = implWrite(connection->socket, sendBuffer, sizeof(p2pHeader)+header.size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
  } else {
    state = stTransferring;
    childOp = implWrite(connection->socket, &header, sizeof(p2pHeader), afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    if (!childOp) {
      state = stFinished;
      childOp = implWrite(connection->socket, data, header.size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
    }
  }

  if (childOp) {
    Context context(sendProc, sendFinish, nullptr, const_cast<void*>(data), 0, header);
    p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, flags | afRunning, timeout, callback, arg, p2pOpSend, &context));

    op->rwState = state;
    childOp->arg = op;
    combinerPushOperation(childOp, aaStart);
    return &op->root;
  }

  return nullptr;
}

static asyncOpRoot *implp2pSendProxy(aioObjectRoot *object, AsyncFlags flags, uint64_t usTimeout, void *callback, void *arg, void *contextPtr)
{
  struct Context *context = (struct Context*)contextPtr;
  return implp2pSend(reinterpret_cast<p2pConnection*>(object), context->Buffer, context->Header, flags, usTimeout, callback, arg);
}

void aiop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, AsyncFlags flags, uint64_t timeout, p2pwriteCb *callback, void *arg)
{
  Context context(sendProc, sendFinish, nullptr, const_cast<void*>(data), 0, p2pHeader(id, type, size));
  auto makeResult = [](void*){};
  auto initOp = [](asyncOpRoot*, void*) {};
  runAioOperation(&connection->root, newAsyncOp, implp2pSendProxy, makeResult, initOp, flags, timeout, reinterpret_cast<void*>(callback), arg, p2pOpSend, &context);
}

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{ 
  Context context(acceptProc, 0, nullptr, nullptr, 0, p2pHeader());
  p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afCoroutine, timeout, reinterpret_cast<void*>(callback), arg, p2pOpAccept, &context));
  combinerPushOperation(&op->root, aaStart);
  coroutineYield();
  AsyncOpStatus status = opGetStatus(&op->root);
  releaseAsyncOp(&op->root);
  return status == aosSuccess ? 0 : -status;
}

int iop2pConnect(p2pConnection *connection, const HostAddress *address, uint64_t timeout, p2pConnectData *data)
{
  Context context(connectProc, 0, nullptr, nullptr, 0, p2pHeader());
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  p2pOp *op = reinterpret_cast<p2pOp*>(newAsyncOp(&connection->root, afCoroutine, timeout, nullptr, nullptr, p2pOpConnect, &context));
  op->address = *address;
  combinerPushOperation(&op->root, aaStart);
  coroutineYield();
  AsyncOpStatus status = opGetStatus(&op->root);
  releaseAsyncOp(&op->root);
  return status == aosSuccess ? 0 : -status;
}

ssize_t iop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, AsyncFlags flags, uint64_t timeout)
{
  Context context(sendProc, 0, nullptr, const_cast<void*>(data), 0, p2pHeader(id, type, size));
  auto initOp = [](asyncOpRoot*, void*) {};
  asyncOpRoot *op = runIoOperation(&connection->root, newAsyncOp, implp2pSendProxy, initOp, flags, timeout, p2pOpSend, &context);

  if (op) {
    AsyncOpStatus status = opGetStatus(op);
    releaseAsyncOp(op);
    return status == aosSuccess ? static_cast<ssize_t>(size) : -status;
  } else {
    return static_cast<ssize_t>(size);
  }
}

ssize_t iop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header)
{
  Context context(recvStreamProc, 0, &stream, nullptr, maxMsgSize, p2pHeader());
  auto initOp = [](asyncOpRoot *op, void *contextPtr) { reinterpret_cast<p2pOp*>(op)->header = static_cast<Context*>(contextPtr)->Header; };
  p2pOp *op = reinterpret_cast<p2pOp*>(runIoOperation(&connection->root, newAsyncOp, implp2pRecvStreamProxy, initOp, flags, timeout, p2pOpRecvStream, &context));
  *header = context.Header;

  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    *header = op->header;
    releaseAsyncOp(&op->root);
    return status == aosSuccess ? static_cast<ssize_t>(header->size) : -status;
  } else {
    return static_cast<ssize_t>(header->size);
  }
}

ssize_t iop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header)
{
  Context context(recvBufferProc, 0, nullptr, buffer, bufferSize, p2pHeader());
  auto initOp = [](asyncOpRoot *op, void *contextPtr) { reinterpret_cast<p2pOp*>(op)->header = static_cast<Context*>(contextPtr)->Header; };
  p2pOp *op = reinterpret_cast<p2pOp*>(runIoOperation(&connection->root, newAsyncOp, implp2pRecvProxy, initOp, flags, timeout, p2pOpRecv, &context));
  *header = context.Header;

  if (op) {
    AsyncOpStatus status = opGetStatus(&op->root);
    *header = op->header;
    releaseAsyncOp(&op->root);
    return status == aosSuccess ? static_cast<ssize_t>(header->size) : -status;
  } else {
    return static_cast<ssize_t>(header->size);
  }
}
