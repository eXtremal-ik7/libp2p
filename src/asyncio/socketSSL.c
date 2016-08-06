#include "Debug.h"
#include "asyncio/objectPool.h"
#include "asyncio/socketSSL.h" 
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <unistd.h>

#define DEFAULT_SSL_BUFFER_SIZE 65536

const char *sslPoolId = "SSL";

typedef enum {
  sslStConnecting = 0,
  sslStReadNewFrame,
  sslStReading
} SSLSocketStateTy;

typedef enum {
  sslOpConnect = 0,
  sslOpRead,
  sslOpWrite
} SSLOpTy;

void connectProc(aioInfo *info);
void readProc(aioInfo *info);
void writeProc(aioInfo *info);

static SSLOp *allocSSLOp(SSLSocket *socket,
                         sslCb callback,
                         void *arg,
                         void *buffer,
                         size_t size,
                         AsyncFlags flags)
{
  SSLOp *op = queryObject(socket->base, sslPoolId);
  if (!op) {
    op = malloc(sizeof(SSLOp));
    op->sslBuffer = (uint8_t*)malloc(1024);
    op->sslBufferSize = 1024;
  }

  op->info.socket = socket;
  op->info.callback = callback;
  op->info.arg = arg;
  op->info.buffer = buffer;
  op->info.flags = flags;
  op->info.transactionSize = size;
  op->info.bytesTransferred = 0;
  op->next = 0;
  return op;
}

static void finishSSLOp(SSLOp *Op, AsyncOpStatus status)
{
  SSLOp *current = Op->next;
  if (Op->type != sslOpWrite)
    Op->info.socket->current = current;
 
  Op->info.status = status;
  if (Op->info.callback)
    Op->info.callback(&Op->info);
  releaseObject(Op->info.socket->base, Op, sslPoolId);
  if (current && Op->type != sslOpWrite) {
    SSLSocket *S = current->info.socket;
    switch (current->type) {
      case sslOpConnect :
        aioConnect(S->object, &current->address, current->usTimeout, connectProc, current);
        break;
      case sslOpRead :
        asyncRead(S->object, S->sslReadBuffer, S->sslReadBufferSize, afNone, current->usTimeout, readProc, current);
        break;
    }
  }
}

size_t copyFromOut(SSLSocket *S, SSLOp *Op)
{
  size_t nBytes = BIO_ctrl_pending(S->bioOut);
  if (nBytes > Op->sslBufferSize) {
    Op->sslBuffer = realloc(Op->sslBuffer, nBytes);
    Op->sslBufferSize = nBytes;
  }
  
  BIO_read(S->bioOut, Op->sslBuffer, nBytes);  
  return nBytes;
}


void connectProc(aioInfo *info)
{
  SSLOp *Op = (SSLOp*)info->arg;
  SSLSocket *S = Op->info.socket;
  if (info->status == aosSuccess) {
    if (Op->state == sslStReadNewFrame) {
      BIO_write(S->bioIn, S->sslReadBuffer, info->bytesTransferred);
      Op->state = sslStConnecting;
    }

    int connectResult = SSL_connect(S->ssl);
    int errCode = SSL_get_error(S->ssl, connectResult);
    if (connectResult == 1) {
      // Successfully connected
      finishSSLOp(Op, aosSuccess);
    } else if (errCode == SSL_ERROR_WANT_READ) {
      // Need data exchange
      size_t connectSize = copyFromOut(S, Op);
      Op->state = sslStReadNewFrame;
      asyncWrite(S->object, Op->sslBuffer, connectSize, afNone, 3000000, 0, 0);
      asyncRead(S->object, S->sslReadBuffer, S->sslReadBufferSize, afNone, 3000000, connectProc, Op);
    } else {
      finishSSLOp(Op, aosUnknownError);          
    }
  } else {
    finishSSLOp(Op, info->status);
  }
}

