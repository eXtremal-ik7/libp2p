// Lock free unbounded queue
// Based on bounded queue code from Dmitry Vyukov
// http://www.1024cores.net

#include "asyncio/ringBuffer.h"
#include "atomic.h"
#include <assert.h>
#include <stdlib.h>

#define CONCURRENT_QUEUE_INITIAL_SIZE_LOG2 12

static void partitionInit(ConcurrentQueuePartition *buffer, size_t size)
{
  assert((size & (size-1)) == 0 && "Invalid ring buffer size");
  if (!buffer->queue) {
    ConcurrentQueueElement *queue = (ConcurrentQueueElement*)malloc(size*sizeof(ConcurrentQueueElement));
    for (size_t i = 0; i < size; i++)
      queue[i].sequence = i;
    if (!__pointer_atomic_compare_and_swap((void *volatile*)&buffer->queue, 0, queue))
      free(queue);
  }
}

static int partitionPush(ConcurrentQueuePartition *buffer, void *data, size_t mask)
{
  ConcurrentQueueElement *element = 0;
  size_t pos = buffer->enqueuePos;
  for (;;) {
    element = &buffer->queue[pos & mask];
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

static int partitionPop(ConcurrentQueuePartition *buffer, void **data, size_t mask)
{
  if (!buffer->queue)
    return 0;

  ConcurrentQueueElement *element = 0;
  size_t pos = buffer->dequeuePos;
  for (;;) {
    element = &buffer->queue[pos & mask];
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
  element->sequence = pos + (mask+1);
  return 1;
}

void concurrentQueuePush(ConcurrentQueue *queue, void *data)
{
  for (;;) {
    uint32_t currentWritePartition = queue->WritePartition;
    ConcurrentQueuePartition *partition = &queue->Partitions[currentWritePartition];
    size_t partitionSize = (size_t)1 << (currentWritePartition + CONCURRENT_QUEUE_INITIAL_SIZE_LOG2);
    size_t mask = partitionSize-1;

    partitionInit(partition, partitionSize);
    if (partitionPush(partition, data, mask))
      return;

    __uint_atomic_compare_and_swap(&queue->WritePartition, currentWritePartition, currentWritePartition+1);
  }
}

int concurrentQueuePop(ConcurrentQueue *queue, void **data)
{
  for (;;) {
    uint32_t currentReadPartition = queue->ReadPartition;
    ConcurrentQueuePartition *partition = &queue->Partitions[currentReadPartition];
    size_t partitionSize = (size_t)1 << (currentReadPartition + CONCURRENT_QUEUE_INITIAL_SIZE_LOG2);
    size_t mask = partitionSize-1;

    if (partitionPop(partition, data, mask))
      return 1;

    if (currentReadPartition == queue->WritePartition)
      return 0;

    __uint_atomic_compare_and_swap(&queue->ReadPartition, currentReadPartition, currentReadPartition+1);
  }
}
