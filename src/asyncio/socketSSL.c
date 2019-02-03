#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/objectPool.h"
#include "asyncio/socket.h"
#include "asyncio/socketSSL.h" 
#include "asyncioImpl.h"
#include "atomic.h"
#include <openssl/bio.h>
#include <openssl/ssl.h>

#define DEFAULT_SSL_READ_BUFFER_SIZE 16384
#define DEFAULT_SSL_WRITE_BUFFER_SIZE 16384

static __tls ObjectPool sslSocketPool;
static __tls ObjectPool sslPoolId;
static __tls ObjectPool sslPoolTimerId;

typedef enum {
  sslStInitalize = 0,
  sslStProcessing
} SSLSocketStateTy;

typedef enum {
  sslOpConnect = 0,
  sslOpRead,
  sslOpWrite
} SSLOpTy;

__NO_PADDING_BEGIN
typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  size_t transferred;
} coroReturnStruct;
__NO_PADDING_END

static AsyncOpStatus connectProc(asyncOpRoot *opptr);
static AsyncOpStatus readProc(asyncOpRoot *opptr);
static void sslWriteWriteCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg);


static asyncOpRoot *alloc()
{
  SSLOp *op = (SSLOp*)malloc(sizeof(SSLOp));
  return (asyncOpRoot*)op;
}

static int cancel(asyncOpRoot *opptr)
{
  SSLSocket *S = (SSLSocket*)opptr->object;
  cancelIo((aioObjectRoot*)S->object);
  return 0;
}

static void connectFinish(asyncOpRoot *opptr)
{
  ((sslConnectCb*)opptr->callback)(opGetStatus(opptr), (SSLSocket*)opptr->object, opptr->arg);
}

static void rwFinish(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  ((sslCb*)opptr->callback)(opGetStatus(opptr), (SSLSocket*)opptr->object, op->bytesTransferred, opptr->arg);
}

static SSLOp *allocReadSSLOp(aioExecuteProc executeProc,
                             aioFinishProc finishProc,
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
    initAsyncOpRoot(&sslPoolId, &sslPoolTimerId, alloc, executeProc, cancel, finishProc, &socket->root, callback, arg, flags, opCode, timeout);
  op->buffer = buffer;
  op->transactionSize = size;
  op->bytesTransferred = 0;
  op->state = sslStInitalize;
  return op;
}

static SSLOp *allocWriteSSLOp(aioExecuteProc executeProc,
                              aioFinishProc finishProc,
                              SSLSocket *socket,
                              void *callback,
                              void *arg,
                              const void *buffer,
                              size_t size,
                              AsyncFlags flags,
                              int opCode,
                              uint64_t timeout)
{
  SSLOp *op = (SSLOp*)
    initAsyncOpRoot(&sslPoolId, &sslPoolTimerId, alloc, executeProc, cancel, finishProc, &socket->root, callback, arg, flags, opCode, timeout);
  op->buffer = (void*)(uintptr_t)buffer;
  op->transactionSize = size;
  op->bytesTransferred = 0;
  op->state = sslStInitalize;
  return op;
}

static void coroutineConnectCb(AsyncOpStatus status, SSLSocket *object, void *arg)
{
  __UNUSED(object)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->transferred = transferred;
  coroutineCall(r->coroutine);
}

size_t copyFromOut(SSLSocket *S)
{
  size_t nBytes = BIO_ctrl_pending(S->bioOut);
  if (nBytes > S->sslWriteBufferSize) {
    S->sslWriteBuffer = realloc(S->sslWriteBuffer, nBytes);
    S->sslWriteBufferSize = nBytes;
  }
  
  // TODO: correct processing >4Gb data blocks
  BIO_read(S->bioOut, S->sslWriteBuffer, (int)nBytes);
  return nBytes;
}

static void sslConnectConnectCb(AsyncOpStatus status, aioObject *object, void *arg)
{
  __UNUSED(object)
  resumeParent((asyncOpRoot*)arg, status);
}

static void sslConnectReadCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object)
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
    size_t connectSize = copyFromOut(socket);
    aioWrite(socket->object, socket->sslWriteBuffer, connectSize, afWaitAll, 0, 0, 0);
    aioRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslConnectReadCb, op);
    return aosPending;
  } else {
    return aosUnknownError;
  }
}

