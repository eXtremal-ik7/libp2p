#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"
#include "asyncio/socket.h"
#include "asyncio/socketSSL.h" 
#include <openssl/bio.h>
#include <openssl/ssl.h>

#define DEFAULT_SSL_BUFFER_SIZE 65536

static const char *sslPoolId = "SSL";
static const char *sslPoolTimerId = "SSLTimer";

typedef enum {
  sslStInitalize = 0,
  sslStProcessing
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

static void socketConnectCb(AsyncOpStatus status, aioObject *object, void *arg);
static AsyncOpStatus connectProc(asyncOpRoot *opptr);
static AsyncOpStatus readProc(asyncOpRoot *opptr);
static void sslWriteWriteCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);


static asyncOpRoot *alloc(asyncBase *base)
{
  SSLOp *op = (SSLOp*)malloc(sizeof(SSLOp));
  op->sslBuffer = (uint8_t*)malloc(1024);
  op->sslBufferSize = 1024;
  return (asyncOpRoot*)op;
}

static void finish(asyncOpRoot *root, AsyncOpActionTy action)
{
  SSLOp *op = (SSLOp*)root;
  SSLSocket *S = (SSLSocket*)root->object;  
  AsyncOpStatus status = opGetStatus(root);
  
    // cleanup child operation after timeout
  if (action == aaCancel) {
    cancelIo((aioObjectRoot*)S->object);
    return;
  }
  
  if (root->callback && status != aosCanceled) {
    switch (root->opCode) {
      case sslOpConnect :
        ((sslConnectCb*)root->callback)(status, S, root->arg);
        break;
      case sslOpRead :
      case sslOpWrite :
        ((sslCb*)root->callback)(status, S, op->bytesTransferred, root->arg);
        break;
    }
  }
}

static SSLOp *allocSSLOp(SSLSocket *socket,
                         aioExecuteProc executeProc,
                         void *callback,
                         void *arg,
                         void *buffer,
                         size_t size,
                         AsyncFlags flags,
                         int opCode,
                         uint64_t timeout)
{
  SSLOp *op = (SSLOp*)
    initAsyncOpRoot(sslPoolId, sslPoolTimerId, alloc, executeProc, finish, &socket->root, callback, arg, flags, opCode, timeout);
  op->buffer = buffer;
  op->transactionSize = size;
  op->bytesTransferred = 0;
  op->state = sslStInitalize;
  return op;
}

static void coroutineConnectCb(AsyncOpStatus status, SSLSocket *object, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
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
  
  // TODO: correct processing >4Gb data blocks
  BIO_read(S->bioOut, Op->sslBuffer, (int)nBytes);  
  return nBytes;
}

static void sslConnectConnectCb(AsyncOpStatus status, aioObject *object, void *arg)
{
  resumeParent((asyncOpRoot*)arg, status);
}

static void sslConnectReadCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  SSLOp *op = (SSLOp*)arg;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  // TODO: correct processing >4Gb data blocks
  BIO_write(socket->bioIn, socket->sslReadBuffer, (int)transferred);
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus connectProc(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)op->root.object;

  if (op->state == sslStInitalize) {
      op->state = sslStProcessing;
      aioConnect(socket->object, &op->address, 0, sslConnectConnectCb, op);
      return aosPending;
  }

  int connectResult = SSL_connect(socket->ssl);
  int errCode = SSL_get_error(socket->ssl, connectResult);
  if (connectResult == 1) {
    // Successfully connected
    return aosSuccess;
  } else if (errCode == SSL_ERROR_WANT_READ) {
    // Need data exchange
    size_t connectSize = copyFromOut(socket, op);
    aioWrite(socket->object, op->sslBuffer, connectSize, afWaitAll, 0, 0, 0);
    aioRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslConnectReadCb, op);
    return aosPending;
  } else {
    return aosUnknownError;
  }
}

