#include "asyncio/coroutine.h"
#include "p2p/p2pproto.h"
#include "p2p/p2pformat.h"
#include <stdlib.h>

const char *p2pPoolId = "P2P";
const char *p2pPoolTimerId = "P2PTimer";

enum p2pOpTy {
  p2pOpAccept = 0,
  p2pOpConnect,
  p2pOpRecv,
  p2pOpRecvStream,
  p2pOpSend
};

enum p2pPeerState {
  // recv states
  stWaitHeader = 0,
  stWaitMsgBody,
  
  // connect states
  stWaitConnectMsgSend,
  stWaitConnectResponse,
  
  // accept states
  stWaitConnectMsg,
  stWaitAnswerSend
//   stWaitErrorSend
};

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  p2pHeader *header;
  int status;
  size_t msgSize;
} coroReturnStruct;

void p2pRecvProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);
void p2pRecvStreamProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);

static int p2pStatusFromError(p2pErrorTy error)
{
  switch (error) {
    case p2pOk : return aosSuccess;
    case p2pErrorAuthFailed : return p2pStAuthFailed;
    case p2pErrorAppNotFound : return p2pStAppNotFound;
    default : return aosUnknownError;
  }
}

static asyncOpRoot *alloc(asyncBase *base)
{
  return (asyncOpRoot*)malloc(sizeof(p2pOp));
}

static void start(asyncOpRoot *root)
{
  p2pOp *op = (p2pOp*)root;   
  aioObject *object = ((p2pConnection*)root->object)->socket;
  root->object->links++;
  switch (root->opCode) {
    case p2pOpRecv :
      aioRead(root->base, object, &op->header, sizeof(p2pHeader), afWaitAll, 0, p2pRecvProc, op);
      break;
    case p2pOpRecvStream :
      aioRead(root->base, object, &op->header, sizeof(p2pHeader), afWaitAll, 0, p2pRecvStreamProc, op);
      break;
    case p2pOpAccept :
    case p2pOpConnect :
    case p2pOpSend :
      // TODO: return error
      break;
  }
}

static void finish(asyncOpRoot *root, int status)
{
  p2pOp *op = (p2pOp*)root;  
  p2pConnection *connection = (p2pConnection*)root->object;  
  connection->root.links--;
  
  // cleanup child operation after timeout
  if (status == aosTimeout || status == aosCanceled) {
    if (root->opCode == p2pOpAccept || root->opCode == p2pOpConnect)
      cancelIo((aioObjectRoot*)connection, root->base);
    else
      cancelIo((aioObjectRoot*)connection->socket, root->base);
  }
  
  if (root->callback && status != aosCanceled) {
    switch (root->opCode) {
      case p2pOpAccept :
        ((p2pAcceptCb*)root->callback)(status, root->base, connection, 0, root->arg);
        break;
      case p2pOpConnect :
        ((p2pConnectCb*)root->callback)(status, root->base, connection, root->arg);
        break;
      case p2pOpRecv :
      case p2pOpRecvStream :
      case p2pOpSend :
        ((p2pCb*)root->callback)(status, root->base, connection, op->header, root->arg);
        break;
    }
  }
}

static p2pOp *allocp2pOp(asyncBase *base,
                         p2pConnection *connection,
                         void *buffer,
                         size_t bufferSize,
                         p2pStream *stream,
                         void *callback,
                         void *arg,
                         int opCode,
                         uint64_t timeout)
{
  p2pOp *op = (p2pOp*)
    initAsyncOpRoot(base, p2pPoolId, p2pPoolTimerId, alloc, start, finish, &connection->root, callback, arg, 0, opCode, timeout);
  op->buffer = buffer;
  op->bufferSize = bufferSize;
  op->stream = stream;
  return op;
}

static void coroutineConnectCb(int status, asyncBase *base, p2pConnection *connection, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  coroutineCall(r->coroutine);
}

static void coroutineSendCb(int status, asyncBase *base, p2pConnection *connection, p2pHeader header, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  r->msgSize = header.size;
  coroutineCall(r->coroutine);
}