static void sslReadReadCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  SSLOp *op = (SSLOp*)arg;
  SSLSocket *socket = (SSLSocket*)op->root.object;
  // TODO: correct processing >4Gb data blocks
  if (status == aosSuccess)
    BIO_write(socket->bioIn, socket->sslReadBuffer, (int)transferred);
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus readProc(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)op->root.object;
    
  for (;;) {
    uint8_t *ptr = ((uint8_t*)op->buffer) + op->bytesTransferred;
    size_t size = op->transactionSize-op->bytesTransferred;
    
    size_t readResult = 0;
    int R;
    // TODO: correct processing >4Gb data blocks
    while ( (R = SSL_read(socket->ssl, ptr, (int)size)) > 0) {
      readResult += (size_t)R;
      ptr += R;
      size -= (size_t)R;
    }
    
    op->bytesTransferred += readResult;
    if (op->bytesTransferred == op->transactionSize || (op->bytesTransferred && !(op->root.flags & afWaitAll))) {
      return aosSuccess;
    } else {
      size_t bytes = 0;
      asyncOpRoot *readOp = implRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslReadReadCb, op, &bytes);
      if (!readOp) {
        BIO_write(socket->bioIn, socket->sslReadBuffer, (int)bytes);
      } else {
        opStart(readOp);
        return aosPending;
      }
    }
  }
}

void sslSocketDestructor(aioObjectRoot *root)
{
  SSLSocket *socket = (SSLSocket*)root;
  deleteAioObject(socket->object);
  objectRelease(root, &sslSocketPool);
}


SSLSocket *sslSocketNew(asyncBase *base)
{
  SSLSocket *S = objectGet(&sslSocketPool);
  if (!S) {
    S = (SSLSocket*)malloc(sizeof(SSLSocket));
#ifdef DEPRECATEDIN_1_1_0
    S->sslContext = SSL_CTX_new (TLS_client_method());
#else
    S->sslContext = SSL_CTX_new (TLSv1_1_client_method());
#endif
    S->ssl = SSL_new(S->sslContext);
    S->bioIn = BIO_new(BIO_s_mem());
    S->bioOut = BIO_new(BIO_s_mem());
    SSL_set_bio(S->ssl, S->bioIn, S->bioOut);
    S->sslReadBufferSize = DEFAULT_SSL_READ_BUFFER_SIZE;
    S->sslReadBuffer = (uint8_t*)malloc(S->sslReadBufferSize);
    S->sslWriteBufferSize = DEFAULT_SSL_READ_BUFFER_SIZE;
    S->sslWriteBuffer = (uint8_t*)malloc(S->sslReadBufferSize);
  } else {
    SSL_clear(S->ssl);
  }

  initObjectRoot(&S->root, base, ioObjectUserDefined, sslSocketDestructor);
  S->object = newSocketIo(base, socketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP, 1));
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
  SSLOp *newOp = allocReadSSLOp(connectProc, connectFinish, socket, (void*)callback, arg, 0, 0, afNone, sslOpConnect, usTimeout);
  newOp->address = *address;
  opStart(&newOp->root);
}

asyncOpRoot *implSslRead(SSLSocket *socket,
                         void *buffer,
                         size_t size,
                         AsyncFlags flags,
                         uint64_t usTimeout,
                         sslCb callback,
                         void *arg,
                         size_t *bytesTransferred)
{
  size_t sslBytesTransferred = 0;

  for (;;) {
    uint8_t *ptr = ((uint8_t*)buffer) + sslBytesTransferred;
    size_t remaining = size - sslBytesTransferred;
    size_t readResult = 0;
    int R;
    // TODO: correct processing >4Gb data blocks
    while ( (R = SSL_read(socket->ssl, ptr, (int)remaining)) > 0) {
      readResult += (size_t)R;
      ptr += R;
      remaining -= (size_t)R;
    }

    sslBytesTransferred += readResult;
    if (sslBytesTransferred == size || (sslBytesTransferred && !(flags & afWaitAll))) {
      *bytesTransferred = sslBytesTransferred;
      return 0;
    } else {
      size_t bytes = 0;
      asyncOpRoot *readOp = implRead(socket->object, socket->sslReadBuffer, socket->sslReadBufferSize, afNone, 0, sslReadReadCb, 0, &bytes);
      if (!readOp) {
        BIO_write(socket->bioIn, socket->sslReadBuffer, (int)bytes);
      } else {
        SSLOp *sslOp = allocReadSSLOp(readProc, rwFinish, socket, (void*)callback, arg, buffer, size, flags|afRunning, sslOpRead, usTimeout);
        sslOp->bytesTransferred = sslBytesTransferred;
        readOp->arg = sslOp;
        opStart(readOp);
        return &sslOp->root;
      }
    }
  }
}

