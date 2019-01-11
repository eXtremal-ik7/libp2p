#include "asyncio/coroutine.h"
#include "p2p/p2pproto.h"
#include "p2p/p2pformat.h"
#include <stdlib.h>

static const char *p2pPoolId = "P2P";
static const char *p2pPoolTimerId = "P2PTimer";

enum p2pOpTy {
  p2pOpAccept = OPCODE_READ,
  p2pOpRecv,
  p2pOpRecvStream,
  p2pOpConnect = OPCODE_WRITE,
  p2pOpSend
};

enum p2pRwState {
  stMsgWaitHeader = 0,
  stMsgWait,
  stMsgWaitBody
};

enum p2pPeerState {
  stInitialize = 0,

  // connect states
  stConnectConnected,  
  stConnectWaitMsgSend,
  stConnectWaitResponse,
  stConnectResponseReceived,
  
  // accept states
  stAcceptWaitAuth,
  stAcceptAuthReceived,
  stAcceptWaitAnswerSend,

  // other states
  stTransferring
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

static AsyncOpStatus recvBuffer(p2pOp *op, void *data = nullptr, uint32_t size = 0);
static AsyncOpStatus recvStream(p2pOp *op, p2pStream *stream = nullptr, uint32_t limit = 0);
static AsyncOpStatus sendBuffer(p2pOp *op, p2pHeader header, void *data);

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

static void transferCb(AsyncOpStatus status, aioObject *socket, size_t bytesRead, void *arg)
{
  __UNUSED(socket);
  __UNUSED(bytesRead);
  resumeParent(static_cast<asyncOpRoot*>(arg), status);
}

static AsyncOpStatus recvBuffer(p2pOp *op, void *data, uint32_t size)
{
  int exit = 0;
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(op->root.object);
  if (data) {
    op->rwState = stMsgWait;
    op->bufferSize = size;
  }
  while (!exit) {
    exit = true;
    switch (op->rwState) {
      case stMsgWait : {
        op->rwState = stMsgWaitHeader;
        ssize_t result = aioRead(connection->socket, &op->header, sizeof(p2pHeader), afWaitAll | afActiveOnce, 0, transferCb, op);
        exit = !(result == sizeof(p2pHeader));
        break;
      }
      case stMsgWaitHeader : {
        if (op->header.size > op->bufferSize)
          return aosBufferTooSmall;
        op->rwState = stMsgWaitBody;
        ssize_t result = aioRead(connection->socket, op->buffer, op->header.size, afWaitAll | afActiveOnce, 0, transferCb, op);
        exit = !(result == op->header.size);
        break;
      }
      case stMsgWaitBody : {
        return aosSuccess;
      }
    }
  }

  return aosPending;
}

static AsyncOpStatus recvStream(p2pOp *op, p2pStream *stream, uint32_t limit)
{
  int exit = 0;
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(op->root.object);
  if (stream) {
    stream->reset();
    op->stream = stream;
    op->bufferSize = limit;
    op->rwState = stMsgWait;
  }
  while (!exit) {
    exit = true;
    switch (op->rwState) {
      case stMsgWait : {
        op->rwState = stMsgWaitHeader;
        ssize_t result = aioRead(connection->socket, &op->header, sizeof(p2pHeader), afWaitAll | afActiveOnce, 0, transferCb, op);
        exit = !(result == sizeof(p2pHeader));
        break;
      }
      case stMsgWaitHeader : {
        if (op->header.size > op->bufferSize)
          return aosBufferTooSmall;
        op->rwState = stMsgWaitBody;
        op->stream->reset();
        void *data = op->stream->alloc(op->header.size);
        ssize_t result = aioRead(connection->socket, data, op->header.size, afWaitAll | afActiveOnce, 0, transferCb, op);
        exit = !(result == op->header.size);
        break;
      }
      case stMsgWaitBody : {
        op->stream->seekSet(0);
        return aosSuccess;
      }
    }
  }

  return aosPending;
}

static AsyncOpStatus sendBuffer(p2pOp *op, p2pHeader header, void *data)
{
  p2pConnection *connection = reinterpret_cast<p2pConnection*>(op->root.object);
  aioWrite(connection->socket, &header, sizeof(header), afWaitAll, 0, nullptr, nullptr);
  return aioWrite(connection->socket, data, header.size, afWaitAll | afActiveOnce, 0, transferCb, op) == header.size ?
    aosSuccess : aosPending;
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
                         void *buffer,
                         uint32_t bufferSize,
                         p2pStream *stream,
                         void *callback,
                         void *arg,
                         int opCode,
                         uint64_t timeout)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(
    initAsyncOpRoot(p2pPoolId, p2pPoolTimerId, alloc, executeProc, cancel, finishProc, &connection->root, callback, arg, afNone, opCode, timeout));
  if (stream)
    op->stream = stream;
  else
    op->buffer = buffer;
  op->bufferSize = bufferSize;
  op->state = stInitialize;
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
  xmstream &stream = connection->stream;
  stream.~xmstream();
  deleteAioObject(connection->socket);
  free(connection);
}

