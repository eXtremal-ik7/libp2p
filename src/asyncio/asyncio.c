#include "Debug.h"
#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncioInternal.h"
#include <stdlib.h>
#include <time.h>

const char *poolId = "asyncIo";
const char *timerPoolId = "asyncIoTimer";

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

void userEventTrigger(aioObject *event)
{
  // timer must be already stopped if need
  asyncOpRoot *root = event->root.readQueue.head;
  if (root->callback)
    ((aioEventCb*)root->callback)(root->base, event, root->arg);
}

static void startMethod(asyncOpRoot *root)
{
  asyncOp *op = (asyncOp*)root;
  switch (root->opCode) {
    case actConnect :
    case actAccept :
      // Connect and accept operations can't be queued
      // TODO: return error
      break; 
    case actRead :
      root->base->methodImpl.read(op, 0);
      break;      
    case actWrite :
      root->base->methodImpl.write(op, 0);      
      break;      
    case actReadMsg :
      root->base->methodImpl.readMsg(op, 0);      
      break;      
    case actWriteMsg :
      root->base->methodImpl.writeMsg(op, &op->host, 0);
      break;
  }
}


static void finishMethod(asyncOpRoot *root, int status)
{
  asyncOp *op = (asyncOp*)root;
  
  // TODO: timer must be independent from OS I/O method used
  if (root->poolId == timerPoolId)
    root->base->methodImpl.stopTimer(op);
    
  // Do callback if need
  if (root->callback) {
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
        ((aioCb*)root->callback)(status, root->base, object, op->bytesTransferred, root->arg);          
        break;      
      case actWriteMsg :
        ((aioCb*)root->callback)(status, root->base, object, op->bytesTransferred, root->arg);          
        break;
    }
  }
}


static asyncOp *initAsyncOp(aioObject *object,
                            void *callback,
                            void *arg,
                            void *buffer,
                            dynamicBuffer *dynamicArray,
                            size_t transactionSize,
                            AsyncFlags flags,
                            uint64_t timeout,
                            int opCode)
{
  int needTimer = (object->type == ioObjectUserEvent) || (flags & afRealtime);
  const char *pool = needTimer ? timerPoolId : poolId;
  
  asyncOp *newOp = queryObject(object->base, pool);
  if (!newOp) {
    newOp = object->base->methodImpl.newAsyncOp(object->base, needTimer);
    newOp->root.base = object->base;
    newOp->root.poolId = pool;
    newOp->root.startMethod = startMethod;
    newOp->root.finishMethod = finishMethod;
  }
  
  newOp->root.executeQueue.prev = 0;
  newOp->root.executeQueue.next = 0;  
  newOp->root.timeoutQueue.prev = 0;
  newOp->root.timeoutQueue.next = 0;  
  newOp->root.object = &object->root;
  newOp->root.flags = flags;
  newOp->root.opCode = opCode;
  newOp->root.endTime = 0;
  newOp->root.callback = callback;
  newOp->root.arg = arg;

  if (buffer)
    newOp->buffer = buffer;
  else if (dynamicArray)
    newOp->dynamicArray = dynamicArray;
  newOp->transactionSize = transactionSize;
  newOp->bytesTransferred = 0;
  if (timeout) {
    if (needTimer) {
      // start timer for this operation
      newOp->root.base->methodImpl.startTimer(newOp, timeout, 1);
    } else {
      // add operation to timeout grid
      newOp->root.endTime = ((uint64_t)time(0))*1000000ULL + timeout;
      addToTimeoutQueue(object->base, &newOp->root);
    }
  }
  
  return newOp;
}

static void coroutineEventCb(asyncBase *base, aioObject *event, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  coroutineCall(r->coroutine);
}