static void coroutineRecvCb(int status, asyncBase *base, p2pConnection *connection, p2pHeader header, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  *r->header = header;
  coroutineCall(r->coroutine);
}

static void destructor(aioObjectRoot *root)
{
  p2pConnection *connection = (p2pConnection*)root;
  connection->stream.~xmstream();
  free(connection);
}

p2pConnection *p2pConnectionNew(aioObject *socket)
{
  p2pConnection *connection =
    (p2pConnection*)initObjectRoot(ioObjectUserDefined, sizeof(p2pConnection), destructor);
  new(&connection->stream) xmstream;
  connection->socket = socket;
  return connection;
}

void p2pConnectionDelete(p2pConnection *connection)
{
  deleteAioObject(connection->socket);
  connection->root.links--;   // TODO: atomic  
  checkForDeleteObject(&connection->root);
}


static void acceptProc(int status, asyncBase *base, p2pConnection *connection, p2pHeader header, void *arg)
{
  p2pOp *op = (p2pOp*)arg; 
  if (status != aosSuccess) {
    finishOperation(&op->root, status, 1);
    return;
  }
 
  if (op->state == stWaitConnectMsg) {
    p2pStream *stream = op->stream;
    if (!stream->readConnectMessage(&op->connectMsg)) {
      finishOperation(&op->root, p2pStFormatError, 1);
      return;
    }
 
    p2pErrorTy result = ((p2pAcceptCb*)op->root.callback)(aosPending, base, connection, &op->connectMsg, op->root.arg);
    stream->reset();
    stream->writeStatusMessage(result);
    aiop2pSend(base, connection, 3000000, stream->data(), p2pHeader(p2pMsgStatus, stream->sizeOf()), acceptProc, op);
    op->lastError = p2pStatusFromError(result);
    op->state = stWaitAnswerSend;
  } else if (op->state == stWaitAnswerSend) {
    finishOperation(&op->root, op->lastError, 1);
  }
}

void aiop2pAccept(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{  
  p2pOp *op = allocp2pOp(base, connection, 0, 0, 0, (void*)callback, arg, p2pOpAccept, timeout);
  
  // don't add to execute queue
  connection->root.links++;
  op->state = stWaitConnectMsg;
  aiop2pRecv(base, connection, 0, &connection->stream, 1024, acceptProc, op);
}


static void connectProc(int status, asyncBase *base, p2pConnection *connection, p2pHeader header, void *arg)
{
  p2pOp *op = (p2pOp*)arg;
  if (status != aosSuccess) {
    finishOperation(&op->root, status, 1);
    return;
  }
  
  if (op->state == stWaitConnectResponse) {
    p2pErrorTy error;
    if (connection->stream.readStatusMessage(&error))
      finishOperation(&op->root, p2pStatusFromError(error), 1);
    else
      finishOperation(&op->root, p2pStFormatError, 1);
  } else {
    finishOperation(&op->root, aosUnknownError, 1);
  }
}

void aiop2pConnect(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pConnectData *data, p2pConnectCb *callback, void *arg)
{
  // don't add to execute queue
  p2pOp *op = allocp2pOp(base, connection, 0, 0, 0, (void*)callback, arg, p2pOpConnect, timeout);
  op->state = stWaitConnectResponse;  
  connection->root.links++;  
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  aiop2pSend(base, connection, 0, connection->stream.data(), p2pHeader(p2pMsgConnect, connection->stream.sizeOf()), 0, 0);
  aiop2pRecv(base, connection, 0, &connection->stream, 1024, connectProc, op);
}

void p2pRecvProc(AsyncOpStatus status, asyncBase *base, aioObject *connection, size_t transferred, void *arg)
{
  p2pOp *op = (p2pOp*)arg;
  if (status == aosSuccess) {
    if (op->state == stWaitHeader) {
      // TODO: correct processing zero-sized messages
      size_t msgSize = op->header.size;
      if (msgSize <= op->bufferSize) {
        op->state = stWaitMsgBody;
        aioRead(base, connection, op->buffer, msgSize, afWaitAll, 0, p2pRecvProc, op);
      } else {
        finishOperation(&op->root, p2pStBufferTooSmall, 1);
      }
    } else if (op->state == stWaitMsgBody) {
      finishOperation(&op->root, aosSuccess, 1);
    }
  } else {
    finishOperation(&op->root, status, 1);
  }
}

void p2pRecvStreamProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  p2pOp *op = (p2pOp*)arg;
  if (status == aosSuccess) {
    p2pConnection *connection = (p2pConnection*)op->root.object;
    if (op->state == stWaitHeader) {
      size_t msgSize = op->header.size;
      if (op->bufferSize == 0 || msgSize <= op->bufferSize) {
        op->state = stWaitMsgBody;
        connection->stream.reset();
        aioRead(base, object, connection->stream.alloc(msgSize), msgSize, afWaitAll, 0, p2pRecvStreamProc, op);
      } else {
        finishOperation(&op->root, aosUnknownError, 1);
      }
    } else if (op->state == stWaitMsgBody) {
      connection->stream.seekSet(0);
      finishOperation(&op->root, aosSuccess, 1);
    }
  } else {
    finishOperation(&op->root, status, 1);
  }
}

void sendProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  finishOperation((asyncOpRoot*)arg, status, 1);
}

void aiop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(base, connection, buffer, bufferSize, 0, (void*)callback, arg, p2pOpRecv, timeout);
  op->state = stWaitHeader;
  if (addToExecuteQueue(&connection->root, &op->root, 0))
    start(&op->root);
}

void aiop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(base, connection, 0, maxMsgSize, 0, (void*)callback, arg, p2pOpRecvStream, timeout);
  op->state = stWaitHeader;
  if (addToExecuteQueue(&connection->root, &op->root, 0))
    start(&op->root);  
}

void aiop2pSend(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *data, p2pHeader header, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(base, connection, 0, 0, 0, (void*)callback, arg, p2pOpSend, timeout);  
  
  // TODO: acquireSpinlock(connection)
  connection->root.links++;
  aioWrite(base, connection->socket, &header, sizeof(p2pHeader), afWaitAll, 0, 0, 0);
  aioWrite(base, connection->socket, data, header.size, afWaitAll, 0, sendProc, op);
  // TODO: releaseSpinlock(connection)
}

int iop2pAccept(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{
  p2pHeader header;
  p2pConnectData data;

  if (iop2pRecv(base, connection, timeout, &connection->stream, 1024, &header) == -1)
    return aosUnknownError;

  if (!connection->stream.readConnectMessage(&data))
    return p2pStFormatError;

  p2pErrorTy error = callback(aosPending, base, connection, &data, arg);
  connection->stream.reset();
  connection->stream.writeStatusMessage(error);
  bool sendResult = iop2pSend(base, connection, timeout, connection->stream.data(), 0, p2pMsgStatus, connection->stream.sizeOf());
  
  int status = sendResult ? p2pStatusFromError(error) : aosUnknownError;
  callback(sendResult ? p2pStatusFromError(error) : aosUnknownError, base, connection, 0, arg);
  return status;
}

int iop2pConnect(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pConnectData *data)
{
  coroReturnStruct r = {coroutineCurrent()};
  aiop2pConnect(base, connection, timeout, data, coroutineConnectCb, &r);
  coroutineYield();
  return r.status;
}

bool iop2pSend(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size)
{
  coroReturnStruct r = {coroutineCurrent()};
  aiop2pSend(base, connection, timeout, data, p2pHeader(id, type, size), coroutineSendCb, &r);
  coroutineYield();
  return r.status == aosSuccess;
}

ssize_t iop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header)
{
  coroReturnStruct r = {coroutineCurrent(), header};
  aiop2pRecv(base, connection, timeout, buffer, bufferSize, coroutineRecvCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? header->size : -1;
}

ssize_t iop2pRecv(asyncBase *base, p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header)
{
  coroReturnStruct r = {coroutineCurrent(), header};
  aiop2pRecv(base, connection, timeout, stream, maxMsgSize, coroutineRecvCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? header->size : -1;
}
