#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "macro.h"
#include "asyncio/asyncioTypes.h"

#define MAX_SYNCHRONOUS_FINISHED_OPERATION 32

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
  aosUnknown = -1,
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
  afActiveOnce = 8,
  afRunning = 16,
  afCoroutine = 32
} AsyncFlags;

typedef enum AsyncOpActionTy {
  aaNone = 0,
  aaStart,
  aaCancel,
  aaFinish,
  aaContinue
} AsyncOpActionTy;

typedef enum AsyncOpRunningTy {
  arWaiting = 0,
  arRunning,
  arCancelling
} AsyncOpRunningTy;

#ifdef __cplusplus
__NO_UNUSED_FUNCTION_BEGIN
static inline AsyncFlags operator|(AsyncFlags a, AsyncFlags b) {
  return static_cast<AsyncFlags>(static_cast<int>(a) | static_cast<int>(b));
}
__NO_UNUSED_FUNCTION_END
#endif

typedef struct ObjectPool ObjectPool;
typedef struct asyncBase asyncBase;
typedef struct aioObjectRoot aioObjectRoot;
typedef struct asyncOpRoot asyncOpRoot;
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
typedef int aioCancelProc(asyncOpRoot*);
typedef void aioFinishProc(asyncOpRoot*);
typedef void aioObjectDestructor(aioObjectRoot*);
typedef void aioObjectDestructorCb(aioObjectRoot*, void*);
typedef void userEventDestructorCb(aioUserEvent*, void*);

extern __tls unsigned currentFinishedSync;
extern __tls unsigned messageLoopThreadId;

#ifndef __cplusplus
#define TAG(x) ((tag_t)x)
#else
#define TAG(x) static_cast<tag_t>(x)
#endif

#define TAG_STATUS_SIZE 8
#define TAG_STATUS_MASK ((((tag_t)1) << TAG_STATUS_SIZE)-1)
#define TAG_GENERATION_MASK (~TAG_STATUS_MASK)

#define TAG_READ (TAG(1) << (sizeof(tag_t)*8 - 2))
#define TAG_READ_MASK (TAG(3) << (sizeof(tag_t)*8 - 2))
#define TAG_WRITE (TAG(1) << (sizeof(tag_t)*8 - 4))
#define TAG_WRITE_MASK (TAG(3) << (sizeof(tag_t)*8 - 4))
#define TAG_ERROR (TAG(1) << (sizeof(tag_t)*8 - 6))
#define TAG_ERROR_MASK (TAG(3) << (sizeof(tag_t)*8 - 6))
#define TAG_DELETE (TAG(1) << (sizeof(tag_t)*8 - 8))
#define TAG_CANCELIO (TAG(1) << (sizeof(tag_t)*8 - 9))

// Event tag area
#if defined(OS_32)
#define TAG_EVENT_OP          TAG(0x00010000UL)
#define TAG_EVENT_DELETE      TAG(0x10000000UL)
#define TAG_EVENT_OP_MASK     TAG(0xFFFF0000UL)
#define TAG_EVENT_MASK        TAG(0x0FFFFFFFUL)
#define TAG_EVENT_DELETE_MASK TAG(0xF0000000UL)
#elif defined(OS_64)
#define TAG_EVENT_OP          TAG(0x0000000100000000ULL)
#define TAG_EVENT_DELETE      TAG(0x1000000000000000ULL)
#define TAG_EVENT_OP_MASK     TAG(0xFFFFFFFF00000000ULL)
#define TAG_EVENT_MASK        TAG(0x0FFFFFFFFFFFFFFFULL)
#define TAG_EVENT_DELETE_MASK TAG(0xF000000000000000ULL)
#else
#error Configution incomplete
#endif

#define OPCODE_READ 0
#define OPCODE_WRITE (1<<(sizeof(int)*8-4))
#define OPCODE_OTHER (1<<(sizeof(int)*8-2))

tag_t objectIncrementReference(aioObjectRoot *object, tag_t count);
tag_t objectDecrementReference(aioObjectRoot *object, tag_t count);
tag_t eventIncrementReference(aioUserEvent *event, tag_t tag);
tag_t eventDecrementReference(aioUserEvent *event, tag_t tag);
int eventTryActivate(aioUserEvent *event);
void eventDeactivate(aioUserEvent *event);

void *__tagged_alloc(size_t size);
void *__tagged_pointer_make(void *ptr, tag_t data);
void __tagged_pointer_decode(void *ptr, void **outPtr, tag_t *outData);

void eqRemove(List *list, asyncOpRoot *op);
void eqPushBack(List *list, asyncOpRoot *op);

__NO_UNUSED_FUNCTION_BEGIN
static inline tag_t __tag_get_opcount(tag_t tag)
{
  return tag & ~(TAG_READ_MASK | TAG_WRITE_MASK | TAG_ERROR_MASK | TAG_DELETE | TAG_CANCELIO);
}

static inline tag_t __tag_make_processed(tag_t currentTag, tag_t enqueued)
{
  return (currentTag & (TAG_READ_MASK | TAG_WRITE_MASK | TAG_ERROR_MASK | TAG_DELETE | TAG_CANCELIO)) | enqueued;
}
__NO_UNUSED_FUNCTION_END

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
  aioObjectDestructorCb *destructorCb;
  void *destructorCbArg;
};

