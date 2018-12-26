#include "Debug.h"
#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/device.h"
#include "asyncio/socket.h"
#include "asyncioImpl.h"
#include "atomic.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *poolId = "asyncIo";
static const char *timerPoolId = "asyncIoTimer";
static const char *eventPoolId = "asyncIoEvent";

#ifdef OS_WINDOWS
asyncBase *iocpNewAsyncBase();
#endif
#ifdef OS_LINUX
asyncBase *selectNewAsyncBase(void);
asyncBase *epollNewAsyncBase(void);
#endif
#if defined(OS_DARWIN) || defined (OS_FREEBSD)
asyncBase *kqueueNewAsyncBase();
#endif

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  socketTy acceptSocket;
  size_t bytesTransferred;
} coroReturnStruct;

static void finishMethod(asyncOpRoot *root)
{
  if (!root->callback)
    return;

  asyncOp *op = (asyncOp*)root;
  AsyncOpStatus status = opGetStatus(root);
  aioObject *object = (aioObject*)root->object;
  switch (root->opCode) {
    case actConnect :
      ((aioConnectCb*)root->callback)(status, object, root->arg);
      break;
    case actAccept :
      ((aioAcceptCb*)root->callback)(status, object, op->host, op->acceptSocket, root->arg);
      break;
    case actRead :
      ((aioCb*)root->callback)(status, object, op->bytesTransferred, root->arg);
      break;
    case actWrite :
      ((aioCb*)root->callback)(status, object, op->bytesTransferred, root->arg);
      break;
    case actReadMsg :
      ((aioReadMsgCb*)root->callback)(status, object, op->host, op->bytesTransferred, root->arg);
      break;
    case actWriteMsg :
      ((aioCb*)root->callback)(status, object, op->bytesTransferred, root->arg);
      break;
  }
}

static void eventFinish(asyncOpRoot *root)
{
  ((aioEventCb*)root->callback)((aioUserEvent*)root, root->arg);
}

static asyncOp *initAsyncOp(aioExecuteProc *startProc,
                            aioObject *object,
                            void *callback,
                            void *arg,
                            AsyncFlags flags,
                            int opCode,
                            uint64_t timeout,
                            void *buffer,
                            size_t transactionSize)
{
  asyncOp *op = (asyncOp*)initAsyncOpRoot(poolId,
                                          timerPoolId,
                                          object->root.base->methodImpl.newAsyncOp,
                                          startProc,
                                          finishMethod,
                                          &object->root,
                                          callback,
                                          arg,
                                          flags,
                                          opCode,
                                          timeout);

  op->state = 0;
  op->buffer = buffer;
  op->transactionSize = transactionSize;
  op->bytesTransferred = 0;

  if ((opCode == actWrite || opCode == actWriteMsg) && !(flags & afNoCopy)) {
    if (op->internalBuffer == 0) {
      op->internalBuffer = malloc(transactionSize);
      op->internalBufferSize = transactionSize;
    } else if (op->internalBufferSize < transactionSize) {
      op->internalBufferSize = transactionSize;
      op->internalBuffer = realloc(op->internalBuffer, transactionSize);
    }

    memcpy(op->internalBuffer, buffer, transactionSize);
    op->buffer = op->internalBuffer;
  }

  return op;
}

static void coroutineEventCb(aioObject *event, void *arg)
{
  __UNUSED(event)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  assert(coroutineIsMain() && "no main coroutine!\n");
  coroutineCall(r->coroutine);
}

static void coroutineConnectCb(AsyncOpStatus status, aioObject *object, void *arg)
{
  __UNUSED(object)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  assert(coroutineIsMain() && "no main coroutine!\n");
  coroutineCall(r->coroutine);
}

static void coroutineAcceptCb(AsyncOpStatus status, aioObject *listener, HostAddress address, socketTy socket, void *arg)
{
  __UNUSED(listener)
  __UNUSED(address)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->acceptSocket = socket;
  assert(coroutineIsMain() && "no main coroutine!\n");
  coroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->bytesTransferred = transferred;
  assert(coroutineIsMain() && "no main coroutine!\n");
  coroutineCall(r->coroutine);
}

