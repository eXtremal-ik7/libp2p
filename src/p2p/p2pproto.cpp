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

void p2pRecvProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);
void p2pRecvStreamProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);

static inline AsyncOpStatus p2pStatusFromError(p2pErrorTy error)
{
  switch (error) {
    case p2pOk : return aosSuccess;
    case p2pErrorAuthFailed : return p2pMakeStatus(p2pStAuthFailed);
    case p2pErrorAppNotFound : return p2pMakeStatus(p2pStAppNotFound);
  }
}

static asyncOpRoot *alloc()
{
  return (asyncOpRoot*)malloc(sizeof(p2pOp));
}

//static AsyncOpStatus start(asyncOpRoot *root)
//{
//  p2pOp *op = (p2pOp*)root;
//  aioObject *object = ((p2pConnection*)root->object)->socket;
//  root->object->refs++;
//  switch (root->opCode) {
//    case p2pOpRecv :
//      aioRead(object, &op->header, sizeof(p2pHeader), afWaitAll, 0, p2pRecvProc, op);
//      break;
//    case p2pOpRecvStream :
//      aioRead(object, &op->header, sizeof(p2pHeader), afWaitAll, 0, p2pRecvStreamProc, op);
//      break;
//    case p2pOpAccept :
//    case p2pOpConnect :
//    case p2pOpSend :
//      // TODO: return error
//      break;
//  }

//  return aosPending;
//}

static void finish(asyncOpRoot *root)
{
  p2pOp *op = (p2pOp*)root;  
  p2pConnection *connection = (p2pConnection*)root->object;  
  connection->root.refs--;
  AsyncOpStatus status = opGetStatus(root);
  
  // cleanup child operation after timeout
  if (status == aosTimeout || status == aosCanceled) {
    if (root->opCode == p2pOpAccept || root->opCode == p2pOpConnect)
      cancelIo((aioObjectRoot*)connection);
    else
      cancelIo((aioObjectRoot*)connection->socket);
  }
  
  if (root->callback && status != aosCanceled) {
    switch (root->opCode) {
      case p2pOpAccept :
        ((p2pAcceptCb*)root->callback)(status, connection, 0, root->arg);
        break;
      case p2pOpConnect :
        ((p2pConnectCb*)root->callback)(status, connection, root->arg);
        break;
      case p2pOpRecv :
      case p2pOpRecvStream :
      case p2pOpSend :
        ((p2pCb*)root->callback)(status, connection, op->header, root->arg);
        break;
    }
  }
}

static p2pOp *allocp2pOp(p2pConnection *connection,
                         aioExecuteProc *executeProc,
                         void *buffer,
                         size_t bufferSize,
                         p2pStream *stream,
                         void *callback,
                         void *arg,
                         int opCode,
                         uint64_t timeout)
{
  p2pOp *op = (p2pOp*)
    initAsyncOpRoot(p2pPoolId, p2pPoolTimerId, alloc, executeProc, finish, &connection->root, callback, arg, afNone, opCode, timeout);
  op->buffer = buffer;
  op->bufferSize = bufferSize;
  op->stream = stream;
  return op;
}

static void coroutineConnectCb(int status, p2pConnection *connection, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  coroutineCall(r->coroutine);
}

static void coroutineSendCb(int status, p2pConnection *connection, p2pHeader header, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  r->msgSize = header.size;
  coroutineCall(r->coroutine);
}

static void coroutineRecvCb(int status, p2pConnection *connection, p2pHeader header, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  *r->header = header;
  coroutineCall(r->coroutine);
}

static void destructor(aioObjectRoot *root)
{
  p2pConnection *connection = (p2pConnection*)root;
  xmstream &stream = connection->stream;
  stream.~xmstream();
  free(connection);
}

p2pConnection *p2pConnectionNew(asyncBase *base, aioObject *socket)
{
  p2pConnection *connection = (p2pConnection*)malloc(sizeof(p2pConnection));
  initObjectRoot(&connection->root, base, ioObjectUserDefined, destructor);
  new(&connection->stream) xmstream;
  connection->socket = socket;
  return connection;
}

void p2pConnectionDelete(p2pConnection *connection)
{
//  deleteAioObject(connection->socket);
//  connection->root.refs--;   // TODO: atomic
//  checkForDeleteObject(&connection->root);
}


