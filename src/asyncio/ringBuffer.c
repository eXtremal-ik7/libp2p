// Lock free bounded queue
// Original code from Dmitry Vyukov
// http://www.1024cores.net

#include "asyncio/ringBuffer.h"
#include "atomic.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

void ringBufferInit(RingBuffer *buffer, size_t initialSize)
{
  assert((initialSize & (initialSize-1)) == 0 && "Invalid ring buffer size");
  buffer->queue = malloc(initialSize*sizeof(void*));
  buffer->queueSize = initialSize;
  buffer->queueSizeMask = initialSize-1;
  buffer->enqueuePos = 0;
  buffer->dequeuePos = 0;
}

void ringBufferFree(RingBuffer *buffer)
{
  free(buffer->queue);
}

int ringBufferEmpty(RingBuffer *buffer)
{
  return buffer->dequeuePos == buffer->enqueuePos;
}

void ringBufferEnqueue(RingBuffer *buffer, void *data)
{
  if (buffer->enqueuePos - buffer->dequeuePos == buffer->queueSize) {
    // realloc
    size_t newSize = buffer->queueSize*2;
    void **newQueue = malloc(newSize*sizeof(void*));
    for (size_t i = 0, pos = buffer->dequeuePos; pos != buffer->enqueuePos; pos++, i++)
      newQueue[i] = buffer->queue[pos & buffer->queueSizeMask];

    free(buffer->queue);
    buffer->queue = newQueue;
    buffer->queueSize = newSize;
    buffer->queueSizeMask = newSize-1;
    buffer->enqueuePos = newSize/2;
    buffer->dequeuePos = 0;
  }

  buffer->queue[buffer->enqueuePos++ & buffer->queueSizeMask] = data;
}

int ringBufferDequeue(RingBuffer *buffer, void **data)
{
  if (buffer->dequeuePos != buffer->enqueuePos) {
    *data = buffer->queue[buffer->dequeuePos++ & buffer->queueSizeMask];
    return 1;
  } else {
    return 0;
  }
}

void concurrentRingBufferInit(ConcurrentRingBuffer *buffer, size_t size)
{
  assert((size & (size-1)) == 0 && "Invalid ring buffer size");
  buffer->queue = malloc(size*sizeof(ConcurrentRingBufferElement));
  buffer->queueSize = size;
  buffer->queueSizeMask = size-1;
  buffer->enqueuePos = 0;
  buffer->dequeuePos = 0;
  for (size_t i = 0; i < size; i++)
    buffer->queue[i].sequence = i;
}

void concurrentRingBufferFree(ConcurrentRingBuffer *buffer)
{
  free(buffer->queue);
}

int concurrentRingBufferEmpty(ConcurrentRingBuffer *buffer)
{
  return buffer->dequeuePos == buffer->enqueuePos;
}

int concurrentRingBufferEnqueue(ConcurrentRingBuffer *buffer, void *data)
{
  ConcurrentRingBufferElement *element = 0;
  size_t pos = buffer->enqueuePos;
  for (;;) {
    element = &buffer->queue[pos & buffer->queueSizeMask];
    size_t seq = element->sequence;
    intptr_t diff = (intptr_t)seq - (intptr_t)pos;
    if (diff == 0) {
      if (__uintptr_atomic_compare_and_swap(&buffer->enqueuePos, pos, pos+1))
        break;
    } else if (diff < 0) {
      // Queue is full
      return 0;
    } else {
      pos = buffer->enqueuePos;
    }
  }

  element->data = data;
  element->sequence = pos + 1;
  return 1;
}

int concurrentRingBufferDequeue(ConcurrentRingBuffer *buffer, void **data)
{
  ConcurrentRingBufferElement *element = 0;
  size_t pos = buffer->dequeuePos;
  for (;;) {
    element = &buffer->queue[pos & buffer->queueSizeMask];
    size_t seq = element->sequence;
    intptr_t diff = (intptr_t)seq - (intptr_t)(pos+1);
    if (diff == 0) {
      if (__uintptr_atomic_compare_and_swap(&buffer->dequeuePos, pos, pos+1))
        break;
    } else if (diff < 0) {
      // Queue is empty
      return 0;
    } else {
      pos = buffer->dequeuePos;
    }
  }

  *data = element->data;
  element->sequence = pos + buffer->queueSize;
  return 1;
}
