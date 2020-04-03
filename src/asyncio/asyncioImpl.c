#include "asyncioImpl.h"
#include "asyncio/coroutine.h"
#include "atomic.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef WIN32
#include <signal.h>
#endif

#define PAGE_MAP_SIZE (1u << 16)

#define TAGGED_POINTER_DATA_SIZE 6
#define TAGGED_POINTER_ALIGNMENT (((intptr_t)1) << TAGGED_POINTER_DATA_SIZE)
#define TAGGED_POINTER_DATA_MASK (TAGGED_POINTER_ALIGNMENT-1)
#define TAGGED_POINTER_PTR_MASK (~TAGGED_POINTER_DATA_MASK)

__tls unsigned currentFinishedSync;
__tls unsigned messageLoopThreadId;
__tls RingBuffer localQueue;

static const char *asyncOpLinkListPool = "asyncOpLinkList";
static const char *asyncOpActionPool = "asyncOpAction";

void eqRemove(List *list, asyncOpRoot *op)
{
  if (op->executeQueue.prev) {
    op->executeQueue.prev->executeQueue.next = op->executeQueue.next;
  } else {
    list->head = op->executeQueue.next;
    if (list->head == 0)
      list->tail = 0;
  }

  if (op->executeQueue.next) {
    op->executeQueue.next->executeQueue.prev = op->executeQueue.prev;
  } else {
    list->tail = op->executeQueue.prev;
    if (list->tail == 0)
      list->head = 0;
  }
}

void eqPushBack(List *list, asyncOpRoot *op)
{
  op->executeQueue.prev = list->tail;
  op->executeQueue.next = 0;
  if (list->tail) {
    list->tail->executeQueue.next = op;
    list->tail = op;
  } else {
    list->head = list->tail = op;
  }
}

static inline uint64_t getPt(uint64_t endTime)
{
  return (endTime / 1000000) + (endTime % 1000000 != 0);
}

static inline void pageMapKeys(uint64_t tm, uint32_t *lo, uint32_t *hi)
{
  uint32_t ltm = (uint32_t)tm;
  *hi = ltm >> 16;
  *lo = ltm & 0xFFFF;
}

static inline void *pageMapAlloc()
{
  return calloc(PAGE_MAP_SIZE, sizeof(void*));
}

void *__tagged_alloc(size_t size)
{
#ifdef OS_COMMONUNIX
  void *memptr;
  return posix_memalign(&memptr, TAGGED_POINTER_ALIGNMENT, size) == 0 ? memptr: 0;
#else
  return _aligned_malloc(size, TAGGED_POINTER_ALIGNMENT);
#endif
}

void *__tagged_pointer_make(void *ptr, tag_t data)
{
  return (void*)(((intptr_t)ptr) + ((intptr_t)(data & TAGGED_POINTER_DATA_MASK)));
}

void __tagged_pointer_decode(void *ptr, void **outPtr, tag_t *outData)
{
  intptr_t p = (intptr_t)ptr;
  *outPtr = (void*)(p & TAGGED_POINTER_PTR_MASK);
  *outData = p & TAGGED_POINTER_DATA_MASK;
}

void pageMapInit(pageMap *map)
{
  map->map = (asyncOpListLink***)pageMapAlloc();
  map->lock = 0;
}

asyncOpListLink *pageMapExtractAll(pageMap *map, uint64_t tm)
{
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(tm, &lo, &hi);
  asyncOpListLink **p1 = map->map[hi];

  if (p1) {
  __spinlock_acquire(&map->lock);
  asyncOpListLink *first = p1[lo];
    p1[lo] = 0;
    __spinlock_release(&map->lock);
    return first;
  } else {
    return 0;
  }
}

