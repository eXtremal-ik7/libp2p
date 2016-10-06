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
typedef void startTimerTy(asyncOpRoot*, uint64_t, int);
typedef void stopTimerTy(asyncOpRoot*);
typedef void activateTy(asyncOpRoot*);
typedef void asyncConnectTy(asyncOp*, const HostAddress*, uint64_t);
typedef void asyncAcceptTy(asyncOp*, uint64_t);
typedef void asyncReadTy(asyncOp*, uint64_t);
typedef void asyncWriteTy(asyncOp*, uint64_t);
typedef void asyncReadMsgTy(asyncOp*, uint64_t);
typedef void asyncWriteMsgTy(asyncOp*, const HostAddress*, uint64_t);
typedef void asyncMonitorTy(asyncOp*);
typedef void asyncMonitorStopTy(asyncOp*);


struct asyncImpl {
  postEmptyOperationTy *postEmptyOperation;
  nextFinishedOperationTy *nextFinishedOperation;
  newAioObjectTy *newAioObject;
  newAsyncOpTy *newAsyncOp;
  deleteObjectTy *deleteObject;
  startTimerTy *startTimer;
  stopTimerTy *stopTimer;
  activateTy *activate;
  asyncConnectTy *connect;
  asyncAcceptTy *accept;
  asyncReadTy *read;
  asyncWriteTy *write;
  asyncReadMsgTy *readMsg;
  asyncWriteMsgTy *writeMsg;
  asyncMonitorTy *monitor;
  asyncMonitorStopTy *montitorStop;
};


struct asyncBase {
  enum AsyncMethod method;
  struct asyncImpl methodImpl;
  struct ObjectPool pool;
  OpRing timeGrid;
#ifndef NDEBUG
  int opsCount;
#endif
};


struct aioObject {
  aioObjectRoot root;
  asyncBase *base;
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
};
