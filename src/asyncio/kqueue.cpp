#include "asyncioInternal.h"
#include "asyncio/dynamicBuffer.h"

#include <iostream>
#include <errno.h>
#include <sys/event.h>
#include <sys/time.h>
#include <map>
#include <queue>
#include <signal.h>
#include "assert.h"
#include "config.h"

#define MAX_EVENTS 128

struct descrStruct;

typedef int descr;

typedef std::deque<asyncOp*> opDequeTy;
typedef std::vector<struct kevent> keVectorTy;
typedef std::queue<int> timerQueueTy;

struct descrStruct {
  descrStruct() {}
  opDequeTy writeOps;
  opDequeTy readOps;
};

enum pipeDescrs {
  Read = 0,
  Write
};

typedef enum pipeCmd {
  Reset = 0,
  UserEventActivate
} pipeCmd;

typedef struct pipeMsg {
  pipeCmd cmd;
  void *data;
} pipeMsg;

typedef std::map<descr, descrStruct> fdMapTy;

struct kqueueBase {
  asyncBase B;
  int pipeFd[2];
  fdMapTy fdMap;
  int kqueueFd;
  timerQueueTy timerQueue;
  keVectorTy keVector;
  int timersUsed;
};

struct asyncOp {
  aioInfo info;
  int counter;
  opDequeTy *opDeque;
  int useInternalBuffer;
  void *internalBuffer;
  size_t internalBufferSize;
  int timerId;
};

void kqueuePostEmptyOperation(asyncBase *base);
void kqueueNextFinishedOperation(asyncBase *base);
aioObject *kqueueNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOp *kqueueNewAsyncOp(asyncBase *base);
void kqueue(asyncOp *op);
void kqueueStartTimer(asyncOp *op, uint64_t usTimeout, int count);
void kqueueStopTimer(asyncOp *op);
void kqueueActivate(asyncOp *op);
void kqueueAsyncConnect(asyncOp *op,
                       const HostAddress *address,
                       uint64_t usTimeout);
void kqueueAsyncAccept(asyncOp *op, uint64_t usTimeout);
void kqueueAsyncRead(asyncOp *op, uint64_t usTimeout);
void kqueueAsyncWrite(asyncOp *op, uint64_t usTimeout);
void kqueueAsyncReadMsg(asyncOp *op, uint64_t usTimeout);
void kqueueAsyncWriteMsg(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout);
void kqueueMonitor(asyncOp *op);
void kqueueMonitorStop(asyncOp *op);
void kqueueDeleteOp(asyncOp *op);

static struct asyncImpl kqueueImpl = {
  kqueuePostEmptyOperation,
  kqueueNextFinishedOperation,
  kqueueNewAioObject,
  kqueueNewAsyncOp,
  kqueueDeleteOp,
  kqueueStartTimer,
  kqueueStopTimer,
  kqueueActivate,
  kqueueAsyncConnect,
  kqueueAsyncAccept,
  kqueueAsyncRead,
  kqueueAsyncWrite,
  kqueueAsyncReadMsg,
  kqueueAsyncWriteMsg,
  kqueueMonitor,
  kqueueMonitorStop
};

static int isWriteOperation(IoActionTy action)
{
  return (action == ioConnect ||
          action == ioWrite ||
          action == ioWriteMsg);
}

static int getFd(asyncOp *op)
{
  switch (op->info.object->type) {
    case ioObjectDevice :
      return op->info.object->hDevice;
      break;
    case ioObjectSocket :
    case ioObjectSocketSyn :
      return op->info.object->hSocket;
      break;
    default :
      return -1;
      break;
  }
}

static void pushKevent(kqueueBase *base, int id, int filter, int flags,
                       int fflags, int data, void *const udata)
{
  struct kevent buf;
  EV_SET(&buf, id, filter, flags, fflags, data, udata);
  base->keVector.push_back(buf);
}

static void finishOperation(asyncOp *op,
                            AsyncOpStatus status,
                            int needStopTimer)
{
  if (needStopTimer)
     kqueueStopTimer(op);
  op->info.status = status;
  if (op->info.callback)
    op->info.callback(&op->info);
  if (op->info.object->type != ioObjectUserEvent) {
    if (status != aosMonitoring) {
      kqueueDeleteOp(op);
      cqueue_ptr_push(&op->info.object->base->asyncOps, op);
    }
    if (op->opDeque->empty()) {
      int ioFilter;
      ioFilter = isWriteOperation(op->info.currentAction) ? EVFILT_WRITE : EVFILT_READ;
      pushKevent((kqueueBase*)op->info.object->base,
                 getFd(op),
                 ioFilter,
                 EV_DISABLE,
                 0, 0, 0
                 );
    }
  }
}