static void acceptProc(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  p2pOp *op = (p2pOp*)link->op;
  if (status != aosSuccess) {
//    finishOperation(&op->root, status, 1);
    opReleaseLink(link, status);
    return;
  }
 
  if (op->state == stWaitConnectMsg) {
    p2pStream *stream = op->stream;
    if (!stream->readConnectMessage(&op->connectMsg)) {
//      finishOperation(&op->root, p2pStFormatError, 1);
      opReleaseLink(link, p2pMakeStatus(p2pStFormatError));
      return;
    }
 
    p2pErrorTy result = ((p2pAcceptCb*)op->root.callback)(aosPending, connection, &op->connectMsg, op->root.arg);
    stream->reset();
    stream->writeStatusMessage(result);
    aiop2pSend(connection, 3000000, stream->data(), p2pHeader(p2pMsgStatus, stream->sizeOf()), acceptProc, op);
    op->lastError = p2pStatusFromError(result);
    op->state = stWaitAnswerSend;
  } else if (op->state == stWaitAnswerSend) {
//    finishOperation(&op->root, op->lastError, 1);
    opReleaseLink(link, op->lastError);
  }
}

static AsyncOpStatus acceptStart(asyncOpRoot *opptr)
{
  return aosUnknownError;
}

void aiop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{  
  p2pOp *op = allocp2pOp(connection, acceptStart, 0, 0, 0, (void*)callback, arg, p2pOpAccept, timeout);
  op->state = stWaitConnectMsg;
  opStart(&op->root);

//  // don't add to execute queue
//  connection->root.refs++;
//  op->state = stWaitConnectMsg;
//  aiop2pRecv(connection, 0, &connection->stream, 1024, acceptProc, op);
}


static void connectProc(AsyncOpStatus status, p2pConnection *connection, p2pHeader header, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  p2pOp *op = (p2pOp*)link->op;
  if (status != aosSuccess) {
//    finishOperation(&op->root, status, 1);
    opReleaseLink(link, status);
    return;
  }
  
  if (op->state == stWaitConnectResponse) {
    p2pErrorTy error;
    if (connection->stream.readStatusMessage(&error))
      opReleaseLink(link, p2pStatusFromError(error));
//      finishOperation(&op->root, p2pStatusFromError(error), 1);
    else
      opReleaseLink(link, p2pMakeStatus(p2pStFormatError));
//      finishOperation(&op->root, p2pStFormatError, 1);
  } else {
    opReleaseLink(link, aosUnknownError);
//    finishOperation(&op->root, aosUnknownError, 1);
  }
}

static AsyncOpStatus connectStart(asyncOpRoot *opptr)
{
  return aosUnknownError;
}

void aiop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data, p2pConnectCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, connectStart, 0, 0, 0, (void*)callback, arg, p2pOpConnect, timeout);
  op->state = stWaitConnectResponse;  
  opStart(&op->root);
//  connection->root.refs++;
//  connection->stream.reset();
//  connection->stream.writeConnectMessage(*data);
//  aiop2pSend(connection, 0, connection->stream.data(), p2pHeader(p2pMsgConnect, connection->stream.sizeOf()), 0, 0);
//  aiop2pRecv(connection, 0, &connection->stream, 1024, connectProc, op);
}

void p2pRecvProc(AsyncOpStatus status, aioObject *connection, size_t transferred, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  p2pOp *op = (p2pOp*)link->op;
  if (status == aosSuccess) {
    if (op->state == stWaitHeader) {
      // TODO: correct processing zero-sized messages
      size_t msgSize = op->header.size;
      if (msgSize <= op->bufferSize) {
        op->state = stWaitMsgBody;
        aioRead(connection, op->buffer, msgSize, afWaitAll, 0, p2pRecvProc, op);
      } else {
        opReleaseLink(link, aosBufferTooSmall);
//        finishOperation(&op->root, p2pStBufferTooSmall, 1);
      }
    } else if (op->state == stWaitMsgBody) {
      opReleaseLink(link, aosSuccess);
//      finishOperation(&op->root, aosSuccess, 1);
    }
  } else {
//    finishOperation(&op->root, status, 1);
    opReleaseLink(link, status);
  }
}

