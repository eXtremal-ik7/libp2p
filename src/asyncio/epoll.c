#include "asyncio/dynamicBuffer.h"
#include "asyncioInternal.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <sys/epoll.h>

#include "Debug.h"

#define MAX_EVENTS 256

extern const char *poolId;

enum pipeDescrs {
  Read = 0,
  Write
};

typedef enum pipeCmd {
  Reset = 0,
  Timeout,
  UserEvent
} pipeCmd;

typedef enum setMode {
  Add = 0,
  Del
} setMode;


typedef struct pipeMsg {
  pipeCmd cmd;
  void *data;
} pipeMsg;

typedef struct asyncOpList {
  asyncOp *head;
  asyncOp *tail;
} asyncOpList;

typedef struct fdStruct {
  int initialized;
  asyncOpList readOps;
  asyncOpList writeOps;
} fdStruct;

struct asyncOp {
  aioInfo info;
  timer_t timerId;
  int counter;
  asyncOpList *list;
  asyncOp *prev;
  asyncOp *next;
  int useInternalBuffer;
  void *internalBuffer;
  size_t internalBufferSize;
};


typedef struct epollBase {
  asyncBase B;
  int pipeFd[2];

  fdStruct *fdMap;
  int fdMapSize;
  
  int epollFd;
} epollBase;

void epollPostEmptyOperation(asyncBase *base);
void epollNextFinishedOperation(asyncBase *base);
aioObject *epollNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOp *epollNewAsyncOp(asyncBase *base);
void epollDeleteOp(asyncOp *op);
void epollStartTimer(asyncOp *op, uint64_t usTimeout, int count);
void epollStopTimer(asyncOp *op);
void epollActivate(asyncOp *op);
void epollAsyncConnect(asyncOp *op,
                       const HostAddress *address,
                       uint64_t usTimeout);
void epollAsyncAccept(asyncOp *op, uint64_t usTimeout);
void epollAsyncRead(asyncOp *op, uint64_t usTimeout);
void epollAsyncWrite(asyncOp *op, uint64_t usTimeout);
void epollAsyncReadMsg(asyncOp *op, uint64_t usTimeout);
void epollAsyncWriteMsg(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout);
void epollMonitor(asyncOp *op);
void epollMonitorStop(asyncOp *op);

static struct asyncImpl epollImpl = {
  epollPostEmptyOperation,
  epollNextFinishedOperation,
  epollNewAioObject,
  epollNewAsyncOp,
  epollDeleteOp,
  epollStartTimer,
  epollStopTimer,
  epollActivate,
  epollAsyncConnect,
  epollAsyncAccept,
  epollAsyncRead,
  epollAsyncWrite,
  epollAsyncReadMsg,
  epollAsyncWriteMsg,
  epollMonitor,
  epollMonitorStop
};

static void epollControl(int epollFd, int action, int events, int fd)
{
  struct epoll_event ev;
  ev.data.fd = fd;
  ev.events = events;
  if (epoll_ctl(epollFd,
                action,
                ev.data.fd,
                &ev) == -1)
    fprintf(stderr, "epoll_ctl error, errno: %s\n", strerror(errno));
}

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


static void initList(asyncOpList *list)
{
  list->head = 0;
  list->tail = 0;
}

static fdStruct *getFdStruct(epollBase *base, int fd)
{
  int i;
  fdStruct *fds;
  if (fd < base->fdMapSize) {
    fds = &base->fdMap[fd];
  } else {
    int newfdMapSize = base->fdMapSize;
    while (newfdMapSize <= fd)
      newfdMapSize *= 2;
    
    base->fdMap = realloc(base->fdMap, newfdMapSize*sizeof(fdStruct));
    for (i = base->fdMapSize; i < newfdMapSize; i++)
      base->fdMap[i].initialized = 0;
    
    base->fdMapSize = newfdMapSize;
    fds = &base->fdMap[fd];
  }
  
  if (!fds->initialized) {
    initList(&fds->readOps);
    initList(&fds->writeOps);
    fds->initialized = 1;
  }
  
  return fds;  
}

