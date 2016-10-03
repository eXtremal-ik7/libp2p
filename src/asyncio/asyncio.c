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

static void exceptionCb(int opCode, void *callback, void *arg)
{
  if (callback) {
    aioInfo info;
    info.status = aosTimeout;
    info.arg = arg;
    ((asyncCb*)callback)(&info);
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
                            uint64_t timeout)
{
  aioInfo *info;
  
  int needTimer = (object->type == ioObjectUserEvent) || (flags & afRealtime);
  const char *pool = needTimer ? poolId : timerPoolId;
  
  asyncOp *newOp = queryObject(object->base, pool);
  if (!newOp) {
    newOp = object->base->methodImpl.newAsyncOp(object->base, needTimer);
    info = (aioInfo*)newOp;
    info->root.base = object->base;
    info->root.poolId = pool;
    info->root.exceptionCallback = exceptionCb;
  } else {
    info = (aioInfo*)newOp;
  }
  
  info->root.executeQueue.prev = 0;
  info->root.executeQueue.next = 0;  
  info->root.timeoutQueue.prev = 0;
  info->root.timeoutQueue.next = 0;  
  info->object = object;
  if (needYield) {
    info->coroutine = coroutineCurrent();
  } else {
    info->coroutine = 0;
    info->callback = callback;
    info->arg = arg;
  }
  info->root.callback = callback;
  info->root.arg = arg;

  if (buffer)
    info->buffer = buffer;
  else if (dynamicArray)
    info->dynamicArray = dynamicArray;
  info->transactionSize = transactionSize;
  info->flags = flags;
  info->bytesTransferred = 0;
  if (!needTimer && timeout) {
    info->root.endTime = ((uint64_t)time(0))*1000000ULL + timeout;
    addToTimeoutQueue(object->base, &info->root);
  } else {
    info->root.endTime = 0;
  }
  
  return newOp;
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
  object->root.head = initAsyncOp(object, 0, callback, arg, 0, 0, 0, 0, 0);
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
    aioOpRoot *op = object->root.head;
    objectRelease(&op->base->pool, op, op->poolId);
  }
    
  object->base->methodImpl.deleteObject(object);
}

void userEventStartTimer(aioObject *event, uint64_t usTimeout, int counter)
{
  aioOpRoot *op = event->root.head;  
  op->base->methodImpl.startTimer((asyncOp*)op, usTimeout, counter);
}


void userEventStopTimer(aioObject *event)
{
  aioOpRoot *op = event->root.head;    
  op->base->methodImpl.stopTimer((asyncOp*)op);
}


void userEventActivate(aioObject *event)
{
  aioOpRoot *op = event->root.head;    
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
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, 0, 0, afNone, usTimeout);
  op->base->methodImpl.connect(newOp, address, usTimeout);
}


void aioAccept(aioObject *op,
               uint64_t usTimeout,
               asyncCb callback,
               void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, 0, 0, afNone, usTimeout);
  op->base->methodImpl.accept(newOp, usTimeout);
}


void aioRead(aioObject *op,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             asyncCb callback,
             void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout);
  op->base->methodImpl.read(newOp, usTimeout);
}


void aioWrite(aioObject *op,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              asyncCb callback,
              void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout);  
  op->base->methodImpl.write(newOp, usTimeout);
}


void aioReadMsg(aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, 0, buffer, 0, afNone, usTimeout);
  op->base->methodImpl.readMsg(newOp, usTimeout);
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
  asyncOp *newOp = initAsyncOp(op, 0, callback, arg, buffer, 0, size, flags, usTimeout);  
  op->base->methodImpl.writeMsg(newOp, address, usTimeout);
}


int ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, 0, 0, 0, afNone, usTimeout);
  op->base->methodImpl.connect(newOp, address, usTimeout);
  
  coroutineYield();
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? 0 : -1;
}


socketTy ioAccept(aioObject *op, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, 0, 0, 0, afNone, usTimeout);
  op->base->methodImpl.accept(newOp, usTimeout);

  coroutineYield();
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->acceptSocket : -1;
}


ssize_t ioRead(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, buffer, 0, size, flags, usTimeout);
  op->base->methodImpl.read(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}


ssize_t ioWrite(aioObject *op, void *buffer, size_t size, AsyncFlags flags,uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, buffer, 0, size, flags, usTimeout);  
  op->base->methodImpl.write(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}


ssize_t ioReadMsg(aioObject *op, dynamicBuffer *buffer,uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, 0, buffer, 0, afNone, usTimeout);
  op->base->methodImpl.readMsg(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}



ssize_t ioWriteMsg(aioObject *op, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 1, 0, 0, buffer, 0, size, flags, usTimeout);  
  op->base->methodImpl.writeMsg(newOp, address, usTimeout);
    
  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1; 
}


void ioSleep(aioObject *event, uint64_t usTimeout)
{
  asyncOp *op = (asyncOp*)event->root.head;
  aioInfo *info = (aioInfo*)op;
  info->coroutine = coroutineCurrent();
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
