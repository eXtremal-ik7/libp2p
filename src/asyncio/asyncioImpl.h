#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/api.h"
#include "asyncio/objectPool.h"

typedef enum IoActionTy {
  actAccept = OPCODE_READ,
  actRead,
  actReadMsg,
  actConnect = OPCODE_WRITE,
  actWrite,
  actWriteMsg,
  actUserEvent = OPCODE_OTHER,
  actEmpty
} IoActionTy;

typedef void combinerTy(aioObjectRoot*, tag_t, asyncOpRoot*, AsyncOpActionTy);
typedef void wakeupOperationTy(asyncBase*);
typedef void postEmptyOperationTy(asyncBase*);
typedef void nextFinishedOperationTy(asyncBase*);
typedef aioObject *newAioObjectTy(asyncBase*, IoObjectTy, void*);
typedef void deleteObjectTy(aioObject*);
typedef void initializeTimerTy(asyncBase*, asyncOpRoot*);
typedef void startTimerTy(asyncOpRoot*);
typedef void stopTimerTy(asyncOpRoot*);
typedef void activateTy(aioUserEvent*);

struct asyncImpl {
  combinerTy *combiner;
  wakeupOperationTy *wakeup;
  postEmptyOperationTy *postEmptyOperation;
  nextFinishedOperationTy *nextFinishedOperation;
  newAioObjectTy *newAioObject;
  newAsyncOpTy *newAsyncOp;
  aioCancelProc *cancelAsyncOp;
  deleteObjectTy *deleteObject;
  initializeTimerTy *initializeTimer;
  startTimerTy *startTimer;
  stopTimerTy *stopTimer;
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
  struct pageMap timerMap;
  time_t lastCheckPoint;
  volatile unsigned messageLoopThreadCounter;
  volatile unsigned timerMapLock;
  asyncOpRoot *globalQueue;
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
  tag_t tag;
  asyncBase *base;
  int counter;
  userEventDestructorCb *destructorCb;
  void *destructorCbArg;
};

void pageMapInit(pageMap *map);
void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void processTimeoutQueue(asyncBase *base, time_t currentTime);

int copyFromBuffer(void *dst, size_t *offset, struct ioBuffer *src, size_t size);
#ifdef __cplusplus
}

#endif
