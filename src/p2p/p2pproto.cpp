#include "p2p/p2pproto.h"
#include "p2p/p2pformat.h"
#include <stdlib.h>

const char *p2pPoolId = "P2P";

enum {
  p2pOpAccept = 0,
  p2pOpConnect,
  p2pOpRecv,
  p2pOpRecvStream,
  p2pOpSend
} p2pOpTy;

enum p2pPeerState {
  // recv states
  stWaitHeader = 0,
  stWaitMsgBody,
  
  // connect states
  stWaitConnectMsgSend,
  stWaitConnectResponse,
  
  // accept states
  stWaitConnectMsg,
  stWaitOkSend,
  stWaitErrorSend
};

void p2pRecvProc(aioInfo *info);
void p2pRecvStreamProc(aioInfo *info);

static int p2pStatusFromError(p2pErrorTy error)
{
  switch (error) {
    case p2pOk : return aosSuccess;
    case p2pErrorAuthFailed : return p2pStAuthFailed;
    case p2pErrorAppNotFound : return p2pStAppNotFound;
    default : return aosUnknownError;
  }
}

static p2pOp *allocp2pOp(p2pConnection *connection,
                         void *buffer,
                         size_t bufferSize,
                         p2pStream *stream,
                         void *callback,
                         void *arg)
{
  p2pOp *op = (p2pOp*)queryObject(aioObjectBase(connection->socket), p2pPoolId);
  if (!op) {
    op = (p2pOp*)malloc(sizeof(p2pOp));
  }

  op->info.connection = connection;  
  op->info.buffer = buffer;
  op->info.bufferSize = bufferSize;
  op->info.stream = stream;
  op->info.arg = arg;
  op->callback = callback;
  op->next = 0;
  return op;
}


static void finishP2POp(p2pOp *op, int status)
{
  p2pOp *current = op->next;
  if (op->type != p2pOpSend)
    op->info.connection->current = current;
  op->info.status = status;
  
  if (op->callback) {
    if (op->type == p2pOpAccept)
      ((p2pAcceptCb*)op->callback)(0, &op->info);
    else
      ((p2pCb*)op->callback)(&op->info);
  }
  
  releaseObject(aioObjectBase(op->info.connection->socket), op, p2pPoolId);  
  if (current && current->type != p2pOpSend) {
    p2pConnection *connection = current->info.connection;
    switch (current->type) {
      case p2pOpRecv :
        aioRead(connection->socket, &current->info.header, sizeof(p2pHeader), afWaitAll, current->usTimeout, p2pRecvProc, current);
        break;
      case p2pOpRecvStream :
        aioRead(connection->socket, &current->info.header, sizeof(p2pHeader), afWaitAll, current->usTimeout, p2pRecvStreamProc, current);
        break;
    }
  }
}


p2pConnection *p2pConnectionNew(aioObject *socket)
{
  p2pConnection *connection = new p2pConnection;
  connection->socket = socket;
  connection->current = 0;
  return connection;
}

void p2pConnectionDelete(p2pConnection *connection)
{
//   deleteAioObject(connection->socket);
  delete connection;
}


static void acceptProc(p2pInfo *info)
{
  p2pOp *op = (p2pOp*)info->arg; 
  if (info->status != aosSuccess) {
    finishP2POp(op, info->status);
    return;
  }
 
  if (op->state == stWaitConnectMsg) {
    p2pStream *stream = op->info.stream;
    if (!stream->readConnectMessage(&op->connectMsg)) {
      finishP2POp(op, p2pStFormatError);
      return;
    }
 
    p2pErrorTy result = ((p2pAcceptCb*)op->callback)(&op->connectMsg, &op->info);
    stream->reset();
    stream->writeStatusMessage(result);
    aiop2pSend(info->connection, 3000000, stream->data(), p2pHeader(p2pMsgStatus, stream->sizeOf()), acceptProc, op);
    op->state = (op->info.status == aosSuccess) ? stWaitOkSend : stWaitErrorSend;
    op->info.status = p2pStatusFromError(result);
  } else if (op->state == stWaitOkSend) {
    finishP2POp(op, aosSuccess);
  } else {
    finishP2POp(op, op->info.status);
    // TODO: close connection
  }
}

void aiop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb callback, void *arg)
{
  // TODO: return error
  if (connection->current)
    return;
  
  p2pOp *op = allocp2pOp(connection, 0, 0, 0, (void*)callback, arg);
  op->type = p2pOpAccept;
  op->state = stWaitConnectMsg;
  aiop2pRecv(connection, timeout, &connection->stream, 1024, acceptProc, op);
}


static void connectProc(p2pInfo *info)
{
  p2pOp *op = (p2pOp*)info->arg;
  if (info->status != aosSuccess)
    finishP2POp(op, info->status);
  
  if (op->state == stWaitConnectResponse) {
    p2pErrorTy error;
    if (op->info.connection->stream.readStatusMessage(&error))
      finishP2POp(op, p2pStatusFromError(error));
    else
      finishP2POp(op, p2pStFormatError);
  } else {
    finishP2POp(op, aosUnknownError);
  }
}

void aiop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data, p2pCb callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, 0, 0, 0, (void*)callback, arg);
  op->type = p2pOpConnect;
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  op->state = stWaitConnectResponse;  
  aiop2pSend(connection, timeout, connection->stream.data(), p2pHeader(p2pMsgConnect, connection->stream.sizeOf()), 0, 0);
  aiop2pRecv(connection, timeout, &op->info.connection->stream, 1024, connectProc, op);
}