static void sslReadReadCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  SSLOp *op = (SSLOp*)arg;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  // TODO: correct processing >4Gb data blocks
  BIO_write(socket->bioIn, socket->sslReadBuffer, (int)transferred);
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus readProc(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  if (op->state == sslStInitalize) {
    op->state = sslStProcessing;
    aioRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslReadReadCb, op);
    return aosPending;
  }
    
  uint8_t *ptr = ((uint8_t*)op->buffer) + op->bytesTransferred;
  size_t size = op->transactionSize-op->bytesTransferred;
    
  int readResult = 0;
  int R;
  // TODO: correct processing >4Gb data blocks
  while ( (R = SSL_read(socket->ssl, ptr, (int)size)) > 0) {
    readResult += R;
    ptr += R;
    size -= R;
  }
    
  op->bytesTransferred += readResult;
  if (op->bytesTransferred == op->transactionSize || (op->bytesTransferred && !(op->root.flags & afWaitAll))) {
    return aosSuccess;
  } else {
    aioRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslReadReadCb, op);
    return aosPending;
  }
}

void sslSocketDestructor(aioObjectRoot *root)
{
  SSLSocket *socket = (SSLSocket*)root;
  free(socket->sslReadBuffer);  
  SSL_free(socket->ssl);
  SSL_CTX_free(socket->sslContext);
  deleteAioObject(socket->object);
  free(socket);
}


SSLSocket *sslSocketNew(asyncBase *base)
{
  SSLSocket *S = (SSLSocket*)malloc(sizeof(SSLSocket));
  initObjectRoot(&S->root, base, ioObjectUserDefined, sslSocketDestructor);
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
  objectDelete(&socket->root);
}

socketTy sslGetSocket(const SSLSocket *socket)
{
  return aioObjectSocket(socket->object);
}

void aioSslConnect(SSLSocket *socket,
                   const HostAddress *address,
                   uint64_t usTimeout,
                   sslConnectCb callback,
                   void *arg)
{
  SSL_set_connect_state(socket->ssl);
  SSLOp *newOp = allocSSLOp(socket, connectProc, callback, arg, 0, 0, afNone, sslOpConnect, usTimeout);
  newOp->address = *address;
  opStart(&newOp->root);
}

void aioSslRead(SSLSocket *socket,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                sslCb callback,
                void *arg)
{
  SSLOp *newOp = allocSSLOp(socket, readProc, callback, arg, buffer, size, afNone, sslOpRead, usTimeout);
  opStart(&newOp->root);
}

void sslWriteWriteCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus writeProc(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)opptr->object;

  if (op->state == sslStInitalize) {
    op->state = sslStProcessing;
  SSL_write(socket->ssl, op->buffer, (int)op->transactionSize);
  size_t writeSize = copyFromOut(socket, op);
  aioWrite(socket->object, op->sslBuffer, writeSize, afWaitAll, 0, sslWriteWriteCb, op);
  return aosPending;
  } else {
  return aosSuccess;
  }
}

void aioSslWrite(SSLSocket *socket,
                 const void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 sslCb callback,
                 void *arg)
{
  SSLOp *newOp = allocSSLOp(socket, writeProc, callback, arg, (void*)buffer, size, afNone, sslOpWrite, usTimeout);
  opStart(&newOp->root);
}

int ioSslConnect(SSLSocket *socket, const HostAddress *address, uint64_t usTimeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSL_set_connect_state(socket->ssl);
  SSLOp *op = allocSSLOp(socket, connectProc, coroutineConnectCb, &r, 0, 0, afNone, sslOpConnect, usTimeout);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -(int)r.status;
}

ssize_t ioSslRead(SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSLOp *op = allocSSLOp(socket, readProc, coroutineCb, &r, buffer, size, afNone, sslOpRead, usTimeout);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -(int)r.status;
}

ssize_t ioSslWrite(SSLSocket *socket, const void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSLOp *op = allocSSLOp(socket, writeProc, coroutineCb, &r, (void*)buffer, size, afNone, sslOpWrite, usTimeout);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -(int)r.status;
}
