#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/api.h"
#include "asyncio/ringBuffer.h"

#define TAGGED_POINTER_DATA_SIZE 6
#define TAGGED_POINTER_ALIGNMENT (((intptr_t)1) << TAGGED_POINTER_DATA_SIZE)
#define TAGGED_POINTER_DATA_MASK (TAGGED_POINTER_ALIGNMENT-1)
#define TAGGED_POINTER_PTR_MASK (~TAGGED_POINTER_DATA_MASK)

typedef enum IoActionTy {
  actAccept = OPCODE_READ,
  actRead,
  actReadMsg,
  actConnect = OPCODE_WRITE,
  actWrite,
  actWriteMsg,
  actUserEvent = OPCODE_OTHER,
} IoActionTy;

typedef void combinerTaskHandlerTy(aioObjectRoot*, asyncOpRoot*, AsyncOpActionTy);
typedef void enqueueOperationTy(asyncBase*, asyncOpRoot*);
typedef void postEmptyOperationTy(asyncBase*);
typedef void nextFinishedOperationTy(asyncBase*);
typedef aioObject *newAioObjectTy(asyncBase*, IoObjectTy, void*);
typedef void deleteObjectTy(aioObject*);
typedef void startTimerTy(asyncOpRoot*);
typedef void stopTimerTy(asyncOpRoot*);
typedef void deleteTimerTy(asyncOpRoot*);
typedef void activateTy(aioUserEvent*);

struct asyncImpl {
  combinerTaskHandlerTy *combinerTaskHandler;
  enqueueOperationTy *enqueue;
  postEmptyOperationTy *postEmptyOperation;
  nextFinishedOperationTy *nextFinishedOperation;
  newAioObjectTy *newAioObject;
  newAsyncOpTy *newAsyncOp;
  aioCancelProc *cancelAsyncOp;
  deleteObjectTy *deleteObject;
  initializeTimerTy *initializeTimer;
  startTimerTy *startTimer;
  stopTimerTy *stopTimer;
  deleteTimerTy *deleteTimer;
  activateTy *activate;
  aioExecuteProc *connect;
  aioExecuteProc *accept;
  aioExecuteProc *read;
  aioExecuteProc *write;
  aioExecuteProc *readMsg;
  aioExecuteProc *writeMsg;
};

struct asyncBase {
  enum AsyncMethod method;
  struct asyncImpl methodImpl;
  struct ConcurrentQueue globalQueue;
  struct pageMap timerMap;
  time_t lastCheckPoint;
  volatile unsigned messageLoopThreadCounter;
  volatile unsigned timerMapLock;

#ifndef NDEBUG
  int opsCount;
#endif
};

struct ioBuffer {
  void *ptr;
  size_t totalSize;
  size_t dataSize;
  size_t offset;
};

struct aioObject {
  aioObjectRoot root;
  union {
    iodevTy hDevice;
    socketTy hSocket;
  };

  struct ioBuffer buffer;
};

struct asyncOp {
  asyncOpRoot root;
  int state;
  void *buffer;
  size_t transactionSize;
  size_t bytesTransferred;
  socketTy acceptSocket;
  HostAddress host;

  void *internalBuffer;
  size_t internalBufferSize;
};

struct aioUserEvent {
  asyncOpRoot root;
  uintptr_t tag;
  asyncBase *base;
  int counter;
  int isSemaphore;
  userEventDestructorCb *destructorCb;
  void *destructorCbArg;
};

void pageMapInit(pageMap *map);
void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void processTimeoutQueue(asyncBase *base, time_t currentTime);

int copyFromBuffer(void *dst, size_t *offset, struct ioBuffer *src, size_t size);
#ifdef __cplusplus
}

#endif
