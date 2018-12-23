#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "asyncio/asyncioTypes.h"

#define MAX_SYNCHRONOUS_FINISHED_OPERATION 32

#if defined(OS_32)
typedef uint32_t tag_t;
#elif defined(OS_64)
typedef uint64_t tag_t;
#else
#error Configution incomplete
#endif

typedef enum AsyncMethod {
  amOSDefault = 0,
  amSelect,
  amPoll,
  amEPoll,
  amKQueue,
  amIOCP,
} AsyncMethod;


typedef enum IoObjectTy {
  ioObjectSocket,
  ioObjectDevice,
  ioObjectTimer,
  ioObjectUserDefined
} IoObjectTy;


typedef enum AsyncOpStatus {
  aosSuccess = 0,
  aosPending,
  aosTimeout,
  aosDisconnected,
  aosCanceled,
  aosBufferTooSmall,
  aosUnknownError,
  aosLast
} AsyncOpStatus;


typedef enum AsyncFlags {
  afNone = 0,
  afWaitAll = 1,
  afNoCopy = 2,
  afRealtime = 4,
  afActiveOnce = 8
} AsyncFlags;

typedef enum AsyncOpActionTy {
  aaNone = 0,
  aaStart,
  aaFinish,
  aaIOCPRestart,
  aaIOCPCancel
} AsyncOpActionTy;

typedef struct asyncBase asyncBase;
typedef struct aioObjectRoot aioObjectRoot;
typedef struct asyncOpRoot asyncOpRoot;
typedef struct asyncOpLink asyncOpLink;
typedef struct asyncOpListLink asyncOpListLink;
typedef struct asyncOpAction asyncOpAction;
typedef struct coroutineTy coroutineTy;

typedef struct aioObject aioObject;
typedef struct aioUserEvent aioUserEvent;
typedef struct asyncOp asyncOp;

typedef struct List {
  asyncOpRoot *head;
  asyncOpRoot *tail;
} List;

typedef struct ListMt {
  asyncOpAction *head;
  asyncOpAction *tail;
  unsigned lock;
} ListMt;

typedef void processOperationCb(asyncOpRoot*, AsyncOpActionTy, List*, tag_t*);
typedef asyncOpRoot *newAsyncOpTy();
typedef AsyncOpStatus aioExecuteProc(asyncOpRoot*);
typedef void aioFinishProc(asyncOpRoot*);
typedef void aioObjectDestructor(aioObjectRoot*);

extern __tls List threadLocalQueue;
extern __tls unsigned currentFinishedSync;
extern __tls unsigned messageLoopThreadId;

#define TAG_READ (((tag_t)1) << (sizeof(tag_t)*8 - 2))
#define TAG_READ_MASK (((tag_t)3) << (sizeof(tag_t)*8 - 2))
#define TAG_WRITE (((tag_t)1) << (sizeof(tag_t)*8 - 4))
#define TAG_WRITE_MASK (((tag_t)3) << (sizeof(tag_t)*8 - 4))
#define TAG_ERROR (((tag_t)1) << (sizeof(tag_t)*8 - 6))
#define TAG_ERROR_MASK (((tag_t)3) << (sizeof(tag_t)*8 - 6))
#define TAG_DELETE (((tag_t)1) << (sizeof(tag_t)*8 - 8))
#define TAG_CANCELIO (((tag_t)1) << (sizeof(tag_t)*8 - 9))

#define OPCODE_READ 0
#define OPCODE_WRITE (1<<(sizeof(int)*8-2))
#define OPCODE_OTHER (1<<(sizeof(int)*8-1))

static inline tag_t __tag_atomic_fetch_and_add(tag_t volatile *tag, tag_t value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(tag, value);
#else
#ifdef OS_32
  return InterlockedExchangeAdd((volatile LONG*)tag, value);
#else
  return InterlockedExchangeAdd64((volatile LONG64*)tag, value);
#endif
#endif
}

static inline unsigned __uint_atomic_fetch_and_add(unsigned volatile *tag, unsigned value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(tag, value);
#else
  return InterlockedExchangeAdd((volatile LONG*)tag, value);
#endif
}

static inline int __uint_atomic_compare_and_swap(unsigned volatile *tag, unsigned v1, unsigned v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
  return InterlockedCompareExchange((volatile LONG*)tag, v2, v1) == v1;
#endif
}

static inline int __tag_atomic_compare_and_swap(tag_t volatile *tag, tag_t v1, tag_t v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
#ifdef OS_32
  return InterlockedCompareExchange((volatile LONG*)tag, v2, v1) == v1;
#else
  return InterlockedCompareExchange64((volatile LONG64*)tag, v2, v1) == v1;
#endif
#endif
}