static void coroutineReadMsgCb(AsyncOpStatus status, aioObject *object, HostAddress address, size_t transferred, void *arg)
{
  __UNUSED(object)
  __UNUSED(address)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->bytesTransferred = transferred;
  assert(coroutineIsMain() && "no main coroutine!\n");
  coroutineCall(r->coroutine);
}

socketTy aioObjectSocket(aioObject *object)
{
  return object->hSocket;
}

iodevTy aioObjectDevice(aioObject *object)
{
  return object->hDevice;
}

asyncBase *createAsyncBase(AsyncMethod method)
{
  asyncBase *base = 0;
  switch (method) {
#if defined(OS_WINDOWS)
    case amIOCP :
      base = iocpNewAsyncBase();
      break;
#elif defined(OS_LINUX)
    case amSelect :
      base = selectNewAsyncBase();
      break;
    case amEPoll :
      base = epollNewAsyncBase();
      break;
#elif defined(OS_DARWIN) || defined(OS_FREEBSD)
   case amKQueue :
      base = kqueueNewAsyncBase();
     break;
#endif
    case amOSDefault :
    default:
#if defined(OS_WINDOWS)
      base = iocpNewAsyncBase();
#elif defined(OS_LINUX)
      base = epollNewAsyncBase();
#elif defined(OS_DARWIN) || defined(OS_FREEBSD)
      base = kqueueNewAsyncBase();
#else
      base = selectNewAsyncBase();
#endif
      break;
  }

#ifndef NDEBUG
  base->opsCount = 0;
#endif
  pageMapInit(&base->timerMap);
  base->timerMapLock = 0;
  base->lastCheckPoint = time(0);
  base->messageLoopThreadCounter = 0;
  return base;
}

void asyncLoop(asyncBase *base)
{
  base->methodImpl.nextFinishedOperation(base);
}


void postQuitOperation(asyncBase *base)
{
  base->methodImpl.postEmptyOperation(base);
}


aioUserEvent *newUserEvent(asyncBase *base, aioEventCb callback, void *arg)
{
  aioUserEvent *event = (aioUserEvent*)objectGet(eventPoolId);
  if (!event) {
    event = malloc(sizeof(aioUserEvent));
    event->root.poolId = eventPoolId;
    base->methodImpl.initializeTimer(base, &event->root);
  }

  event->root.opCode = actUserEvent;
  event->root.finishMethod = eventFinish;
  event->root.callback = (void*)callback;
  event->root.arg = arg;
  event->base = base;
  event->counter = 0;
  return event;
}


aioObject *newSocketIo(asyncBase *base, socketTy hSocket)
{
  return base->methodImpl.newAioObject(base, ioObjectSocket, &hSocket);
}

aioObject *newDeviceIo(asyncBase *base, iodevTy hDevice)
{
  return base->methodImpl.newAioObject(base, ioObjectDevice, &hDevice);
}

void deleteAioObject(aioObject *object)
{
  objectDelete(&object->root);
}

asyncBase *aioGetBase(aioObject *object)
{
  return object->root.base;
}

void userEventStartTimer(aioUserEvent *event, uint64_t usTimeout, int counter)
{
  event->counter = counter;
  event->timeout = usTimeout;
  event->base->methodImpl.startTimer(&event->root, usTimeout, 1);
}


void userEventStopTimer(aioUserEvent *event)
{
  event->counter = 0;
  event->base->methodImpl.stopTimer(&event->root);
}


void userEventActivate(aioUserEvent *event)
{
  event->base->methodImpl.activate(event);
}

void deleteUserEvent(aioUserEvent *event)
{
  event->base->methodImpl.stopTimer(&event->root);
  objectRelease(&event->root, event->root.poolId);
}

void aioConnect(aioObject *object,
                const HostAddress *address,
                uint64_t usTimeout,
                aioConnectCb callback,
                void *arg)
{
  asyncOp *op = initAsyncOp(object->root.base->methodImpl.connect, object, (void*)callback, arg, afNone, actConnect, usTimeout, 0, 0);
  op->host = *address;
  opStart(&op->root);
}