static void processReadyFd(opDequeTy *dequeptr, struct kevent *ev, int isRead)
{
  int available, readyForRead, readyForWrite;
  asyncOp *op;
  if (dequeptr->empty())
    return;
  assert(!dequeptr->empty() && "Error: Queue is empty!");
  op = dequeptr->front();
  if (ev->flags == EV_EOF || ev->data == 0) {
    if (op->info.object->type == ioObjectSocket ||
        op->info.object->type == ioObjectSocketSyn)
      finishOperation(op, aosDisconnected, 1);
    return;
  }

  available = ev->data;

  switch (op->info.currentAction) {
  case ioConnect : {
    int error;
    socklen_t size = sizeof(error);
    getsockopt(op->info.object->hSocket,
               SOL_SOCKET, SO_ERROR, &error, &size);
    finishOperation(op, (error == 0) ? aosSuccess : aosUnknownError, 1);
    break;
  }
  case ioAccept : {
    struct sockaddr_in clientAddr;
    socklen_t clientAddrSize = sizeof(clientAddr);
    op->info.acceptSocket =
        accept(ev->ident, (struct sockaddr *)&clientAddr, &clientAddrSize);

    if (op->info.acceptSocket != -1) {
      op->info.host.family = 0;
      op->info.host.ipv4 = clientAddr.sin_addr.s_addr;
      op->info.host.port = clientAddr.sin_port;
      finishOperation(op, aosSuccess, 1);
    } else {
      finishOperation(op, aosUnknownError, 1);
    }
    break;
  }
  case ioRead : {
    uint8_t *ptr = (uint8_t *)op->info.buffer + op->info.bytesTransferred;
    readyForRead =
        (op->info.transactionSize - op->info.bytesTransferred
         < (size_t)available) ?
          op->info.transactionSize - op->info.bytesTransferred
        : (size_t)available;

    read(ev->ident, ptr, readyForRead);
    op->info.bytesTransferred += readyForRead;
    if (op->info.bytesTransferred == op->info.transactionSize ||
        !(op->info.flags & afWaitAll))
      finishOperation(op, aosSuccess, 1);
    break;
  }
  case ioWrite : {
    uint8_t *ptr = op->useInternalBuffer ?
          (uint8_t*)op->internalBuffer : (uint8_t*)op->info.buffer;
    ptr += op->info.bytesTransferred;
    readyForWrite =
        (op->info.transactionSize - op->info.bytesTransferred
         < (size_t)available) ?
          op->info.transactionSize - op->info.bytesTransferred
        : (size_t)available;
    write(ev->ident, ptr, readyForWrite);
    if (op->info.object->type == ioObjectSocket && errno == EPIPE)
      finishOperation(op, aosDisconnected, 1);
    else {
      op->info.bytesTransferred += readyForWrite;
      if (op->info.bytesTransferred == op->info.transactionSize ||
          !(op->info.flags & afWaitAll))
        finishOperation(op, aosSuccess, 1);
    }
    break;
  }
  case ioReadMsg : {
    void *ptr = dynamicBufferAlloc(op->info.dynamicArray, available);
    read(ev->ident, ptr, available);
    op->info.bytesTransferred += available;
    finishOperation(op, aosSuccess, 1);
    break;
  }
  case ioWriteMsg : {
    struct sockaddr_in remoteAddress;
    uint8_t *ptr = op->useInternalBuffer ?
          (uint8_t*)op->internalBuffer : (uint8_t*)op->info.buffer;
    ptr += op->info.bytesTransferred;
    remoteAddress.sin_family = op->info.host.family;
    remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
    remoteAddress.sin_port = op->info.host.port;

    sendto(ev->ident, ptr, op->info.transactionSize, 0,
           (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
    if (op->info.object->type == ioObjectSocket && errno == EPIPE)
      finishOperation(op, aosDisconnected, 1);
    else {
      op->info.bytesTransferred += readyForWrite;
      if (op->info.bytesTransferred == op->info.transactionSize ||
          !(op->info.flags & afWaitAll))
        finishOperation(op, aosSuccess, 1);
    }
    break;
  }
  case ioMonitor : {
    finishOperation(op, aosMonitoring, 0);
    break;
  }
  default :
    break;
  }
}

extern "C" asyncBase *kqueueNewAsyncBase()
{
  kqueueBase *base = new kqueueBase;
  struct sigaction sAction;
  if (base) {
    cqueue_ptr_init(&base->B.asyncOps, 64);
    pipe(base->pipeFd);
    base->B.methodImpl = kqueueImpl;
    base->kqueueFd = kqueue();
    pushKevent(base,
               base->pipeFd[Read],
               EVFILT_READ,
               EV_ADD | EV_ENABLE,
               0, 0, 0
               );
    base->timersUsed = 0;
    memset(&sAction, sizeof(sAction), 0);
    sAction.sa_handler = SIG_IGN;

    if (sigaction(SIGPIPE, &sAction, NULL) == -1) {
      fprintf(stderr, " * kqueueNewAsyncBase: sigaction error\n");
    }
  }
  return (asyncBase*)base;

}

aioObject *kqueueNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object = new aioObject;
  object->base = base;
  object->type = type;
  switch (object->type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy*)data;
      break;
    case ioObjectSocket :
    case ioObjectSocketSyn :
      object->hSocket = *(socketTy*)data;
      break;
    default:
      break;
  }

  return object;
}

