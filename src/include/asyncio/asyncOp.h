#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct aioObjectRoot aioObjectRoot;
typedef struct asyncOpRoot asyncOpRoot;
typedef struct asyncOp asyncOp;
typedef struct asyncBase asyncBase;

typedef void aioStartProc(asyncOpRoot*);
typedef void aioFinishProc(asyncOpRoot*, int);

typedef struct List {
  asyncOpRoot *head;
  asyncOpRoot *tail;
} List;

typedef struct ListImpl {
  asyncOpRoot *prev;
  asyncOpRoot *next;
} ListImpl;

typedef struct OpRing {
  asyncOpRoot **data;
  size_t size;
  size_t offset;
  uint64_t begin;
  asyncOpRoot *other;
} OpRing;

struct aioObjectRoot {
  List readQueue;
  List writeQueue;
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
};

void opRingInit(OpRing *buffer, size_t size, uint64_t begin);
uint64_t opRingBegin(OpRing *buffer);
asyncOpRoot *opRingGet(OpRing *buffer, uint64_t pt);
void opRingShift(OpRing *buffer, uint64_t newBegin);
void opRingPop(OpRing *buffer, uint64_t pt);
void opRingPush(OpRing *buffer, asyncOpRoot *op, uint64_t pt);


// Must be thread-safe
int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue);
asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op);

void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op);
void processTimeoutQueue(asyncBase *base);

void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_ASYNCOP_H_