void aioAccept(aioObject *object,
               uint64_t usTimeout,
               aioAcceptCb callback,
               void *arg)
{
  asyncOp *op = initAsyncOp(object->root.base->methodImpl.accept, object, callback, arg, afNone, actAccept, usTimeout, 0, 0);
  opStart(&op->root);
}


ssize_t aioRead(aioObject *object,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                aioCb callback,
                void *arg)
{
  if (__tag_atomic_fetch_and_add(&object->root.tag, 1) == 0) {
    size_t bytesTransferred = 0;
    int result = object->root.type == ioObjectSocket ?
      socketSyncRead(object->hSocket, buffer, size, flags & afWaitAll, &bytesTransferred) :
      deviceSyncRead(object->hDevice, buffer, size, flags & afWaitAll, &bytesTransferred);
    if (result) {
      if (flags & afSerialized) {
        callback(aosSuccess, object, bytesTransferred, arg);
      }

      tag_t tag = __tag_atomic_fetch_and_add(&object->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        object->root.base->methodImpl.combiner(&object->root, tag, 0, aaNone);
      if (!(flags & afSerialized)) {
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
          return (ssize_t)bytesTransferred;
        } else {
          asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)callback, arg, flags, actRead, usTimeout, buffer, size);
          op->bytesTransferred = bytesTransferred;
          opForceStatus(&op->root, aosSuccess);
          addToThreadLocalQueue(&op->root);
        }
      }
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)callback, arg, flags, actRead, usTimeout, buffer, size);
      op->bytesTransferred = bytesTransferred;
      object->root.base->methodImpl.combiner(&object->root, 1, &op->root, aaStart);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)callback, arg, flags, actRead, usTimeout, buffer, size);
    combinerAddAction(&object->root, &op->root, aaStart);
  }

  return -(ssize_t)aosPending;
}



ssize_t aioWrite(aioObject *object,
                 const void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 aioCb callback,
                 void *arg)
{
  if (__tag_atomic_fetch_and_add(&object->root.tag, 1) == 0) {
    size_t bytesTransferred = 0;
    int result = object->root.type == ioObjectSocket ?
      socketSyncWrite(object->hSocket, buffer, size, flags & afWaitAll, &bytesTransferred) :
      deviceSyncWrite(object->hDevice, buffer, size, flags & afWaitAll, &bytesTransferred);
    if (result) {
      if (flags & afSerialized)
        callback(aosSuccess, object, bytesTransferred, arg);

      tag_t tag = __tag_atomic_fetch_and_add(&object->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        object->root.base->methodImpl.combiner(&object->root, tag, 0, aaNone);

      if (!(flags & afSerialized)) {
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
          return (ssize_t)bytesTransferred;
        } else {
          asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)callback, arg, flags, actWrite, usTimeout, (void*)buffer, size);
          op->bytesTransferred = bytesTransferred;
          opForceStatus(&op->root, aosSuccess);
          addToThreadLocalQueue(&op->root);
        }
      }
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)callback, arg, flags, actWrite, usTimeout, (void*)buffer, size);
      op->bytesTransferred = bytesTransferred;
      object->root.base->methodImpl.combiner(&object->root, 1, &op->root, aaStart);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)callback, arg, flags, actWrite, usTimeout, (void*)buffer, size);
    combinerAddAction(&object->root, &op->root, aaStart);
  }

  return -(ssize_t)aosPending;
}

ssize_t aioReadMsg(aioObject *object,
                   void *buffer,
                   size_t size,
                   AsyncFlags flags,
                   uint64_t usTimeout,
                   aioReadMsgCb callback,
                   void *arg)
{
  struct sockaddr_in source;
  socketLenTy addrlen = sizeof(source);
#ifdef OS_WINDOWS
  ssize_t result = recvfrom(object->hSocket, buffer, (int)size, 0, (struct sockaddr*)&source, &addrlen);
#else
  ssize_t result = recvfrom(object->hSocket, buffer, size, 0, (struct sockaddr*)&source, &addrlen);
#endif
  if (result >= 0) {
    // Data received synchronously
    HostAddress host;
    host.family = 0;
    host.ipv4 = source.sin_addr.s_addr;
    host.port = source.sin_port;
    currentFinishedSync++;
    if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
      return result;
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.readMsg, object, (void*)callback, arg, flags, actReadMsg, usTimeout, buffer, size);
      op->bytesTransferred = (size_t)result;
      op->host = host;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.readMsg, object, (void*)callback, arg, flags, actReadMsg, usTimeout, buffer, size);
    opStart(&op->root);
  }

  return -(ssize_t)aosPending;
}



