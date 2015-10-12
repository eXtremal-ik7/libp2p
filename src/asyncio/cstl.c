#include "cstl.h"
#include <stdlib.h>


static void cqueue_ptr_grow(cqueue_ptr *queue)
{
  size_t newSize = queue->allocatedSize * 2;
  void **newBuffer = malloc(sizeof(void*) * newSize);

  size_t i, j = 0;
  for (i = queue->begin; i != queue->end; i = (i+1) % queue->allocatedSize, j++)
    newBuffer[j] = queue->M[i];

  free(queue->M);
  queue->M = newBuffer;
  queue->allocatedSize = newSize;
  queue->begin = 0;
  queue->end = j;
}


void cqueue_ptr_init(cqueue_ptr *queue, size_t size)
{
  queue->M = malloc(sizeof(void*) * size);
  queue->allocatedSize = size;
  queue->begin = 0;
  queue->end = 0;
}


void cqueue_ptr_push(cqueue_ptr *queue, void *data)
{
  size_t newEnd = (queue->end + 1) % queue->allocatedSize;
  if (newEnd == queue->begin) {
    // reallocate memory for queue
    cqueue_ptr_grow(queue);
    newEnd = queue->end + 1;
  }

  queue->M[queue->end] = data;
  queue->end = newEnd;
}


int cqueue_ptr_peek(cqueue_ptr *queue, void **data)
{
  if (queue->begin != queue->end) {
    *data = queue->M[queue->begin];
    return 1;
  } else {
    return 0;
  }
}


int cqueue_ptr_pop(cqueue_ptr *queue, void **data)
{
  if (queue->begin != queue->end) {
    *data = queue->M[queue->begin];
    queue->begin = (queue->begin + 1) % queue->allocatedSize;
    return 1;
  } else {
    return 0;
  }
}
