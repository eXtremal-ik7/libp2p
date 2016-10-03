#include "asyncio/asyncOp.h"
#include "asyncioInternal.h"

static inline uint64_t getPt(uint64_t endTime)
{
  return (endTime / 1000000) + (endTime % 1000000 != 0);
}

void opRingInit(OpRing *buffer, size_t size, uint64_t begin)
{
  buffer->data = malloc(size*sizeof(void*));
  buffer->size = size;
  buffer->begin = begin;
  buffer->offset = 0;
  buffer->other = 0;
  memset(buffer->data, 0, sizeof(void*)*size);
}

uint64_t opRingBegin(OpRing *buffer)
{
  return buffer->begin;
}

aioOpRoot *opRingGet(OpRing *buffer, uint64_t pt)
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

void opRingPush(OpRing *buffer, aioOpRoot *op, uint64_t pt)
{
  aioOpRoot *oldOp;
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
}


void addToExecuteQueue(aioObjectRoot *object, aioOpRoot *op)
{
  
}

void removeFromExecuteQueue(aioOpRoot *op)
{
  
}


void addToTimeoutQueue(asyncBase *base, aioOpRoot *op)
{
  opRingPush(&base->timeGrid, op, getPt(op->endTime));
}


void removeFromTimeoutQueue(asyncBase *base, aioOpRoot *op)
{
  if (op->timeoutQueue.prev)
    op->timeoutQueue.prev->timeoutQueue.next = op->timeoutQueue.next;
  else
    opRingPop(&base->timeGrid, getPt(op->endTime));
  if (op->timeoutQueue.next)
    op->timeoutQueue.next->timeoutQueue.prev = op->timeoutQueue.prev;
}

void processTimeoutQueue(asyncBase *base)
{
  // check timeout queue
  uint64_t currentTime = time(0);
  uint64_t begin = opRingBegin(&base->timeGrid);
  while (begin < currentTime) {
    aioOpRoot *op = opRingGet(&base->timeGrid, begin);
    while (op) {
      aioOpRoot *current = op;
      
      // remove from execute queue
      removeFromExecuteQueue(current);
      
      // destroy operation
      int opCode = current->opCode;
      void *callback = current->callback;
      void *arg = current->arg;
      aioExceptionCb *cb = current->exceptionCallback;
      (*cb)(opCode, callback, arg);
      op = current->timeoutQueue.next;
      objectRelease(&base->pool, current, current->poolId);
    }
      
    begin++;
  }

  opRingShift(&base->timeGrid, currentTime);
}
