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

#include <map>
#include <queue>
#include <stdio.h>

extern const char *poolId;

struct asyncOpList;

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


struct asyncOpList {
  asyncOp *head;
  asyncOp *tail;
};


typedef std::map<int, asyncOpList*> OpLinksMap;


typedef struct selectBase {
  asyncBase B;
  int pipeFd[2];  
  OpLinksMap readOps;
  OpLinksMap writeOps;
} selectBase;


void selectPostEmptyOperation(asyncBase *base);
void selectNextFinishedOperation(asyncBase *base);
aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOp *selectNewAsyncOp(asyncBase *base);
void selectDeleteOp(asyncOp *op);
void selectStartTimer(asyncOp *op, uint64_t usTimeout, int count);
void selectStopTimer(asyncOp *op);
void selectActivate(asyncOp *op);
void selectAsyncConnect(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout);
void selectAsyncAccept(asyncOp *op, uint64_t usTimeout);
void selectAsyncRead(asyncOp *op, uint64_t usTimeout);
void selectAsyncWrite(asyncOp *op, uint64_t usTimeout);
void selectAsyncReadMsg(asyncOp *op, uint64_t usTimeout);
void selectAsyncWriteMsg(asyncOp *op,
                         const HostAddress *address,
                         uint64_t usTimeout);
void selectMonitor(asyncOp *op);
void selectMonitorStop(asyncOp *op);


static struct asyncImpl selectImpl = {
  selectPostEmptyOperation,
  selectNextFinishedOperation,
  selectNewAioObject,
  selectNewAsyncOp,
  selectDeleteOp,
  selectStartTimer,
  selectStopTimer,
  selectActivate,
  selectAsyncConnect,
  selectAsyncAccept,
  selectAsyncRead,
  selectAsyncWrite,
  selectAsyncReadMsg,
  selectAsyncWriteMsg,
  selectMonitor,
  selectMonitorStop
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


static void asyncOpLink(asyncOpList *list, asyncOp *op)
{
  if (list->tail) {
    list->tail->next = op;
    op->prev = list->tail;
    op->next = 0;
    list->tail = op;
  } else {
    op->prev = 0;
    op->next = 0;
    list->head = list->tail = op;
  }
  
  op->list = list;
}


static void asyncOpUnlink(asyncOp *op)
{
  if (op->list) {
    asyncOpList *list = op->list;
    if (list->head == op)
      list->head = op->next;
    if (list->tail == op)
      list->tail = op->prev;
    
    if (op->prev)
      op->prev->next = op->next;
    if (op->next)
      op->next->prev = op->prev;

    op->list = 0;
//     cqueue_ptr_push(&op->info.object->base->asyncOps, op);
    objectRelease(&op->info.object->base->pool, op, poolId);
  }
}


static asyncOpList *getFdOperations(OpLinksMap &opMap, int fd)
{
  OpLinksMap::iterator F = opMap.find(fd);
  if (F == opMap.end()) {
    asyncOpList *list = new asyncOpList;
    list->head = 0;
    list->tail = 0;
    F = opMap.insert(F, std::make_pair(fd, list));
  }
  return F->second;
}


static void timerCb(int sig, siginfo_t *si, void *uc)
{
  asyncOp *op = (asyncOp*)si->si_value.sival_ptr;
  selectBase *base = (selectBase*)op->info.object->base;
  
  if (op->counter > 0)
    op->counter--;
  write(base->pipeFd[1], &op, sizeof(op));
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
    fprintf(stderr, " * selectStopTimer: timer_settime error\n");    
  }  
}


static void startOperation(asyncOp *op,
                           IoActionTy action,
                           uint64_t usTimeout)
{
  selectBase *localBase = (selectBase*)op->info.object->base;
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


  OpLinksMap &links = isWriteOperation(action) ?
    localBase->writeOps : localBase->readOps;
  
  switch (action) {
    case ioMonitorStop :
      asyncOpUnlink(op);
      break;
    default :
      asyncOpLink(getFdOperations(links, getFd(op)), op);
      break;
  }

  if (action == ioMonitorStop || action == ioMonitor)
    selectPostEmptyOperation((asyncBase*)localBase);
  
  if (usTimeout)
    startTimer(op, usTimeout, 0);
}


