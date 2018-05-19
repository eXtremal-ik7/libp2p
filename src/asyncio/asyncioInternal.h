#include "asyncio/asyncOp.h"
#include "asyncio/asyncioTypes.h"
#include "asyncio/objectPool.h"
#include "cstl.h"
#include <stddef.h>
#include <stdint.h>

struct dynamicBuffer;

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

  union {
    struct dynamicBuffer *dynamicArray;
    void *buffer;
  };

  size_t transactionSize;
  size_t bytesTransferred;
  socketTy acceptSocket;
  HostAddress host;

  void *internalBuffer;
  size_t internalBufferSize;  
};