asyncOp *kqueueNewAsyncOp(asyncBase *base)
{
  asyncOp *op = new asyncOp;
  if (op) {
    op->timerId = 0;
    op->internalBuffer = 0;
    op->internalBufferSize = 0;
  }
  return op;
}

static void startOperation(asyncOp *op,
                           IoActionTy action,
                           uint64_t usTimeout)
{
  kqueueBase *localBase = (kqueueBase*)op->info.object->base;
  op->info.currentAction = action;

  if (op->info.currentAction == ioMonitor)
    op->info.status = aosMonitoring;
  else
    op->info.status = aosPending;

  if (op->useInternalBuffer && (action == ioWrite || action == ioWriteMsg)) {
    if (op->internalBuffer == 0) {
      op->internalBuffer = malloc(op->info.transactionSize);
      op->internalBufferSize = op->info.transactionSize;
    } else if (op->internalBufferSize < op->info.transactionSize) {
      op->internalBufferSize = op->info.transactionSize;
      op->internalBuffer = realloc(op->internalBuffer,
                                   op->info.transactionSize);
    }
    memcpy(op->internalBuffer, op->info.buffer, op->info.transactionSize);
  }

  if (localBase->fdMap.find(getFd(op)) == localBase->fdMap.end())
    localBase->fdMap[getFd(op)] = descrStruct();

  opDequeTy *dequeptr;
  dequeptr = isWriteOperation(op->info.currentAction) ?
        &(localBase->fdMap[getFd(op)].writeOps) :
        &(localBase->fdMap[getFd(op)].readOps);
  int ioFilter = isWriteOperation(op->info.currentAction) ?
        EVFILT_WRITE :
        EVFILT_READ;

  switch (action) {
    case ioMonitorStop :
      break;
    default :
      if (dequeptr->empty())
        pushKevent(localBase, getFd(op), ioFilter,
                   EV_ADD | EV_ENABLE,
                   0, 0, 0
                   );
      dequeptr->push_back(op);
      op->opDeque = dequeptr;
      break;
  }

  if (action == ioMonitorStop || action == ioMonitor);

  if (usTimeout) {
    kqueueStartTimer(op, usTimeout, 1);
  }
}

void kqueueDeleteOp(asyncOp *op)
{
  op->opDeque->erase(std::find(op->opDeque->begin(), op->opDeque->end(), op));
}

void kqueuePostEmptyOperation(asyncBase *base)
{
  pipeMsg msg = {Reset, 0};
  kqueueBase *localBase = (kqueueBase*)base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}


