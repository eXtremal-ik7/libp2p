#include "Debug.h"
#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncio/device.h"
#include "asyncio/socket.h"
#include "asyncioImpl.h"
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
asyncBase *selectNewAsyncBase();
asyncBase *epollNewAsyncBase();
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


static asyncOpRoot *userEventAlloc(asyncBase *base)
{
  return (asyncOpRoot*)malloc(sizeof(aioUserEvent));
}

static void finishMethod(asyncOpRoot *root)
{
  if (!root->callback)
    return;

  asyncOp *op = (asyncOp*)root;
  AsyncOpStatus status = opGetStatus(root);
  aioObject *object = (aioObject*)root->object;
  switch (root->opCode) {
    case actConnect :
      ((aioConnectCb*)root->callback)(status, root->base, object, root->arg);
      break;
    case actAccept :
      ((aioAcceptCb*)root->callback)(status, root->base, object, op->host, op->acceptSocket, root->arg);
      break;
    case actRead :
      ((aioCb*)root->callback)(status, root->base, object, op->bytesTransferred, root->arg);
      break;      
    case actWrite :
      ((aioCb*)root->callback)(status, root->base, object, op->bytesTransferred, root->arg);
      break;      
    case actReadMsg :
      ((aioReadMsgCb*)root->callback)(status, root->base, object, op->host, op->bytesTransferred, root->arg);
      break;      
    case actWriteMsg :
      ((aioCb*)root->callback)(status, root->base, object, op->bytesTransferred, root->arg);
      break;
  }
}

static void eventFinish(asyncOpRoot *root)
{
  ((aioEventCb*)root->callback)(root->base, (aioUserEvent*)root, root->arg);
}

static asyncOp *initAsyncOp(asyncBase *base,
                            aioObject *object,
                            void *callback,
                            void *arg,
                            void *buffer,
                            size_t transactionSize,
                            AsyncFlags flags,
                            uint64_t timeout,
                            int opCode,
                            aioExecuteProc *startProc)
{
  asyncOp *op = (asyncOp*)initAsyncOpRoot(base,
                                          poolId, timerPoolId,
                                          base->methodImpl.newAsyncOp,
                                          startProc, finishMethod,
                                          &object->root, callback, arg,
                                          flags, opCode, timeout);
  
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

static void coroutineEventCb(asyncBase *base, aioObject *event, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  assert(coroutineIsMain() && "no main coroutine!\n");
  ioCoroutineCall(r->coroutine);
}

static void coroutineConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  assert(coroutineIsMain() && "no main coroutine!\n");
  ioCoroutineCall(r->coroutine);
}

static void coroutineAcceptCb(AsyncOpStatus status, asyncBase *base, aioObject *listener, HostAddress address, socketTy socket, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->acceptSocket = socket;
  assert(coroutineIsMain() && "no main coroutine!\n");
  ioCoroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  r->bytesTransferred = transferred;
  if (r->coroutine != coroutineCurrent())
  ioCoroutineCall(r->coroutine);
}

static void coroutineReadMsgCb(AsyncOpStatus status, asyncBase *base, aioObject *object, HostAddress address, size_t transferred, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  r->bytesTransferred = transferred;
  assert(coroutineIsMain() && "no main coroutine!\n");
  ioCoroutineCall(r->coroutine);
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
  
//  initObjectPool(&base->pool);
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
  aioUserEvent *event =
    (aioUserEvent*)initAsyncOpRoot(base,
                                   eventPoolId, eventPoolId,
                                   userEventAlloc,
                                   0, eventFinish, 0, callback, arg,
                                   0, actUserEvent, 0);
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
  cancelIo(&object->root, 0);
  objectDeleteRef(&object->root, 1);
}

void userEventStartTimer(aioUserEvent *event, uint64_t usTimeout, int counter)
{
  event->counter = counter;
  event->timeout = usTimeout;
  event->root.base->methodImpl.startTimer(&event->root, usTimeout, 1);
}


void userEventStopTimer(aioUserEvent *event)
{   
  event->counter = 0;  
  event->root.base->methodImpl.stopTimer(&event->root);
}


void userEventActivate(aioUserEvent *event)
{  
  event->root.base->methodImpl.activate(&event->root);
}

