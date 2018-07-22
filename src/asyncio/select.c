#include "asyncioImpl.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <stdio.h>

void userEventTrigger(aioObjectRoot *event);

typedef struct selectOp {
  asyncOp info;
} selectOp;


typedef enum {
  mtRead = 1,
  mtWrite = 2,
  mtError = 4
} MaskTy;

typedef struct fdStruct {
  aioObject *object;
  int mask;
} fdStruct;

typedef struct selectBase {
  asyncBase B;
  int pipeFd[2];  
  fdStruct *fdMap;
} selectBase;


void selectPostEmptyOperation(asyncBase *base);
void selectNextFinishedOperation(asyncBase *base);
aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *selectNewAsyncOp(asyncBase *base);
void selectDeleteObject(aioObject *object);
void selectFinishOp(selectOp *op);
void selectInitializeTimer(asyncOpRoot *op);
void selectStartTimer(asyncOpRoot *op, uint64_t usTimeout);
void selectStopTimer(asyncOpRoot *op);
void selectActivate(asyncOpRoot *op);
void selectAsyncConnect(selectOp *op, const HostAddress *address, uint64_t usTimeout);
void selectAsyncAccept(selectOp *op, uint64_t usTimeout);
void selectAsyncRead(selectOp *op, uint64_t usTimeout);
void selectAsyncWrite(selectOp *op, uint64_t usTimeout);
void selectAsyncReadMsg(selectOp *op, uint64_t usTimeout);
void selectAsyncWriteMsg(selectOp *op, const HostAddress *address, uint64_t usTimeout);


static struct asyncImpl selectImpl = {
  selectPostEmptyOperation,
  selectNextFinishedOperation,
  selectNewAioObject,
  selectNewAsyncOp,
  selectDeleteObject,
  (finishOpTy*)selectFinishOp,
  selectInitializeTimer,
  (startTimerTy*)selectStartTimer,
  (stopTimerTy*)selectStopTimer,
  (activateTy*)selectActivate,
  (asyncConnectTy*)selectAsyncConnect,
  (asyncAcceptTy*)selectAsyncAccept,
  (asyncReadTy*)selectAsyncRead,
  (asyncWriteTy*)selectAsyncWrite,
  (asyncReadMsgTy*)selectAsyncReadMsg,
  (asyncWriteMsgTy*)selectAsyncWriteMsg
};


static int isWriteOperation(int action)
{
  return (action == actConnect ||
          action == actWrite ||
          action == actWriteMsg);
}

static aioObject *getObject(selectOp *op)
{
  return (aioObject*)op->info.root.object;
}

static int getFd(selectOp *op)
{
  aioObject *object = getObject(op);
  switch (object->root.type) {
    case ioObjectDevice :
      return object->hDevice;
      break;
    case ioObjectSocket :
      return object->hSocket;
      break;
    default :
      return -1;
      break;      
  }
}

static fdStruct *getFdOperations(selectBase *base, int fd)
{
  return &base->fdMap[fd];  
}


static void asyncOpLink(fdStruct *list, selectOp *op)
{
  list->mask |= isWriteOperation(op->info.root.opCode) ? mtWrite : mtRead;
  list->object = getObject(op);
}


static void asyncOpUnlink(selectOp *op)
{
  if (!op->info.root.executeQueue.next) {
    selectBase *localBase = (selectBase*)op->info.root.base;
    fdStruct *list = getFdOperations(localBase, getFd(op));
    list->mask &= ~(isWriteOperation(op->info.root.opCode) ? mtWrite : mtRead);
  }
}

static void timerCb(int sig, siginfo_t *si, void *uc)
{
  asyncOpRoot *op = (asyncOpRoot*)si->si_value.sival_ptr;
  selectBase *base = (selectBase*)op->base;
  
  if (op->opCode == actUserEvent) {
    aioUserEvent *event = (aioUserEvent*)op;
    if (event->counter > 0)
      event->counter--;
  }
  
  write(base->pipeFd[1], &op, sizeof(op));
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
  memset(&its, 0, sizeof(its));
  if (timer_settime(op->timerId, 0, &its, NULL) == -1) {
    fprintf(stderr, " * selectStopTimer: timer_settime error\n");    
  }  
}