static void asyncOpLink(fdStruct *fds, asyncOp *op)
{
  asyncOpList *list;
  asyncOpList *oppositeList;
  int event;
  int oppositeEvent;
  if (isWriteOperation(op->info.currentAction)) {
    list = &fds->writeOps;
    oppositeList = &fds->readOps;
    event = EPOLLOUT;
    oppositeEvent = EPOLLIN;
  } else {
    list = &fds->readOps;    
    oppositeList = &fds->writeOps;
    event = EPOLLIN;
    oppositeEvent = EPOLLOUT;
  }
  
  if (list->tail) {
    list->tail->next = op;
    op->prev = list->tail;
    op->next = 0;
    list->tail = op;
  } else {
    op->prev = 0;
    op->next = 0;
    list->head = list->tail = op;
    epollBase *base = (epollBase*)op->info.object->base;
    int action = oppositeList->head ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    int events = oppositeList->head ? event | oppositeEvent : event;
    epollControl(base->epollFd, action, events, getFd(op));
  }

  op->list = list;
}

static void asyncOpUnlink(asyncOp *op)
{
  epollBase *base = (epollBase*)op->info.object->base;  
  asyncOpList *list = op->list;
  asyncOpList *oppositeList;
  fdStruct *fds = getFdStruct(base, getFd(op));
  int oppositeEvent;
  if (isWriteOperation(op->info.currentAction)) {
    oppositeList = &fds->readOps;
    oppositeEvent = EPOLLIN;
  } else {
    oppositeList = &fds->writeOps;
    oppositeEvent = EPOLLOUT;
  }  
  
  if (list) {
    if (list->head == op)
      list->head = op->next;
    if (list->tail == op)
      list->tail = op->prev;

    if (op->prev)
      op->prev->next = op->next;
    if (op->next)
      op->next->prev = op->prev;

    if (list->head == 0) {
      int action = oppositeList->head ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
      int events = oppositeList->head ? oppositeEvent : 0;
      epollControl(base->epollFd, action, events, getFd(op));
    }

    op->list = 0;
    objectRelease(&op->info.object->base->pool, op, poolId);
  }
}

static void timerCb(int sig, siginfo_t *si, void *uc)
{
  asyncOp *op = (asyncOp *)si->si_value.sival_ptr;
  epollBase *base = (epollBase *)op->info.object->base;

  pipeMsg msg = {Timeout, (void *)op};

  if (op->counter > 0)
    op->counter--;
  write(base->pipeFd[Write], &msg, sizeof(pipeMsg));
}

static void startTimer(asyncOp *op, uint64_t usTimeout, int periodic)
{
  struct itimerspec its;
  its.it_value.tv_sec = usTimeout / 1000000;
  its.it_value.tv_nsec = (usTimeout % 1000000) * 1000;
  its.it_interval.tv_sec = periodic ? its.it_value.tv_sec : 0;
  its.it_interval.tv_nsec = periodic ? its.it_value.tv_nsec : 0;
  if (timer_settime(op->timerId, 0, &its, NULL) == -1) {
    fprintf(stderr, " * startTimer: timer_settime error %s\n", strerror(errno));
  }
}

static void stopTimer(asyncOp *op)
{
  struct itimerspec its;
  op->counter = 0;
  memset(&its, 0, sizeof(its));
  if (timer_settime(op->timerId, 0, &its, NULL) == -1) {
    fprintf(stderr, " * epollStopTimer: timer_settime error\n");
  }
}

static void startOperation(asyncOp *op,
                           IoActionTy action,
                           uint64_t usTimeout)
{
  epollBase *localBase = (epollBase *)op->info.object->base;
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

  switch (action) {
    case ioMonitorStop :
      asyncOpUnlink(op);
      break;
    default :
      asyncOpLink(getFdStruct(localBase, getFd(op)), op);      
      break;
  }

  if (action == ioMonitorStop || action == ioMonitor)
    epollPostEmptyOperation((asyncBase *)localBase);

  if (usTimeout)
    startTimer(op, usTimeout, 0);
}

