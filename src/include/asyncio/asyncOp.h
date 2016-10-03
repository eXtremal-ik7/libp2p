#ifndef __ASYNCIO_ASYNCOP_H_
#define __ASYNCIO_ASYNCOP_H_

#include <stddef.h>
#include <stdint.h>

typedef struct aioObjectRoot aioObjectRoot;
typedef struct aioOpRoot aioOpRoot;
typedef struct asyncBase asyncBase;

typedef void aioExceptionCb(int, void*, void*);

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
  aioOpRoot *head;
  aioOpRoot *tail;
};

struct aioOpRoot {
  // constant members
  asyncBase *base;
  const char *poolId;
  aioExceptionCb *exceptionCallback;  
  
  ListImpl executeQueue;
  ListImpl timeoutQueue;
  aioObjectRoot *object;
  void *callback;
  void *arg;
  int opCode;  
  int threadId;
  uint64_t endTime;
};

void opRingInit(OpRing *buffer, size_t size, uint64_t begin);
uint64_t opRingBegin(OpRing *buffer);
aioOpRoot *opRingGet(OpRing *buffer, uint64_t pt);
void opRingShift(OpRing *buffer, uint64_t newBegin);
void opRingPop(OpRing *buffer, uint64_t pt);
void opRingPush(OpRing *buffer, aioOpRoot *op, uint64_t pt);


// Must be thread-safe
void addToExecuteQueue(aioObjectRoot *object, aioOpRoot *op);
void removeFromExecuteQueue(aioOpRoot *op);

void addToTimeoutQueue(asyncBase *base, aioOpRoot *op);
void removeFromTimeoutQueue(asyncBase *base, aioOpRoot *op);
void processTimeoutQueue(asyncBase *base);

#endif //__ASYNCIO_ASYNCOP_H_