ssize_t aioWriteMsg(aioObject *object,
                    const HostAddress *address,
                    const void *buffer,
                    size_t size,
                    AsyncFlags flags,
                    uint64_t usTimeout,
                    aioCb callback,
                    void *arg)
{
  // Datagram socket can be accessed by multiple threads without lock
  struct sockaddr_in remoteAddress;
  remoteAddress.sin_family = address->family;
  remoteAddress.sin_addr.s_addr = address->ipv4;
  remoteAddress.sin_port = address->port;
#ifdef OS_WINDOWS
  ssize_t result = sendto(object->hSocket, buffer, (int)size, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
#else
  ssize_t result = sendto(object->hSocket, buffer, size, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
#endif
  if (result >= 0) {
    currentFinishedSync++;
    if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == 0 || flags & afActiveOnce)) {
      return result;
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.writeMsg, object, (void*)callback, arg, flags, actWriteMsg, usTimeout, (void*)buffer, size);
      op->bytesTransferred = (size_t)result;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.writeMsg, object, (void*)callback, arg, flags, actWriteMsg, usTimeout, (void*)buffer, size);
    op->host = *address;
    opStart(&op->root);
  }

  return -(ssize_t)aosPending;
}


int ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r;
  r.coroutine = coroutineCurrent();
  aioConnect(op, address, usTimeout, coroutineConnectCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -(int)r.status;
}


socketTy ioAccept(aioObject *op, uint64_t usTimeout)
{
  coroReturnStruct r;
  r.coroutine = coroutineCurrent();
  aioAccept(op, usTimeout, coroutineAcceptCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.acceptSocket : -(int)r.status;
}


ssize_t ioRead(aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent(), aosPending, INVALID_SOCKET, 0};
  combinerCallArgs ccArgs;
  if (__tag_atomic_fetch_and_add(&object->root.tag, 1) == 0) {
    size_t bytesTransferred = 0;
    int result = object->root.type == ioObjectSocket ?
      socketSyncRead(object->hSocket, buffer, size, flags & afWaitAll, &bytesTransferred) :
      deviceSyncRead(object->hDevice, buffer, size, flags & afWaitAll, &bytesTransferred);
    if (result) {
      tag_t tag = __tag_atomic_fetch_and_add(&object->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallDelayed(&ccArgs, &object->root, tag, 0, aaNone, 0);

      if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION) {
        return (ssize_t)bytesTransferred;
      } else {
        asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)coroutineCb, &r, flags, actRead, usTimeout, buffer, size);
        op->bytesTransferred = bytesTransferred;
        opForceStatus(&op->root, aosSuccess);
        addToThreadLocalQueue(&op->root);
      }
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)coroutineCb, &r, flags, actRead, usTimeout, buffer, size);
      op->bytesTransferred = bytesTransferred;
      combinerCallDelayed(&ccArgs, &object->root, 1, &op->root, aaStart, 0);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.read, object, (void*)coroutineCb, &r, flags, actRead, usTimeout, buffer, size);
    combinerAddAction(&object->root, &op->root, aaStart);
  }

  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
}


ssize_t ioWrite(aioObject *object, const void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent(), aosPending, INVALID_SOCKET, 0};
  combinerCallArgs ccArgs;

  if (__tag_atomic_fetch_and_add(&object->root.tag, 1) == 0) {
    size_t bytesTransferred = 0;
    int result = object->root.type == ioObjectSocket ?
      socketSyncWrite(object->hSocket, buffer, size, flags & afWaitAll, &bytesTransferred) :
      deviceSyncWrite(object->hDevice, buffer, size, flags & afWaitAll, &bytesTransferred);
    if (result) {
      tag_t tag = __tag_atomic_fetch_and_add(&object->root.tag, (tag_t)0 - 1) - 1;
      if (tag)
        combinerCallDelayed(&ccArgs, &object->root, tag, 0, aaNone, 0);

      if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION) {
        return (ssize_t)bytesTransferred;
      } else {
        asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)coroutineCb, &r, flags, actWrite, usTimeout, buffer, size);
        op->bytesTransferred = bytesTransferred;
        opForceStatus(&op->root, aosSuccess);
        addToThreadLocalQueue(&op->root);
      }
    } else {
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)coroutineCb, &r, flags, actWrite, usTimeout, buffer, size);
      op->bytesTransferred = bytesTransferred;
      combinerCallDelayed(&ccArgs, &object->root, 1, &op->root, aaStart, 0);
    }
  } else {
    asyncOp *op = initAsyncOp(object->root.base->methodImpl.write, object, (void*)coroutineCb, &r, flags, actWrite, usTimeout, buffer, size);
    combinerAddAction(&object->root, &op->root, aaStart);
  }

  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
}

