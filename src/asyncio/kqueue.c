#include "asyncioImpl.h"
#include "atomic.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *socketPool = "aioObject";

#define MAX_EVENTS 256

enum pipeDescrs {
  Read = 0,
  Write
};

typedef struct kqueueBase {
  asyncBase B;
  int kqueueFd;
  intptr_t timerIdCounter;
} kqueueBase;

typedef struct aioTimer {
  aioObjectRoot root;
  intptr_t fd;
  asyncOpRoot *op;
} aioTimer;

void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType);
void kqueueWakeup(asyncBase *base);
void kqueuePostEmptyOperation(asyncBase *base);
void kqueueNextFinishedOperation(asyncBase *base);
aioObject *kqueueNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *kqueueNewAsyncOp(void);
int kqueueCancelAsyncOp(asyncOpRoot *opptr);
void kqueueDeleteObject(aioObject *object);
void kqueueInitializeTimer(asyncBase *base, asyncOpRoot *op);
void kqueueStartTimer(asyncOpRoot *op);
void kqueueStopTimer(asyncOpRoot *op);
void kqueueActivate(aioUserEvent *op);
AsyncOpStatus kqueueAsyncConnect(asyncOpRoot *opptr);
AsyncOpStatus kqueueAsyncAccept(asyncOpRoot *opptr);
AsyncOpStatus kqueueAsyncRead(asyncOpRoot *opptr);
AsyncOpStatus kqueueAsyncWrite(asyncOpRoot *opptr);
AsyncOpStatus kqueueAsyncReadMsg(asyncOpRoot *op);
AsyncOpStatus kqueueAsyncWriteMsg(asyncOpRoot *op);

static struct asyncImpl kqueueImpl = {
  combiner,
  kqueueWakeup,
  kqueuePostEmptyOperation,
  kqueueNextFinishedOperation,
  kqueueNewAioObject,
  kqueueNewAsyncOp,
  kqueueCancelAsyncOp,
  kqueueDeleteObject,
  kqueueInitializeTimer,
  kqueueStartTimer,
  kqueueStopTimer,
  kqueueActivate,
  kqueueAsyncConnect,
  kqueueAsyncAccept,
  kqueueAsyncRead,
  kqueueAsyncWrite,
  kqueueAsyncReadMsg,
  kqueueAsyncWriteMsg
};

static void kqueueControl(int kqueueFd, uint16_t flags, int16_t filter, int fd, void *ptr)
{
  struct kevent event;
  EV_SET(&event, fd, filter, flags, 0, 0, ptr);
  if (kevent(kqueueFd, &event, 1, 0, 0, 0) == -1)
    fprintf(stderr, "kqueue event error, errno: %s\n", strerror(errno));
}

static int getFd(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice :
      return object->hDevice;
    case ioObjectSocket :
      return object->hSocket;
    default :
      return -1;
  }
}

asyncBase *kqueueNewAsyncBase()
{
  kqueueBase *base = malloc(sizeof(kqueueBase));
  if (base) {
    base->B.methodImpl = kqueueImpl;
    base->kqueueFd = kqueue();
    if (base->kqueueFd == -1) {
      fprintf(stderr, " * kqueueNewAsyncBase: kqueue_create failed\n");
    }

    base->timerIdCounter = 1;
    kqueueControl(base->kqueueFd, EV_ADD | EV_ONESHOT, EVFILT_USER, 1, 0);
  }

  return (asyncBase *)base;
}

void kqueueWakeup(asyncBase *base)
{
  kqueueBase *localBase = (kqueueBase*)base;
  kqueueControl(localBase->kqueueFd, EV_ENABLE, EVFILT_USER, 1, 0);
}

void kqueuePostEmptyOperation(asyncBase *base)
{
  kqueueBase *localBase = (kqueueBase*)base;
  unsigned count = base->messageLoopThreadCounter;
  unsigned i;
  for (i = 0; i < count; i++) {
    asyncOpRoot *op = kqueueNewAsyncOp();
    op->opCode = actEmpty;
    fnPush(base, op);
  }

  kqueueControl(localBase->kqueueFd, EV_ENABLE, EVFILT_USER, 1, 0);
}