void kqueueNextFinishedOperation(asyncBase *base)
{
  kqueueBase *localBase = (kqueueBase*)base;
  pipeMsg msg;
  int kq, nev, n, i;
  int eventsNum;
  opDequeTy *dequeptr;
  struct kevent *evlist = new struct kevent[MAX_EVENTS];
  asyncOp *op;
  while (true) {
    eventsNum = localBase->keVector.size();

    nev = kevent(localBase->kqueueFd,
                 localBase->keVector.data(),
                 eventsNum,
                 evlist,
                 MAX_EVENTS,
                 0
                 );
    localBase->keVector.clear();
    if (nev == -1)
      fprintf(stderr, "kevent error: %s", strerror(errno));
    if (nev == 0) {
      fprintf(stderr, "kevent error: no events");
      return;
    }
    for (n = 0; n < nev; n++) {
      if (evlist[n].filter == EVFILT_READ) {
        if (evlist[n].ident == localBase->pipeFd[Read]) {
          for (i = 0; i < evlist[n].data / sizeof(pipeMsg); i++) {
            read(localBase->pipeFd[Read], &msg, sizeof(pipeMsg));
            op = (asyncOp*)msg.data;
            switch (msg.cmd) {
            case Reset :
              return;
              break;
            case UserEventActivate :
              finishOperation(op, aosSuccess, op->counter == 0);
              break;
            }
          }
        } else {
          dequeptr = &(localBase->fdMap[evlist[n].ident].readOps);
          processReadyFd(dequeptr, &evlist[n], 1);
        }
      } else if (evlist[n].filter == EVFILT_WRITE) {
        dequeptr = &(localBase->fdMap[evlist[n].ident].writeOps);
        processReadyFd(dequeptr, &evlist[n], 0);
      } else if (evlist[n].filter == EVFILT_TIMER) {
        op = (asyncOp*)evlist[n].udata;
        if (op->info.object->type == ioObjectUserEvent)
          finishOperation(op, aosSuccess, op->counter == 0);
        else {
          finishOperation(op, aosTimeout, 1);
        }
      }

    }
  }
}



void kqueueStartTimer(asyncOp *op, uint64_t usTimeout, int count)
{
  kqueueBase *base = (kqueueBase*)op->info.object->base;
  if (!base->timerQueue.empty()) {
    op->timerId = base->timerQueue.front();
    base->timerQueue.pop();
  } else {
    op->timerId = ++base->timersUsed;
  }
  op->counter = count;
#ifdef HAVE_NOTE_USECONDS
  pushKevent(base,
             op->timerId,
             EVFILT_TIMER,
             EV_ADD | EV_ENABLE,
             NOTE_USECONDS,
             usTimeout,
             (void*)op
             );
#else
  pushKevent(base,
             op->timerId,
             EVFILT_TIMER,
             EV_ADD | EV_ENABLE,
             0,
             usTimeout / 1000,
             (void*)op
             );  
#endif

}

void kqueueStopTimer(asyncOp *op)
{
  kqueueBase *base = (kqueueBase*)op->info.object->base;
  if (op->timerId != 0) {
    op->counter = 0;
    base->timerQueue.push(op->timerId);
#ifdef HAVE_NOTE_USECONDS    
    pushKevent(base,
               op->timerId,
               EVFILT_TIMER,
               EV_DELETE | EV_DISABLE,
               NOTE_USECONDS,
               0,
               0
               );

#else
    pushKevent(base,
               op->timerId,
               EVFILT_TIMER,
               EV_DELETE | EV_DISABLE,
               0,
               0,
               0
               );    
#endif
    op->timerId = 0;    
  }
}

void kqueueActivate(asyncOp *op)
{
  pipeMsg msg = {UserEventActivate, (void *)op};
  kqueueBase *localBase = (kqueueBase*)op->info.object->base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}

void kqueueAsyncConnect(asyncOp *op,
                       const HostAddress *address,
                       uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  int err = connect(op->info.object->hSocket,
                    (struct sockaddr *)&localAddress,
                    sizeof(localAddress));

  if (err == -1 && errno != EINPROGRESS) {
    fprintf(stderr, "connect error, errno: %s\n", strerror(errno));
    return;
  }

  startOperation(op, ioConnect, usTimeout);
}

void kqueueAsyncAccept(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioAccept, usTimeout);
}

void kqueueAsyncRead(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioRead, usTimeout);
}

void kqueueAsyncWrite(asyncOp *op, uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);
  startOperation(op, ioWrite, usTimeout);
}

void kqueueMonitorStop(asyncOp *op)
{
  startOperation(op, ioMonitorStop, 0);
}

void kqueueMonitor(asyncOp *op)
{
  startOperation(op, ioMonitor, 0);
}

void kqueueAsyncWriteMsg(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);
  op->info.host = *address;
  startOperation(op, ioWriteMsg, usTimeout);
}

void kqueueAsyncReadMsg(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioReadMsg, usTimeout);
}
