#ifndef __ASYNCTYPES_H_
#define __ASYNCTYPES_H_

#include "config.h"
#include "asyncio/socket.h"

#if defined(OS_WINDOWS)
#include <Windows.h>

typedef HANDLE iodevTy;
#elif defined(OS_COMMONUNIX)
typedef int iodevTy;
#endif


typedef enum IoObjectTy {
  ioObjectUserEvent = 0,
  ioObjectSocket,
  ioObjectSocketSyn,
  ioObjectDevice
} IoObjectTy;


typedef enum IoActionTy {
  actNoAction = -1,
  actConnect = 0,
  actAccept,
  actRead,
  actWrite,
  actReadMsg,
  actWriteMsg,
  actMonitor,
  actMonitorStop
} IoActionTy;


typedef enum AsyncMethod {
  amOSDefault = 0,
  amSelect,
  amPoll,
  amEPoll,
  amKQueue,
  amIOCP,
} AsyncMethod;


typedef enum AsyncOpStatus {
  aosPending = 0,
  aosSuccess,
  aosTimeout,
  aosDisconnected,
  aosUnknownError,
  aosMonitoring,
  aosLast
} AsyncOpStatus;


typedef enum AsyncFlags {
  afNone = 0,
  afWaitAll = 1,
  afNoCopy = 2
} AsyncFlags;

typedef enum AsyncMonitorState {
  monitorStart = 0,
  monitorStop
} AsyncMonitorState;


typedef struct asyncBase asyncBase;
typedef struct aioObject aioObject;
typedef struct asyncOp asyncOp;
typedef struct aioInfo aioInfo;
typedef struct coroutineTy coroutineTy;
typedef void asyncCb(aioInfo *info);


struct aioInfo {
  aioObject *object;
  IoActionTy currentAction; 
  AsyncOpStatus status;
  
  union {
    asyncCb *callback;
    coroutineTy *coroutine;
  };
  
  void *arg;

  union {
    struct dynamicBuffer *dynamicArray;
    void *buffer;
  };

  AsyncFlags flags;
  size_t transactionSize;
  size_t bytesTransferred;
  socketTy acceptSocket;
  HostAddress host;
};


#endif //__ASYNCTYPES_H_