static void startOperation(selectOp *op,
                           IoActionTy action,
                           uint64_t usTimeout)
{
  selectBase *localBase = (selectBase*)op->info.root.base;
/*
  OpLinksMap &links = isWriteOperation(action) ?
    localBase->writeOps : localBase->readOps;*/
  
  asyncOpLink(getFdOperations(localBase, getFd(op)), op);    
}


asyncBase *selectNewAsyncBase()
{
  selectBase *base = malloc(sizeof(selectBase));
  if (base) {
    struct sigaction sAction;

    pipe(base->pipeFd);    
    base->fdMap = (fdStruct*)calloc(getdtablesize(), sizeof(fdStruct));
    base->B.methodImpl = selectImpl;

#ifdef OS_QNX    
    sAction.sa_flags = SA_SIGINFO;
#else
    sAction.sa_flags = SA_SIGINFO | SA_RESTART;    
#endif
    sAction.sa_sigaction = timerCb;
    sigemptyset(&sAction.sa_mask);
    if (sigaction(SIGRTMIN, &sAction, NULL) == -1) {
      fprintf(stderr, " * selectNewAsyncBase: sigaction error\n");
    }
    
    memset(&sAction, 0, sizeof(sAction));
    sAction.sa_handler = SIG_IGN;        
    if (sigaction(SIGPIPE, &sAction, NULL) == -1) {
      fprintf(stderr, " * selectNewAsyncBase: sigaction error\n");
    }
  }

  return (asyncBase*)base;
}


void selectPostEmptyOperation(asyncBase *base)
{
  void *p = 0;
  selectBase *localBase = (selectBase*)base;  
  write(localBase->pipeFd[1], &p, sizeof(p));
}


static void finish(selectOp *op, AsyncOpStatus status)
{
  asyncOpUnlink(op);
  finishOperation(&op->info.root, status, 1);
}


static void processReadyFds(selectBase *base,
                            fd_set *fds,
                            int nfds,
                            int isRead)
{
  for (int fd = 0; fd < nfds; fd++) {
    fdStruct *list = getFdOperations(base, fd);
    if (!FD_ISSET(fd, fds) || !list->object)
      continue;
  
    int available;
    aioObject *object = list->object;
    selectOp *op = (selectOp*)(isRead ? object->root.readQueue.head : object->root.writeQueue.head);    
    if (!op)
      continue;
  
    assert(fd == getFd(op) && "Lost asyncop found!");
    ioctl(fd, FIONREAD, &available);  
    if (object->root.type == ioObjectSocket && available == 0 && isRead) {
      if (op->info.root.opCode != actAccept) {
        finish(op, aosDisconnected);
        continue;
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
          accept(fd, (struct sockaddr*)&clientAddr, &clientAddrSize);
                
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
          (op->info.transactionSize - op->info.bytesTransferred < (size_t)available) ?
            op->info.transactionSize - op->info.bytesTransferred :
            (size_t)available;
        read(fd, ptr, readyForRead);
        op->info.bytesTransferred += readyForRead;
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
        if (available <= op->info.transactionSize) {
          struct sockaddr_in source;
          socklen_t addrlen = sizeof(source);
          recvfrom(fd, op->info.buffer, available, 0, (struct sockaddr*)&source, &addrlen);
          op->info.host.family = 0;
          op->info.host.ipv4 = source.sin_addr.s_addr;
          op->info.host.port = source.sin_port;
          op->info.bytesTransferred += available;
          finish(op, aosSuccess);
        } else {
          finish(op, aosBufferTooSmall);
        }
        break;
      }

      case actWriteMsg : {
        struct sockaddr_in remoteAddress;
        void *ptr = op->info.buffer;
        remoteAddress.sin_family = op->info.host.family;
        remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
        remoteAddress.sin_port = op->info.host.port;

        sendto(fd, ptr, op->info.transactionSize, 0,
               (struct sockaddr*)&remoteAddress, sizeof(remoteAddress));
        finish(op, aosSuccess);
        break;
      }
      
      default :
        break;
    }        
  }
}