static inline int __pointer_atomic_compare_and_swap(void *volatile *tag, void *v1, void *v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
  return InterlockedCompareExchangePointer(tag, v2, v1) == v1;
#endif
}

void *__tagged_alloc(size_t size);
void *__tagged_pointer_make(void *ptr, tag_t data);
void __tagged_pointer_decode(void *ptr, void **outPtr, tag_t *outData);

void eqRemove(List *list, asyncOpRoot *op);
void eqPushBack(List *list, asyncOpRoot *op);
void fnPushBack(List *list, asyncOpRoot *op);

static inline tag_t __tag_get_opcount(tag_t tag)
{
  return tag & ~(TAG_READ_MASK | TAG_WRITE_MASK | TAG_ERROR_MASK | TAG_DELETE | TAG_CANCELIO);
}

static inline tag_t __tag_make_processed(tag_t currentTag, tag_t enqueued)
{
  return (currentTag & (TAG_READ_MASK | TAG_WRITE_MASK | TAG_ERROR_MASK | TAG_DELETE | TAG_CANCELIO)) | enqueued;
}

static inline void __spinlock_acquire(unsigned *lock)
{
  for (;;) {
    for (int i = 0; i < 7777; i++) {
      if (__uint_atomic_compare_and_swap(lock, 0, 1))
        return;
    }
#ifdef OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
  }
}

static inline int __spinlock_try_acquire(volatile unsigned *lock)
{
  return __uint_atomic_compare_and_swap(lock, 0, 1) ? 1 : 0;
}

static inline void __spinlock_release(volatile unsigned *lock)
{
  *lock = 0;
}

typedef struct asyncOpLink {
  asyncOpRoot *op;
  tag_t tag;
} asyncOpLink;

typedef struct asyncOpListLink {
  asyncOpRoot *op;
  tag_t tag;
  asyncOpListLink *prev;
  asyncOpListLink *next;
} asyncOpListLink;

typedef struct asyncOpAction {
  asyncOpRoot *op;
  AsyncOpActionTy actionType;
  asyncOpAction *next;
} asyncOpAction;

typedef struct ListImpl {
  asyncOpRoot *prev;
  asyncOpRoot *next;
} ListImpl;

typedef struct pageMap {
  asyncOpListLink ***map;
  unsigned lock;
} pageMap;

struct aioObjectRoot {
  asyncBase *base;
  volatile tag_t tag;
  tag_t refs;
  List readQueue;
  List writeQueue;
  ListMt announcementQueue;
  IoObjectTy type;
  aioObjectDestructor *destructor;
};

struct asyncOpRoot {
  volatile tag_t tag;
  const char *poolId;
  aioExecuteProc *executeMethod;
  aioFinishProc *finishMethod;  
  ListImpl executeQueue;
  asyncOpRoot *next;
  aioObjectRoot *object;
  void *callback;
  void *arg;
  int opCode;
  AsyncFlags flags;
  void *timerId;
  union {
    uint64_t timeout;
    uint64_t endTime;
  };
};

void initObjectRoot(aioObjectRoot *object, asyncBase *base, IoObjectTy type, aioObjectDestructor destructor);



void cancelIo(aioObjectRoot *object);
void objectAddRef(aioObjectRoot *object);
void objectDeleteRef(aioObjectRoot *object, tag_t count);

tag_t opGetGeneration(asyncOpRoot *op);
AsyncOpStatus opGetStatus(asyncOpRoot *op);
int opSetStatus(asyncOpRoot *op, tag_t tag, AsyncOpStatus status);
void opForceStatus(asyncOpRoot *op, AsyncOpStatus status);
tag_t opEncodeTag(asyncOpRoot *op, tag_t tag);

void opRelease(asyncOpRoot *op, AsyncOpStatus status, List *executeList, List *finished);
void processOperationList(aioObjectRoot *object, List *finished, tag_t *needStart, processOperationCb *processCb, tag_t *enqueued);
void executeOperationList(List *list, List *finished);
void cancelOperationList(List *list, List *finished, AsyncOpStatus status);

void combinerAddAction(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy actionType);
void combinerCall(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);
void combinerCallDelayed(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType, int needLock);
void opStart(asyncOpRoot *op);
void opCancel(asyncOpRoot *op, tag_t generation, AsyncOpStatus status);

void addToThreadLocalQueue(asyncOpRoot *op);
void executeThreadLocalQueue();

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
                             uint64_t timeout);

void ioCoroutineCall(coroutineTy *coroutine);

// Must be thread-safe
int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue);
void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op);
asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op);
void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_ASYNCOP_H_