asyncBase *epollNewAsyncBase()
{
  epollBase *base = malloc(sizeof(epollBase));
  if (base) {
    struct sigaction sAction;

    pipe(base->pipeFd);
    base->B.methodImpl = epollImpl;

    base->fdMap = (fdStruct*)calloc(MAX_EVENTS, sizeof(fdStruct));
    base->fdMapSize = MAX_EVENTS;

    base->epollFd = epoll_create(MAX_EVENTS);
    if (base->epollFd == -1) {
      fprintf(stderr, " * epollNewAsyncBase: epoll_create ");
    }

    epollControl(base->epollFd, EPOLL_CTL_ADD, EPOLLIN, base->pipeFd[Read]);

    sAction.sa_flags = SA_SIGINFO | SA_RESTART;
    sAction.sa_sigaction = timerCb;
    sigemptyset(&sAction.sa_mask);

    if (sigaction(SIGRTMIN, &sAction, NULL) == -1) {
      fprintf(stderr, " * epollNewAsyncBase: sigaction error\n");
    }

    memset(&sAction, sizeof(sAction), 0);
    sAction.sa_handler = SIG_IGN;

    if (sigaction(SIGPIPE, &sAction, NULL) == -1) {
      fprintf(stderr, " * epollNewAsyncBase: sigaction error\n");
    }
  }

  return (asyncBase *)base;
}

void epollPostEmptyOperation(asyncBase *base)
{
  pipeMsg msg = {Reset, 0};

  epollBase *localBase = (epollBase *)base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}

static void finishOperation(asyncOp *op,
                            AsyncOpStatus status,
                            int needStopTimer)
{

  if (needStopTimer)
    stopTimer(op);
  op->info.status = status;
  if (op->info.callback)
    op->info.callback(&op->info);
  if (status != aosMonitoring)
    asyncOpUnlink(op);
}