void deleteUserEvent(aioUserEvent *event)
{
  asyncOpRoot *root = &event->root;
  root->base->methodImpl.stopTimer(root);
  objectRelease(root, root->poolId);
}

void *queryObject(asyncBase *base, const void *id)
{
  void *op = objectGet(id);
  if (!op) {
#ifndef NDEBUG
    base->opsCount++;
    dbgPrint(" * asyncio: %i's new object (%s)\n", base->opsCount, id);
#endif
  }

  return op;
}

void releaseObject(asyncBase *base, void *object, const void *type)
{
  objectRelease(object, type);
}


void aioConnect(asyncBase *base,
                aioObject *object,
                const HostAddress *address,
                uint64_t usTimeout,
                aioConnectCb callback,
                void *arg)
{
  objectAddRef(&object->root);
  asyncOp *op = initAsyncOp(base, object, callback, arg, 0, 0, afNone, usTimeout, actConnect, base->methodImpl.connect);
  op->host = *address;
  opStart(&op->root);
  //addToExecuteQueue2(&object->root, &op->root);

  //asyncOp *newOp = initAsyncOp(base, op, callback, arg, 0, 0, afNone, usTimeout, actConnect, base->methodImpl.connect);
  //if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 1))
  //  ((asyncOpRoot*)newOp)->base->methodImpl.connect(newOp, address);
}


void aioAccept(asyncBase *base,
               aioObject *object,
               uint64_t usTimeout,
               aioAcceptCb callback,
               void *arg)
{
  objectAddRef(&object->root);
  asyncOp *op = initAsyncOp(base, object, callback, arg, 0, 0, afNone, usTimeout, actAccept, base->methodImpl.accept);
  opStart(&op->root);
  //addToExecuteQueue2(&object->root, &op->root);

  //asyncOp *newOp = initAsyncOp(base, op, callback, arg, 0, 0, afNone, usTimeout, actAccept, base->methodImpl.accept);
  //if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 0))
  //  ((asyncOpRoot*)newOp)->base->methodImpl.accept(newOp);
}


void aioRead(asyncBase *base,
             aioObject *object,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             aioCb callback,
             void *arg)
{
  objectAddRef(&object->root);
  asyncOp *op = initAsyncOp(base, object, callback, arg, buffer, size, flags, usTimeout, actRead, base->methodImpl.read);
  opStart(&op->root);
  //asyncOp *newOp = initAsyncOp(base, op, callback, arg, buffer, size, flags, usTimeout, actRead, base->methodImpl.read);
  //if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 0))
  //  startMethod(&newOp->root);  
}


void aioWrite(asyncBase *base,
              aioObject *object,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              aioCb callback,
              void *arg)
{
  objectAddRef(&object->root);
  asyncOp *op = initAsyncOp(base, object, callback, arg, buffer, size, flags, usTimeout, actWrite, base->methodImpl.write);
  opStart(&op->root);
  //if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 1))
  //  startMethod(&newOp->root);  
}

void aioReadMsg(asyncBase *base,
                aioObject *object,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                aioReadMsgCb callback,
                void *arg)
{
  objectAddRef(&object->root);
  struct sockaddr_in source;
  socketLenTy addrlen = sizeof(source);
#ifdef OS_WINDOWS
  ssize_t result = recvfrom(object->hSocket, buffer, (int)size, 0, (struct sockaddr*)&source, &addrlen);
#else
  ssize_t result = recvfrom(object->hSocket, buffer, size, 0, (struct sockaddr*)&source, &addrlen);
#endif
  if (result != -1) {
    // Data received synchronously
    HostAddress host;
    host.family = 0;
    host.ipv4 = source.sin_addr.s_addr;
    host.port = source.sin_port;
    currentFinishedSync++;
    if (!callback || ((currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION) && (flags & afActiveOnce))) {
      objectDeleteRef(&object->root, 1);
      return;
    } else {
      asyncOp *op = initAsyncOp(base, object, (void*)callback, arg, buffer, size, 0, 0, actReadMsg, base->methodImpl.readMsg);
      op->bytesTransferred = result;
      op->host = host;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
    }
  } else {
    asyncOp *op = initAsyncOp(base, object, (void*)callback, arg, buffer, size, flags, usTimeout, actReadMsg, base->methodImpl.readMsg);
    opStart(&op->root);
  }
}