static void coroutineConnectCb(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineAcceptCb(AsyncOpStatus status, asyncBase *base, aioObject *listener, HostAddress address, socketTy socket, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->acceptSocket = socket;
  coroutineCall(r->coroutine);
}

static void coroutineCb(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;  
  r->bytesTransferred = transferred;
  coroutineCall(r->coroutine);
}


intptr_t argAsInteger(void *arg)
{
  return (intptr_t)arg;
}


void *intArg(intptr_t id)
{
  return (void*)id;
}

asyncBase *aioObjectBase(aioObject *object)
{
  return object->base;
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
  
  initObjectPool(&base->pool);
#ifndef NDEBUG
  base->opsCount = 0;
#endif
  opRingInit(&base->timeGrid, 1024, time(0));
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


aioObject *newUserEvent(asyncBase *base, aioEventCb callback, void *arg)
{
  aioObject *object = malloc(sizeof(aioObject));
  object->base = base;
  object->type = ioObjectUserEvent;  
  object->root.readQueue.head =
    (asyncOpRoot*)initAsyncOp(object, callback, arg, 0, 0, 0, 0, 0, actNoAction);
  return object;
}


aioObject *newSocketIo(asyncBase *base, socketTy hSocket)
{
  return base->methodImpl.newAioObject(base, ioObjectSocket, &hSocket);
}

aioObject *newSocketSynIo(asyncBase *base, socketTy hSocket)
{
  return base->methodImpl.newAioObject(base, ioObjectSocketSyn, &hSocket);
}


aioObject *newDeviceIo(asyncBase *base, iodevTy hDevice)
{
  return base->methodImpl.newAioObject(base, ioObjectDevice, &hDevice);  
}

void deleteAioObject(aioObject *object)
{
  if (object->type == ioObjectUserEvent) {
    asyncOpRoot *op = object->root.readQueue.head;
    objectRelease(&op->base->pool, op, op->poolId);
  }
    
  object->base->methodImpl.deleteObject(object);
}

void userEventStartTimer(aioObject *event, uint64_t usTimeout, int counter)
{
  asyncOpRoot *op = event->root.readQueue.head;  
  op->base->methodImpl.startTimer((asyncOp*)op, usTimeout, counter);
}


void userEventStopTimer(aioObject *event)
{
  asyncOpRoot *op = event->root.readQueue.head;    
  op->base->methodImpl.stopTimer((asyncOp*)op);
}


void userEventActivate(aioObject *event)
{
  asyncOpRoot *op = event->root.readQueue.head;    
  op->base->methodImpl.activate((asyncOp*)op);
}


void *queryObject(asyncBase *base, const void *id)
{
  void *op = objectGet(&base->pool, id);
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
  objectRelease(&base->pool, object, type);
}


void aioConnect(aioObject *op,
                const HostAddress *address,
                uint64_t usTimeout,
                aioConnectCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, 0, 0, afNone, usTimeout, actConnect);
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 1))
    ((asyncOpRoot*)newOp)->base->methodImpl.connect(newOp, address, 0);
}


void aioAccept(aioObject *op,
               uint64_t usTimeout,
               aioAcceptCb callback,
               void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, 0, 0, afNone, usTimeout, actAccept);
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 0))
    ((asyncOpRoot*)newOp)->base->methodImpl.accept(newOp, 0);
}


void aioRead(aioObject *op,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             aioCb callback,
             void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags, usTimeout, actRead);
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 0))
    startMethod(&newOp->root);  
}


void aioWrite(aioObject *op,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              aioCb callback,
              void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags, usTimeout, actWrite);  
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 1))
    startMethod(&newOp->root);  
}


void aioReadMsg(aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                aioCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, buffer, 0, afNone, usTimeout, actReadMsg);
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 0))
    startMethod(&newOp->root);  
}



void aioWriteMsg(aioObject *op,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 aioCb callback,
                 void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags, usTimeout, actWriteMsg);  
  newOp->host = *address;
  if (addToExecuteQueue((aioObjectRoot*)op, (asyncOpRoot*)newOp, 1))
    startMethod(&newOp->root);  
}


int ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioConnect(op, address, usTimeout, coroutineConnectCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -1;
}


socketTy ioAccept(aioObject *op, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioAccept(op, usTimeout, coroutineAcceptCb, &r); 
  coroutineYield();  
  return r.status == aosSuccess ? r.acceptSocket : -1;
}


ssize_t ioRead(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioRead(op, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;  
}


ssize_t ioWrite(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioWrite(op, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}


ssize_t ioReadMsg(aioObject *op, dynamicBuffer *buffer,uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioReadMsg(op, buffer, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}



ssize_t ioWriteMsg(aioObject *op, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  aioWriteMsg(op, address, buffer, size, flags, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? r.bytesTransferred : -1;
}


void ioSleep(aioObject *event, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};  
  asyncOp *op = (asyncOp*)event->root.readQueue.head;
  event->root.readQueue.head->callback = coroutineEventCb;
  event->root.readQueue.head->arg = &r;
  event->root.readQueue.head->base->methodImpl.startTimer(op, usTimeout, 1);
  coroutineYield();
}

// asyncOp *asyncMonitor(aioObject *op, asyncCb callback, void *arg)
// {
//   asyncOp *newOp = queryObject(op->base, poolId);
//   aioInfo *info = (aioInfo*)newOp;
//   
//   info->object = op;
//   info->callback = callback;
//   info->arg = arg;
//   info->dynamicArray = 0;
//   info->bytesTransferred = 0;
//   op->base->methodImpl.monitor(newOp);
// 
//   return newOp;
// }
// 
// void asyncMonitorStop(asyncOp *op)
// {
//   aioInfo *info = (aioInfo*)op;
//   if (info->status == aosMonitoring)
//     info->object->base->methodImpl.montitorStop(op);
// }
