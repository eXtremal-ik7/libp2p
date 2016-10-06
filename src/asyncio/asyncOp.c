#include "asyncio/asyncOp.h"
#include "asyncioInternal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static inline uint64_t getPt(uint64_t endTime)
{
  return (endTime / 1000000) + (endTime % 1000000 != 0);
}

void opRingInit(OpRing *buffer, size_t size, uint64_t begin)
{
  buffer->data = calloc(size, sizeof(void*));
  buffer->size = size;
  buffer->begin = begin;
  buffer->offset = 0;
  buffer->other = 0;
}

uint64_t opRingBegin(OpRing *buffer)
{
  return buffer->begin;
}

asyncOpRoot *opRingGet(OpRing *buffer, uint64_t pt)
{
  uint64_t distance = pt - buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    return buffer->data[index];
  } else {
    return 0;
  }
}

void opRingShift(OpRing *buffer, uint64_t newBegin)
{
  uint64_t distance = newBegin - buffer->begin;
  if (distance == 0)
    return;
  
  size_t newOffset = buffer->offset + distance;
  if (distance < buffer->size) {
    size_t d1;
    size_t d2;
    int rotate = newOffset > buffer->size;
    if (rotate) {
      newOffset %= buffer->size;
      d1 = buffer->size - buffer->offset;
      d2 = newOffset;
    } else {
      d1 = newOffset - buffer->offset;
      d2 = 0;
    }

    memset(&buffer->data[buffer->offset], 0, d1*sizeof(void*));
    memset(buffer->data, 0, d2*sizeof(void*));
  } else {
    memset(buffer->data, 0, sizeof(void*)*buffer->size);    
  }
  
  buffer->begin = newBegin;
  buffer->offset = newOffset;
  
  // TODO: move from other to main grid
}

void opRingPop(OpRing *buffer, uint64_t pt)
{
  uint64_t distance = pt-buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    if (buffer->data[index])
      buffer->data[index] = buffer->data[index]->timeoutQueue.next;
  } else {
    if (buffer->other)
      buffer->other = buffer->other->timeoutQueue.next;
  }
}

void opRingPush(OpRing *buffer, asyncOpRoot *op, uint64_t pt)
{
  asyncOpRoot *oldOp;
  uint64_t distance = pt-buffer->begin;
  if (distance < buffer->size) {
    size_t index = (buffer->offset + distance) % buffer->size;
    oldOp = buffer->data[index];
    buffer->data[index] = op;
  } else {
    oldOp = buffer->other;
    buffer->other = op;
  }
  
  op->timeoutQueue.prev = 0;
  op->timeoutQueue.next = oldOp;
  if (oldOp)
    oldOp->timeoutQueue.prev = op;  
}


int addToExecuteQueue(aioObjectRoot *object, asyncOpRoot *op, int isWriteQueue)
{
  // TODO: make thread safe
  List *list = isWriteQueue ? &object->writeQueue : &object->readQueue;
  op->executeQueue.prev = list->tail;
  op->executeQueue.next = 0;
  if (list->tail)
    list->tail->executeQueue.next = op;
  list->tail = op;
  if (op->executeQueue.prev == 0) {
    list->head = op;
    return 1;
  }
  
  return 0;
}

asyncOpRoot *removeFromExecuteQueue(asyncOpRoot *op)
{
  // TODO: make thread safe
  aioObjectRoot *object = op->object;  
  if (op->executeQueue.next) {
    op->executeQueue.next->executeQueue.prev = op->executeQueue.prev;
  } else {
    if (object->readQueue.tail == op)
      object->readQueue.tail = op->executeQueue.prev;
    else if (object->writeQueue.tail == op)
      object->writeQueue.tail = op->executeQueue.prev;
  }  

  if (op->executeQueue.prev) {
    op->executeQueue.prev->executeQueue.next = op->executeQueue.next;
  } else {
    if (object->readQueue.head == op) {
      object->readQueue.head = op->executeQueue.next;
      // Start next 'read' operation
      if (object->readQueue.head)
        return object->readQueue.head;
    } else if (object->writeQueue.head == op) {
      object->writeQueue.head = op->executeQueue.next;
      // Start next 'write' operation
      if (object->writeQueue.head)
        return object->writeQueue.head;
    }
  }
    
  return 0;
}


void addToTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  opRingPush(&base->timeGrid, op, getPt(op->endTime));
}


void removeFromTimeoutQueue(asyncBase *base, asyncOpRoot *op)
{
  if (op->timeoutQueue.prev) {
    op->timeoutQueue.prev->timeoutQueue.next = op->timeoutQueue.next;
  } else {
    assert(opRingGet(&base->timeGrid, getPt(op->endTime)) == op && "opRing lost operation found");
    opRingPop(&base->timeGrid, getPt(op->endTime));
  }
  if (op->timeoutQueue.next)
    op->timeoutQueue.next->timeoutQueue.prev = op->timeoutQueue.prev;
}

void processTimeoutQueue(asyncBase *base)
{
  // check timeout queue
  uint64_t currentTime = time(0);
  uint64_t begin = opRingBegin(&base->timeGrid);
  while (begin < currentTime) {
    asyncOpRoot *op = opRingGet(&base->timeGrid, begin);
    while (op) {
      asyncOpRoot *next = op->timeoutQueue.next;
      finishOperation(op, aosTimeout, 0);
      op = next;
    }
      
    begin++;
  }

  opRingShift(&base->timeGrid, currentTime);
}

static inline void startOperation(asyncOpRoot *op, asyncBase *previousOpBase)
{
  // TODO: use pipe for send operation to another async base
  uint64_t timePt = ((uint64_t)time(0))*1000000;
  if (op->endTime && op->endTime >= timePt)
    op->startMethod(op);
}

void finishOperation(asyncOpRoot *op, int status, int needRemoveFromTimeGrid)
{
  // TODO: normal timer check
  if (op->poolId == "timer pool") {
    // stop timer call 
  } else if (op->endTime && needRemoveFromTimeGrid) {
    removeFromTimeoutQueue(op->base, op);
  }
  
  // Remove operation from execute queue
  asyncBase *base = op->base;
  asyncOpRoot *nextOp = removeFromExecuteQueue(op);
  
  // Release operation
  objectRelease(&op->base->pool, op, op->poolId);
  
  // Do callback if need
  op->finishMethod(op, status);
  
  // Start next operation
  if (nextOp)
    startOperation(nextOp, base);
}