void combiner(aioObjectRoot *object, tag_t tag, asyncOpRoot *op, AsyncOpActionTy actionType)
{
  kqueueBase *base = (kqueueBase*)object->base;
  tag_t currentTag = tag;
  asyncOpRoot *newOp = op;
  int hasFd = (object->type == ioObjectDevice) ||
              (object->type == ioObjectSocket);
  if (hasFd && getFd((aioObject*)object) == -1)
    return;

  while (currentTag) {
    int hasReadOp = object->readQueue.head != 0;
    int hasWriteOp = object->writeQueue.head != 0;

    if (currentTag & TAG_ERROR) {
      // EV_EOF mapped to TAG_ERROR, cancel all operations with aosDisconnected status
      int available;
      int fd = getFd((aioObject*)object);
      ioctl(fd, FIONREAD, &available);
      if (available == 0)
        cancelOperationList(&object->readQueue, aosDisconnected);
      cancelOperationList(&object->writeQueue, aosDisconnected);
    }

    if (currentTag & TAG_CANCELIO) {
      cancelOperationList(&object->readQueue, aosCanceled);
      cancelOperationList(&object->writeQueue, aosCanceled);
    }

    if (currentTag & TAG_DELETE) {
      // Perform delete and exit combiner
      object->destructor(object);
      return;
    }

    // Check for pending operations
    tag_t pendingOperationsNum;
    tag_t enqueuedOperationsNum = 0;
    tag_t needStart = currentTag;
    if ( (pendingOperationsNum = __tag_get_opcount(currentTag)) ) {
      if (newOp) {
        processAction(newOp, actionType, &needStart);
        enqueuedOperationsNum = 1;
        newOp = 0;
      } else {
        while (enqueuedOperationsNum < pendingOperationsNum)
          processOperationList(object, &needStart, &enqueuedOperationsNum);
      }
    }

    if (needStart & TAG_READ_MASK)
      executeOperationList(&object->readQueue);
    if (needStart & TAG_WRITE_MASK)
      executeOperationList(&object->writeQueue);

    if (hasFd) {
      int fd = getFd((aioObject*)object);
      if (object->readQueue.head) {
        if (!hasReadOp || (currentTag & TAG_READ_MASK))
          kqueueControl(base->kqueueFd, EV_ADD | EV_ONESHOT | EV_EOF, EVFILT_READ, fd, object);
      } else {
        if (hasReadOp && !(currentTag & TAG_READ_MASK))
          kqueueControl(base->kqueueFd, EV_DELETE| EV_ONESHOT | EV_EOF, EVFILT_READ, fd, object);
      }

      if (object->writeQueue.head) {
        if (!hasWriteOp || (currentTag & TAG_WRITE_MASK))
          kqueueControl(base->kqueueFd, EV_ADD | EV_ONESHOT | EV_EOF, EVFILT_WRITE, fd, object);
      } else {
        if (hasWriteOp && !(currentTag & TAG_WRITE_MASK))
          kqueueControl(base->kqueueFd, EV_DELETE | EV_ONESHOT | EV_EOF, EVFILT_WRITE, fd, object);
      }
    }

    // Try exit combiner
    tag_t processed = __tag_make_processed(currentTag, enqueuedOperationsNum);
    currentTag = __tag_atomic_fetch_and_add(&object->tag, -processed);
    currentTag -= processed;
  }
}

void kqueueNextFinishedOperation(asyncBase *base)
{
  int nfds, n;
  struct kevent events[MAX_EVENTS];
  kqueueBase *localBase = (kqueueBase *)base;
  messageLoopThreadId = __sync_fetch_and_add(&base->messageLoopThreadCounter, 1);

  while (1) {
    do {
      if (!executeGlobalQueue(base)) {
        unsigned remainingThreads = __uint_atomic_fetch_and_add(&base->messageLoopThreadCounter, 0u-1);
        if (remainingThreads == 1) {
          // Try finish all operations
          executeGlobalQueue(base);
        }

        kqueueControl(localBase->kqueueFd, EV_ENABLE, EVFILT_USER, 1, 0);
        return;
      }

      struct timespec timeout;
      timeout.tv_sec = base->globalQueue ? 0 : 1;
      timeout.tv_nsec = 0;
      nfds = kevent(localBase->kqueueFd, 0, 0, events, MAX_EVENTS, &timeout);

      time_t currentTime = time(0);
      if (currentTime % base->messageLoopThreadCounter == messageLoopThreadId)
        processTimeoutQueue(base, currentTime);
    } while (nfds <= 0 && errno == EINTR);

    for (n = 0; n < nfds; n++) {
      tag_t timerId;
      aioObjectRoot *object;
      __tagged_pointer_decode(events[n].udata, (void**)&object, &timerId);
      if (object == 0) {
        kqueueControl(localBase->kqueueFd, EV_ADD | EV_ONESHOT, EVFILT_USER, 1, 0);
      } else if (object->type == ioObjectTimer) {
        aioTimer *timer = (aioTimer*)object;
        asyncOpRoot *op = timer->op;
        if (op->opCode == actUserEvent) {
            aioUserEvent *event = (aioUserEvent*)op;
            if (!(eventIncrementReference(event, TAG_EVENT_TIMER) & 0xF000000000000000ULL)) {
              // TODO: compare timer and event tag
              if (event->counter > 0 && --event->counter == 0) {
                kqueueStopTimer(op);
              }

              op->finishMethod(op);
            }

            eventDecrementReference(event, TAG_EVENT_TIMER);
        } else {
          opCancel(op, opEncodeTag(op, timerId), aosTimeout);
        }
      } else {
        tag_t eventMask = 0;
        if (events[n].filter == EVFILT_READ)
          eventMask |= TAG_READ;
        else if (events[n].filter == EVFILT_WRITE)
          eventMask |= TAG_WRITE;
        if (events[n].flags & EV_EOF)
          eventMask |= TAG_ERROR;

        tag_t currentTag = __tag_atomic_fetch_and_add(&object->tag, eventMask);
        if (!currentTag)
          combiner(object, eventMask, 0, aaNone);
      }
    }
  }
}