ssize_t aioSslRead(SSLSocket *socket,
                   void *buffer,
                   size_t size,
                   AsyncFlags flags,
                   uint64_t usTimeout,
                   sslCb callback,
                   void *arg)
{
#define MAKE_OP allocReadSSLOp(readProc, rwFinish, socket, (void*)callback, arg, buffer, size, flags, sslOpRead, usTimeout)
  if (__tag_atomic_fetch_and_add(&socket->root.tag, 1) == 0) {
    if (!socket->root.readQueue.head) {
      size_t bytesTransferred;
      asyncOpRoot *op = implSslRead(socket, buffer, size, flags, usTimeout, callback, arg, &bytesTransferred);
      if (!op) {
        if (flags & afSerialized)
          callback(aosSuccess, socket, bytesTransferred, arg);

        tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
        if (tag)
          combinerCallWithoutLock(&socket->root, tag, 0, aaNone);

        if (!(flags & afSerialized)) {
          if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
            return (ssize_t)bytesTransferred;
          } else {
            SSLOp *op = MAKE_OP;
            op->bytesTransferred = bytesTransferred;
            opForceStatus(&op->root, aosSuccess);
            addToThreadLocalQueue(&op->root);
          }
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallWithoutLock(&socket->root, 1, op, aaStart);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
          if (tag)
            combinerCallWithoutLock(&socket->root, tag, 0, aaNone);
          addToThreadLocalQueue(op);
        }
      }
    } else {
      eqPushBack(&socket->root.readQueue, &MAKE_OP->root);
      tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallWithoutLock(&socket->root, tag, 0, aaNone);
    }
  } else {
    combinerAddAction(&socket->root, &MAKE_OP->root, aaStart);
  }

  return -(ssize_t)aosPending;
#undef MAKE_OP
}

void sslWriteWriteCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object);
  __UNUSED(transferred);
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus writeProc(asyncOpRoot *opptr)
{
  SSLOp *op = (SSLOp*)opptr;
  SSLSocket *socket = (SSLSocket*)opptr->object;

  if (op->state == sslStInitalize) {
    size_t bytes = 0;
    op->state = sslStProcessing;
    SSL_write(socket->ssl, op->buffer, (int)op->transactionSize);
    size_t writeSize = copyFromOut(socket);
    asyncOpRoot *writeOp = implWrite(socket->object, socket->sslWriteBuffer, writeSize, afWaitAll, 0, sslWriteWriteCb, op, &bytes);
    if (writeOp)
      opStart(writeOp);
    return writeOp ? aosPending : aosSuccess;
  } else {
    return aosSuccess;
  }
}

asyncOpRoot *implSslWrite(SSLSocket *socket,
                          const void *buffer,
                          size_t size,
                          AsyncFlags flags,
                          uint64_t usTimeout,
                          sslCb callback,
                          void *arg)
{
  SSL_write(socket->ssl, buffer, (int)size);
  size_t writeSize = copyFromOut(socket);
  size_t bytes = 0;
  asyncOpRoot *op = implWrite(socket->object, socket->sslWriteBuffer, writeSize, afWaitAll, 0, sslWriteWriteCb, 0, &bytes);
  if (!op) {
    return 0;
  } else {
    SSLOp *sslOp = allocWriteSSLOp(writeProc, rwFinish, socket, (void*)callback, arg, buffer, size, flags|afRunning, sslOpWrite, usTimeout);
    sslOp->state = sslStProcessing;
    op->arg = sslOp;
    opStart(op);
    return &sslOp->root;
  }

}

