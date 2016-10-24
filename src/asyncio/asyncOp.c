#include "asyncio/asyncOp.h"
#include "asyncio/objectPool.h"
#include "asyncioInternal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef WIN32
#include <signal.h>
#endif

static inline uint64_t getPt(uint64_t endTime)
{
  return (endTime / 1000000) + (endTime % 1000000 != 0);
}

static inline void acquireSpinlock(aioObjectRoot *object)
{
  // TODO: implement
}

static inline void releaseSpinlock(aioObjectRoot *object)
{
  // TODO: implement
}

void opRingInit(OpRing *buffer, size_t size, uint64_t begin)
{
  buffer->data = calloc(size, sizeof(void*));
  buffer->size = size;
  buffer->begin = begin;
  buffer->offset = 0;
  buffer->other = 0;
}

uint64_t opRingBegin(OpRing *buffer)
{
  return buffer->begin;
}

asyncOpRoot *opRingGet(OpRing *buffer, uint64_t pt)
{
  uint64_t distance = pt - buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    return buffer->data[index];
  } else {
    return 0;
  }
}

void opRingShift(OpRing *buffer, uint64_t newBegin)
{
  uint64_t distance = newBegin - buffer->begin;
  if (distance == 0)
    return;
  
  size_t newOffset = buffer->offset + distance;
  if (distance < buffer->size) {
    size_t d1;
    size_t d2;
    int rotate = newOffset > buffer->size;
    if (rotate) {
      newOffset %= buffer->size;
      d1 = buffer->size - buffer->offset;
      d2 = newOffset;
    } else {
      d1 = newOffset - buffer->offset;
      d2 = 0;
    }

    memset(&buffer->data[buffer->offset], 0, d1*sizeof(void*));
    memset(buffer->data, 0, d2*sizeof(void*));
  } else {
    memset(buffer->data, 0, sizeof(void*)*buffer->size);    
  }
  
  buffer->begin = newBegin;
  buffer->offset = newOffset;
  
  // TODO: move from other to main grid
}

void opRingPop(OpRing *buffer, uint64_t pt)
{
  uint64_t distance = pt-buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    if (buffer->data[index])
      buffer->data[index] = buffer->data[index]->timeoutQueue.next;
  } else {
    if (buffer->other)
      buffer->other = buffer->other->timeoutQueue.next;
  }
}

void opRingPush(OpRing *buffer, asyncOpRoot *op, uint64_t pt)
{
  asyncOpRoot *oldOp;
  uint64_t distance = pt-buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    oldOp = buffer->data[index];
    buffer->data[index] = op;
  } else {
    oldOp = buffer->other;
    buffer->other = op;
  }
  
  op->timeoutQueue.prev = 0;
  op->timeoutQueue.next = oldOp;
  if (oldOp)
    oldOp->timeoutQueue.prev = op;  
}

timerTy nullTimer()
{
#ifdef WIN32
  return INVALID_HANDLE_VALUE;
#else
  return 0;
#endif
}

timerTy createTimer(void *arg)
{
#ifdef WIN32
  return CreateWaitableTimer(NULL, FALSE, NULL);
#else
  timerTy timerId = 0;
  struct sigevent sEvent;
  sEvent.sigev_notify = SIGEV_SIGNAL;
  sEvent.sigev_signo = SIGRTMIN;
  sEvent.sigev_value.sival_ptr = arg;
  timer_create(CLOCK_REALTIME, &sEvent, &timerId);
  return timerId;
#endif
}

aioObjectRoot *initObjectRoot(int type, size_t size, aioObjectDestructor destructor)
{
  aioObjectRoot *object = (aioObjectRoot*)calloc(size, 1);
  object->type = type;
  object->links = 1;
  object->destructor = destructor;
  return object;
}

void checkForDeleteObject(aioObjectRoot *object)
{
  if (!object->readQueue.head && !object->writeQueue.head && object->links == 0)
    object->destructor(object);
}

void cancelIo(aioObjectRoot *object, asyncBase *base)
{
  asyncOpRoot *op;
  
  for (;;) {
    acquireSpinlock(object);
    op = object->readQueue.head;
    if (!op) {
      releaseSpinlock(object);
      break;
    }
    
    if (op->base == base) {
      object->readQueue.head = op->executeQueue.next;
      releaseSpinlock(object);
    } else {
      releaseSpinlock(object);
      // TODO: send message to another base
      return;
    }
    
    if (op->timerId != nullTimer()) {
      base->methodImpl.stopTimer(op);
    } else if (op->endTime) {
      removeFromTimeoutQueue(base, op);
    }
    
    objectRelease(&base->pool, op, op->poolId); 
    op->finishMethod(op, aosCanceled);
  }
  
  for (;;) {
    acquireSpinlock(object);
    op = object->writeQueue.head;
    if (!op) {
      releaseSpinlock(object);
      break;
    }
    
    if (op->base == base) {
      object->writeQueue.head = op->executeQueue.next;
      releaseSpinlock(object);
    } else {
      releaseSpinlock(object);
      // TODO send message to another base
      return;
    }
    
    if (op->timerId != nullTimer()) {
      base->methodImpl.stopTimer(op);
    } else if (op->endTime) {
      removeFromTimeoutQueue(base, op);
    }
    
    objectRelease(&base->pool, op, op->poolId); 
    op->finishMethod(op, aosCanceled);
  }
  
  
  checkForDeleteObject(object);
}