void readProc(aioInfo *info)
{
  SSLOp *Op = (SSLOp*)info->arg;
  SSLSocket *S = Op->info.socket;
  if (info->status == aosSuccess) {
    if (Op->state == sslStReadNewFrame) {
      BIO_write(S->bioIn, S->sslReadBuffer, info->bytesTransferred);
      Op->state = sslStReading;
    }
    
    uint8_t *ptr = ((uint8_t*)Op->info.buffer) + Op->info.bytesTransferred;
    size_t size = Op->info.transactionSize-Op->info.bytesTransferred;
    
    int readResult = 0;
    int R;
    while ( (R = SSL_read(S->ssl, ptr, size)) > 0) {
      readResult += R;
      ptr += R;
      size -= R;
    }
    
    Op->info.bytesTransferred += readResult;
    if (Op->info.bytesTransferred == Op->info.transactionSize || (Op->info.bytesTransferred && !(Op->info.flags & afWaitAll))) {
      finishSSLOp(Op, aosSuccess);
    } else {
      Op->state = sslStReadNewFrame;
      asyncRead(S->object, S->sslReadBuffer, S->sslReadBufferSize, afNone, 3000000, readProc, Op);
    }    
  } else {
    finishSSLOp(Op, info->status);
  }
}



void writeProc(aioInfo *info)
{
  SSLOp *Op = (SSLOp*)info->arg;
  if (info->status == aosSuccess) {
    finishSSLOp(Op, aosSuccess);
  } else {
    finishSSLOp(Op, info->status);
  }
}


SSLSocket *sslSocketNew(asyncBase *base)
{
  SSLSocket *S = (SSLSocket*)malloc(sizeof(SSLSocket));
  S->base = base;
  S->sslContext = SSL_CTX_new (TLSv1_1_client_method());
  S->ssl = SSL_new(S->sslContext);  
  S->bioIn = BIO_new(BIO_s_mem());
  S->bioOut = BIO_new(BIO_s_mem());  
  SSL_set_bio(S->ssl, S->bioIn, S->bioOut);
  
  S->S = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  S->object = newSocketIo(base, S->S);
  
  S->sslReadBufferSize = DEFAULT_SSL_BUFFER_SIZE;
  S->sslReadBuffer = (uint8_t*)malloc(S->sslReadBufferSize);
  
  S->current = 0;
  return S;
}

socketTy sslGetSocket(const SSLSocket *socket)
{
  return socket->S;
}

void sslConnect(SSLSocket *socket,
                const HostAddress *address,
                uint64_t usTimeout,
                sslCb callback,
                void *arg)
{
  SSL_set_connect_state(socket->ssl);
  
  SSLOp *newOp = allocSSLOp(socket, callback, arg, 0, 0, afNone);
  newOp->type = sslOpConnect;
  newOp->state = sslStConnecting;
  
  if (!socket->current) {
    socket->current = newOp;
    aioConnect(socket->object, address, usTimeout, connectProc, newOp);    
  } else {
    newOp->address = *address;
    newOp->usTimeout = usTimeout;
    socket->current->next = newOp;
  }
}


void sslRead(SSLSocket *socket,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             sslCb callback,
             void *arg)
{
  size_t readSize = BIO_ctrl_pending(socket->bioIn);

  SSLOp *newOp = allocSSLOp(socket, callback, arg, buffer, size, afNone);  
  newOp->type = sslOpRead;
  newOp->state = sslStReadNewFrame;

  if (readSize >= size) {
    BIO_read(socket->bioOut, buffer, size);
    finishSSLOp(newOp, aosSuccess);
  } else if (!socket->current) {
    socket->current = newOp;
    asyncRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, usTimeout, readProc, newOp);
  } else {
    newOp->usTimeout = usTimeout;
    socket->current->next = newOp;
  }
}


void sslWrite(SSLSocket *socket,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              sslCb callback,
              void *arg)
{
  SSL_write(socket->ssl, buffer, size);
  SSLOp *newOp = allocSSLOp(socket, callback, arg, buffer, size, afNone);  
  size_t writeSize = copyFromOut(socket, newOp);  
  newOp->type = sslOpWrite;
  asyncWrite(socket->object, newOp->sslBuffer, writeSize, afWaitAll, usTimeout, writeProc, newOp);    
}