struct asyncOpRoot {
  volatile tag_t tag;
  const char *poolId;
  aioExecuteProc *executeMethod;
  aioCancelProc *cancelMethod;
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
  AsyncOpRunningTy running;
};

void initObjectRoot(aioObjectRoot *object, asyncBase *base, IoObjectTy type, aioObjectDestructor destructor);
void objectSetDestructorCb(aioObjectRoot *object, aioObjectDestructorCb callback, void *arg);
void eventSetDestructorCb(aioUserEvent *event, userEventDestructorCb callback, void *arg);

void cancelIo(aioObjectRoot *object);
void objectDelete(aioObjectRoot *object);

tag_t opGetGeneration(asyncOpRoot *op);
AsyncOpStatus opGetStatus(asyncOpRoot *op);
int opSetStatus(asyncOpRoot *op, tag_t tag, AsyncOpStatus status);
void opForceStatus(asyncOpRoot *op, AsyncOpStatus status);
tag_t opEncodeTag(asyncOpRoot *op, tag_t tag);

void opRelease(asyncOpRoot *op, AsyncOpStatus status, List *executeList);
void processAction(asyncOpRoot *opptr, AsyncOpActionTy actionType, tag_t *needStart);
void processOperationList(aioObjectRoot *object, tag_t *needStart, tag_t *enqueued);
void executeOperationList(List *list);
void cancelOperationList(List *list, AsyncOpStatus status);

void combinerAddAction(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy actionType);
void combinerCall(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);
void combinerCallWithoutLock(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy action);

void opStart(asyncOpRoot *op);
void opCancel(asyncOpRoot *op, tag_t generation, AsyncOpStatus status);
void resumeParent(asyncOpRoot *op, AsyncOpStatus status);

void addToGlobalQueue(asyncOpRoot *op);
int executeGlobalQueue(asyncBase *base);

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
                             uint64_t timeout);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include "atomic.h"
#include "asyncio/coroutine.h"

template<typename f1, typename f2, typename f3, typename f4>
static inline void aioMethod(f1 createProc,
                             f2 implCallProc,
                             f3 makeResultProc,
                             f4 initOpProc,
                             aioObjectRoot *object,
                             AsyncFlags flags,
                             void *callback,
                             int opCode)
{
  if (__tag_atomic_fetch_and_add(&object->tag, 1) == 0) {
    List &list = !(opCode & OPCODE_WRITE) ? object->readQueue : object->writeQueue;
    if (!list.head) {
      asyncOpRoot *op = implCallProc();
      if (!op) {
        tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
        if (tag)
          combinerCallWithoutLock(object, tag, nullptr, aaNone);
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && (callback == nullptr || flags & afActiveOnce)) {
          makeResultProc();
        } else {
          asyncOpRoot *op = createProc();
          initOpProc(op);
          opForceStatus(op, aosSuccess);
          addToGlobalQueue(op);
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallWithoutLock(object, 1, op, aaStart);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
          if (tag)
            combinerCallWithoutLock(object, tag, nullptr, aaNone);
          addToGlobalQueue(op);
        }
      }
    } else {
      eqPushBack(&list, createProc());
      tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
      if (tag)
        combinerCallWithoutLock(object, tag, nullptr, aaNone);
    }
  } else {
    combinerAddAction(object, createProc(), aaStart);
  }
}

template<typename f1, typename f2, typename f3>
static inline asyncOpRoot *ioMethod(f1 createProc,
                                    f2 implCallProc,
                                    f3 initOpProc,
                                    aioObjectRoot *object,
                                    int opCode)
{
  asyncOpRoot *op;
  if (__tag_atomic_fetch_and_add(&object->tag, 1) == 0) {
    List &list = !(opCode & OPCODE_WRITE) ? object->readQueue : object->writeQueue;
    if (!list.head) {
      op = implCallProc();
      if (!op) {
        tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
        if (tag)
          combinerCallWithoutLock(object, tag, nullptr, aaNone);
        if (++currentFinishedSync < MAX_SYNCHRONOUS_FINISHED_OPERATION && !tag) {
          // nothing to do
        } else {
          op = createProc();
          initOpProc(op);
          opForceStatus(op, aosSuccess);
          addToGlobalQueue(op);
        }
      } else {
        if (opGetStatus(op) == aosPending) {
          combinerCallWithoutLock(object, 1, op, aaStart);
        } else {
          tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
          if (tag)
            combinerCallWithoutLock(object, tag, nullptr, aaNone);
          addToGlobalQueue(op);
        }
      }
    } else {
      op = createProc();
      eqPushBack(&list, op);
      tag_t tag = __tag_atomic_fetch_and_add(&object->tag, static_cast<tag_t>(0) - 1) - 1;
      if (tag)
        combinerCallWithoutLock(object, tag, nullptr, aaNone);
    }
  } else {
    op = createProc();
    combinerAddAction(object, op, aaStart);
  }

  if (op)
    coroutineYield();
  return op;
}
#endif

#endif //__ASYNCIO_ASYNCOP_H_
