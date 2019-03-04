#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"
#include "p2p/p2pproto.h"
#include "p2p/p2pformat.h"
#include <stdlib.h>

static const char *p2pConnectionPool = "P2PConnection";
static const char *p2pPoolId = "P2P";
static const char *p2pPoolTimerId = "P2PTimer";

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

__NO_PADDING_BEGIN
typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  p2pAcceptCb *acceptCb;
  void *arg;
  p2pHeader *header;
  int status;
} coroReturnStruct;
__NO_PADDING_END

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

static asyncOpRoot *alloc()
{
  return static_cast<asyncOpRoot*>(malloc(sizeof(p2pOp)));
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

static p2pOp *allocp2pOp(aioExecuteProc *executeProc,
                         aioFinishProc *finishProc,
                         p2pConnection *connection,
                         AsyncFlags flags,
                         void *buffer,
                         uint32_t bufferSize,
                         p2pStream *stream,
                         void *callback,
                         void *arg,
                         int opCode,
                         uint64_t timeout)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(
    initAsyncOpRoot(p2pPoolId, p2pPoolTimerId, alloc, executeProc, cancel, finishProc, &connection->root, callback, arg, flags, opCode, timeout));
  if (stream)
    op->stream = stream;
  else
    op->buffer = buffer;
  op->bufferSize = bufferSize;
  op->state = stInitialize;
  op->rwState = stInitialize;
  return op;
}

static p2pErrorTy coroutineAcceptCb(AsyncOpStatus status, p2pConnection *connection, p2pConnectData *data, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  if (data) {
    return r->acceptCb(status, connection, data, r->arg);
  } else {
    r->status = status;
    coroutineCall(r->coroutine);
    return p2pOk;
  }
}


static void coroutineConnectCb(AsyncOpStatus status, p2pConnection *connection, void *arg)
{
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  __UNUSED(connection);
  r->status = status;  
  coroutineCall(r->coroutine);
}

static void coroutineSendCb(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *arg)
{
  __UNUSED(connection);
  __UNUSED(header);
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;  
  coroutineCall(r->coroutine);
}

static void coroutineRecvCb(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *data, void *arg)
{
  __UNUSED(connection);
  __UNUSED(data);
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;  
  *r->header = header;
  coroutineCall(r->coroutine);
}

static void coroutineRecvStreamCb(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, p2pStream *stream, void *arg)
{
  __UNUSED(connection);
  __UNUSED(stream);
  coroReturnStruct *r = static_cast<coroReturnStruct*>(arg);
  r->status = status;
  *r->header = header;
  coroutineCall(r->coroutine);
}

static void destructor(aioObjectRoot *root)
{
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(root);
  deleteAioObject(connection->socket);
  objectRelease(root, p2pConnectionPool);
}

p2pConnection *p2pConnectionNew(aioObject *socket)
{
  p2pConnection *connection = static_cast<p2pConnection*>(objectGet(p2pConnectionPool));
  if (!connection) {
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
  p2pOp *op = allocp2pOp(acceptProc, acceptFinish, connection, afNone, nullptr, 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpAccept, timeout);
  opStart(&op->root);
}

static void connectCb(AsyncOpStatus status, aioObject *socket, void *arg)
{
  __UNUSED(socket)
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
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  p2pOp *op = allocp2pOp(connectProc, connectFinish, connection, afNone, nullptr, 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpConnect, timeout);
  op->address = *address;
  opStart(&op->root);
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

  opStart(childOp);
  return aosPending;
}

asyncOpRoot *implp2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2preadCb *callback, void *arg, p2pHeader *header)
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
      p2pOp *op = allocp2pOp(recvBufferProc, recvFinish, connection, afNone, buffer, bufferSize, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpRecv, timeout);
      opForceStatus(&op->root, aosBufferTooSmall);
      return &op->root;
    }

    break;
  }

  if (childOp) {
    p2pOp *op = allocp2pOp(recvBufferProc, recvFinish, connection, flags|afRunning, buffer, bufferSize, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpRecv, timeout);
    op->header = *header;
    op->rwState = state;
    childOp->arg = op;
    if (state == stTransferring)
      implReadModify(childOp, &op->header, sizeof(p2pHeader));
    opStart(childOp);
    return &op->root;
  }

  return nullptr;
}

void aiop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2preadCb *callback, void *arg)
{
  p2pHeader header;
  aioMethod([=](){
              return &allocp2pOp(recvBufferProc, recvFinish, connection, flags, buffer, bufferSize, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpRecv, timeout)->root;
            },
            [=, &header]() { return implp2pRecv(connection, buffer, bufferSize, flags, timeout, callback, arg, &header); },
            [=, &header]() { callback(aosSuccess, connection, header, buffer, arg); },
            []() {},
            [&header](asyncOpRoot *op) { reinterpret_cast<p2pOp*>(op)->header = header; },
            &connection->root,
            flags,
            reinterpret_cast<void*>(callback),
            p2pOpSend);
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
          childOp = implRead(connection->socket, op->stream->alloc(op->header.size), op->header.size, afWaitAll, 0, resumeRwCb, opptr, &bytes);
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

  opStart(childOp);
  return aosPending;
}

asyncOpRoot *implp2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2preadStreamCb *callback, void *arg, p2pHeader *header)
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
      childOp = implRead(connection->socket, stream.alloc(header->size), header->size, afWaitAll, 0, resumeRwCb, nullptr, &bytes);
      if (childOp) {
        state = stFinished;
        break;
      }
    } else {
      p2pOp *op = allocp2pOp(recvStreamProc, recvStreamFinish, connection, afNone, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(callback), arg, p2pOpRecvStream, timeout);
      opForceStatus(&op->root, aosBufferTooSmall);
      return &op->root;
    }

    break;
  }

  if (childOp) {
    p2pOp *op = allocp2pOp(recvStreamProc, recvStreamFinish, connection, flags|afRunning, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(callback), arg, p2pOpRecvStream, timeout);
    op->header = *header;
    op->rwState = state;
    childOp->arg = op;
    if (state == stTransferring)
      implReadModify(childOp, &op->header, sizeof(p2pHeader));
    opStart(childOp);
    return &op->root;
  }

  return nullptr;
}

