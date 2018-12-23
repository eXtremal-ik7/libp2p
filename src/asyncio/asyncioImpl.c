#include "asyncioImpl.h"
#include "asyncio/coroutine.h"
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

#define TAG_STATUS_SIZE 8
#define TAG_STATUS_MASK ((((tag_t)1) << TAG_STATUS_SIZE)-1)
#define TAG_GENERATION_MASK (~TAG_STATUS_MASK)

static __tls aioObjectRoot* dccObject;
static __tls tag_t dccTag;
static __tls asyncOpRoot *dccOp;
static __tls AsyncOpActionTy dccActionType;
static __tls int dccNeedLock;

__tls List threadLocalQueue;
__tls unsigned currentFinishedSync;
__tls unsigned messageLoopThreadId;

static const char *asyncOpLinkListPool = "asyncOpLinkListPool";
static const char *asyncOpActionPool = "asyncOpActionPool";

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

void fnPushBack(List *list, asyncOpRoot *op)
{
  op->next = 0;
  if (list->tail) {
    list->tail->next = op;
    list->tail = op;
  } else {
    list->head = list->tail = op;
  }
}

static inline uint64_t getPt(uint64_t endTime)
{
  return (endTime / 1000000) + (endTime % 1000000 != 0);
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

asyncOpListLink *pageMapExtractAll(pageMap *map, time_t tm)
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
    asyncOpListLink *link = pageMapExtractAll(&base->timerMap, begin);
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
}

void cancelIo(aioObjectRoot *object)
{
  if (__tag_atomic_fetch_and_add(&object->tag, TAG_CANCELIO) == 0)
    object->base->methodImpl.combiner(object, TAG_CANCELIO, 0, aaNone);
}

void objectAddRef(aioObjectRoot *object)
{
  __tag_atomic_fetch_and_add(&object->refs, 1);
}

void objectDeleteRef(aioObjectRoot *object, tag_t count)
{
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4146)
#endif
  if (__tag_atomic_fetch_and_add(&object->refs, -count) == count) {
    // try delete 
    if (__tag_atomic_fetch_and_add(&object->tag, TAG_DELETE) == 0)
      object->base->methodImpl.combiner(object, TAG_DELETE, 0, aaNone);
  }
#ifdef _MSC_VER
#pragma warning(pop)
#endif
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
                                      (generation<<TAG_STATUS_SIZE) | status);
}

void opForceStatus(asyncOpRoot *op, AsyncOpStatus status)
{
  op->tag = (op->tag & TAG_GENERATION_MASK) | status;
}

tag_t opEncodeTag(asyncOpRoot *op, tag_t tag)
{
  return ((op->tag >> TAG_STATUS_SIZE) & ~((tag_t)TAGGED_POINTER_DATA_MASK)) | (tag & (tag_t)TAGGED_POINTER_DATA_MASK);
}

asyncOpRoot *initAsyncOpRoot(const char *nonTimerPool,
                             const char *timerPool,
                             newAsyncOpTy *newOpProc,
                             aioExecuteProc *startMethod,
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
  op->finishMethod = finishMethod;
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
  return op;
}

void processOperationList(aioObjectRoot *object, List *finished, tag_t *needStart, processOperationCb *processCb, tag_t *enqueued)
{
  asyncOpAction *action;
  __spinlock_acquire(&object->announcementQueue.lock);
  action = object->announcementQueue.head;
  object->announcementQueue.head = 0;
  object->announcementQueue.tail = 0;
  __spinlock_release(&object->announcementQueue.lock);

  while (action) {
    processCb(action->op, action->actionType, finished, needStart);
    objectRelease(action, asyncOpActionPool);
    action = action->next;
    (*enqueued)++;
  }
}

void opRelease(asyncOpRoot *op, AsyncOpStatus status, List *executeList, List *finished)
{
  if (op->timerId && status != aosTimeout) {
    if (op->flags & afRealtime)
      op->object->base->methodImpl.stopTimer(op);
    else
      removeFromTimeoutQueue(op->object->base, op);
  }

  if (executeList)
    eqRemove(executeList, op);
  fnPushBack(finished, op);
}

void executeOperationList(List *list, List *finished)
{
  asyncOpRoot *op = list->head;
  while (op) {
    asyncOpRoot *next = op->executeQueue.next;
    AsyncOpStatus status = op->executeMethod(op);
    if (status == aosPending)
      break;

    if (opSetStatus(op, opGetGeneration(op), status))
      opRelease(op, status, 0, finished);
    else
      op->executeQueue.prev = op->executeQueue.next = 0;
    op = next;
  }

  list->head = op;
  if (!op)
    list->tail = 0;
}

void cancelOperationList(List *list, List *finished, AsyncOpStatus status)
{
  asyncOpRoot *op = list->head;
  while (op) {
    asyncOpRoot *next = op->executeQueue.next;
    if (opSetStatus(op, opGetGeneration(op), status))
      opRelease(op, status, 0, finished);
    else
      op->executeQueue.prev = op->executeQueue.next = 0;
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

void combinerCallDelayed(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType, int needLock)
{
  dccObject = object;
  dccTag = tag;
  dccOp = op;
  dccActionType = actionType;
  dccNeedLock = needLock;
}

void opStart(asyncOpRoot *op)
{
  combinerCall(op->object, 1, op, aaStart);
}

void opCancel(asyncOpRoot *op, tag_t generation, AsyncOpStatus status)
{
  op->object->base->methodImpl.finishOp(op, generation, status);
}


void addToThreadLocalQueue(asyncOpRoot *op)
{
  fnPushBack(&threadLocalQueue, op);
}

void executeThreadLocalQueue()
{
  if (dccObject) {
    if (dccNeedLock)
      combinerCall(dccObject, dccTag, dccOp, dccActionType);
    else
      dccObject->base->methodImpl.combiner(dccObject, dccTag, dccOp, dccActionType);
    dccObject = 0;
  }

  asyncOpRoot *op = threadLocalQueue.head;
  threadLocalQueue.head = 0;
  threadLocalQueue.tail = 0;
  while (op) {
    currentFinishedSync = 0;
    asyncOpRoot *nextOp = op->next;
    aioObjectRoot *object = op->object;
    objectRelease(op, op->poolId);
    op->finishMethod(op);
    objectDeleteRef(object, 1);
    op = nextOp;
  }
}

void ioCoroutineCall(coroutineTy *coroutine)
{
  coroutineCall(coroutine);
  if (dccObject) {
    if (dccNeedLock)
      combinerCall(dccObject, dccTag, dccOp, dccActionType);
    else
      dccObject->base->methodImpl.combiner(dccObject, dccTag, dccOp, dccActionType);
    dccObject = 0;
  }
}

int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue)
{
  return 0;
}

asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op)
{
  return 0;
}

static inline void startOperation(asyncOpRoot *op, asyncBase *previousOpBase)
{
}

void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid)
{
}