aioObject *kqueueNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object = (aioObject*)objectGet(socketPool);
  if (!object) {
    object = __tagged_alloc(sizeof(aioObject));
    object->buffer.ptr = 0;
    object->buffer.totalSize = 0;
  }

  initObjectRoot(&object->root, base, type, (aioObjectDestructor*)kqueueDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy *)data;
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy *)data;
      break;
    default :
      break;
  }

  object->buffer.offset = 0;
  object->buffer.dataSize = 0;
  return object;
}

asyncOpRoot *kqueueNewAsyncOp()
{
  asyncOp *op = __tagged_alloc(sizeof(asyncOp));
  if (op) {
    op->internalBuffer = 0;
    op->internalBufferSize = 0;
  }

  return &op->root;
}

int kqueueCancelAsyncOp(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return 1;
}

void kqueueDeleteObject(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice :
      close(object->hDevice);
      object->hDevice = -1;
      break;
    case ioObjectSocket :
      close(object->hSocket);
      object->hSocket = -1;
      break;
    default :
      break;
  }
  objectRelease(object, socketPool);
}

void kqueueInitializeTimer(asyncBase *base, asyncOpRoot *op)
{
  kqueueBase *localBase = (kqueueBase*)base;
  aioTimer *timer = __tagged_alloc(sizeof(aioTimer));
  timer->root.base = base;
  timer->root.type = ioObjectTimer;
  timer->fd = __sync_fetch_and_add(&localBase->timerIdCounter, 1);
  timer->op = op;
  op->timerId = timer;
}

void kqueueStartTimer(asyncOpRoot *op)
{
  struct kevent event;
  int periodic = op->opCode == actUserEvent;  
  aioTimer *timer = (aioTimer*)op->timerId;
  EV_SET(&event,
         timer->fd,
         EVFILT_TIMER,
         EV_ADD | EV_ENABLE | (periodic ? 0 : EV_ONESHOT),
         NOTE_USECONDS,
         op->timeout,
         __tagged_pointer_make(timer, opGetGeneration(op)));
  if (kevent(((kqueueBase*)timer->root.base)->kqueueFd, &event, 1, 0, 0, 0) == -1)
    fprintf(stderr, "kqueueStartTimer: %s\n", strerror(errno));
}


void kqueueStopTimer(asyncOpRoot *op)
{
  struct kevent event;
  aioTimer *timer = (aioTimer*)op->timerId;
  EV_SET(&event, timer->fd, EVFILT_TIMER, EV_DELETE, 0, 0, 0);
  if (kevent(((kqueueBase*)timer->root.base)->kqueueFd, &event, 1, 0, 0, 0) == -1)
    fprintf(stderr, "kqueueStopTimer: %s\n", strerror(errno));
}


void kqueueActivate(aioUserEvent *op)
{
  kqueueBase *localBase = (kqueueBase*)op->base;
  fnPush(&localBase->B, &op->root);
  kqueueControl(localBase->kqueueFd, EV_ENABLE, EVFILT_USER, 1, 0);
}


AsyncOpStatus kqueueAsyncConnect(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);
  if (op->state == 0) {
    op->state = 1;
    struct sockaddr_in localAddress;
    localAddress.sin_family = op->host.family;
    localAddress.sin_addr.s_addr = op->host.ipv4;
    localAddress.sin_port = op->host.port;
    int result = connect(fd, (struct sockaddr *)&localAddress, sizeof(localAddress));
    if (result == -1 && errno != EINPROGRESS)
      return aosUnknownError;
    else
      return aosPending;
  } else {
    int error;
    socklen_t size = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size);
    return (error == 0) ? aosSuccess : aosUnknownError;
  }
}


