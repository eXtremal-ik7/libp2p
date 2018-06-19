#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include "asyncio/asyncioTypes.h"

typedef struct asyncBase asyncBase;
typedef struct aioObjectRoot aioObjectRoot;
typedef struct asyncOpRoot asyncOpRoot;
typedef struct coroutineTy coroutineTy;

typedef struct aioObject aioObject;
typedef struct asyncOp asyncOp;

typedef asyncOpRoot *newAsyncOpTy(asyncBase*);
typedef void aioStartProc(asyncOpRoot*);
typedef void aioFinishProc(asyncOpRoot*, int);
typedef void aioObjectDestructor(aioObjectRoot*);

typedef enum AsyncMethod {
  amOSDefault = 0,
  amSelect,
  amPoll,
  amEPoll,
  amKQueue,
  amIOCP,
} AsyncMethod;


typedef enum IoObjectTy {
  ioObjectUserEvent = 0,
  ioObjectSocket,
  ioObjectDevice,
  ioObjectUserDefined
} IoObjectTy;


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


typedef struct List {
  asyncOpRoot *head;
  asyncOpRoot *tail;
} List;

typedef struct ListImpl {
  asyncOpRoot *prev;
  asyncOpRoot *next;
} ListImpl;

typedef struct pageMap {
  asyncOpRoot ***map;
} pageMap;

struct aioObjectRoot {
  List readQueue;
  List writeQueue;
  int type;
  int links;
  aioObjectDestructor *destructor;
};

struct asyncOpRoot {
  // constant members
  asyncBase *base;
  const char *poolId;
  aioStartProc *startMethod;
  aioFinishProc *finishMethod;  
  
  ListImpl executeQueue;
  ListImpl timeoutQueue;
  aioObjectRoot *object;
  void *callback;
  void *arg;
  int opCode;
  int flags;
  uint64_t endTime;
  timerTy timerId;
  int counter;  
};

aioObjectRoot *initObjectRoot(int type, size_t size, aioObjectDestructor destructor);
void checkForDeleteObject(aioObjectRoot *object);
void cancelIo(aioObjectRoot *object, asyncBase *base);

asyncOpRoot *initAsyncOpRoot(asyncBase *base,
                             const char *nonTimerPool,
                             const char *timerPool,
                             newAsyncOpTy *newOpProc,
                             aioStartProc *startMethod,
                             aioFinishProc *finishMethod,
                             aioObjectRoot *object,
                             void *callback,
                             void *arg,
                             int flags,
                             int opCode,
                             uint64_t timeout);

// Must be thread-safe
int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue);
asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op);
void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_ASYNCOP_H_