void pageMapAdd(pageMap *map, asyncOpListLink *link)
{
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(getPt(link->op->endTime), &lo, &hi);
  asyncOpListLink **p1 = map->map[hi];
  if (!p1) {
    p1 = (asyncOpListLink**)pageMapAlloc();
    if (!__pointer_atomic_compare_and_swap((void* volatile*)&map->map[hi], 0, p1)) {
      free(p1);
      p1 = map->map[hi];
    }
  }

  link->prev = 0;

  __spinlock_acquire(&map->lock);
  link->next = p1[lo];
  if (p1[lo])
    p1[lo]->prev = link;
  p1[lo] = link;
  __spinlock_release(&map->lock);
}

int pageMapRemove(pageMap *map, asyncOpListLink *link)
{
  int removed = 0;
  uint32_t lo;
  uint32_t hi;
  pageMapKeys(getPt(link->op->endTime), &lo, &hi);
  asyncOpListLink **p1 = map->map[hi];
  if (!p1)
    return removed;

  __spinlock_acquire(&map->lock);
  if (p1[lo]) {
    if (link == p1[lo])
      p1[lo] = link->next;
    if (link->prev)
      link->prev->next = link->next;
    if (link->next)
      link->next->prev = link->prev;
    removed = 1;
  }
  __spinlock_release(&map->lock);

  return removed;
}

tag_t objectIncrementReference(aioObjectRoot *object, tag_t count)
{
  tag_t result = __tag_atomic_fetch_and_add(&object->refs, count);
  assert(result != 0 && "Removed object access detected");
  return result;
}

tag_t objectDecrementReference(aioObjectRoot *object, tag_t count)
{
  tag_t result = __tag_atomic_fetch_and_add(&object->refs, (tag_t)0-count);
  if (result == count) {
    // try delete
    assert(!(object->tag & TAG_DELETE) && "Double free detected");
    if (__tag_atomic_fetch_and_add(&object->tag, TAG_DELETE) == 0)
      object->base->methodImpl.combiner(object, TAG_DELETE, 0, aaNone);
  }

  return result;
}

tag_t eventIncrementReference(aioUserEvent *event, tag_t tag)
{
  return __tag_atomic_fetch_and_add(&event->tag, tag);
}

tag_t eventDecrementReference(aioUserEvent *event, tag_t tag)
{
  tag_t result = __tag_atomic_fetch_and_add(&event->tag, (tag_t)0 - tag) & TAG_EVENT_MASK;
  if (result == (tag & TAG_EVENT_MASK)) {
    objectRelease(&event->root, event->root.poolId);
    if (event->destructorCb)
      event->destructorCb(event, event->destructorCbArg);
  }

  return result;
}

int eventTryActivate(aioUserEvent *event)
{
  if (!event->isSemaphore) {
    tag_t result = __tag_atomic_fetch_and_add(&event->tag, TAG_EVENT_OP + 1);
    if ((result & TAG_EVENT_OP_MASK) == 0) {
      return 1;
    } else {
      __tag_atomic_fetch_and_add(&event->tag, -(TAG_EVENT_OP + 1));
      return 0;
    }
  } else {
    __tag_atomic_fetch_and_add(&event->tag, 1);
    return 1;
  }
}

void eventDeactivate(aioUserEvent *event)
{
  if (!event->isSemaphore)
    __tag_atomic_fetch_and_add(&event->tag, (tag_t)0-TAG_EVENT_OP);
}

void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  asyncOpListLink *timerLink = objectGet(asyncOpLinkListPool);
  if (!timerLink)
    timerLink = malloc(sizeof(asyncOpListLink));
  timerLink->op = op;
  timerLink->tag = opGetGeneration(op);
  pageMapAdd(&base->timerMap, timerLink);
  op->timerId = timerLink;
}


void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  asyncOpListLink *timerLink = (asyncOpListLink*)op->timerId;
  if (timerLink && pageMapRemove(&base->timerMap, timerLink)) {
    objectRelease(timerLink, asyncOpLinkListPool);
    op->timerId = 0;
  }
}