void p2pRecvStreamProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  p2pOp *op = (p2pOp*)link->op;
  if (status == aosSuccess) {
    p2pConnection *connection = (p2pConnection*)op->root.object;
    if (op->state == stWaitHeader) {
      size_t msgSize = op->header.size;
      if (op->bufferSize == 0 || msgSize <= op->bufferSize) {
        op->state = stWaitMsgBody;
        connection->stream.reset();
        aioRead(object, connection->stream.alloc(msgSize), msgSize, afWaitAll, 0, p2pRecvStreamProc, op);
      } else {
//        finishOperation(&op->root, aosUnknownError, 1);
        opReleaseLink(link, aosUnknownError);
      }
    } else if (op->state == stWaitMsgBody) {
      connection->stream.seekSet(0);
//      finishOperation(&op->root, aosSuccess, 1);
      opReleaseLink(link, aosSuccess);
    }
  } else {
//    finishOperation(&op->root, status, 1);
    opReleaseLink(link, status);
  }
}

void sendProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  opReleaseLink(link, status);
//  finishOperation((asyncOpRoot*)arg, status, 1);
}

static AsyncOpStatus recvStart(asyncOpRoot *opptr)
{
  return aosUnknownError;
}

void aiop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, recvStart, buffer, bufferSize, 0, (void*)callback, arg, p2pOpRecv, timeout);
  op->state = stWaitHeader;
  opStart(&op->root);
//  if (addToExecuteQueue(&connection->root, &op->root, 0))
//    start(&op->root);
}

void aiop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, recvStart, 0, maxMsgSize, 0, (void*)callback, arg, p2pOpRecvStream, timeout);
  op->state = stWaitHeader;
  opStart(&op->root);
//  if (addToExecuteQueue(&connection->root, &op->root, 0))
//    start(&op->root);
}

static AsyncOpStatus sendStart(asyncOpRoot *opptr)
{
  return aosUnknownError;
}

void aiop2pSend(p2pConnection *connection, uint64_t timeout, void *data, p2pHeader header, p2pCb *callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, sendStart, 0, 0, 0, (void*)callback, arg, p2pOpSend, timeout);
  
//  // TODO: acquireSpinlock(connection)
//  connection->root.refs++;
//  aioWrite(connection->socket, &header, sizeof(p2pHeader), afWaitAll, 0, 0, 0);
//  aioWrite(connection->socket, data, header.size, afWaitAll, 0, sendProc, op);
//  // TODO: releaseSpinlock(connection)
}

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb *callback, void *arg)
{
//  p2pHeader header;
//  p2pConnectData data;

//  if (iop2pRecv(base, connection, timeout, &connection->stream, 1024, &header) == -1)
//    return aosUnknownError;

//  if (!connection->stream.readConnectMessage(&data))
//    return p2pStFormatError;

//  p2pErrorTy error = callback(aosPending, connection, &data, arg);
//  connection->stream.reset();
//  connection->stream.writeStatusMessage(error);
//  bool sendResult = iop2pSend(base, connection, timeout, connection->stream.data(), 0, p2pMsgStatus, connection->stream.sizeOf());
  
//  int status = sendResult ? p2pStatusFromError(error) : aosUnknownError;
//  callback(sendResult ? p2pStatusFromError(error) : aosUnknownError, connection, 0, arg);
//  return status;
}

int iop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data)
{
//  coroReturnStruct r = {coroutineCurrent()};
//  aiop2pConnect(base, connection, timeout, data, coroutineConnectCb, &r);
//  coroutineYield();
//  return r.status;
}

bool iop2pSend(p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size)
{
//  coroReturnStruct r = {coroutineCurrent()};
//  aiop2pSend(base, connection, timeout, data, p2pHeader(id, type, size), coroutineSendCb, &r);
//  coroutineYield();
//  return r.status == aosSuccess;
}

ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header)
{
//  coroReturnStruct r = {coroutineCurrent(), header};
//  aiop2pRecv(base, connection, timeout, buffer, bufferSize, coroutineRecvCb, &r);
//  coroutineYield();
//  return r.status == aosSuccess ? header->size : -1;
}

ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header)
{
//  coroReturnStruct r = {coroutineCurrent(), header};
//  aiop2pRecv(base, connection, timeout, stream, maxMsgSize, coroutineRecvCb, &r);
//  coroutineYield();
//  return r.status == aosSuccess ? header->size : -1;
}
