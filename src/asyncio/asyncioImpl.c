#include "asyncioImpl.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef WIN32
#include <signal.h>
#endif

#define PAGE_MAP_SIZE (1u << 16)

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

static inline void pageMapKeys(time_t tm, uint32_t *lo, uint32_t *hi)
{
  uint32_t ltm = (uint32_t)tm;
  *hi = ltm >> 16;
  *lo = ltm & 0xFFFF;
}

static inline void *pageMapAlloc()
{
  return calloc(PAGE_MAP_SIZE, sizeof(void*));
}

void pageMapInit(pageMap *map)
{
  map->map = (asyncOpRoot***)pageMapAlloc();
}

asyncOpRoot *pageMapExtractAll(pageMap *map, time_t tm)
{
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(tm, &lo, &hi);
  asyncOpRoot **p1 = map->map[hi];
  if (p1) {
    asyncOpRoot *first = p1[lo];
    p1[lo] = 0;
    return first;
  } else {
    return 0;
  }
}

void pageMapAdd(pageMap *map, asyncOpRoot *op)
{
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(getPt(op->endTime), &lo, &hi);
  asyncOpRoot **p1 = map->map[hi];
  if (!p1) {
    p1 = (asyncOpRoot**)pageMapAlloc();
    map->map[hi] = p1;
  }
  
  op->timeoutQueue.prev = 0;
  op->timeoutQueue.next = p1[lo];
  if (p1[lo])
    p1[lo]->timeoutQueue.prev = op;
  p1[lo] = op;
}

void pageMapRemove(pageMap *map, asyncOpRoot *op)
{
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(getPt(op->endTime), &lo, &hi);
  asyncOpRoot **p1 = map->map[hi];
  if (p1 && op == p1[lo])
    p1[lo] = op->timeoutQueue.next;
  
  if (op->timeoutQueue.prev)
    op->timeoutQueue.prev->timeoutQueue.next = op->timeoutQueue.next;
  if (op->timeoutQueue.next)
    op->timeoutQueue.next->timeoutQueue.prev = op->timeoutQueue.prev;
}

void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  // TODO acquireSpinlock(base)
  pageMapAdd(&base->timerMap, op);
  // TODO releaseSpinlock(base)
}


void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  pageMapRemove(&base->timerMap, op);
}

void processTimeoutQueue(asyncBase *base)
{
  // check timeout queue
  time_t currentTime = time(0);  
  time_t begin = base->lastCheckPoint;
  for (; begin <= currentTime; begin++) {
    // TODO acquireSpinlock(base)
    asyncOpRoot *op = pageMapExtractAll(&base->timerMap, begin);
    // TODO releaseSpinlock(base)
    while (op) {
      asyncOpRoot *next = op->timeoutQueue.next;
      finishOperation(op, aosTimeout, 0);
      op = next;
    }
      
    begin++;
  }
  
  // TODO acquireSpinlock(base)
  base->lastCheckPoint = currentTime;
  // TODO releaseSpinlock(base)
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
      if (object->readQueue.head == 0)
        object->readQueue.tail = 0;
      releaseSpinlock(object);
    } else {
      releaseSpinlock(object);
      // TODO: send message to another base
      return;
    }
    
    if (op->flags & afRealtime) {
      base->methodImpl.stopTimer(op);
    } else if (op->endTime) {
      removeFromTimeoutQueue(base, op);
    }
    
    op->finishMethod(op, aosCanceled);
    objectRelease(&base->pool, op, op->poolId);     
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
      if (object->writeQueue.head == 0)
        object->writeQueue.tail = 0;      
      releaseSpinlock(object);
    } else {
      releaseSpinlock(object);
      // TODO send message to another base
      return;
    }
    
    if (op->flags & afRealtime) {
      base->methodImpl.stopTimer(op);
    } else if (op->endTime) {
      removeFromTimeoutQueue(base, op);
    }
    
    op->finishMethod(op, aosCanceled);
    objectRelease(&base->pool, op, op->poolId);     
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
  int realtime = (opCode == actUserEvent) || (flags & afRealtime);
  const char *pool = realtime ? timerPool : nonTimerPool;  
  asyncOpRoot *op = (asyncOpRoot*)objectGet(&base->pool, pool);  
  if (!op) {
    op = newOpProc(base);
    op->base = base;    
    op->poolId = pool;
    op->startMethod = startMethod;
    op->finishMethod = finishMethod;
    if (realtime)
      op->base->methodImpl.initializeTimer(op);
  }
  
  op->executeQueue.prev = 0;
  op->executeQueue.next = 0;  
  op->timeoutQueue.prev = 0;
  op->timeoutQueue.next = 0;  
  op->object = object;
  op->flags = flags;
  op->opCode = opCode;
  op->callback = callback;
  op->arg = arg;
  
  if (timeout) {
    if (realtime) {
      // start timer for this operation
      op->base->methodImpl.startTimer(op, timeout);
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
  
  if (op->flags & afRealtime) {
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
