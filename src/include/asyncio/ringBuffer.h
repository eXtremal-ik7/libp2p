// Lock free bounded queue
// Original code from Dmitry Vyukov
// http://www.1024cores.net

#ifndef __ASYNCIO_RINGBUFFER_H_
#define __ASYNCIO_RINGBUFFER_H_

#include <stddef.h>

typedef struct RingBuffer {
  void **queue;
  size_t queueSize;
  size_t queueSizeMask;
  size_t enqueuePos;
  size_t dequeuePos;
} RingBuffer;

typedef struct ConcurrentRingBufferElement {
  void *data;
  volatile size_t sequence;
} ConcurrentRingBufferElement;

typedef struct ConcurrentRingBuffer {
  ConcurrentRingBufferElement *queue;
  size_t queueSize;
  size_t queueSizeMask;
  volatile size_t enqueuePos;
  volatile size_t dequeuePos;
} ConcurrentRingBuffer;


// Ring buffer API
void ringBufferInit(RingBuffer *buffer, size_t initialSize);
void ringBufferFree(RingBuffer *buffer);
int ringBufferEmpty(RingBuffer *buffer);
void ringBufferEnqueue(RingBuffer *buffer, void *data);
int ringBufferDequeue(RingBuffer *buffer, void **data);

// Concurrent ring buffer API
void concurrentRingBufferInit(ConcurrentRingBuffer *buffer, size_t size);
void concurrentRingBufferTryInit(ConcurrentRingBuffer *buffer, size_t size);
void concurrentRingBufferFree(ConcurrentRingBuffer *buffer);
int concurrentRingBufferEmpty(ConcurrentRingBuffer *buffer);
int concurrentRingBufferEnqueue(ConcurrentRingBuffer *buffer, void *data);
int concurrentRingBufferDequeue(ConcurrentRingBuffer *buffer, void **data);


#endif //__ASYNCIO_RINGBUFFER_H_
