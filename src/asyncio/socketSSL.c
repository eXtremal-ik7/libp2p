#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"
#include "asyncio/socketSSL.h" 
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <unistd.h>

#define DEFAULT_SSL_BUFFER_SIZE 65536

const char *sslPoolId = "SSL";
const char *sslPoolTimerId = "SSLTimer";

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

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  size_t transferred;
} coroReturnStruct;

static void socketConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg);
void connectProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);
void readProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);
void writeProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);


static asyncOpRoot *alloc(asyncBase *base)
{
  SSLOp *op = (SSLOp*)malloc(sizeof(SSLOp));
  op->sslBuffer = (uint8_t*)malloc(1024);
  op->sslBufferSize = 1024;
  return (asyncOpRoot*)op;
}

static void start(asyncOpRoot *op)
{
  SSLSocket *S = (SSLSocket*)op->object;
  switch (op->opCode) {
    case sslOpConnect :
    case sslOpWrite :
      // TODO: return error
      break;
    case sslOpRead :
      aioRead(op->base, S->object, S->sslReadBuffer, S->sslReadBufferSize, afNone, 0, readProc, op);  
      break;
  }
}

static void finish(asyncOpRoot *root, int status)
{
  SSLOp *op = (SSLOp*)root;
  if (root->callback) {
    SSLSocket *S = (SSLSocket*)root->object;
    switch (root->opCode) {
      case sslOpConnect :
        ((sslConnectCb*)root->callback)(status, root->base, S, root->arg);
        break;
      case sslOpRead :
      case sslOpWrite :
        ((sslCb*)root->callback)(status, root->base, S, op->bytesTransferred, root->arg);
        break;
    }
  }
}

static SSLOp *allocSSLOp(asyncBase *base,
                         SSLSocket *socket,
                         void *callback,
                         void *arg,
                         void *buffer,
                         size_t size,
                         AsyncFlags flags,
                         int opCode,
                         uint64_t timeout)
{
  SSLOp *op = (SSLOp*)
    initAsyncOpRoot(base, sslPoolId, sslPoolTimerId, alloc, start, finish, &socket->root, callback, arg, flags, opCode, timeout);
  op->buffer = buffer;
  op->transactionSize = size;
  op->bytesTransferred = 0;
  return op;
}

static void coroutineConnectCb(AsyncOpStatus status, asyncBase *base, SSLSocket *object, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, asyncBase *base, SSLSocket *object, size_t transferred, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->transferred = transferred;
  coroutineCall(r->coroutine);
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


void connectProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  SSLOp *Op = (SSLOp*)arg;
  SSLSocket *S = (SSLSocket*)Op->root.object;
  if (status == aosSuccess) {
    if (Op->state == sslStReadNewFrame) {
      BIO_write(S->bioIn, S->sslReadBuffer, transferred);
      Op->state = sslStConnecting;
    }

    int connectResult = SSL_connect(S->ssl);
    int errCode = SSL_get_error(S->ssl, connectResult);
    if (connectResult == 1) {
      // Successfully connected
      finishOperation(&Op->root, aosSuccess, 1);
    } else if (errCode == SSL_ERROR_WANT_READ) {
      // Need data exchange
      size_t connectSize = copyFromOut(S, Op);
      Op->state = sslStReadNewFrame;
      aioWrite(base, object, Op->sslBuffer, connectSize, afWaitAll, 0, 0, 0);
      aioRead(base, object, S->sslReadBuffer, S->sslReadBufferSize, afNone, 0, connectProc, Op);
    } else {
      finishOperation(&Op->root, aosUnknownError, 1);        
    }
  } else {
    finishOperation(&Op->root, status, 1); 
  }
}

static void socketConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  connectProc(status, base, object, 0, arg);
}

void readProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  SSLOp *Op = (SSLOp*)arg;
  SSLSocket *S = (SSLSocket*)Op->root.object;
  if (status == aosSuccess) {
    if (Op->state == sslStReadNewFrame) {
      BIO_write(S->bioIn, S->sslReadBuffer, transferred);
      Op->state = sslStReading;
    }
    
    uint8_t *ptr = ((uint8_t*)Op->buffer) + Op->bytesTransferred;
    size_t size = Op->transactionSize-Op->bytesTransferred;
    
    int readResult = 0;
    int R;
    while ( (R = SSL_read(S->ssl, ptr, size)) > 0) {
      readResult += R;
      ptr += R;
      size -= R;
    }
    
    Op->bytesTransferred += readResult;
    if (Op->bytesTransferred == Op->transactionSize || (Op->bytesTransferred && !(Op->root.flags & afWaitAll))) {
      finishOperation(&Op->root, aosSuccess, 1);
    } else {
      Op->state = sslStReadNewFrame;
      aioRead(base, object, S->sslReadBuffer, S->sslReadBufferSize, afNone, 0, readProc, Op);
    }    
  } else {
    finishOperation(&Op->root, status, 1);
  }
}



void writeProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  finishOperation((asyncOpRoot*)arg, status, 1);
}


void sslSocketDestructor(aioObjectRoot *root)
{
  SSLSocket *socket = (SSLSocket*)root;
  free(socket->sslReadBuffer);  
  SSL_free(socket->ssl);
  SSL_CTX_free(socket->sslContext);
  free(socket);
}


SSLSocket *sslSocketNew(asyncBase *base)
{
  SSLSocket *S = (SSLSocket*)initObjectRoot(ioObjectUserDefined, sizeof(SSLSocket), sslSocketDestructor);
  S->sslContext = SSL_CTX_new (TLSv1_1_client_method());
  S->ssl = SSL_new(S->sslContext);  
  S->bioIn = BIO_new(BIO_s_mem());
  S->bioOut = BIO_new(BIO_s_mem());  
  SSL_set_bio(S->ssl, S->bioIn, S->bioOut);
  
  socketTy socket = socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1);
  S->object = newSocketIo(base, socket);  
  S->sslReadBufferSize = DEFAULT_SSL_BUFFER_SIZE;
  S->sslReadBuffer = (uint8_t*)malloc(S->sslReadBufferSize);
  return S;
}

void sslSocketDelete(SSLSocket *socket)
{
  deleteAioObject(socket->object);
  socket->root.links--;   // TODO: atomic
  checkForDeleteObject(&socket->root);
}

socketTy sslGetSocket(const SSLSocket *socket)
{
  return aioObjectSocket(socket->object);
}

void aioSslConnect(asyncBase *base,
                   SSLSocket *socket,
                   const HostAddress *address,
                   uint64_t usTimeout,
                   sslConnectCb callback,
                   void *arg)
{
  SSL_set_connect_state(socket->ssl);
  SSLOp *newOp = allocSSLOp(base, socket, callback, arg, 0, 0, afNone, sslOpConnect, usTimeout);
  newOp->state = sslStConnecting;
  if (addToExecuteQueue(&socket->root, &newOp->root, 0))
    aioConnect(base, socket->object, address, 0, socketConnectCb, newOp);
}


void aioSslRead(asyncBase *base,
                SSLSocket *socket,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                sslCb callback,
                void *arg)
{
  SSLOp *newOp = allocSSLOp(base, socket, callback, arg, buffer, size, afNone, sslOpRead, usTimeout);
  newOp->state = sslStReadNewFrame;
  if (addToExecuteQueue(&socket->root, &newOp->root, 0))
    start(&newOp->root);
}


void aioSslWrite(asyncBase *base,
                 SSLSocket *socket,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 sslCb callback,
                 void *arg)
{
  SSL_write(socket->ssl, buffer, size);
  
  // Allocate operation only for buffer
  SSLOp *newOp = allocSSLOp(base, socket, callback, arg, buffer, size, afNone, sslOpWrite, usTimeout);  
  size_t writeSize = copyFromOut(socket, newOp);  
  aioWrite(base, socket->object, newOp->sslBuffer, writeSize, afWaitAll, usTimeout, writeProc, newOp);
  releaseObject(base, newOp, newOp->root.poolId);
}

int ioSslConnect(asyncBase *base, SSLSocket *socket, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioSslConnect(base, socket, address, usTimeout, coroutineConnectCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -1;
}

ssize_t ioSslRead(asyncBase *base, SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioSslRead(base, socket, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -1;
}

ssize_t ioSslWrite(asyncBase *base, SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioSslWrite(base, socket, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -1;
}