static void processReadyFd(epollBase *base,
                           int fd,
                           int isRead)
{
  int available;

  fdStruct *fds = getFdStruct(base, fd);
  asyncOpList *list = isRead ? &fds->readOps : &fds->writeOps;
  asyncOp *op = list->head;
  if (!op)
    return;
  assert(fd == getFd(op) && "Lost asyncop found!");
  ioctl(fd, FIONREAD, &available);
  if (op->info.object->type == ioObjectSocket && available == 0 && isRead) {
    if (op->info.currentAction != ioAccept) {
      finishOperation(op, aosDisconnected, 1);
      return;
    }
  }

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
        accept(fd, (struct sockaddr *)&clientAddr, &clientAddrSize);

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
      int readyForRead = 0;
      uint8_t *ptr = (uint8_t *)op->info.buffer + op->info.bytesTransferred;
      readyForRead =
        (op->info.transactionSize - op->info.bytesTransferred
         < (size_t)available) ?
        op->info.transactionSize - op->info.bytesTransferred
        : (size_t)available;

      read(fd, ptr, readyForRead);
      op->info.bytesTransferred += readyForRead;
      if (op->info.bytesTransferred == op->info.transactionSize ||
          !(op->info.flags & afWaitAll))
        finishOperation(op, aosSuccess, 1);
      break;
    }
    case ioWrite : {
      void *buffer = op->useInternalBuffer ?
                     op->internalBuffer : op->info.buffer;
      uint8_t *ptr = (uint8_t*)buffer + op->info.bytesTransferred;
      size_t remaining = op->info.transactionSize - op->info.bytesTransferred;
      ssize_t bytesWritten = write(fd, ptr, remaining);
      if (bytesWritten == -1) {
        if (op->info.object->type == ioObjectSocket && errno == EPIPE) {
          finishOperation(op, aosDisconnected, 1);
        } else {
          finishOperation(op, aosUnknownError, 1);
        }
      } else {
        op->info.bytesTransferred += bytesWritten;
        if (op->info.bytesTransferred == op->info.transactionSize)
          finishOperation(op, aosSuccess, 1);
      }

      break;
    }
    case ioReadMsg : {
      void *ptr = dynamicBufferAlloc(op->info.dynamicArray, available);
      read(fd, ptr, available);
      op->info.bytesTransferred += available;
      finishOperation(op, aosSuccess, 1);
      break;
    }
    case ioWriteMsg : {
      struct sockaddr_in remoteAddress;
      void *ptr = op->useInternalBuffer ?
                  op->internalBuffer : op->info.buffer;
      remoteAddress.sin_family = op->info.host.family;
      remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
      remoteAddress.sin_port = op->info.host.port;

      sendto(fd, ptr, op->info.transactionSize, 0,
             (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
      finishOperation(op, aosSuccess, 1);
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

void epollNextFinishedOperation(asyncBase *base)
{
  int nfds, n, i;
  struct epoll_event events[MAX_EVENTS];
  epollBase *localBase = (epollBase *)base;
  pipeMsg msg;
  asyncOp *op;

  while (1) {
    do {
      nfds = epoll_wait(localBase->epollFd, events, MAX_EVENTS, -1);
    } while (nfds <= 0 && errno == EINTR);

    for (n = 0; n < nfds; n++) {
      if (events[n].data.fd == localBase->pipeFd[Read]) {
        int available;
        ioctl(localBase->pipeFd[Read], FIONREAD, &available);
        for (i = 0; i < available / sizeof(pipeMsg); i++) {
          read(localBase->pipeFd[Read], &msg, sizeof(pipeMsg));

          op = (asyncOp *)msg.data;
          switch (msg.cmd) {
            case Reset :
              return;
              break;
            case Timeout :
              if (op->info.object->type == ioObjectUserEvent)
                finishOperation(op, aosSuccess, op->counter == 0);
              else
                finishOperation(op, aosTimeout, 0);
              break;
            case UserEvent :
              finishOperation(op, aosSuccess, op->counter == 0);
              break;
          }
        }
      } else {
        if (events[n].events & EPOLLIN)
          processReadyFd(localBase, events[n].data.fd, 1);
        else if (events[n].events & EPOLLOUT)
          processReadyFd(localBase, events[n].data.fd, 0);
      }
    }
  }
}


aioObject *epollNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object = malloc(sizeof(aioObject));
  object->base = base;
  object->type = type;
  switch (object->type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy *)data;
      break;
    case ioObjectSocket :
    case ioObjectSocketSyn :
      object->hSocket = *(socketTy *)data;
      break;
    default :
      break;
  }

  return object;
}

asyncOp *epollNewAsyncOp(asyncBase *base)
{
  asyncOp *op = malloc(sizeof(asyncOp));
  if (op) {
    struct sigevent sEvent;
    op->internalBuffer = 0;
    op->internalBufferSize = 0;
    op->list = 0;
    op->next = 0;
    op->prev = 0;
    sEvent.sigev_notify = SIGEV_SIGNAL;
    sEvent.sigev_signo = SIGRTMIN;
    sEvent.sigev_value.sival_ptr = op;
    if (timer_create(CLOCK_REALTIME, &sEvent, &op->timerId) == -1) {
      fprintf(stderr,
              " * newepollOp: timer_create error %s\n",
              strerror(errno));
    }
  }

  return op;
}


void epollDeleteOp(asyncOp *op)
{
  asyncOpUnlink(op);
  free(op);
}


void epollStartTimer(asyncOp *op, uint64_t usTimeout, int count)
{
  op->counter = (count > 0) ? count : -1;
  startTimer(op, usTimeout, 1);
}


void epollStopTimer(asyncOp *op)
{
  stopTimer(op);
}


void epollActivate(asyncOp *op)
{
  pipeMsg msg = {UserEvent, (void *)op};
  epollBase *localBase = (epollBase *)op->info.object->base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}


void epollAsyncConnect(asyncOp *op,
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


void epollAsyncAccept(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioAccept, usTimeout);
}


void epollAsyncRead(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioRead, usTimeout);
}


void epollAsyncWrite(asyncOp *op, uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);
  startOperation(op, ioWrite, usTimeout);
}


void epollAsyncReadMsg(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioReadMsg, usTimeout);
}


void epollAsyncWriteMsg(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);
  op->info.host = *address;
  startOperation(op, ioWriteMsg, usTimeout);
}


void epollMonitor(asyncOp *op)
{
  startOperation(op, ioMonitor, 0);
}


void epollMonitorStop(asyncOp *op)
{
  startOperation(op, ioMonitorStop, 0);
}


