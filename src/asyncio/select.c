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

enum pipeDescrs {
  Read = 0,
  Write
};

void userEventTrigger(aioObjectRoot *event);

typedef struct selectOp {
  asyncOp info;
} selectOp;


typedef enum {
  mtRead = 1,
  mtWrite = 2,
  mtError = 4
} MaskTy;

__NO_PADDING_BEGIN
typedef struct fdStruct {
  aioObject *object;
  int mask;
} fdStruct;
__NO_PADDING_END

typedef struct selectBase {
  asyncBase B;
  int pipeFd[2];  
  fdStruct *fdMap;
} selectBase;

void selectCombinerTaskHandler(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy opMethod);
void selectEnqueue(asyncBase *base, asyncOpRoot *op);
void selectPostEmptyOperation(asyncBase *base);
void selectNextFinishedOperation(asyncBase *base);
aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data);
asyncOpRoot *selectNewAsyncOp(asyncBase *base, int isRealTime, ConcurrentQueue *objectPool, ConcurrentQueue *objectTimerPool);
int selectCancelAsyncOp(asyncOpRoot *opptr);
void selectDeleteObject(aioObject *object);
void selectInitializeTimer(asyncBase *base, asyncOpRoot *op);
void selectStartTimer(asyncOpRoot *op);
void selectStopTimer(asyncOpRoot *op);
void selectDeleteTimer(asyncOpRoot *op);
void selectActivate(aioUserEvent *op);
AsyncOpStatus selectAsyncConnect(asyncOpRoot *opptr);
AsyncOpStatus selectAsyncAccept(asyncOpRoot *opptr);
AsyncOpStatus selectAsyncRead(asyncOpRoot *opptr);
AsyncOpStatus selectAsyncWrite(asyncOpRoot *opptr);
AsyncOpStatus selectAsyncReadMsg(asyncOpRoot *op);
AsyncOpStatus selectAsyncWriteMsg(asyncOpRoot *op);


static struct asyncImpl selectImpl = {
  selectCombinerTaskHandler,
  selectEnqueue,
  selectPostEmptyOperation,
  selectNextFinishedOperation,
  selectNewAioObject,
  selectNewAsyncOp,
  selectCancelAsyncOp,
  selectDeleteObject,
  selectInitializeTimer,
  selectStartTimer,
  selectStopTimer,
  selectDeleteTimer,
  selectActivate,
  selectAsyncConnect,
  selectAsyncAccept,
  selectAsyncRead,
  selectAsyncWrite,
  selectAsyncReadMsg,
  selectAsyncWriteMsg
};

//static aioObject *getObject(selectOp *op)
//{
//  return (aioObject*)op->info.root.object;
//}

//static int getFd(selectOp *op)
//{
//  aioObject *object = getObject(op);
//  switch (object->root.type) {
//    case ioObjectDevice :
//      return object->hDevice;
//    case ioObjectSocket :
//      return object->hSocket;
//    default :
//      return -1;
//  }
//}

static fdStruct *getFdOperations(selectBase *base, int fd)
{
  return &base->fdMap[fd];  
}

