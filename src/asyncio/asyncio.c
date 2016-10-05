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
  aioOpRoot *root = event->root.readQueue.head;
  if (root->callback) {
    // Extract all needed data
    aioInfo localInfoCopy = *(aioInfo*)root;
    ((asyncCb*)root->callback)(&localInfoCopy);
  }
}

static int startMethod(asyncOp *op)
{
  switch (((aioOpRoot*)op)->opCode) {
    case actConnect :
    case actAccept :
      // Connect and accept operations can't be queued
      // TODO: return error
      return 1;      
    case actRead :
      ((aioOpRoot*)op)->base->methodImpl.read(op, 0);
      break;      
    case actWrite :
      ((aioOpRoot*)op)->base->methodImpl.write(op, 0);      
      break;      
    case actReadMsg :
      ((aioOpRoot*)op)->base->methodImpl.readMsg(op, 0);      
      break;      
    case actWriteMsg :
      ((aioOpRoot*)op)->base->methodImpl.writeMsg(op, &((aioInfo*)op)->host, 0);
      break;
  }
  
  return 0;
}


static void finishMethod(asyncOp *op, int status)
{
  aioOpRoot *root = (aioOpRoot*)op;  

  // TODO: timer must be independent from OS I/O method used
  if (root->poolId == timerPoolId)
    root->base->methodImpl.stopTimer(op);
    
  // Extract all needed data
  aioInfo localInfoCopy = *(aioInfo*)op;
  localInfoCopy.status = status;
    
  // Do callback if need
  if (root->callback) {
    switch (root->opCode) {
      case actConnect :
        ((asyncCb*)root->callback)(&localInfoCopy);
        break;
      case actAccept :
        ((asyncCb*)root->callback)(&localInfoCopy);          
        break;
      case actRead :
        ((asyncCb*)root->callback)(&localInfoCopy);          
        break;      
      case actWrite :
        ((asyncCb*)root->callback)(&localInfoCopy);          
        break;      
      case actReadMsg :
        ((asyncCb*)root->callback)(&localInfoCopy);          
        break;      
      case actWriteMsg :
        ((asyncCb*)root->callback)(&localInfoCopy);          
        break;
    }
  }
}


static asyncOp *initAsyncOp(aioObject *object,
                            int needYield,    
                            asyncCb callback,
                            void *arg,
                            void *buffer,
                            dynamicBuffer *dynamicArray,
                            size_t transactionSize,
                            AsyncFlags flags,
                            uint64_t timeout,
                            int opCode)
{
  aioInfo *info;
  
  int needTimer = (object->type == ioObjectUserEvent) || (flags & afRealtime);
  const char *pool = needTimer ? timerPoolId : poolId;
  
  asyncOp *newOp = queryObject(object->base, pool);
  if (!newOp) {
    newOp = object->base->methodImpl.newAsyncOp(object->base, needTimer);
    info = (aioInfo*)newOp;
    info->root.base = object->base;
    info->root.poolId = pool;
    info->root.startMethod = startMethod;
    info->root.finishMethod = finishMethod;
  } else {
    info = (aioInfo*)newOp;
  }
  
  info->root.executeQueue.prev = 0;
  info->root.executeQueue.next = 0;  
  info->root.timeoutQueue.prev = 0;
  info->root.timeoutQueue.next = 0;  
  info->root.object = object;
  info->root.flags = flags;
  info->root.opCode = opCode;
  info->object = object;
  info->root.endTime = 0;
  info->callback = callback;
  info->arg = arg;
  info->root.callback = callback;
  info->root.arg = arg;

  if (buffer)
    info->buffer = buffer;
  else if (dynamicArray)
    info->dynamicArray = dynamicArray;
  info->transactionSize = transactionSize;
  info->flags = flags;
  info->bytesTransferred = 0;
  if (timeout) {
    if (needTimer) {
      // start timer for this operation
      info->root.base->methodImpl.startTimer(newOp, timeout, 1);
    } else {
      // add operation to timeout grid
      info->root.endTime = ((uint64_t)time(0))*1000000ULL + timeout;
      addToTimeoutQueue(object->base, &info->root);
    }
  }
  
  return newOp;
}

static void coroutineCb(aioInfo *info)
{
  coroReturnStruct *r = (coroReturnStruct*)info->arg;
  r->status = info->status;
  r->acceptSocket = info->acceptSocket;
  r->bytesTransferred = info->bytesTransferred;
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


aioObject *newUserEvent(asyncBase *base, asyncCb callback, void *arg)
{
  aioObject *object = malloc(sizeof(aioObject));
  object->base = base;
  object->type = ioObjectUserEvent;  
  object->root.readQueue.head = initAsyncOp(object, 0, callback, arg, 0, 0, 0, 0, 0, actNoAction);
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
    aioOpRoot *op = object->root.readQueue.head;
    objectRelease(&op->base->pool, op, op->poolId);
  }
    
  object->base->methodImpl.deleteObject(object);
}

void userEventStartTimer(aioObject *event, uint64_t usTimeout, int counter)
{
  aioOpRoot *op = event->root.readQueue.head;  
  op->base->methodImpl.startTimer((asyncOp*)op, usTimeout, counter);
}


void userEventStopTimer(aioObject *event)
{
  aioOpRoot *op = event->root.readQueue.head;    
  op->base->methodImpl.stopTimer((asyncOp*)op);
}


void userEventActivate(aioObject *event)
{
  aioOpRoot *op = event->root.readQueue.head;    
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
                asyncCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, 0, 0, afNone, usTimeout, actConnect);
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 1))
    ((aioOpRoot*)newOp)->base->methodImpl.connect(newOp, address, 0);
}


void aioAccept(aioObject *op,
               uint64_t usTimeout,
               asyncCb callback,
               void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, 0, 0, afNone, usTimeout, actAccept);
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 0))
    ((aioOpRoot*)newOp)->base->methodImpl.accept(newOp, 0);
}


void aioRead(aioObject *op,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             asyncCb callback,
             void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout, actRead);
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 0))
    startMethod(newOp);  
}


void aioWrite(aioObject *op,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              asyncCb callback,
              void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout, actWrite);  
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 1))
    startMethod(newOp);  
}


void aioReadMsg(aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, buffer, 0, afNone, usTimeout, actReadMsg);
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 0))
    startMethod(newOp);  
}



void aioWriteMsg(aioObject *op,
                 const HostAddress *address,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 asyncCb callback,
                 void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout, actWriteMsg);  
  if (addToExecuteQueue((aioObjectRoot*)op, (aioOpRoot*)newOp, 1))
    startMethod(newOp);  
}


int ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioConnect(op, address, usTimeout, coroutineCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -1;
}


socketTy ioAccept(aioObject *op, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioAccept(op, usTimeout, coroutineCb, &r); 
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
  aioInfo *info = (aioInfo*)op;
  event->root.readQueue.head->callback = coroutineCb;
  info->arg = &r;
  info->object->base->methodImpl.startTimer(op, usTimeout, 1);
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