p2pConnection *p2pConnectionNew(aioObject *socket)
{
  p2pConnection *connection = static_cast<p2pConnection*>(malloc(sizeof(p2pConnection)));
  initObjectRoot(&connection->root, aioGetBase(socket), ioObjectUserDefined, destructor);
  new(&connection->stream) xmstream;
  connection->socket = socket;
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
        op->state = stAcceptWaitAuth;
        result = recvStream(op, &connection->stream, 4096);
        if (result == aosSuccess)
          op->state = stAcceptAuthReceived;
        break;
      }

      case stAcceptWaitAuth : {
        result = recvStream(op);
        if (result == aosSuccess)
          op->state = stAcceptAuthReceived;
        break;
      }

      case stAcceptAuthReceived : {
        if (connection->stream.readConnectMessage(&op->connectMsg)) {
          op->state = stAcceptWaitAnswerSend;
          p2pErrorTy authStatus = reinterpret_cast<p2pAcceptCb*>(op->root.callback)(aosPending, connection, &op->connectMsg, op->root.arg);
          connection->stream.reset();
          connection->stream.writeStatusMessage(authStatus);
          result = sendBuffer(op, p2pHeader(p2pMsgStatus, static_cast<uint32_t>(connection->stream.sizeOf())), connection->stream.data());
          op->lastError = p2pStatusFromError(authStatus);
        } else {
          result = p2pMakeStatus(p2pStFormatError);
        }

        break;
      }

      case stAcceptWaitAnswerSend : {
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
  p2pOp *op = allocp2pOp(acceptProc, acceptFinish, connection, nullptr, 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpAccept, timeout);
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
        result = sendBuffer(op, p2pHeader(p2pMsgConnect, static_cast<uint32_t>(connection->stream.sizeOf())), connection->stream.data());
        break;
      }
      case stConnectWaitMsgSend : {
        op->state = stConnectWaitResponse;
        result = recvStream(op, &connection->stream, 4096);
        if (result == aosSuccess)
          op->state = stConnectResponseReceived;
        break;
      }
      case stConnectWaitResponse : {
        result = recvStream(op);
        if (result == aosSuccess)
          op->state = stConnectResponseReceived;
        break;
      }

      case stConnectResponseReceived : {
        p2pErrorTy error;
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
  p2pOp *op = allocp2pOp(connectProc, connectFinish, connection, nullptr, 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpConnect, timeout);
  op->address = *address;
  opStart(&op->root);
}

static AsyncOpStatus recvBufferProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  if (op->state == stInitialize) {
    op->state = stTransferring;
    return recvBuffer(op, op->buffer, op->bufferSize);
  } else {
    return recvBuffer(op);
  }
}

void aiop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, uint64_t timeout, p2preadCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(recvBufferProc, recvFinish, connection, buffer, bufferSize, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpRecv, timeout);
  opStart(&op->root);
}

static AsyncOpStatus recvStreamProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  if (op->state == stInitialize) {
    op->state = stTransferring;
    return recvStream(op, op->stream, op->bufferSize);
  } else {
    return recvStream(op);
  }
}

void aiop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, uint64_t timeout, p2preadStreamCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(recvStreamProc, recvStreamFinish, connection, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(callback), arg, p2pOpRecvStream, timeout);
  opStart(&op->root);
}

static AsyncOpStatus sendProc(asyncOpRoot *opptr)
{
  p2pOp *op = reinterpret_cast<p2pOp*>(opptr);
  if (op->state == stInitialize) {
    op->state = stTransferring;
    return sendBuffer(op, op->header, op->buffer);
  } else {
    return aosSuccess;
  }
}

void aiop2pSend(p2pConnection *connection, const void *data, p2pHeader header, uint64_t timeout, p2pwriteCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(sendProc, sendFinish, connection, const_cast<void*>(data), 0, nullptr, reinterpret_cast<void*>(callback), arg, p2pOpSend, timeout);
  op->header = header;
  opStart(&op->root);
}

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{ 
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), callback, arg, nullptr, aosPending};
  p2pOp *op = allocp2pOp(acceptProc, acceptFinish, connection, nullptr, 0, nullptr, reinterpret_cast<void*>(coroutineAcceptCb), &r, p2pOpAccept, timeout);
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
  p2pOp *op = allocp2pOp(connectProc, connectFinish, connection, nullptr, 0, nullptr, reinterpret_cast<void*>(coroutineConnectCb), &r, p2pOpConnect, timeout);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -r.status;
}

ssize_t iop2pSend(p2pConnection *connection, const void *data, uint32_t id, uint32_t type, uint32_t size, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, nullptr, aosPending};
  p2pOp *op = allocp2pOp(sendProc, sendFinish, connection, const_cast<void*>(data), 0, nullptr, reinterpret_cast<void*>(coroutineSendCb), &r, p2pOpSend, timeout);
  op->header = p2pHeader(id, type, size);
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? static_cast<ssize_t>(size) : -r.status;
}

ssize_t iop2pRecvStream(p2pConnection *connection, p2pStream &stream, uint32_t maxMsgSize, p2pHeader *header, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, header, aosPending};
  p2pOp *op = allocp2pOp(recvStreamProc, recvStreamFinish, connection, nullptr, maxMsgSize, &stream, reinterpret_cast<void*>(coroutineRecvStreamCb), &r, p2pOpRecvStream, timeout);
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? static_cast<ssize_t>(header->size) : -r.status;
}

ssize_t iop2pRecv(p2pConnection *connection, void *buffer, uint32_t bufferSize, p2pHeader *header, uint64_t timeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), nullptr, nullptr, header, aosPending};
  p2pOp *op = allocp2pOp(recvBufferProc, recvFinish, connection, buffer, bufferSize, nullptr, reinterpret_cast<void*>(coroutineRecvCb), &r, p2pOpRecv, timeout);
  combinerCallDelayed(&ccArgs, &connection->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? static_cast<ssize_t>(header->size) : -r.status;
}