void processTimeoutQueue(asyncBase *base, time_t currentTime)
{
  // TODO: handle system date change
  if (base->lastCheckPoint >= currentTime || !__spinlock_try_acquire(&base->timerMapLock))
    return;

  // check timeout queue
  time_t begin = base->lastCheckPoint;
  for (; begin < currentTime; begin++) {
    asyncOpListLink *link = pageMapExtractAll(&base->timerMap, (uint64_t)begin);
    while (link) {
      asyncOpListLink *next = link->next;
      opCancel(link->op, link->tag, aosTimeout);
      objectRelease(link, asyncOpLinkListPool);
      link = next;
    }
  }

  base->lastCheckPoint = currentTime;
  __spinlock_release(&base->timerMapLock);
}

void initObjectRoot(aioObjectRoot *object, asyncBase *base, IoObjectTy type, aioObjectDestructor destructor)
{
  object->tag = 0;
  object->readQueue.head = object->readQueue.tail = 0;
  object->writeQueue.head = object->writeQueue.tail = 0;
  object->announcementQueue.head = object->announcementQueue.tail = 0;
  object->announcementQueue.lock = 0;
  object->base = base;
  object->type = type;
  object->refs = 1;
  object->destructor = destructor;
  object->destructorCb = 0;
  object->destructorCbArg = 0;
}

void objectSetDestructorCb(aioObjectRoot *object, aioObjectDestructorCb callback, void *arg)
{
  object->destructorCb = callback;
  object->destructorCbArg = arg;
}

void eventSetDestructorCb(aioUserEvent *event, userEventDestructorCb callback, void *arg)
{
  event->destructorCb = callback;
  event->destructorCbArg = arg;
}

void cancelIo(aioObjectRoot *object)
{
  if (__tag_atomic_fetch_and_add(&object->tag, TAG_CANCELIO) == 0)
    object->base->methodImpl.combiner(object, TAG_CANCELIO, 0, aaNone);
}

void objectDelete(aioObjectRoot *object)
{
  cancelIo(object);
  objectDecrementReference(object, 1);
}

tag_t opGetGeneration(asyncOpRoot *op)
{
  return op->tag >> TAG_STATUS_SIZE;
}

AsyncOpStatus opGetStatus(asyncOpRoot *op)
{
  return op->tag & TAG_STATUS_MASK;
}

int opSetStatus(asyncOpRoot *op, tag_t generation, AsyncOpStatus status)
{
  return __tag_atomic_compare_and_swap(&op->tag,
                                      (generation<<TAG_STATUS_SIZE) | aosPending,
                                      (generation<<TAG_STATUS_SIZE) | (tag_t)status);
}

void opForceStatus(asyncOpRoot *op, AsyncOpStatus status)
{
  op->tag = (op->tag & TAG_GENERATION_MASK) | (tag_t)status;
}

tag_t opEncodeTag(asyncOpRoot *op, tag_t tag)
{
  return ((op->tag >> TAG_STATUS_SIZE) & ~((tag_t)TAGGED_POINTER_DATA_MASK)) | (tag & (tag_t)TAGGED_POINTER_DATA_MASK);
}

asyncOpRoot *initAsyncOpRoot(const char *nonTimerPool,
                             const char *timerPool,
                             newAsyncOpTy *newOpProc,
                             aioExecuteProc *startMethod,
                             aioCancelProc *cancelMethod,
                             aioFinishProc *finishMethod,
                             aioObjectRoot *object,
                             void *callback,
                             void *arg,
                             AsyncFlags flags,
                             int opCode,
                             uint64_t timeout)
{
  int realtime = (opCode == actUserEvent) || (flags & afRealtime);
  const char *pool = realtime ? timerPool : nonTimerPool;
  asyncOpRoot *op = (asyncOpRoot*)objectGet(pool);
  if (!op) {
    op = newOpProc();
    op->poolId = pool;
    if (realtime)
      object->base->methodImpl.initializeTimer(object->base, op);
    op->tag = 0;
  }

  op->tag = ((opGetGeneration(op)+1) << TAG_STATUS_SIZE) | aosPending;
  op->executeMethod = startMethod;
  op->cancelMethod = cancelMethod;
  // TODO: better type control
  op->finishMethod = (flags & afCoroutine) ? (aioFinishProc*)coroutineCurrent() : finishMethod;
  op->executeQueue.prev = 0;
  op->executeQueue.next = 0;
  op->next = 0;
  op->object = object;
  op->flags = flags;
  op->opCode = opCode;
  op->callback = callback;
  op->arg = arg;
  op->timeout = timeout;
  op->timerId = realtime ? op->timerId : 0;
  op->running = (flags & afRunning) ? arRunning : arWaiting;
  objectIncrementReference(object, 1);
  return op;
}

