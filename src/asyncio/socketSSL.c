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

static void socketConnectCb(AsyncOpStatus status, aioObject *object, void *arg);
static void connectProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);
static void readProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);
static void writeProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);


static asyncOpRoot *alloc(asyncBase *base)
{
  SSLOp *op = (SSLOp*)malloc(sizeof(SSLOp));
  op->sslBuffer = (uint8_t*)malloc(1024);
  op->sslBufferSize = 1024;
  return (asyncOpRoot*)op;
}

static void finish(asyncOpRoot *root)
{
  SSLOp *op = (SSLOp*)root;
  SSLSocket *S = (SSLSocket*)root->object;  
  AsyncOpStatus status = opGetStatus(root);
  
    // cleanup child operation after timeout
  if (status != aosSuccess)
    cancelIo((aioObjectRoot*)S->object);
  
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


static void connectProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  SSLOp *op = (SSLOp*)link->op;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  if (status == aosSuccess) {
    if (op->state == sslStReadNewFrame) {
      // TODO: correct processing >4Gb data blocks
      BIO_write(socket->bioIn, socket->sslReadBuffer, (int)transferred);
      op->state = sslStConnecting;
    }

    int connectResult = SSL_connect(socket->ssl);
    int errCode = SSL_get_error(socket->ssl, connectResult);
    if (connectResult == 1) {
      // Successfully connected
      opReleaseLink(link, aosSuccess);
    } else if (errCode == SSL_ERROR_WANT_READ) {
      // Need data exchange
      size_t connectSize = copyFromOut(socket, op);
      op->state = sslStReadNewFrame;
      aioWrite(object, op->sslBuffer, connectSize, afWaitAll, 0, 0, 0);
      aioRead(object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, connectProc, link);
    } else {
      opReleaseLink(link, aosUnknownError);
    }
  } else {
    opReleaseLink(link, status);
  }
}

static void socketConnectCb(AsyncOpStatus status, aioObject *object, void *arg)
{
  connectProc(status, object, 0, arg);
}

void readProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  asyncOpLink *link = (asyncOpLink*)arg;
  SSLOp *op = (SSLOp*)link->op;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  if (status == aosSuccess) {
    if (op->state == sslStReadNewFrame) {
      // TODO: correct processing >4Gb data blocks
      BIO_write(socket->bioIn, socket->sslReadBuffer, (int)transferred);
      op->state = sslStReading;
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
      opReleaseLink(link, aosSuccess);
    } else {
      op->state = sslStReadNewFrame;
      aioRead(object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, readProc, link);
    }    
  } else {
    opReleaseLink(link, status);
  }
}



void writeProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  opReleaseLink((asyncOpLink*)arg, status);
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
  cancelIo(&socket->root);
  objectDeleteRef(&socket->root, 1);
}

socketTy sslGetSocket(const SSLSocket *socket)
{
  return aioObjectSocket(socket->object);
}

static AsyncOpStatus sslConnectStart(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)opptr->object;
  asyncOpLink *opLink = opAllocateLink(opptr);
  aioConnect(socket->object, &op->address, 0, socketConnectCb, opLink);
  return aosPending;
}

void aioSslConnect(SSLSocket *socket,
                   const HostAddress *address,
                   uint64_t usTimeout,
                   sslConnectCb callback,
                   void *arg)
{
  objectAddRef(&socket->root);
  SSL_set_connect_state(socket->ssl);
  SSLOp *newOp = allocSSLOp(socket, sslConnectStart, callback, arg, 0, 0, afNone, sslOpConnect, usTimeout);
  newOp->address = *address;
  newOp->state = sslStConnecting;
  opStart(&newOp->root);
}

static AsyncOpStatus sslReadStart(asyncOpRoot *opptr)
{
  SSLSocket *socket = (SSLSocket*)opptr->object;
  asyncOpLink *opLink = opAllocateLink(opptr);
  aioRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, readProc, opLink);
  return aosPending;
}

void aioSslRead(SSLSocket *socket,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                sslCb callback,
                void *arg)
{
  objectAddRef(&socket->root);
  SSLOp *newOp = allocSSLOp(socket, sslReadStart, callback, arg, buffer, size, afNone, sslOpRead, usTimeout);
  newOp->state = sslStReadNewFrame;
  opStart(&newOp->root);
}

static AsyncOpStatus sslWriteStart(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)opptr->object;
  asyncOpLink *opLink = opAllocateLink(opptr);

  SSL_write(socket->ssl, op->buffer, (int)op->transactionSize);
  size_t writeSize = copyFromOut(socket, op);
  aioWrite(socket->object, op->sslBuffer, writeSize, afWaitAll, 0, writeProc, opLink);
  return aosPending;
}

void aioSslWrite(SSLSocket *socket,
                 const void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 sslCb callback,
                 void *arg)
{
  objectAddRef(&socket->root);
  SSLOp *newOp = allocSSLOp(socket, sslWriteStart, callback, arg, (void*)buffer, size, afNone, sslOpWrite, usTimeout);
  opStart(&newOp->root);
}

int ioSslConnect(SSLSocket *socket, const HostAddress *address, uint64_t usTimeout)
{
  objectAddRef(&socket->root);
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSL_set_connect_state(socket->ssl);
  SSLOp *op = allocSSLOp(socket, sslConnectStart, coroutineConnectCb, &r, 0, 0, afNone, sslOpConnect, usTimeout);
  op->address = *address;
  op->state = sslStConnecting;
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -(int)r.status;
}

ssize_t ioSslRead(SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  objectAddRef(&socket->root);
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSLOp *op = allocSSLOp(socket, sslReadStart, coroutineCb, &r, buffer, size, afNone, sslOpRead, usTimeout);
  op->state = sslStReadNewFrame;
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -(int)r.status;
}

ssize_t ioSslWrite(SSLSocket *socket, const void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  objectAddRef(&socket->root);
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSLOp *op = allocSSLOp(socket, sslWriteStart, coroutineCb, &r, (void*)buffer, size, afNone, sslOpWrite, usTimeout);
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? r.transferred : -(int)r.status;
}
