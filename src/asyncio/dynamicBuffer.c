#include "asyncio/dynamicBuffer.h"
#include <stdint.h>
#include <stdlib.h>


void dynamicBufferGrow(dynamicBuffer *buffer, size_t extra)
{
  size_t newMemorySize = buffer->allocatedSize;
  while (newMemorySize < buffer->offset + extra)
    newMemorySize *= 2;

  if (newMemorySize != buffer->allocatedSize) {
    void *newBuffer;
    if (buffer->foreign) {
      newBuffer = malloc(newMemorySize);
      buffer->foreign = 0;
    } else {
      newBuffer = buffer->data ? realloc(buffer->data, newMemorySize) : malloc(newMemorySize);
    }
      
    buffer->allocatedSize = newMemorySize;
    buffer->data = newBuffer;
  }
}


void dynamicBufferInit(dynamicBuffer *buffer, size_t initialSize)
{
  if (initialSize)
    buffer->data = malloc(initialSize);
  buffer->offset = 0;
  buffer->size = 0;
  buffer->allocatedSize = initialSize;
  buffer->foreign = 0;
}


void *dynamicBufferAlloc(dynamicBuffer *buffer, size_t size)
{
  void *ptr;

  dynamicBufferGrow(buffer, size);
  ptr = dynamicBufferPtr(buffer);
  buffer->offset += size;
  buffer->size = (buffer->offset > buffer->size) ?
    buffer->offset : buffer->size;
    
  return ptr;
}


void dynamicBufferClear(dynamicBuffer *buffer)
{
  buffer->offset = 0;
  buffer->size = 0;
}


void *dynamicBufferPtr(dynamicBuffer *buffer)
{
  return (uint8_t*)buffer->data + buffer->offset;  
}


size_t dynamicBufferRemaining(dynamicBuffer *buffer)
{
  return buffer->size - buffer->offset;
}


void dynamicBufferSeek(dynamicBuffer *buffer, SeekTy type, size_t offset)
{
  switch (type) {
    case SeekSet :
      buffer->offset = (offset <= buffer->size) ? offset : buffer->size;
      break;
    case SeekCur :
      buffer->offset = (buffer->offset+offset <= buffer->size) ?
        buffer->offset+offset : buffer->size;
      break;
    case SeekEnd :
      buffer->offset = (offset <= buffer->size) ? buffer->size - offset : 0;
      break;
  }
}