static void timerCb(int sig, siginfo_t *si, void *uc)
{
  __UNUSED(sig);
  __UNUSED(uc);
  asyncOpRoot *op = (asyncOpRoot*)si->si_value.sival_ptr;
  selectBase *base = (selectBase*)op->object->base;
  
  if (op->opCode == actUserEvent) {
    aioUserEvent *event = (aioUserEvent*)op;
    if (event->counter > 0)
      event->counter--;
  }
  
  if (write(base->pipeFd[1], &op, sizeof(op)) <= 0)
    fprintf(stderr, "ERROR(timerCb): write call error");
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

asyncBase *selectNewAsyncBase()
{
__NO_EXPAND_RECURSIVE_MACRO_BEGIN
  selectBase *base = malloc(sizeof(selectBase));
  if (base) {
    struct sigaction sAction;

    if (pipe(base->pipeFd) != 0) {
      free(base);
      return 0;
    }

    base->fdMap = (fdStruct*)calloc((size_t)getdtablesize(), sizeof(fdStruct));
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
__NO_EXPAND_RECURSIVE_MACRO_END
}


void selectCombinerTaskHandler(aioObjectRoot *object, asyncOpRoot *op, AsyncOpActionTy opMethod)
{
  __UNUSED(object);
  __UNUSED(op);
  __UNUSED(opMethod);
}

void selectEnqueue(asyncBase *base, asyncOpRoot *op)
{
  __UNUSED(base);
  __UNUSED(op);
}

void selectPostEmptyOperation(asyncBase *base)
{
  void *p = 0;
  selectBase *localBase = (selectBase*)base;
  if (write(localBase->pipeFd[1], &p, sizeof(p)) <= 0)
    fprintf(stderr, "ERROR(selectPostEmptyOperation): write call error");
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
    int i;
    for (i = 0; i < FD_SETSIZE; i++) {
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
        processTimeoutQueue(base, time(0));
    } while (result <= 0 && errno == EINTR);

    if (FD_ISSET(localBase->pipeFd[0], &readFds)) {
      int available;
      ioctl(localBase->pipeFd[0], FIONREAD, &available);
      asyncOpRoot *op;
      int i;
      for (i = 0; i < available/(int)sizeof(op); i++) {
        if (read(localBase->pipeFd[0], &op, sizeof(op)) <= 0)
          fprintf(stderr, "ERROR(selectNextFinishedOperation): read call error");
        if (!op)
          return;
    
        if (op) {
          if (op->opCode == actUserEvent) {
            aioUserEvent *event = (aioUserEvent*)op;
            if (event->counter == 0)
              stopTimer(op);
            event->root.finishMethod(&event->root);
          } else {
//            if (op->object->type != ioObjectUserDefined)
//              asyncOpUnlink((selectOp*)op);
//            finishOperation(op, aosTimeout, 0);
          }
        }
      }
    }

//    processReadyFds(localBase, &readFds, nfds, 1);
//    processReadyFds(localBase, &writeFds, nfds, 0);
  } 
}


aioObject *selectNewAioObject(asyncBase *base, IoObjectTy type, void *data)
{
  aioObject *object = (aioObject*)calloc(1, sizeof(aioObject));
  initObjectRoot(&object->root, base, type, (aioObjectDestructor*)selectDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy*)data;
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy*)data;
      break;
    default :
      break;
  }

  return object;
}


asyncOpRoot *selectNewAsyncOp(asyncBase *base, int isRealTime, ConcurrentQueue *objectPool, ConcurrentQueue *objectTimerPool)
{
  __UNUSED(base);
  __UNUSED(isRealTime);
  __UNUSED(objectPool);
  __UNUSED(objectTimerPool);
  return 0;
}

int selectCancelAsyncOp(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return 1;
}

void selectDeleteObject(aioObject *object)
{
  free(object);
}

void selectInitializeTimer(asyncBase *base, asyncOpRoot *op)
{
  __UNUSED(base);
  timer_t timerId = 0;
  struct sigevent sEvent;
  sEvent.sigev_notify = SIGEV_SIGNAL;
  sEvent.sigev_signo = SIGRTMIN;
  sEvent.sigev_value.sival_ptr = op;
  timer_create(CLOCK_REALTIME, &sEvent, &timerId);
  op->timerId = (void*)timerId;
}

void selectStartTimer(asyncOpRoot *op)
{
  startTimer(op, 0, 0);
}


void selectStopTimer(asyncOpRoot *op)
{
  stopTimer(op);
}

void selectDeleteTimer(asyncOpRoot *op)
{
  __UNUSED(op);
}

void selectActivate(aioUserEvent *event)
{
  selectBase *localBase = (selectBase*)event->base;
  if (write(localBase->pipeFd[Write], &event, sizeof(void*)) <= 0)
    fprintf(stderr, "ERROR(selectActivate): read call error");
}


AsyncOpStatus selectAsyncConnect(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}


AsyncOpStatus selectAsyncAccept(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}


AsyncOpStatus selectAsyncRead(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}


AsyncOpStatus selectAsyncWrite(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}


AsyncOpStatus selectAsyncReadMsg(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}


AsyncOpStatus selectAsyncWriteMsg(asyncOpRoot *opptr)
{
  __UNUSED(opptr);
  return aosUnknownError;
}