extern "C" asyncBase *selectNewAsyncBase()
{
  selectBase *base = new selectBase;
  if (base) {
    struct sigaction sAction;

    pipe(base->pipeFd);    
    base->B.methodImpl = selectImpl;
    
    sAction.sa_flags = SA_SIGINFO | SA_RESTART;
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


static void processReadyFds(selectBase *base,
                            OpLinksMap &links,
                            fd_set *fds,
                            int isRead)
{
  for (OpLinksMap::iterator I = links.begin(), IE = links.end(); I != IE; ++I) {
    int fd = I->first;    
    if (!FD_ISSET(fd, fds))
      continue;
  
    int available;
    asyncOpList *list = getFdOperations(links, fd);
    asyncOp *op = list->head;
    if (!op)
      continue;
  
    assert(fd == getFd(op) && "Lost asyncop found!");
    ioctl(fd, FIONREAD, &available);  
    if (op->info.object->type == ioObjectSocket && available == 0 && isRead) {
      if (op->info.currentAction != ioAccept) {
        finishOperation(op, aosDisconnected, 1);
        continue;
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
          accept(fd, (sockaddr*)&clientAddr, &clientAddrSize);
                
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
        uint8_t *ptr = (uint8_t*)op->info.buffer + op->info.bytesTransferred;
        readyForRead =
          std::min(op->info.transactionSize - op->info.bytesTransferred,
                   (size_t)available);
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
               (sockaddr*)&remoteAddress, sizeof(remoteAddress));
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
}


void selectNextFinishedOperation(asyncBase *base)
{
  selectBase *localBase = (selectBase*)base;
  
  while (1) {
    int nfds;
    int result;
    fd_set readFds;
    fd_set writeFds;
    FD_ZERO(&readFds);
    FD_ZERO(&writeFds);
    FD_SET(localBase->pipeFd[0], &readFds);

    nfds = localBase->pipeFd[0] + 1;      
    for (OpLinksMap::iterator I = localBase->readOps.begin(),
         IE = localBase->readOps.end(); I != IE; ++I) {
      if (I->second->head) {
        nfds = std::max(nfds, I->first+1);
        FD_SET(I->first, &readFds);
      }
    }
      
    for (OpLinksMap::iterator I = localBase->writeOps.begin(),
         IE = localBase->writeOps.end(); I != IE; ++I) {  
      asyncOp *op = I->second->head;
      if (op) {
        nfds = std::max(nfds, I->first+1);
        if (op->info.object->type == ioObjectSocket)
          FD_SET(I->first, &readFds);
        FD_SET(I->first, &writeFds);
      }
    }

    do {
      result = select(nfds, &readFds, &writeFds, NULL, NULL);
    } while (result <= 0 && errno == EINTR);

    if (FD_ISSET(localBase->pipeFd[0], &readFds)) {
      int available;
      ioctl(localBase->pipeFd[0], FIONREAD, &available);
      asyncOp *op;
      for (int i = 0; i < available/sizeof(op); i++) {
        read(localBase->pipeFd[0], &op, sizeof(op));
        if (!op)
          return;
    
        if (op) {
          if (op->info.object->type == ioObjectUserEvent)
            finishOperation(op, aosSuccess, op->counter == 0);
          else
            finishOperation(op, aosTimeout, 0);   
        }
      }
    }

    processReadyFds(localBase, localBase->readOps, &readFds, 1);
    processReadyFds(localBase, localBase->writeOps, &writeFds, 0);
  } 
}


aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data)
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
  }

  return object;
}


asyncOp *selectNewAsyncOp(asyncBase *base)
{
  asyncOp *op = new asyncOp;
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
              " * newSelectOp: timer_create error %s\n",
              strerror(errno));
    }
  }

  return op;
}


void selectDeleteOp(asyncOp *op)
{
  asyncOpUnlink(op);
  delete op;
}


void selectStartTimer(asyncOp *op, uint64_t usTimeout, int count)
{
  op->counter = (count > 0) ? count : -1;
  startTimer(op, usTimeout, 1); 
}


void selectStopTimer(asyncOp *op)
{
  stopTimer(op);
}


void selectActivate(asyncOp *op)
{
  selectBase *localBase = (selectBase*)op->info.object->base;
  write(localBase->pipeFd[1], op, sizeof(op));
}


void selectAsyncConnect(asyncOp *op,
                        const HostAddress *address,
                        uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  int err = connect(op->info.object->hSocket,
                    (sockaddr*)&localAddress,
                    sizeof(localAddress));

  if (err == -1 && errno != EINPROGRESS) {
    fprintf(stderr, "connect error, errno: %s\n", strerror(errno));
   return;
  }

  startOperation(op, ioConnect, usTimeout);
}


void selectAsyncAccept(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioAccept, usTimeout);
}


void selectAsyncRead(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioRead, usTimeout);
}


void selectAsyncWrite(asyncOp *op, uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);
  startOperation(op, ioWrite, usTimeout);
}


void selectAsyncReadMsg(asyncOp *op, uint64_t usTimeout)
{
  startOperation(op, ioReadMsg, usTimeout);
}


void selectAsyncWriteMsg(asyncOp *op,
                         const HostAddress *address,
                         uint64_t usTimeout)
{
  op->useInternalBuffer = !(op->info.flags & afNoCopy);  
  op->info.host = *address;
  startOperation(op, ioWriteMsg, usTimeout);
}


void selectMonitor(asyncOp *op)
{
  startOperation(op, ioMonitor, 0);
}


void selectMonitorStop(asyncOp *op)
{
  startOperation(op, ioMonitorStop, 0);
}