asyncOpRoot *initAsyncOpRoot(asyncBase *base,
                             const char *nonTimerPool,
                             const char *timerPool,
                             newAsyncOpTy *newOpProc,
                             aioStartProc *startMethod,
                             aioFinishProc *finishMethod,
                             aioObjectRoot *object,
                             void *callback,
                             void *arg,
                             int flags,
                             int opCode,
                             uint64_t timeout)
{
  int realtime = (object->type == ioObjectUserEvent) || (flags & afRealtime);
  const char *pool = realtime ? timerPool : nonTimerPool;  
  asyncOpRoot *op = (asyncOpRoot*)objectGet(&base->pool, pool);  
  if (!op) {
    op = newOpProc(base);
    op->timerId = realtime ? createTimer(op) : nullTimer();
    op->base = base;
    op->poolId = pool;
    op->startMethod = startMethod;
    op->finishMethod = finishMethod;

  }
  
  op->executeQueue.prev = 0;
  op->executeQueue.next = 0;  
  op->timeoutQueue.prev = 0;
  op->timeoutQueue.next = 0;  
  op->object = object;
  op->flags = flags;
  op->opCode = opCode;
  op->endTime = 0;
  op->callback = callback;
  op->arg = arg;
  op->counter = 0;  
  
  if (timeout) {
    if (realtime) {
      // start timer for this operation
      op->base->methodImpl.startTimer(op, timeout, 1);
    } else {
      // add operation to timeout grid
      op->endTime = ((uint64_t)time(0))*1000000ULL + timeout;
      addToTimeoutQueue(op->base, op);
    }
  }
  
  return op;
}


int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue)
{
  // TODO: make thread safe
  // TODO acquireSpinlock(object);
  List *list = isWriteQueue ? &object->writeQueue : &object->readQueue;
  op->executeQueue.prev = list->tail;
  op->executeQueue.next = 0;
  if (list->tail)
    list->tail->executeQueue.next = op;
  list->tail = op;
  if (op->executeQueue.prev == 0) {
    list->head = op;
    // TODO releaseSpinlock(object);
    return 1;
  }
  
  return 0;
}

asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op)
{
  // TODO: make thread safe
  // TODO acquireSpinlock(object)
  aioObjectRoot *object = op->object;  
  if (op->executeQueue.next) {
    op->executeQueue.next->executeQueue.prev = op->executeQueue.prev;
  } else {
    if (object->readQueue.tail == op)
      object->readQueue.tail = op->executeQueue.prev;
    else if (object->writeQueue.tail == op)
      object->writeQueue.tail = op->executeQueue.prev;
  }  

  if (op->executeQueue.prev) {
    op->executeQueue.prev->executeQueue.next = op->executeQueue.next;
  } else {
    if (object->readQueue.head == op) {
      object->readQueue.head = op->executeQueue.next;
      // Start next 'read' operation
      if (object->readQueue.head)
        // TODO releaseSpinlock(object);
        return object->readQueue.head;
    } else if (object->writeQueue.head == op) {
      object->writeQueue.head = op->executeQueue.next;
      // Start next 'write' operation
      if (object->writeQueue.head)
        // TODO releaseSpinlock(object);
        return object->writeQueue.head;
    }
  }
   
  // TODO releaseSpinlock(object);
  return 0;
}


void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  // TODO acquireSpinlock(base)
  opRingPush(&base->timeGrid, op, getPt(op->endTime));
  // TODO releaseSpinlock(base)
}


void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  if (op->timeoutQueue.prev) {
    op->timeoutQueue.prev->timeoutQueue.next = op->timeoutQueue.next;
  } else {
    assert(opRingGet(&base->timeGrid, getPt(op->endTime)) == op && "opRing lost operation found");
    opRingPop(&base->timeGrid, getPt(op->endTime));
  }
  if (op->timeoutQueue.next)
    op->timeoutQueue.next->timeoutQueue.prev = op->timeoutQueue.prev;
}

void processTimeoutQueue(asyncBase *base)
{
  // check timeout queue
  uint64_t currentTime = time(0);
  uint64_t begin = opRingBegin(&base->timeGrid);
  if (!(begin < currentTime))
    return;
  
  while (begin < currentTime) {
    // TODO acquireSpinlock(base)
    asyncOpRoot *op = opRingGet(&base->timeGrid, begin);
    // TODO releaseSpinlock(base)
    while (op) {
      asyncOpRoot *next = op->timeoutQueue.next;
      finishOperation(op, aosTimeout, 0);
      op = next;
    }
      
    begin++;
  }
  // TODO acquireSpinlock(base)
  opRingShift(&base->timeGrid, currentTime);
  // TODO releaseSpinlock(base)
}

static inline void startOperation(asyncOpRoot *op, asyncBase *previousOpBase)
{
  // TODO: use pipe for send operation to another async base
  uint64_t timePt = ((uint64_t)time(0))*1000000;
  if (!op->endTime || op->endTime >= timePt)
    op->startMethod(op);
}

void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid)
{
  asyncBase *base = op->base;  
  aioObjectRoot *object = op->object;
  object->links++; // TODO: atomic
  
  if (op->timerId != nullTimer()) {
    base->methodImpl.stopTimer(op);
  } else if (op->endTime && needRemoveFromTimeGrid) {
    removeFromTimeoutQueue(base, op);
  }
  
  asyncOpRoot *nextOp;
  
  // Remove operation from execute queue
  // Release operation
  // Do callback if need
  if (status == aosTimeout) {
    op->finishMethod(op, status);
    nextOp = removeFromExecuteQueue(op);
    objectRelease(&base->pool, op, op->poolId);    
  } else {
    nextOp = removeFromExecuteQueue(op);
    objectRelease(&base->pool, op, op->poolId);
    op->finishMethod(op, status);
  }

  object->links--; // TODO: atomic
  
  // Start next operation
  if (nextOp)
    startOperation(nextOp, base);
  else
    checkForDeleteObject(object);
}