static void opRun(asyncOpRoot *op, List *list)
{
  eqPushBack(list, op);
  if (op->timeout) {
    asyncBase *base = op->object->base;
    if (op->flags & afRealtime) {
      // start timer for this operation
      base->methodImpl.startTimer(op);
    } else {
      // add operation to timeout grid
      op->endTime = ((uint64_t)time(0)) * 1000000ULL + op->timeout;
      addToTimeoutQueue(base, op);
    }
  }
}

void processAction(asyncOpRoot *opptr, AsyncOpActionTy actionType, tag_t *needStart)
{
  List *list = 0;
  tag_t tag = 0;
  aioObjectRoot *object = opptr->object;
  if (opptr->opCode & OPCODE_WRITE) {
    list = &object->writeQueue;
    tag = TAG_WRITE;
  } else {
    list = &object->readQueue;
    tag = TAG_READ;
  }

  switch (actionType) {
    case aaStart : {
      opRun(opptr, list);
      break;
    }

    case aaCancel : {
      if (opptr->running == arRunning) {
        opptr->running = arCancelling;
        if (opptr->cancelMethod(opptr))
          opRelease(opptr, opGetStatus(opptr), list);
      } else {
        opRelease(opptr, opGetStatus(opptr), list);
      }
      break;
    }

    case aaFinish : {
      opRelease(opptr, opGetStatus(opptr), list);
      break;
    }

    case aaContinue : {
      if (opptr->running == arRunning)
        opptr->running = arWaiting;
      else
        opRelease(opptr, opGetStatus(opptr), list);
      break;
    }

    default :
      break;
  }

  *needStart |= (list->head && list->head->running == arWaiting) ? tag : 0;
}

void processOperationList(aioObjectRoot *object, tag_t *needStart, tag_t *enqueued)
{
  asyncOpAction *action;
  __spinlock_acquire(&object->announcementQueue.lock);
  action = object->announcementQueue.head;
  object->announcementQueue.head = 0;
  object->announcementQueue.tail = 0;
  __spinlock_release(&object->announcementQueue.lock);

  while (action) {
    processAction(action->op, action->actionType, needStart);
    objectRelease(action, asyncOpActionPool);
    action = action->next;
    (*enqueued)++;
  }
}

void opRelease(asyncOpRoot *op, AsyncOpStatus status, List *executeList)
{
  if (op->timerId && status != aosTimeout) {
    if (op->flags & afRealtime)
      op->object->base->methodImpl.stopTimer(op);
    else
      removeFromTimeoutQueue(op->object->base, op);
  }

  if (executeList)
    eqRemove(executeList, op);
  addToGlobalQueue(op);
}

void executeOperationList(List *list)
{
  asyncOpRoot *op = list->head;
  while (op) {
    asyncOpRoot *next = op->executeQueue.next;
    AsyncOpStatus status = op->executeMethod(op);
    if (status == aosPending) {
      op->running = arRunning;
      break;
    }

    if (opSetStatus(op, opGetGeneration(op), status))
      opRelease(op, status, 0);
    else
      op->executeQueue.prev = op->executeQueue.next = 0;
    op = next;
  }

  list->head = op;
  if (!op)
    list->tail = 0;
}

