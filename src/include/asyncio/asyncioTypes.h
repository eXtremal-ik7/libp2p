#ifndef __ASYNCTYPES_H_
#define __ASYNCTYPES_H_

#include "config.h"
#include "asyncio/socket.h"
#include "asyncio/asyncOp.h"

#if defined(OS_WINDOWS)
#include <windows.h>

typedef HANDLE iodevTy;
#elif defined(OS_COMMONUNIX)
typedef int iodevTy;
#endif


typedef enum IoObjectTy {
  ioObjectUserEvent = 0,
  ioObjectSocket,
  ioObjectDevice,
  ioObjectUserDefined
} IoObjectTy;


typedef enum IoActionTy {
  actNoAction = -1,
  actConnect = 0,
  actAccept,
  actRead,
  actWrite,
  actReadMsg,
  actWriteMsg
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
  aosCanceled,
  aosBufferTooSmall,
  aosUnknownError,
  aosLast
} AsyncOpStatus;


typedef enum AsyncFlags {
  afNone = 0,
  afWaitAll = 1,
  afNoCopy = 2,
  afRealtime = 4
} AsyncFlags;


typedef struct asyncBase asyncBase;
typedef struct aioObject aioObject;
typedef struct asyncOp asyncOp;
typedef struct coroutineTy coroutineTy;

#endif //__ASYNCTYPES_H_
