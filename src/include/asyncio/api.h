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
  afSerialized = 16
} AsyncFlags;

typedef enum AsyncOpActionTy {
  aaNone = 0,
  aaStart,
  aaCancel,
  aaFinish,
  aaContinue
} AsyncOpActionTy;

#ifdef __cplusplus
__NO_UNUSED_FUNCTION_BEGIN
static inline AsyncFlags operator|(AsyncFlags a, AsyncFlags b) {
  return static_cast<AsyncFlags>(static_cast<int>(a) | static_cast<int>(b));
}
__NO_UNUSED_FUNCTION_END
#endif


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

extern __tls List threadLocalQueue;
extern __tls unsigned currentFinishedSync;
extern __tls unsigned messageLoopThreadId;

#ifndef __cplusplus
#define TAG(x) ((tag_t)x)
#else
#define TAG(x) static_cast<tag_t>(x)
#endif

#define TAG_READ (TAG(1) << (sizeof(tag_t)*8 - 2))
#define TAG_READ_MASK (TAG(3) << (sizeof(tag_t)*8 - 2))
#define TAG_WRITE (TAG(1) << (sizeof(tag_t)*8 - 4))
#define TAG_WRITE_MASK (TAG(3) << (sizeof(tag_t)*8 - 4))
#define TAG_ERROR (TAG(1) << (sizeof(tag_t)*8 - 6))
#define TAG_ERROR_MASK (TAG(3) << (sizeof(tag_t)*8 - 6))
#define TAG_DELETE (TAG(1) << (sizeof(tag_t)*8 - 8))
#define TAG_CANCELIO (TAG(1) << (sizeof(tag_t)*8 - 9))

#define OPCODE_READ 0
#define OPCODE_WRITE (1<<(sizeof(int)*8-4))
#define OPCODE_OTHER (1<<(sizeof(int)*8-2))



void *__tagged_alloc(size_t size);
void *__tagged_pointer_make(void *ptr, tag_t data);
void __tagged_pointer_decode(void *ptr, void **outPtr, tag_t *outData);

void eqRemove(List *list, asyncOpRoot *op);
void eqPushBack(List *list, asyncOpRoot *op);
void fnPushBack(List *list, asyncOpRoot *op);

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
  int running;
};

void initObjectRoot(aioObjectRoot *object, asyncBase *base, IoObjectTy type, aioObjectDestructor destructor);



void cancelIo(aioObjectRoot *object);
void objectDelete(aioObjectRoot *object);

tag_t opGetGeneration(asyncOpRoot *op);
AsyncOpStatus opGetStatus(asyncOpRoot *op);
int opSetStatus(asyncOpRoot *op, tag_t tag, AsyncOpStatus status);
void opForceStatus(asyncOpRoot *op, AsyncOpStatus status);
tag_t opEncodeTag(asyncOpRoot *op, tag_t tag);

void opRelease(asyncOpRoot *op, AsyncOpStatus status, List *executeList, List *finished);
void processAction(asyncOpRoot *opptr, AsyncOpActionTy actionType, List *finished, tag_t *needStart);
void processOperationList(aioObjectRoot *object, List *finished, tag_t *needStart, tag_t *enqueued);
void executeOperationList(List *list, List *finished);
void cancelOperationList(List *list, List *finished, AsyncOpStatus status);

void combinerAddAction(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy actionType);
void combinerCall(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);

typedef struct combinerCallArgs {
  aioObjectRoot *object;
  tag_t tag;
  asyncOpRoot *op;
  AsyncOpActionTy actionType;
  int needLock;
} combinerCallArgs;

void combinerCallDelayed(combinerCallArgs *args, aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType, int needLock);

void opStart(asyncOpRoot *op);
void opCancel(asyncOpRoot *op, tag_t generation, AsyncOpStatus status);
void resumeParent(asyncOpRoot *op, AsyncOpStatus status);

void addToThreadLocalQueue(asyncOpRoot *op);
void executeThreadLocalQueue();

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

#endif //__ASYNCIO_ASYNCOP_H_