void aioWriteMsg(asyncBase *base,
                 aioObject *object,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 aioCb callback,
                 void *arg)
{
  objectAddRef(&object->root);

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
    currentFinishedSync++;
    if (!callback || ((currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION) && (flags & afActiveOnce))) {
      objectDeleteRef(&object->root, 1);
      return;
    } else {
      asyncOp *op = initAsyncOp(base, object, (void*)callback, arg, buffer, size, 0, 0, actWriteMsg, base->methodImpl.writeMsg);
      op->bytesTransferred = result;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
    }
  } else {
    asyncOp *op = initAsyncOp(base, object, (void*)callback, arg, buffer, size, flags, usTimeout, actWriteMsg, base->methodImpl.writeMsg);
    op->host = *address;
    opStart(&op->root);
  }
}


int ioConnect(asyncBase *base, aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioConnect(base, op, address, usTimeout, coroutineConnectCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -1;
}


socketTy ioAccept(asyncBase *base, aioObject *op, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioAccept(base, op, usTimeout, coroutineAcceptCb, &r); 
  coroutineYield();  
  return r.status == aosSuccess ? r.acceptSocket : -1;
}


ssize_t ioRead(asyncBase *base, aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioRead(base, op, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;  
}


ssize_t ioWrite(asyncBase *base, aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioWrite(base, op, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}

ssize_t ioReadMsg(asyncBase *base, aioObject *object, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  objectAddRef(&object->root);

  // Datagram socket can be accessed by multiple threads without lock
  struct sockaddr_in source;
  socketLenTy addrlen = sizeof(source);
#ifdef OS_WINDOWS
  ssize_t result = recvfrom(object->hSocket, buffer, (int)size, 0, (struct sockaddr*)&source, &addrlen);
#else
  ssize_t result = recvfrom(object->hSocket, buffer, size, 0, (struct sockaddr*)&source, &addrlen);
#endif
  if (result != -1) {
    // Data received synchronously
    if (++currentFinishedSync >= MAX_SYNCHRONOUS_FINISHED_OPERATION) {
      coroReturnStruct r = {coroutineCurrent()};
      asyncOp *op = initAsyncOp(base, object, coroutineReadMsgCb, &r, buffer, size, 0, 0, actReadMsg, base->methodImpl.readMsg);
      op->bytesTransferred = result;
      opForceStatus(&op->root, aosSuccess);
      addToThreadLocalQueue(&op->root);
      coroutineYield();
      return r.status == aosSuccess ? r.bytesTransferred : -1;
    } else {
      objectDeleteRef(&object->root, 1);
      return result;
    }
  }

  coroReturnStruct r = {coroutineCurrent()};
  lastOp = (asyncOpRoot*)initAsyncOp(base, object, coroutineReadMsgCb, &r, buffer, size, flags, usTimeout, actReadMsg, base->methodImpl.readMsg);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}

ssize_t ioWriteMsg(asyncBase *base, aioObject *object, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  objectAddRef(&object->root);

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
      coroReturnStruct r = {coroutineCurrent()};
      asyncOp *newOp = initAsyncOp(base, object, coroutineCb, &r, buffer, size, 0, 0, actWriteMsg, base->methodImpl.writeMsg);
      newOp->host = *address;
      addToThreadLocalQueue(&newOp->root);
      coroutineYield();
      return r.status == aosSuccess ? r.bytesTransferred : -1;
    } else {
      objectDeleteRef(&object->root, 1);
      return result;
    }
  }

  coroReturnStruct r = {coroutineCurrent()};
  asyncOp *op = initAsyncOp(base, object, coroutineCb, &r, buffer, size, flags, usTimeout, actWriteMsg, base->methodImpl.writeMsg);
  op->host = *address;
  lastOp = &op->root;
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}


void ioSleep(aioUserEvent *event, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  event->root.callback = coroutineEventCb;
  event->root.arg = &r;
  event->counter = 1;
  event->root.base->methodImpl.startTimer(&event->root, usTimeout, 0);
  coroutineYield();
}
