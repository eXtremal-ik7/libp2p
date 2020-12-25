// Lock free bounded queue
// Original code from Dmitry Vyukov
// http://www.1024cores.net

#ifndef __ASYNCIO_RINGBUFFER_H_
#define __ASYNCIO_RINGBUFFER_H_

#include <stddef.h>
#include <stdint.h>

typedef struct ConcurrentQueueElement {
  void *data;
  volatile size_t sequence;
} ConcurrentQueueElement;

typedef struct ConcurrentQueuePartition {
  ConcurrentQueueElement *queue;
  volatile size_t enqueuePos;
  volatile size_t dequeuePos;
} ConcurrentQueuePartition;

typedef struct ConcurrentQueue {
  ConcurrentQueuePartition Partitions[64];
  volatile uint32_t ReadPartition;
  volatile uint32_t WritePartition;
} ConcurrentQueue;

// Concurrent ring buffer API
void concurrentQueuePush(ConcurrentQueue *queue, void *data);
int concurrentQueuePop(ConcurrentQueue *queue, void **data);

#endif //__ASYNCIO_RINGBUFFER_H_
