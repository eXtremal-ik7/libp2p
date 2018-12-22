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
  actUserEvent = OPCODE_OTHER
} IoActionTy;

typedef void combinerTy(aioObjectRoot*, tag_t, asyncOpRoot*, AsyncOpActionTy);
typedef void postEmptyOperationTy(asyncBase*);
typedef void nextFinishedOperationTy(asyncBase*);
typedef aioObject *newAioObjectTy(asyncBase*, IoObjectTy, void*);
typedef void deleteObjectTy(aioObject*);
typedef void finishOpTy(asyncOpRoot*, tag_t, AsyncOpStatus);
typedef void initializeTimerTy(asyncBase*, asyncOpRoot*);
typedef void startTimerTy(asyncOpRoot*, uint64_t, int);
typedef void stopTimerTy(asyncOpRoot*);
typedef void activateTy(aioUserEvent*);

struct asyncImpl {
  combinerTy *combiner;
  postEmptyOperationTy *postEmptyOperation;
  nextFinishedOperationTy *nextFinishedOperation;
  newAioObjectTy *newAioObject;
  newAsyncOpTy *newAsyncOp;
  deleteObjectTy *deleteObject;
  finishOpTy *finishOp;
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
#ifndef NDEBUG
  int opsCount;
#endif
};


struct aioObject {
  aioObjectRoot root;
  union {
    iodevTy hDevice;
    socketTy hSocket;
  };
  union {
    void *ptr;
    int32_t i32;
    uint32_t u32;
  };
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
  asyncBase *base;
  uint64_t timeout;
  int counter;
};

void pageMapInit(pageMap *map);
asyncOpListLink *pageMapExtractAll(pageMap *map, time_t tm);
void pageMapAdd(pageMap *map, asyncOpListLink *op);
int pageMapRemove(pageMap *map, asyncOpListLink *op);
void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void processTimeoutQueue(asyncBase *base, time_t currentTime);

#ifdef __cplusplus
}
#endif
