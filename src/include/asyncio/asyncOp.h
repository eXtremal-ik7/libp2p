#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct aioObjectRoot aioObjectRoot;
typedef struct aioOpRoot aioOpRoot;
typedef struct asyncOp asyncOp;
typedef struct asyncBase asyncBase;

typedef void aioStartProc(aioOpRoot*);
typedef void aioFinishProc(aioOpRoot*, int);

typedef struct List {
  aioOpRoot *head;
  aioOpRoot *tail;
} List;

typedef struct ListImpl {
  aioOpRoot *prev;
  aioOpRoot *next;
} ListImpl;

typedef struct OpRing {
  aioOpRoot **data;
  size_t size;
  size_t offset;
  uint64_t begin;
  aioOpRoot *other;
} OpRing;

struct aioObjectRoot {
  List readQueue;
  List writeQueue;
};

struct aioOpRoot {
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
};

void opRingInit(OpRing *buffer, size_t size, uint64_t begin);
uint64_t opRingBegin(OpRing *buffer);
aioOpRoot *opRingGet(OpRing *buffer, uint64_t pt);
void opRingShift(OpRing *buffer, uint64_t newBegin);
void opRingPop(OpRing *buffer, uint64_t pt);
void opRingPush(OpRing *buffer, aioOpRoot *op, uint64_t pt);


// Must be thread-safe
int addToExecuteQueue(aioObjectRoot *object, aioOpRoot *op, int isWriteQueue);
aioOpRoot *removeFromExecuteQueue(aioOpRoot *op);

void addToTimeoutQueue(asyncBase *base, aioOpRoot *op);
void removeFromTimeoutQueue(asyncBase *base, aioOpRoot *op);
void processTimeoutQueue(asyncBase *base);

void finishOperation(aioOpRoot *op, int status, int needRemoveFromTimeGrid);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_ASYNCOP_H_
