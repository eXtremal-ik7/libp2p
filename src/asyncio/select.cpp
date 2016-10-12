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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <map>
#include <stdio.h>

extern "C" void userEventTrigger(aioObjectRoot *event);

typedef struct selectOp {
  asyncOp info;
} selectOp;


typedef enum {
  mtRead = 1,
  mtWrite = 2,
  mtError = 4
} MaskTy;

struct fdStruct {
  aioObject *object;
  int mask;
};


typedef std::map<int, fdStruct*> OpLinksMap;


typedef struct selectBase {
  asyncBase B;
  int pipeFd[2];  
  OpLinksMap readOps;
  OpLinksMap writeOps;
} selectBase;


void selectPostEmptyOperation(asyncBase *base);
void selectNextFinishedOperation(asyncBase *base);
aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *selectNewAsyncOp(asyncBase *base);
void selectDeleteObject(aioObject *object);
void selectFinishOp(selectOp *op);
void selectStartTimer(asyncOpRoot *op, uint64_t usTimeout, int count);
void selectStopTimer(asyncOpRoot *op);
void selectActivate(asyncOpRoot *op);
void selectAsyncConnect(selectOp *op, const HostAddress *address, uint64_t usTimeout);
void selectAsyncAccept(selectOp *op, uint64_t usTimeout);
void selectAsyncRead(selectOp *op, uint64_t usTimeout);
void selectAsyncWrite(selectOp *op, uint64_t usTimeout);
void selectAsyncReadMsg(selectOp *op, uint64_t usTimeout);
void selectAsyncWriteMsg(selectOp *op, const HostAddress *address, uint64_t usTimeout);
void selectMonitor(selectOp *op);
void selectMonitorStop(selectOp *op);


static struct asyncImpl selectImpl = {
  selectPostEmptyOperation,
  selectNextFinishedOperation,
  selectNewAioObject,
  selectNewAsyncOp,
  selectDeleteObject,
  (finishOpTy*)selectFinishOp,
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
    case ioObjectSocketSyn :
      return object->hSocket;
      break;
    default :
      return -1;
      break;      
  }
}

static fdStruct *getFdOperations(OpLinksMap &opMap, int fd)
{
  OpLinksMap::iterator F = opMap.find(fd);
  if (F == opMap.end()) {
    fdStruct *list = new fdStruct;
    list->object = 0;
    list->mask = 0;
    F = opMap.insert(F, std::make_pair(fd, list));
  }
  return F->second;
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
    OpLinksMap &links = isWriteOperation(op->info.root.opCode) ?
      localBase->writeOps : localBase->readOps;
    fdStruct *list = getFdOperations(links, getFd(op));
    list->mask &= ~(isWriteOperation(op->info.root.opCode) ? mtWrite : mtRead);
  }
}





static void timerCb(int sig, siginfo_t *si, void *uc)
{
  asyncOpRoot *op = (asyncOpRoot*)si->si_value.sival_ptr;
  selectBase *base = (selectBase*)op->base;
  
  if (op->counter > 0)
    op->counter--;
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
  op->counter = 0;
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

  OpLinksMap &links = isWriteOperation(action) ?
    localBase->writeOps : localBase->readOps;
  
  asyncOpLink(getFdOperations(links, getFd(op)), op);    
}


extern "C" asyncBase *selectNewAsyncBase()
{
  selectBase *base = new selectBase;
  if (base) {
    struct sigaction sAction;

    pipe(base->pipeFd);    
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
                            OpLinksMap &links,
                            fd_set *fds,
                            int isRead)
{
  for (OpLinksMap::iterator I = links.begin(), IE = links.end(); I != IE; ++I) {
    int fd = I->first;    
    if (!FD_ISSET(fd, fds))
      continue;
  
    int available;
    fdStruct *list = getFdOperations(links, fd);
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
          accept(fd, (sockaddr*)&clientAddr, &clientAddrSize);
                
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
        uint8_t *ptr = (uint8_t*)op->info.buffer + op->info.bytesTransferred;
        readyForRead =
          std::min(op->info.transactionSize - op->info.bytesTransferred,
                   (size_t)available);
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
               (sockaddr*)&remoteAddress, sizeof(remoteAddress));
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
      if (I->second->mask & mtRead) {
        nfds = std::max(nfds, I->first+1);
        FD_SET(I->first, &readFds);
      }
    }
      
    for (OpLinksMap::iterator I = localBase->writeOps.begin(),
         IE = localBase->writeOps.end(); I != IE; ++I) {  
      if (I->second->mask & mtWrite) {
        nfds = std::max(nfds, I->first+1);
        if (I->second->object->root.type == ioObjectSocket)
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
      asyncOpRoot *op;
      for (int i = 0; i < available/(int)sizeof(op); i++) {
        read(localBase->pipeFd[0], &op, sizeof(op));
        if (!op)
          return;
    
        if (op) {
          aioObjectRoot *object = op->object;
          if (object->type == ioObjectUserEvent) {
            if (op->counter == 0)
              stopTimer(op);
            userEventTrigger(object);
          } else {
            if (object->type != ioObjectUserDefined)
              asyncOpUnlink((selectOp*)op);
            finishOperation(op, aosTimeout, 0);
          }
        }
      }
    }

    processReadyFds(localBase, localBase->readOps, &readFds, 1);
    processReadyFds(localBase, localBase->writeOps, &writeFds, 0);
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
    case ioObjectSocketSyn :
      object->hSocket = *(socketTy*)data;
      break;
    default :
      break;
  }

  return object;
}


asyncOpRoot *selectNewAsyncOp(asyncBase *base)
{
  selectOp *op = new selectOp;
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

void selectStartTimer(asyncOpRoot *op, uint64_t usTimeout, int count)
{
  // only for user event, 'op' must have timer
  op->counter = (count > 0) ? count : -1;
  startTimer(op, usTimeout, 1); 
}


void selectStopTimer(asyncOpRoot *op)
{
  // only for user event, 'op' must have timer
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
  int err = connect(getObject(op)->hSocket, (sockaddr*)&localAddress, sizeof(localAddress));

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


void selectMonitor(selectOp *op)
{
  startOperation(op, actMonitor, 0);
}


void selectMonitorStop(selectOp *op)
{
  startOperation(op, actMonitorStop, 0);
}
