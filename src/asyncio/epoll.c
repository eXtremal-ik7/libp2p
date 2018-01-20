#include "asyncio/coroutine.h"
#include "asyncio/dynamicBuffer.h"
#include "asyncioInternal.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>
#include <sys/epoll.h>

#define MAX_EVENTS 256

void userEventTrigger(aioObjectRoot *event);

enum pipeDescrs {
  Read = 0,
  Write
};

typedef enum pipeCmd {
  Reset = 0,
  Timeout,
  UserEvent
} pipeCmd;


typedef struct pipeMsg {
  pipeCmd cmd;
  void *data;
} pipeMsg;

typedef struct fdStruct {
  aioObject *object;
  int mask;
} fdStruct;

typedef struct epollOp {
  asyncOp info;
} epollOp;


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
asyncOpRoot *epollNewAsyncOp(asyncBase *base);
void epollDeleteObject(aioObject *object);
void epollFinishOp(epollOp *op);
void epollStartTimer(asyncOpRoot *op, uint64_t usTimeout, int count);
void epollStopTimer(asyncOpRoot *op);
void epollActivate(asyncOpRoot *op);
void epollAsyncConnect(epollOp *op,
                       const HostAddress *address,
                       uint64_t usTimeout);
void epollAsyncAccept(epollOp *op, uint64_t usTimeout);
void epollAsyncRead(epollOp *op, uint64_t usTimeout);
void epollAsyncWrite(epollOp *op, uint64_t usTimeout);
void epollAsyncReadMsg(epollOp *op, uint64_t usTimeout);
void epollAsyncWriteMsg(epollOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout);
void epollMonitor(epollOp *op);
void epollMonitorStop(epollOp *op);

static struct asyncImpl epollImpl = {
  epollPostEmptyOperation,
  epollNextFinishedOperation,
  epollNewAioObject,
  epollNewAsyncOp,
  epollDeleteObject,
  (finishOpTy*)epollFinishOp,
  (startTimerTy*)epollStartTimer,
  (stopTimerTy*)epollStopTimer,
  (activateTy*)epollActivate,
  (asyncConnectTy*)epollAsyncConnect,
  (asyncAcceptTy*)epollAsyncAccept,
  (asyncReadTy*)epollAsyncRead,
  (asyncWriteTy*)epollAsyncWrite,
  (asyncReadMsgTy*)epollAsyncReadMsg,
  (asyncWriteMsgTy*)epollAsyncWriteMsg
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
  return (action == actConnect ||
          action == actWrite ||
          action == actWriteMsg);
}

static aioObject *getObject(epollOp *op)
{
  return (aioObject*)op->info.root.object;
}

static int getFd(epollOp *op)
{
  aioObject *object = getObject(op);
  switch (object->root.type) {
    case ioObjectDevice :
      return object->hDevice;
      break;
    case ioObjectSocket :
    case ioObjectSocketSyn :
      return object->hSocket;
      break;
    default :
      return -1;
      break;
  }
}

static fdStruct *getFdStruct(epollBase *base, int fd)
{
  fdStruct *fds;
  if (fd < base->fdMapSize) {
    fds = &base->fdMap[fd];
  } else {
    int newfdMapSize = base->fdMapSize;
    while (newfdMapSize <= fd)
      newfdMapSize *= 2;

    base->fdMap = realloc(base->fdMap, newfdMapSize*sizeof(fdStruct));
    memset(&base->fdMap[base->fdMapSize], 0, (newfdMapSize-base->fdMapSize)*sizeof(fdStruct));
    base->fdMapSize = newfdMapSize;
    
    fds = &base->fdMap[fd];
  }
  
  return fds;  
}

static void asyncOpLink(fdStruct *fds, epollOp *op)
{
  int oldMask = fds->mask;
  fds->mask |= isWriteOperation(op->info.root.opCode) ? EPOLLOUT : EPOLLIN;
  if (oldMask != fds->mask) {
    int action = oldMask ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epollControl(((epollBase*)op->info.root.base)->epollFd, action, fds->mask, getFd(op));
  }
  
  fds->object = getObject(op);
}

static void asyncOpUnlink(epollOp *op)
{
  if (!op->info.root.executeQueue.next) {
    epollBase *base = (epollBase*)op->info.root.base;
    int fd = getFd(op);
    fdStruct *fds = getFdStruct(base, fd);
    fds->mask &= ~(isWriteOperation(op->info.root.opCode) ? EPOLLOUT : EPOLLIN);
    int action = fds->mask ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
    epollControl(base->epollFd, action, fds->mask, fd);
  }
}