void cancelOperationList(List *list, AsyncOpStatus status)
{
  asyncOpRoot *op = list->head;
  while (op) {
    asyncOpRoot *next = op->executeQueue.next;
    if (opSetStatus(op, opGetGeneration(op), status)) {
      if (op->running == arRunning) {
        op->running = arCancelling;
        if (op->cancelMethod(op))
          opRelease(op, opGetStatus(op), list);
      } else {
        opRelease(op, opGetStatus(op), list);
      }
    }
    op = next;
  }

  list->head = 0;
  list->tail = 0;
}

void combinerAddAction(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  asyncOpAction *action = objectGet(asyncOpActionPool);
  if (!action)
    action = malloc(sizeof(asyncOpAction));
  action->op = op;
  action->actionType = actionType;
  action->next = 0;

  __spinlock_acquire(&object->announcementQueue.lock);
  if (object->announcementQueue.tail == 0) {
    object->announcementQueue.head = action;
    object->announcementQueue.tail = action;
  } else {
    object->announcementQueue.tail->next = action;
    object->announcementQueue.tail = action;
  }
  __spinlock_release(&object->announcementQueue.lock);
}

void combinerCall(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  if (__tag_atomic_fetch_and_add(&object->tag, tag) == 0)
    object->base->methodImpl.combiner(object, tag, op, actionType);
  else
    combinerAddAction(object, op, actionType);
}

void combinerCallWithoutLock(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy action)
{
  object->base->methodImpl.combiner(object, tag, op, action);
}

void opStart(asyncOpRoot *op)
{
  combinerCall(op->object, 1, op, aaStart);
}

void opCancel(asyncOpRoot *op, tag_t generation, AsyncOpStatus status)
{
  if (opSetStatus(op, generation, status))
    combinerCall(op->object, 1, op, aaCancel);
}

void resumeParent(asyncOpRoot *op, AsyncOpStatus status)
{
  if (status == aosSuccess) {
    combinerCall(op->object, 1, op, aaContinue);
  } else {
    opSetStatus(op, opGetGeneration(op), status);
    combinerCall(op->object, 1, op, aaFinish);
  }
}

void addToGlobalQueue(asyncOpRoot *op)
{
  op->object->base->methodImpl.enqueue(op->object->base, op);
}

int executeGlobalQueue(asyncBase *base)
{ 
  asyncOpRoot *op;
  while (ringBufferDequeue(&localQueue, (void**)&op))
    concurrentRingBufferEnqueue(&base->globalQueue, op);
  while (concurrentRingBufferDequeue(&base->globalQueue, (void**)&op)) {
    if (!op)
      return 0;

    switch (op->opCode) {
      case actUserEvent : {
        aioUserEvent *event = (aioUserEvent*)op;
        eventDeactivate(event);
        op->finishMethod(op);
        eventDecrementReference(event, 1);
        break;
      }

      default : {
        currentFinishedSync = 0;
        if (op->flags & afCoroutine) {
          assert(coroutineIsMain() && "Execute global queue from non-main coroutine");
          coroutineCall((coroutineTy*)op->finishMethod);
        } else {
          aioObjectRoot *object = op->object;
          objectRelease(op, op->poolId);
          if (op->callback)
            op->finishMethod(op);
          objectDecrementReference(object, 1);
        }
      }
    }
  }

  return 1;
}

int copyFromBuffer(void *dst, size_t *offset, struct ioBuffer *src, size_t size)
{
  size_t needRead = size - *offset;
  size_t remaining = src->dataSize - src->offset;
  if (needRead <= remaining) {
    memcpy((uint8_t*)dst + *offset, (uint8_t*)src->ptr + src->offset, needRead);
    *offset += needRead;
    src->offset += needRead;
    return 1;
  } else {
    memcpy((uint8_t*)dst + *offset, (uint8_t*)src->ptr + src->offset, remaining);
    *offset += remaining;
    src->offset = 0;
    src->dataSize = 0;
    return 0;
  }
}