void aiop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2preadStreamCb *callback, void *arg)
{
  p2pHeader header;
  aioMethod([=, &stream](){
              return &allocp2pOp(recvStreamProc, recvStreamFinish, connection, flags, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(callback), arg, p2pOpRecvStream, timeout)->root;
            },
            [=, &stream, &header]() { return implp2pRecvStream(connection, stream, maxMsgSize, flags, timeout, callback, arg, &header); },
            [=, &stream]() { callback(aosSuccess, connection, header, &stream, arg); },
            []() {},
            [&header](asyncOpRoot *op) { reinterpret_cast<p2pOp*>(op)->header = header; },
            &connection->root,
            flags,
            reinterpret_cast<void*>(callback),
            p2pOpSend);
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

  opStart(childOp);
  return aosPending;
}

asyncOpRoot *implp2pSend(p2pConnection *connection, const void *data, p2pHeader header, AsyncFlags flags, uint64_t timeout, p2pwriteCb *callback, void *arg)
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
    p2pOp *op = allocp2pOp(sendProc, sendFinish, connection, flags|afRunning, const_cast<void*>(data), 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpSend, timeout);
    op->rwState = state;
    childOp->arg = op;
    opStart(childOp);
    return &op->root;
  }

  return nullptr;
}

void aiop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, AsyncFlags flags, uint64_t timeout, p2pwriteCb *callback, void *arg)
{
  p2pHeader header(id, type, size);
  aioMethod([=](){
              p2pOp *op = allocp2pOp(sendProc, sendFinish, connection, flags, const_cast<void*>(data), 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpSend, timeout);
              op->header = header;
              return &op->root;
            },
            [=]() { return implp2pSend(connection, data, header, flags, timeout, callback, arg); },
            [=]() { callback(aosSuccess, connection, header, arg); },
            []() {},
            [](asyncOpRoot*) {},
            &connection->root,
            flags,
            reinterpret_cast<void*>(callback),
            p2pOpSend);
}

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{ 
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), callback, arg, nullptr, aosPending};
  p2pOp *op = allocp2pOp(acceptProc, acceptFinish, connection, afNone, nullptr, 0, nullptr, reinterpret_cast<void*>(coroutineAcceptCb), &r, p2pOpAccept, timeout);
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -r.status;
}

int iop2pConnect(p2pConnection *connection, const HostAddress *address, uint64_t timeout, p2pConnectData *data)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, nullptr, aosPending};
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  p2pOp *op = allocp2pOp(connectProc, connectFinish, connection, afNone, nullptr, 0, nullptr, reinterpret_cast<void*>(coroutineConnectCb), &r, p2pOpConnect, timeout);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -r.status;
}

ssize_t iop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, AsyncFlags flags, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, nullptr, aosPending};
  p2pHeader header(id, type, size);
  bool finished =
    ioMethod([=, &r]() {
               p2pOp *op = allocp2pOp(sendProc, sendFinish, connection, flags, const_cast<void*>(data), 0, nullptr, reinterpret_cast<void*>(coroutineSendCb), &r, p2pOpSend, timeout);
               op->header = header;
               return &op->root;
             },
             [=, &r]() { return implp2pSend(connection, data, header, flags, timeout, coroutineSendCb, &r); },
             [](asyncOpRoot*) {},
             &ccArgs,
             &connection->root,
             p2pOpSend);
  if (finished)
    return static_cast<ssize_t>(size);
  else
    return r.status == aosSuccess ? static_cast<ssize_t>(size) : -r.status;
}

ssize_t iop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, header, aosPending};
  bool finished =
    ioMethod([=, &stream, &r](){
               return &allocp2pOp(recvStreamProc, recvStreamFinish, connection, flags, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(coroutineRecvStreamCb), &r, p2pOpRecvStream, timeout)->root;
             },
             [=, &stream, &header, &r]() { return implp2pRecvStream(connection, stream, maxMsgSize, flags, timeout, coroutineRecvStreamCb, &r, header); },
             [&header](asyncOpRoot *op) { reinterpret_cast<p2pOp*>(op)->header = *header; },
             &ccArgs,
             &connection->root,
             p2pOpSend);
  if (finished)
    return static_cast<ssize_t>(header->size);
  else
    return r.status == aosSuccess ? static_cast<ssize_t>(header->size) : -r.status;
}

ssize_t iop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, AsyncFlags flags, uint64_t timeout, p2pHeader *header)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, header, aosPending};
  bool finished =
    ioMethod([=, &r](){
               return &allocp2pOp(recvBufferProc, recvFinish, connection, afNone, buffer, bufferSize, nullptr, reinterpret_cast<void*>(coroutineRecvCb), &r, p2pOpRecv, timeout)->root;
             },
             [=, &header, &r]() { return implp2pRecv(connection, buffer, bufferSize, flags, timeout, coroutineRecvCb, &r, header); },
             [&header](asyncOpRoot *op) { reinterpret_cast<p2pOp*>(op)->header = *header; },
             &ccArgs,
             &connection->root,
             p2pOpSend);
  if (finished)
    return static_cast<ssize_t>(header->size);
  else
    return r.status == aosSuccess ? static_cast<ssize_t>(header->size) : -r.status;

}