static void timerCb(int sig, siginfo_t *si, void *uc)
{
  asyncOpRoot *op = (asyncOpRoot*)si->si_value.sival_ptr;
  epollBase *base = (epollBase *)op->base;

  pipeMsg msg = {Timeout, (void *)op};

  if (op->counter > 0)
    op->counter--;
  write(base->pipeFd[Write], &msg, sizeof(pipeMsg));
}

static void startTimer(asyncOpRoot *op, uint64_t usTimeout, int periodic)
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

static void stopTimer(asyncOpRoot *op)
{
  struct itimerspec its;
  op->counter = 0;
  memset(&its, 0, sizeof(its));
  if (timer_settime(op->timerId, 0, &its, NULL) == -1) {
    fprintf(stderr, " * epollStopTimer: timer_settime error\n");
  }
}

static void startOperation(epollOp *op,
                           IoActionTy action,
                           uint64_t usTimeout)
{
  epollBase *localBase = (epollBase *)op->info.root.base;
  asyncOpLink(getFdStruct(localBase, getFd(op)), op);
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

    memset(&sAction, 0, sizeof(sAction));
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

static void finish(epollOp *op, AsyncOpStatus status)
{
  asyncOpUnlink(op);
  finishOperation(&op->info.root, status, 1);
}



static void processReadyFd(epollBase *base,
                           int fd,
                           int isRead)
{
  int available;

  fdStruct *fds = getFdStruct(base, fd);
  aioObject *object = fds->object;
  epollOp *op = (epollOp*)(isRead ? object->root.readQueue.head : object->root.writeQueue.head);
  if (!op)
    return;
  assert(fd == getFd(op) && "Lost asyncop found!");
  ioctl(fd, FIONREAD, &available);
  if (object->root.type == ioObjectSocket && available == 0 && isRead) {
    if (op->info.root.opCode != actAccept) {
      finish(op, aosDisconnected);
      return;
    }
  }

  switch (op->info.root.opCode) {
    case actConnect : {
      int error;
      socklen_t size = sizeof(error);
      getsockopt(object->hSocket, SOL_SOCKET, SO_ERROR, &error, &size);
      finish(op, (error == 0) ? aosSuccess : aosUnknownError);
      break;
    }
    case actAccept : {
      struct sockaddr_in clientAddr;
      socklen_t clientAddrSize = sizeof(clientAddr);
      op->info.acceptSocket =
        accept(fd, (struct sockaddr *)&clientAddr, &clientAddrSize);

      if (op->info.acceptSocket != -1) {
        op->info.host.family = 0;
        op->info.host.ipv4 = clientAddr.sin_addr.s_addr;
        op->info.host.port = clientAddr.sin_port;
        finish(op, aosSuccess);
      } else {
        finish(op, aosUnknownError);
      }
      break;
    }
    case actRead : {
      int readyForRead = 0;
      uint8_t *ptr = (uint8_t *)op->info.buffer + op->info.bytesTransferred;
      readyForRead =
        (op->info.transactionSize - op->info.bytesTransferred
         < (size_t)available) ?
        op->info.transactionSize - op->info.bytesTransferred
        : (size_t)available;

      ssize_t bytesRead = read(fd, ptr, readyForRead);
      op->info.bytesTransferred += bytesRead;
      if (op->info.bytesTransferred == op->info.transactionSize ||
          !(op->info.root.flags & afWaitAll))
        finish(op, aosSuccess);
      break;
    }
    case actWrite : {
      void *buffer = op->info.buffer;
      uint8_t *ptr = (uint8_t*)buffer + op->info.bytesTransferred;
      size_t remaining = op->info.transactionSize - op->info.bytesTransferred;
      ssize_t bytesWritten = write(fd, ptr, remaining);
      if (bytesWritten == -1) {
        if (object->root.type == ioObjectSocket && errno == EPIPE) {
          finish(op, aosDisconnected);
        } else {
          finish(op, aosUnknownError);
        }
      } else {
        op->info.bytesTransferred += bytesWritten;
        if (op->info.bytesTransferred == op->info.transactionSize)
          finish(op, aosSuccess);
      }

      break;
    }
    case actReadMsg : {
      void *ptr = dynamicBufferAlloc(op->info.dynamicArray, available);
      read(fd, ptr, available);
      op->info.bytesTransferred += available;
      finish(op, aosSuccess);
      break;
    }
    case actWriteMsg : {
      struct sockaddr_in remoteAddress;
      void *ptr = op->info.buffer;
      remoteAddress.sin_family = op->info.host.family;
      remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
      remoteAddress.sin_port = op->info.host.port;

      sendto(fd, ptr, op->info.transactionSize, 0,
             (struct sockaddr *)&remoteAddress, sizeof(remoteAddress));
      finish(op, aosSuccess);
      break;
    }
    case actMonitor : {
      finish(op, aosMonitoring);
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

  while (1) {
    do {
      nfds = epoll_wait(localBase->epollFd, events, MAX_EVENTS, 1000);
    } while (nfds <= 0 && errno == EINTR);
    
    processTimeoutQueue(base);

    for (n = 0; n < nfds; n++) {
      if (events[n].data.fd == localBase->pipeFd[Read]) {
        int available;
        ioctl(localBase->pipeFd[Read], FIONREAD, &available);
        for (i = 0; i < available / sizeof(pipeMsg); i++) {
          read(localBase->pipeFd[Read], &msg, sizeof(pipeMsg));
          asyncOpRoot *op = (asyncOpRoot*)msg.data;
          aioObjectRoot *object;
          switch (msg.cmd) {
            case Reset :
              return;
            case Timeout :
              object = op->object;
              if (object->type == ioObjectUserEvent) {
                if (op->counter == 0)
                  stopTimer(op);
                userEventTrigger(object);
              } else {
                if (object->type != ioObjectUserDefined)
                  asyncOpUnlink((epollOp*)op);
                finishOperation(op, aosTimeout, 0);
              }
              break;
            case UserEvent :
              userEventTrigger(op->object);
              break;
          }
        }
      } else {
        if (events[n].events & EPOLLIN)
          processReadyFd(localBase, events[n].data.fd, 1);
        if (events[n].events & EPOLLOUT)
          processReadyFd(localBase, events[n].data.fd, 0);
      }
    }
  }
}


aioObject *epollNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object =
    (aioObject*)initObjectRoot(type, sizeof(aioObject), (aioObjectDestructor*)epollDeleteObject);
  switch (type) {
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

asyncOpRoot *epollNewAsyncOp(asyncBase *base)
{
  epollOp *op = malloc(sizeof(epollOp));
  if (op) {
    op->info.internalBuffer = 0;
    op->info.internalBufferSize = 0;
  }

  return (asyncOpRoot*)op;
}


void epollDeleteObject(aioObject *object)
{
  free(object);
}

void epollFinishOp(epollOp *op)
{
  asyncOpUnlink(op);
}

void epollStartTimer(asyncOpRoot *op, uint64_t usTimeout, int count)
{
  // only for user event, 'op' must have timer
  op->counter = (count > 0) ? count : -1;
  startTimer(op, usTimeout, 1);
}


void epollStopTimer(asyncOpRoot *op)
{
  // only for user event, 'op' must have timer  
  stopTimer(op);
}


void epollActivate(asyncOpRoot *op)
{
  pipeMsg msg = {UserEvent, (void *)op};
  epollBase *localBase = (epollBase *)op->base;
  write(localBase->pipeFd[Write], &msg, sizeof(pipeMsg));
}


void epollAsyncConnect(epollOp *op,
                       const HostAddress *address,
                       uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  int err = connect(getObject(op)->hSocket, (struct sockaddr *)&localAddress, sizeof(localAddress));

  if (err == -1 && errno != EINPROGRESS) {
    fprintf(stderr, "connect error, errno: %s\n", strerror(errno));
    return;
  }

  startOperation(op, actConnect, usTimeout);
}


void epollAsyncAccept(epollOp *op, uint64_t usTimeout)
{
  startOperation(op, actAccept, usTimeout);
}


void epollAsyncRead(epollOp *op, uint64_t usTimeout)
{
  startOperation(op, actRead, usTimeout);
}


void epollAsyncWrite(epollOp *op, uint64_t usTimeout)
{
  startOperation(op, actWrite, usTimeout);
}


void epollAsyncReadMsg(epollOp *op, uint64_t usTimeout)
{
  startOperation(op, actReadMsg, usTimeout);
}


void epollAsyncWriteMsg(epollOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout)
{
  op->info.host = *address;
  startOperation(op, actWriteMsg, usTimeout);
}


void epollMonitor(epollOp *op)
{
  startOperation(op, actMonitor, 0);
}


void epollMonitorStop(epollOp *op)
{
  startOperation(op, actMonitorStop, 0);
}
