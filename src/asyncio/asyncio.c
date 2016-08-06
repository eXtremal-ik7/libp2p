#include "Debug.h"
#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include "asyncioInternal.h"
#include <stdlib.h>

const char *poolId = "asyncIo";

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

intptr_t argAsInteger(void *arg)
{
  return (intptr_t)arg;
}


void *intArg(intptr_t id)
{
  return (void*)id;
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
  base->coroutineMode = 0;
#ifndef NDEBUG
  base->opsCount = 0;
#endif
   return base;
}

int getCoroutineMode(asyncBase *base)
{
  return base->coroutineMode; 
}

void setCoroutineMode(asyncBase *base, int enabled)
{
  base->coroutineMode = enabled;
}

void asyncLoop(asyncBase *base)
{
  base->methodImpl.nextFinishedOperation(base);
}


void postQuitOperation(asyncBase *base)
{
  base->methodImpl.postEmptyOperation(base);
}


asyncOp *newUserEvent(asyncBase *base, asyncCb callback, void *arg)
{
  asyncOp *op = base->methodImpl.newAsyncOp(base);
  aioInfo *info = (aioInfo*)op;
  info->object = malloc(sizeof(aioObject));
  info->object->base = base;
  info->object->type = ioObjectUserEvent;
  info->callback = callback;
  info->arg = arg;
  return op;
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


void deleteOp(asyncOp *op)
{
  // TODO: implement!
}


void userEventStartTimer(asyncOp *event, uint64_t usTimeout, int counter)
{
  ((aioInfo*)event)->object->base->
    methodImpl.startTimer(event, usTimeout, counter);
}


void userEventStopTimer(asyncOp *event)
{
  ((aioInfo*)event)->object->base->methodImpl.stopTimer(event);
}


void userEventActivate(asyncOp *event)
{
  ((aioInfo*)event)->object->base->methodImpl.activate(event);
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


static asyncOp *initAsyncOp(aioObject *object,
                            asyncCb callback,
                            void *arg,
                            void *buffer,
                            dynamicBuffer *dynamicArray,
                            size_t transactionSize,
                            AsyncFlags flags)
{
  asyncOp *newOp = queryObject(object->base, poolId);
  if (!newOp)
    newOp = object->base->methodImpl.newAsyncOp(object->base);
  
  aioInfo *info = (aioInfo*)newOp;
  info->object = object;
  if (getCoroutineMode(object->base) == 0)  {
    info->callback = callback;
    info->arg = arg;
  } else {
    info->coroutine = coroutineCurrent();
  }
  if (buffer)
    info->buffer = buffer;
  else if (dynamicArray)
    info->dynamicArray = dynamicArray;
  info->transactionSize = transactionSize;
  info->flags = flags;
  info->bytesTransferred = 0;
  return newOp;
}


void aioConnect(aioObject *op,
                const HostAddress *address,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, 0, 0, afNone);
  op->base->methodImpl.connect(newOp, address, usTimeout);
}


void aioAccept(aioObject *op,
               uint64_t usTimeout,
               asyncCb callback,
               void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, 0, 0, afNone);
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
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags);
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
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags);  
  op->base->methodImpl.write(newOp, usTimeout);
}


void aioReadMsg(aioObject *op,
                dynamicBuffer *buffer,
                uint64_t usTimeout,
                asyncCb callback,
                void *arg)
{
  asyncOp *newOp = initAsyncOp(op, callback, arg, 0, buffer, 0, afNone);
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
  asyncOp *newOp = initAsyncOp(op, callback, arg, buffer, 0, size, flags);  
  op->base->methodImpl.writeMsg(newOp, address, usTimeout);
}


void ioConnect(aioObject *op, const HostAddress *address, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, 0, 0, 0, afNone);
  op->base->methodImpl.connect(newOp, address, usTimeout);
  coroutineYield();
}


socketTy ioAccept(aioObject *op, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, 0, 0, 0, afNone);
  op->base->methodImpl.accept(newOp, usTimeout);

  coroutineYield();
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->acceptSocket : -1;
}


ssize_t ioRead(aioObject *op, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, buffer, 0, size, flags);
  op->base->methodImpl.read(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}


ssize_t ioWrite(aioObject *op, void *buffer, size_t size, AsyncFlags flags,uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, buffer, 0, size, flags);  
  op->base->methodImpl.write(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}


ssize_t ioReadMsg(aioObject *op, dynamicBuffer *buffer,uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, 0, buffer, 0, afNone);
  op->base->methodImpl.readMsg(newOp, usTimeout);

  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1;
}



ssize_t ioWriteMsg(aioObject *op, const HostAddress *address, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout)
{
  asyncOp *newOp = initAsyncOp(op, 0, 0, buffer, 0, size, flags);  
  op->base->methodImpl.writeMsg(newOp, address, usTimeout);
    
  coroutineYield();  
  aioInfo *info = (aioInfo*)newOp;
  return (info->status == aosSuccess) ? info->bytesTransferred : -1; 
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
