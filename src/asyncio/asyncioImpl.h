#include "asyncio/api.h"
#include "asyncio/objectPool.h"

typedef enum IoActionTy {
  actNoAction = -1,
  actConnect = 0,
  actAccept,
  actRead,
  actWrite,
  actReadMsg,
  actWriteMsg
} IoActionTy;


typedef void postEmptyOperationTy(asyncBase*);
typedef void nextFinishedOperationTy(asyncBase*);
typedef aioObject *newAioObjectTy(asyncBase*, IoObjectTy, void*);
typedef void deleteObjectTy(aioObject*);
typedef void finishOpTy(asyncOp*);
typedef void startTimerTy(asyncOpRoot*, uint64_t, int);
typedef void stopTimerTy(asyncOpRoot*);
typedef void activateTy(asyncOpRoot*);
typedef void asyncConnectTy(asyncOp*, const HostAddress*);
typedef void asyncAcceptTy(asyncOp*);
typedef void asyncReadTy(asyncOp*);
typedef void asyncWriteTy(asyncOp*);
typedef void asyncReadMsgTy(asyncOp*);
typedef void asyncWriteMsgTy(asyncOp*, const HostAddress*);

struct asyncImpl {
  postEmptyOperationTy *postEmptyOperation;
  nextFinishedOperationTy *nextFinishedOperation;
  newAioObjectTy *newAioObject;
  newAsyncOpTy *newAsyncOp;
  deleteObjectTy *deleteObject;
  finishOpTy *finishOp;
  startTimerTy *startTimer;
  stopTimerTy *stopTimer;
  activateTy *activate;
  asyncConnectTy *connect;
  asyncAcceptTy *accept;
  asyncReadTy *read;
  asyncWriteTy *write;
  asyncReadMsgTy *readMsg;
  asyncWriteMsgTy *writeMsg;
};


struct asyncBase {
  enum AsyncMethod method;
  struct asyncImpl methodImpl;
  struct ObjectPool pool;
  struct pageMap timerMap;
  time_t lastCheckPoint;  
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
};

struct asyncOp {
  asyncOpRoot root;
  void *buffer;
  size_t transactionSize;
  size_t bytesTransferred;
  socketTy acceptSocket;
  HostAddress host;

  void *internalBuffer;
  size_t internalBufferSize;  
};

void pageMapInit(pageMap *map);
asyncOpRoot *pageMapExtractAll(pageMap *map, time_t tm);
void pageMapAdd(pageMap *map, asyncOpRoot *op);
void pageMapRemove(pageMap *map, asyncOpRoot *op);

void processTimeoutQueue(asyncBase *base);