ssize_t ioReadMsg(aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  // Datagram socket can be accessed by multiple threads without lock
  struct sockaddr_in source;
  socketLenTy addrlen = sizeof(source);
#ifdef OS_WINDOWS
  ssize_t result = recvfrom(object->hSocket, buffer, (int)size, 0, (struct sockaddr*)&source, &addrlen);
#else
  ssize_t result = recvfrom(object->hSocket, buffer, size, 0, (struct sockaddr*)&source, &addrlen);
#endif
  if (result >= 0) {
    // Data received synchronously
    if (++currentFinishedSync >= MAX_SYNCHRONOUS_FINISHED_OPERATION) {
      coroReturnStruct r = { coroutineCurrent(), aosPending, INVALID_SOCKET, 0 };
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.readMsg, object, (void*)coroutineReadMsgCb, &r, flags, actReadMsg, usTimeout, buffer, size);
      op->bytesTransferred = (size_t)result;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
      coroutineYield();
      return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
    } else {
      return result;
    }
  }

  coroReturnStruct r = {coroutineCurrent(), aosPending, INVALID_SOCKET, 0};
  combinerCallArgs ccArgs;
  asyncOp *op = initAsyncOp(object->root.base->methodImpl.readMsg, object, (void*)coroutineReadMsgCb, &r, flags, actReadMsg, usTimeout, buffer, size);
  combinerCallDelayed(&ccArgs, &object->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
}

ssize_t ioWriteMsg(aioObject *object, const HostAddress *address, const void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  // Datagram socket can be accessed by multiple threads without lock
  struct sockaddr_in remoteAddress;
  remoteAddress.sin_family = address->family;
  remoteAddress.sin_addr.s_addr = address->ipv4;
  remoteAddress.sin_port = address->port;
#ifdef OS_WINDOWS
  ssize_t result = sendto(object->hSocket, buffer, (int)size, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
#else
  ssize_t result = sendto(object->hSocket, buffer, size, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
#endif
  if (result != -1) {
    // Data received synchronously
    if (++currentFinishedSync >= MAX_SYNCHRONOUS_FINISHED_OPERATION) {
      coroReturnStruct r;
      r.coroutine = coroutineCurrent();
      asyncOp *op = initAsyncOp(object->root.base->methodImpl.writeMsg, object, (void*)coroutineCb, &r, flags, actWriteMsg, usTimeout, (void*)buffer, size);
      op->host = *address;
      addToThreadLocalQueue(&op->root);
      coroutineYield();
      return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
    } else {
      return result;
    }
  }

  coroReturnStruct r = {coroutineCurrent(), aosPending, INVALID_SOCKET, 0};
  combinerCallArgs ccArgs;
  asyncOp *op = initAsyncOp(object->root.base->methodImpl.writeMsg, object, (void*)coroutineCb, &r, flags, actWriteMsg, usTimeout, (void*)buffer, size);
  op->host = *address;
  combinerCallDelayed(&ccArgs, &object->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? (ssize_t)r.bytesTransferred : -(int)r.status;
}


void ioSleep(aioUserEvent *event, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent(), aosPending, INVALID_SOCKET, 0};
  event->root.callback = (void*)coroutineEventCb;
  event->root.arg = &r;
  event->counter = 1;
  event->base->methodImpl.startTimer(&event->root, usTimeout, 0);
  coroutineYield();
}