void p2pRecvProc(aioInfo *info)
{
  p2pOp *op = (p2pOp*)info->arg;
  if (info->status == aosSuccess) {
    if (op->state == stWaitHeader) {
      // TODO: correct processing zero-sized messages
      size_t msgSize = op->info.header.size;
      if (msgSize <= op->info.bufferSize) {
        op->state = stWaitMsgBody;
        aioRead(op->info.connection->socket, op->info.buffer, msgSize, afWaitAll, op->usTimeout, p2pRecvProc, op);
      } else {
        finishP2POp(op, p2pStBufferTooSmall);
      }
    } else if (op->state == stWaitMsgBody) {
      finishP2POp(op, aosSuccess);
    }
  } else {
    finishP2POp(op, info->status);
  }
}

void p2pRecvStreamProc(aioInfo *info)
{
  p2pOp *op = (p2pOp*)info->arg;
  if (info->status == aosSuccess) {
    p2pConnection *connection = op->info.connection;
    if (op->state == stWaitHeader) {
      size_t msgSize = op->info.header.size;
      if (op->info.bufferSize == 0 || msgSize <= op->info.bufferSize) {
        op->state = stWaitMsgBody;
        connection->stream.reset();
        aioRead(connection->socket, connection->stream.alloc(msgSize), msgSize, afWaitAll, op->usTimeout, p2pRecvStreamProc, op);
      } else {
        finishP2POp(op, aosUnknownError);
      }
    } else if (op->state == stWaitMsgBody) {
      connection->stream.seekSet(0);
      finishP2POp(op, aosSuccess);
    }
  } else {
    finishP2POp(op, info->status);
  }
}

void sendProc(aioInfo *info)
{
  finishP2POp((p2pOp*)info->arg, info->status);
}

void aiop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pCb callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, buffer, bufferSize, 0, (void*)callback, arg);
  op->type = p2pOpRecv;
  op->state = stWaitHeader;

  if (!connection->current) {
    aioRead(connection->socket, &op->info.header, sizeof(p2pHeader), afWaitAll, timeout, p2pRecvProc, op);
  } else {
    connection->current->next = op;
  }
}

void aiop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize,  p2pCb callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, 0, maxMsgSize, 0, (void*)callback, arg);
  op->type = p2pOpRecvStream;
  op->state = stWaitHeader;

  if (!connection->current) {
    aioRead(connection->socket, &op->info.header, sizeof(p2pHeader), afWaitAll, timeout, p2pRecvStreamProc, op);
  } else {
    connection->current->next = op;
  }
}

void aiop2pSend(p2pConnection *connection, uint64_t timeout, void *data, p2pHeader header, p2pCb callback, void *arg)
{
  p2pOp *op = allocp2pOp(connection, 0, 0, 0, (void*)callback, arg);  
  op->type = p2pOpSend;

  aioWrite(connection->socket, &header, sizeof(p2pHeader), afWaitAll, timeout, 0, 0);
  aioWrite(connection->socket, data, header.size, afWaitAll, timeout, sendProc, op);
}

int iop2pAccept(p2pConnection *connection, uint64_t timeout, p2pAcceptCb callback, void *arg)
{
  p2pHeader header;
  p2pConnectData data;

  if (iop2pRecv(connection, timeout, &connection->stream, 1024, &header) == -1)
    return aosUnknownError;

  if (!connection->stream.readConnectMessage(&data))
    return p2pStFormatError;

  p2pInfo info;
  info.status = aosPending;
  p2pErrorTy error = callback(&data, &info);
  connection->stream.reset();
  connection->stream.writeStatusMessage(error);
  bool sendResult = iop2pSend(connection, timeout, connection->stream.data(), 0, p2pMsgStatus, connection->stream.sizeOf());
  info.status = sendResult ? p2pStatusFromError(error) : aosUnknownError;
  callback(0, &info);
  return info.status;
}

int iop2pConnect(p2pConnection *connection, uint64_t timeout, p2pConnectData *data)
{
  connection->stream.reset();
  connection->stream.writeConnectMessage(*data);
  p2pHeader header;
  if (!iop2pSend(connection, timeout, connection->stream.data(), 0, p2pMsgConnect, connection->stream.sizeOf()))
    return aosUnknownError;
  
  if (!iop2pRecv(connection, timeout, &connection->stream, 1024, &header) || header.type != p2pMsgStatus)
    return aosUnknownError;
  
  p2pErrorTy error;
  if (!connection->stream.readStatusMessage(&error))
    return p2pStFormatError;

  return p2pStatusFromError(error);
}

bool iop2pSend(p2pConnection *connection, uint64_t timeout, void *data, uint32_t id, uint32_t type, size_t size)
{
  bool success;
  p2pHeader header(id, type, size);
  success = ioWrite(connection->socket, &header, sizeof(p2pHeader), afWaitAll, timeout) == sizeof(p2pHeader);
  success &= (ioWrite(connection->socket, data, size, afWaitAll, timeout) == size);
  return success;
}

ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, void *buffer, size_t bufferSize, p2pHeader *header)
{
  if (ioRead(connection->socket, header, sizeof(p2pHeader), afWaitAll, timeout) == sizeof(p2pHeader) && header->size <= bufferSize)
    return ioRead(connection->socket, buffer, header->size, afWaitAll, timeout) == header->size ? header->size : -1;
}

ssize_t iop2pRecv(p2pConnection *connection, uint64_t timeout, p2pStream *stream, size_t maxMsgSize, p2pHeader *header)
{
  if (ioRead(connection->socket, header, sizeof(p2pHeader), afWaitAll, timeout) == sizeof(p2pHeader) && header->size <= maxMsgSize) {
    stream->reset();
    ssize_t result = ioRead(connection->socket, stream->alloc(header->size), header->size, afWaitAll, timeout) ;
    stream->seekSet(0);
    return result == header->size ? header->size : -1;
  } else {
    return -1;
  }
}