void selectNextFinishedOperation(asyncBase *base)
{
  selectBase *localBase = (selectBase*)base;
  struct timeval tv;
  
  while (1) {
    int nfds;
    int result;
    fd_set readFds;
    fd_set writeFds;
    FD_ZERO(&readFds);
    FD_ZERO(&writeFds);
    FD_SET(localBase->pipeFd[0], &readFds);

    nfds = localBase->pipeFd[0] + 1;      
    for (int i = 0; i < FD_SETSIZE; i++) {
      fdStruct *fds = getFdOperations(localBase, i);
      if (fds->mask & mtRead)
        FD_SET(i, &readFds);
      if (fds->mask & mtWrite)
        FD_SET(i, &writeFds);
      nfds = i+1;
    }

    do {
      tv.tv_sec = 0;
      tv.tv_usec = 500*1000;      
      result = select(nfds, &readFds, &writeFds, NULL, &tv);
      if (result == 0)
        processTimeoutQueue(base);
    } while (result <= 0 && errno == EINTR);

    if (FD_ISSET(localBase->pipeFd[0], &readFds)) {
      int available;
      ioctl(localBase->pipeFd[0], FIONREAD, &available);
      asyncOpRoot *op;
      for (int i = 0; i < available/(int)sizeof(op); i++) {
        read(localBase->pipeFd[0], &op, sizeof(op));
        if (!op)
          return;
    
        if (op) {
          if (op->opCode == actUserEvent) {
            aioUserEvent *event = (aioUserEvent*)op;
            if (event->counter == 0)
              stopTimer(op);
            event->root.finishMethod(&event->root, aosSuccess);
          } else {
            if (op->object->type != ioObjectUserDefined)
              asyncOpUnlink((selectOp*)op);
            finishOperation(op, aosTimeout, 0);
          }
        }
      }
    }

    processReadyFds(localBase, &readFds, nfds, 1);
    processReadyFds(localBase, &writeFds, nfds, 0);
  } 
}


aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object =
    (aioObject*)initObjectRoot(type, sizeof(aioObject), (aioObjectDestructor*)selectDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy*)data;
    case ioObjectSocket :
      object->hSocket = *(socketTy*)data;
      break;
    default :
      break;
  }

  return object;
}


asyncOpRoot *selectNewAsyncOp(asyncBase *base)
{
  selectOp *op = malloc(sizeof(selectOp));
  if (op) {
    op->info.internalBuffer = 0;
    op->info.internalBufferSize = 0;
  }

  return (asyncOpRoot*)op;
}


void selectDeleteObject(aioObject *object)
{
  free(object);
}

void selectFinishOp(selectOp *op)
{
  asyncOpUnlink(op);
}

void selectInitializeTimer(asyncOpRoot *op)
{
  timer_t timerId = 0;
  struct sigevent sEvent;
  sEvent.sigev_notify = SIGEV_SIGNAL;
  sEvent.sigev_signo = SIGRTMIN;
  sEvent.sigev_value.sival_ptr = op;
  timer_create(CLOCK_REALTIME, &sEvent, &timerId);
  op->timerId = (void*)timerId;
}

void selectStartTimer(asyncOpRoot *op, uint64_t usTimeout)
{
  startTimer(op, usTimeout, op->opCode == actUserEvent);
}


void selectStopTimer(asyncOpRoot *op)
{
  stopTimer(op);
}


void selectActivate(asyncOpRoot *op)
{
  selectBase *localBase = (selectBase*)op->base;
  write(localBase->pipeFd[1], op, sizeof(op));
}


void selectAsyncConnect(selectOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  int err = connect(getObject(op)->hSocket, (struct sockaddr*)&localAddress, sizeof(localAddress));

  if (err == -1 && errno != EINPROGRESS) {
    fprintf(stderr, "connect error, errno: %s\n", strerror(errno));
   return;
  }

  startOperation(op, actConnect, usTimeout);
}


void selectAsyncAccept(selectOp *op, uint64_t usTimeout)
{
  startOperation(op, actAccept, usTimeout);
}


void selectAsyncRead(selectOp *op, uint64_t usTimeout)
{
  startOperation(op, actRead, usTimeout);
}


void selectAsyncWrite(selectOp *op, uint64_t usTimeout)
{
  startOperation(op, actWrite, usTimeout);
}


void selectAsyncReadMsg(selectOp *op, uint64_t usTimeout)
{
  startOperation(op, actReadMsg, usTimeout);
}


void selectAsyncWriteMsg(selectOp *op,
                         const HostAddress *address,
                         uint64_t usTimeout)
{
  op->info.host = *address;
  startOperation(op, actWriteMsg, usTimeout);
}