AsyncOpStatus kqueueAsyncAccept(asyncOpRoot *opptr)
{
  struct sockaddr_in clientAddr;
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);
  socklen_t clientAddrSize = sizeof(clientAddr);
  op->acceptSocket =
    accept(fd, (struct sockaddr *)&clientAddr, &clientAddrSize);

  if (op->acceptSocket != -1) {
    int current = fcntl(op->acceptSocket, F_GETFL);
    fcntl(op->acceptSocket, F_SETFL, O_NONBLOCK | current);
    op->host.family = 0;
    op->host.ipv4 = clientAddr.sin_addr.s_addr;
    op->host.port = clientAddr.sin_port;
    return aosSuccess;
  } else {
    return aosUnknownError;
  }
}


AsyncOpStatus kqueueAsyncRead(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  aioObject *object = (aioObject*)op->root.object;
  struct ioBuffer *sb = &object->buffer;
  int fd = getFd(object);

  if (copyFromBuffer(op->buffer, &op->bytesTransferred, sb, op->transactionSize))
    return aosSuccess;

  if (op->transactionSize <= object->buffer.totalSize) {
    while (op->bytesTransferred < op->transactionSize) {
      ssize_t bytesRead = read(fd, sb->ptr, sb->totalSize);
      if (bytesRead == 0)
        return aosDisconnected;
      else if (bytesRead < 0)
        return errno == EAGAIN ? aosPending : aosUnknownError;
      sb->dataSize = (size_t)bytesRead;

      if (copyFromBuffer(op->buffer, &op->bytesTransferred, sb, op->transactionSize) || !(opptr->flags & afWaitAll))
        break;
    }

    return aosSuccess;
  } else {
    ssize_t bytesRead = read(fd,
                             (uint8_t *)op->buffer + op->bytesTransferred,
                             op->transactionSize - op->bytesTransferred);

    if (bytesRead > 0) {
      op->bytesTransferred += (size_t)bytesRead;
      if (op->root.flags & afWaitAll && op->bytesTransferred < op->transactionSize)
        return aosPending;
      else
        return aosSuccess;
    } else if (bytesRead == 0) {
      return op->transactionSize - op->bytesTransferred > 0 ? aosDisconnected : aosSuccess;
    } else {
      return errno == EAGAIN ? aosPending : aosUnknownError;
    }
  }
}


AsyncOpStatus kqueueAsyncWrite(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  ssize_t bytesWritten = write(fd,
                               (uint8_t *)op->buffer + op->bytesTransferred,
                               op->transactionSize - op->bytesTransferred);
  if (bytesWritten > 0) {
    op->bytesTransferred += bytesWritten;
    if (op->root.flags & afWaitAll && op->bytesTransferred < op->transactionSize)
      return aosPending;
    else
      return aosSuccess;
  } else if (bytesWritten == 0) {
    return op->transactionSize - op->bytesTransferred > 0 ? aosDisconnected : aosSuccess;
  } else {
    return errno == EAGAIN ? aosPending : aosUnknownError;
  }
}


AsyncOpStatus kqueueAsyncReadMsg(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  struct sockaddr_in source;
  socklen_t addrlen = sizeof(source);
  ssize_t result = recvfrom(fd, op->buffer, op->transactionSize, 0, (struct sockaddr*)&source, &addrlen);
  if (result != -1) {
    op->host.family = 0;
    op->host.ipv4 = source.sin_addr.s_addr;
    op->host.port = source.sin_port;
    op->bytesTransferred = result;
    return aosSuccess;
  } else {
    if (errno == EAGAIN)
      return aosPending;
    if (errno == ENOMEM)
      return aosBufferTooSmall;
    else
      return aosUnknownError;
  }
}


AsyncOpStatus kqueueAsyncWriteMsg(asyncOpRoot *opptr)
{
  asyncOp *op = (asyncOp*)opptr;
  int fd = getFd((aioObject*)op->root.object);

  struct sockaddr_in remoteAddress;
  remoteAddress.sin_family = op->host.family;
  remoteAddress.sin_addr.s_addr = op->host.ipv4;
  remoteAddress.sin_port = op->host.port;
  ssize_t result = sendto(fd, op->buffer, op->transactionSize, 0, (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
  if (result != -1) {
    return aosSuccess;
  }

  return aosPending;
}