ssize_t aioSslWrite(SSLSocket *socket,
                   const void *buffer,
                   size_t size,
                   AsyncFlags flags,
                   uint64_t usTimeout,
                   sslCb callback,
                   void *arg)
{
#define MAKE_OP allocWriteSSLOp(writeProc, rwFinish, socket, (void*)callback, arg, buffer, size, flags, sslOpWrite, usTimeout)
  if (__tag_atomic_fetch_and_add(&socket->root.tag, 1) == 0) {
    if (!socket->root.writeQueue.head) {
      asyncOpRoot *op = implSslWrite(socket, buffer, size, flags, usTimeout, callback, arg);
      if (!op) {
        if (flags & afSerialized)
          callback(aosSuccess, socket, size, arg);

        tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
        if (tag)
          combinerCallWithoutLock(&socket->root, tag, 0, aaNone);

        if (!(flags & afSerialized)) {
          if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
            return (ssize_t)size;
          } else {
            SSLOp *op = MAKE_OP;
            op->bytesTransferred = size;
            opForceStatus(&op->root, aosSuccess);
            addToThreadLocalQueue(&op->root);
          }
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallWithoutLock(&socket->root, 1, op, aaStart);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
          if (tag)
            combinerCallWithoutLock(&socket->root, tag, 0, aaNone);
          addToThreadLocalQueue(op);
        }
      }
    } else {
      eqPushBack(&socket->root.writeQueue, &MAKE_OP->root);
      tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallWithoutLock(&socket->root, tag, 0, aaNone);
    }
  } else {
    combinerAddAction(&socket->root, &MAKE_OP->root, aaStart);
  }

  return -(ssize_t)aosPending;
#undef MAKE_OP
}

int ioSslConnect(SSLSocket *socket, const HostAddress *address, uint64_t usTimeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  SSL_set_connect_state(socket->ssl);
  SSLOp *op = allocReadSSLOp(connectProc, connectFinish, socket, (void*)coroutineConnectCb, &r, 0, 0, afNone, sslOpConnect, usTimeout);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &socket->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -(int)r.status;
}

ssize_t ioSslRead(SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
#define MAKE_OP allocReadSSLOp(readProc, rwFinish, socket, (void*)coroutineCb, &r, buffer, size, flags, sslOpRead, usTimeout)
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};

  if (__tag_atomic_fetch_and_add(&socket->root.tag, 1) == 0) {
    if (!socket->root.readQueue.head) {
      size_t bytesTransferred;
      asyncOpRoot *op = implSslRead(socket, buffer, size, flags, usTimeout, coroutineCb, &r, &bytesTransferred);
      if (!op) {
        tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
        if (tag)
          combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && !tag) {
          return (ssize_t)bytesTransferred;
        } else {
          SSLOp *op = MAKE_OP;
          op->bytesTransferred = bytesTransferred;
          opForceStatus(&op->root, aosSuccess);
          addToThreadLocalQueue(&op->root);
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallDelayed(&ccArgs, &socket->root, 1, op, aaStart, 0);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
          if (tag)
            combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
          addToThreadLocalQueue(op);
        }
      }
    } else {
      eqPushBack(&socket->root.readQueue, &MAKE_OP->root);
      tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
    }
  } else {
    combinerAddAction(&socket->root, &MAKE_OP->root, aaStart);
  }

  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.transferred : -r.status;
#undef MAKE_OP
}

ssize_t ioSslWrite(SSLSocket *socket, const void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
#define MAKE_OP allocWriteSSLOp(writeProc, rwFinish, socket, (void*)coroutineCb, &r, buffer, size, flags, sslOpWrite, usTimeout)
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  if (__tag_atomic_fetch_and_add(&socket->root.tag, 1) == 0) {
    if (!socket->root.writeQueue.head) {
      asyncOpRoot *op = implSslWrite(socket, buffer, size, flags, usTimeout, coroutineCb, &r);
      if (!op) {
        tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
        if (tag)
          combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && !tag) {
          return (ssize_t)size;
        } else {
          SSLOp *op = MAKE_OP;
          op->bytesTransferred = size;
          opForceStatus(&op->root, aosSuccess);
          addToThreadLocalQueue(&op->root);
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallDelayed(&ccArgs, &socket->root, 1, op, aaStart, 0);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
          if (tag)
            combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
          addToThreadLocalQueue(op);
        }
      }
    } else {
      eqPushBack(&socket->root.writeQueue, &MAKE_OP->root);
      tag_t tag = __tag_atomic_fetch_and_add(&socket->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallDelayed(&ccArgs, &socket->root, tag, 0, aaNone, 0);
    }
  } else {
    combinerAddAction(&socket->root, &MAKE_OP->root, aaStart);
  }

  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.transferred : -r.status;
#undef MAKE_OP
}
