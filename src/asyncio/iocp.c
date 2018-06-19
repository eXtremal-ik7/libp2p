#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include "asyncioImpl.h"
#include "asyncio/dynamicBuffer.h"
#include <stdio.h>
#include <assert.h>


typedef struct iocpOp iocpOp;

typedef struct overlappedExtra {
  OVERLAPPED data;
  iocpOp *op;
} overlappedExtra;

typedef struct recvFromData {
  struct sockaddr_in addr;
  INT size;
} recvFromData;

typedef struct iocpBase {
  asyncBase B;
  HANDLE completionPort;
  HANDLE timerThread;
  LPFN_CONNECTEX ConnectExPtr;
  asyncOp *lastReturned;
} iocpBase;

typedef struct iocpOp {
  asyncOp info;
  HANDLE hTimer;
  LARGE_INTEGER signalTime;
  int counter;
  overlappedExtra overlapped;
} iocpOp;


void postEmptyOperation(asyncBase *base);
void iocpNextFinishedOperation(asyncBase *base);
aioObject *iocpNewAioObject(iocpBase *base, IoObjectTy type, void *data);
asyncOpRoot *iocpNewAsyncOp(asyncBase *base);
void iocpDeleteObject(aioObject *op);
void iocpFinishOp(iocpOp *op);
void iocpStartTimer(iocpOp *op, uint64_t usTimeout, int count);
void iocpStopTimer(iocpOp *op);
void iocpActivate(iocpOp *op);
void iocpAsyncConnect(iocpOp *op, const HostAddress *address, uint64_t usTimeout);
void iocpAsyncAccept(iocpOp *op, uint64_t usTimeout);
void iocpAsyncRead(iocpOp *op, uint64_t usTimeout);
void iocpAsyncWrite(iocpOp *op, uint64_t usTimeout);
void iocpAsyncReadMsg(iocpOp *op, uint64_t usTimeout);
void iocpAsyncWriteMsg(iocpOp *op, const HostAddress *address, uint64_t usTimeout);

static struct asyncImpl iocpImpl = {
  postEmptyOperation,
  iocpNextFinishedOperation,
  (newAioObjectTy*)iocpNewAioObject,
  iocpNewAsyncOp,
  iocpDeleteObject,
  (finishOpTy*)iocpFinishOp,
  (startTimerTy*)iocpStartTimer,
  (stopTimerTy*)iocpStopTimer,
  (activateTy*)iocpActivate,
  (asyncConnectTy*)iocpAsyncConnect,
  (asyncAcceptTy*)iocpAsyncAccept,
  (asyncReadTy*)iocpAsyncRead,
  (asyncWriteTy*)iocpAsyncWrite,
  (asyncReadMsgTy*)iocpAsyncReadMsg,
  (asyncWriteMsgTy*)iocpAsyncWriteMsg
};

static aioObject *getObject(iocpOp *op)
{
  return (aioObject*)op->info.root.object;
}

static DWORD WINAPI timerThreadProc(LPVOID lpParameter)
{
  while (1)
    SleepEx(INFINITE, TRUE);
  return 0;
}

static VOID CALLBACK userEventTimerCb(LPVOID lpArgToCompletionRoutine,
                                      DWORD dwTimerLowValue,
                                      DWORD dwTimerHighValue)
{
  int needReactivate = 1;
  iocpOp *op = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);

  if (op->counter > 0) {
    if (--op->counter == 0)
      needReactivate = 0;
  }

  if (needReactivate) {
    SetWaitableTimer(op->hTimer, &op->signalTime, 0,
                     userEventTimerCb, op, FALSE);
  }
}


static VOID CALLBACK ioFinishedTimerCb(LPVOID lpArgToCompletionRoutine,
                                       DWORD dwTimerLowValue,
                                       DWORD dwTimerHighValue)
{
  // Принудительное завершение асинхронной операции
  iocpOp *op = lpArgToCompletionRoutine;
  iocpBase *localBase = (iocpBase*)op->info.root.base;

  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}


static VOID CALLBACK timerStartProc(ULONG_PTR dwParam)
{
  iocpOp *op = (iocpOp*)dwParam;
  PTIMERAPCROUTINE timerCb;
  
  aioObject *object = getObject(op);
  switch (object->root.type) {
    case ioObjectUserEvent :
      timerCb = userEventTimerCb;
      break;
    default:
      timerCb = ioFinishedTimerCb;
      break;
  }

  SetWaitableTimer(op->hTimer, &op->signalTime, 0, timerCb, op, FALSE);
}


static VOID CALLBACK timerCancelProc(ULONG_PTR dwParam)
{
  iocpOp *op = (iocpOp*)dwParam;
  CancelWaitableTimer(op->hTimer);
}

static void initializeOp(iocpOp *op)
{
  memset(&op->overlapped.data, 0, sizeof(op->overlapped.data));
  op->overlapped.op = op;
}

static void startOrContinueAction(iocpOp *op, void *arg)
{
  WSABUF wsabuf;
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  aioObject *object = getObject(op);
  void *opBuffer = ((uint8_t*)op->info.buffer) + op->info.bytesTransferred;
  size_t size = op->info.transactionSize - op->info.bytesTransferred;

  switch (object->root.type) {
    case ioObjectSocket :
      switch (op->info.root.opCode) {
        case actConnect :
          if (localBase->ConnectExPtr)
            localBase->ConnectExPtr(object->hSocket,
                                    arg,
                                    sizeof(struct sockaddr_in),
                                    NULL,
                                    0,
                                    NULL,
                                    &op->overlapped.data);
          break;
        case actAccept :
          op->info.acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
          AcceptEx(object->hSocket,
                   op->info.acceptSocket,
                   op->info.internalBuffer,
                   0,
                   sizeof(struct sockaddr_in)+16,
                   sizeof(struct sockaddr_in)+16,
                   NULL,
                   &op->overlapped.data);
          break;
        case actRead : {
          DWORD flags = 0;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSARecv(object->hSocket, &wsabuf, 1, NULL, &flags, &op->overlapped.data, NULL);
          break;
        }
        
        case actReadMsg : {
          recvFromData *rf = op->info.internalBuffer;
          rf->size = sizeof(rf->addr);
          DWORD flags = 0;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSARecvFrom(object->hSocket, &wsabuf, 1, NULL, &flags, (SOCKADDR*)&rf->addr, &rf->size, &op->overlapped.data, NULL);
          break;
        }
        
        case actWrite :
          wsabuf.buf = opBuffer;
          wsabuf.len = size;
          WSASend(object->hSocket, &wsabuf, 1, NULL, 0, &op->overlapped.data, NULL);    
          break;

        case actWriteMsg : {
          struct sockaddr_in remoteAddress;
          wsabuf.buf = opBuffer;
          wsabuf.len = size;

          remoteAddress.sin_family = op->info.host.family;
          remoteAddress.sin_addr.s_addr = op->info.host.ipv4;
          remoteAddress.sin_port = op->info.host.port;

          WSASendTo(object->hSocket, &wsabuf, 1, NULL, 0, (struct sockaddr*)&remoteAddress, sizeof(remoteAddress), &op->overlapped.data, NULL);  
          break;
        }

        default :
          return;
      }
      break;

    case ioObjectDevice :
      switch (op->info.root.opCode) {
        case actRead :
        case actReadMsg : {
          ReadFile(object->hDevice, opBuffer, size, NULL, &op->overlapped.data);
          break;
       }
        case actWrite :
          WriteFile(object->hDevice, opBuffer, size, NULL, &op->overlapped.data);
          break;
        default :
          return;
      }
      break;
 
    default :
      return;

  }
}


void postEmptyOperation(asyncBase *base)
{
  iocpBase *localBase = (iocpBase*)base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, 0, 0);
}


asyncBase *iocpNewAsyncBase()
{
  iocpBase *base = malloc(sizeof(iocpBase));
  if (base) {
    SOCKET tmpSocket;
    DWORD numBytes = 0;
    GUID guid = WSAID_CONNECTEX;
    DWORD tid;
    
    base->completionPort =
      CreateIoCompletionPort(INVALID_HANDLE_VALUE, 0, 0, 0);
    base->timerThread =
      CreateThread(NULL, 0x10000, timerThreadProc, NULL, THREAD_PRIORITY_NORMAL, &tid);

    base->ConnectExPtr = 0;
    tmpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    WSAIoctl(tmpSocket,
             SIO_GET_EXTENSION_FUNCTION_POINTER,
             &guid,
             sizeof(guid),
             &base->ConnectExPtr,
             sizeof(base->ConnectExPtr),
             &numBytes,
             NULL,
             NULL);
    CloseHandle((HANDLE)tmpSocket);

    base->B.methodImpl = iocpImpl;
    base->lastReturned = 0;
  }

  return (asyncBase*)base;
}


void iocpNextFinishedOperation(asyncBase *base)
{
  iocpBase *localBase = (iocpBase*)base;
  DWORD bytesNum = 0;
  ULONG_PTR key = 0;
  overlappedExtra *overlapped = 0;
  
  AsyncOpStatus result;
  int needRemoveFromTimeoutQueue;
  int error;

  while (1) {    
    iocpOp *op = 0;
    BOOL status = GetQueuedCompletionStatus(localBase->completionPort,
                                            &bytesNum,
                                            &key,
                                            (OVERLAPPED**)&overlapped,
                                            333);
    
    if (key) {
      op = (iocpOp*)key;
      needRemoveFromTimeoutQueue = 0;  
    } else if (overlapped) {
      op = overlapped->op;
      aioObject *object = getObject(op);
      needRemoveFromTimeoutQueue = 1;
      
      // additional disconnect check
      error = WSAGetLastError();
      if (status == TRUE &&
          bytesNum == 0 &&
          (op->info.root.opCode == actRead ||
           op->info.root.opCode == actWrite ||
           op->info.root.opCode == actReadMsg ||
           op->info.root.opCode == actWriteMsg)) {
        status = FALSE;
        error = ERROR_NETNAME_DELETED;
      }
      
      if (status == FALSE) {
        result = aosUnknownError;
        if (object->root.type == ioObjectSocket) {
          if (error == WSAEMSGSIZE) {
            // Receive buffer too small
            result = aosBufferTooSmall;
          } else if (error == ERROR_NETNAME_DELETED) {
            result = aosDisconnected;
          }
        }
      } else {
        result = aosSuccess;
        if (op->info.root.opCode == actAccept) {
          struct sockaddr_in *localAddr = 0;
          struct sockaddr_in *remoteAddr = 0;
          INT localAddrLength;
          INT remoteAddrLength;
          GetAcceptExSockaddrs(op->info.internalBuffer,
                               bytesNum,
                               sizeof(struct sockaddr_in)+16,
                               sizeof(struct sockaddr_in)+16,
                               (struct sockaddr**)&localAddr, &localAddrLength,
                               (struct sockaddr**)&remoteAddr, &remoteAddrLength);
          if (localAddr && remoteAddr) {
            op->info.host.family = remoteAddr->sin_family;
            op->info.host.ipv4 = remoteAddr->sin_addr.s_addr;
            op->info.host.port = remoteAddr->sin_port;
          } else {
            result = aosUnknownError;
          }
        } else if (op->info.root.opCode == actRead ||
                   op->info.root.opCode == actWrite) {
          op->info.bytesTransferred += bytesNum;
          if ((op->info.root.flags & afWaitAll) &&
              op->info.bytesTransferred < op->info.transactionSize) {
            startOrContinueAction(op, 0);
            continue;
          }
        } else if (op->info.root.opCode == actReadMsg) {
          struct recvFromData *rf = op->info.internalBuffer;
          op->info.bytesTransferred = bytesNum;
          op->info.host.family = rf->addr.sin_family;
          op->info.host.ipv4 = rf->addr.sin_addr.s_addr;
          op->info.host.port = rf->addr.sin_port;          
        } else if (op->info.root.opCode == actWriteMsg) {
          op->info.bytesTransferred = bytesNum;
        }
      }
    } else if (status == TRUE) {
      processTimeoutQueue(base);
      return;
    }

    if (op)
      finishOperation(&op->info.root, result, needRemoveFromTimeoutQueue);

    processTimeoutQueue(base);
  }
}


aioObject *iocpNewAioObject(iocpBase *base, IoObjectTy type, void *data)
{
  aioObject *object =
    (aioObject*)initObjectRoot(type, sizeof(aioObject), (aioObjectDestructor*)iocpDeleteObject);
  switch (type) {
    case ioObjectDevice :
      object->hDevice = *(iodevTy *)data;
      CreateIoCompletionPort(object->hDevice, base->completionPort, 0, 1);
      break;
    case ioObjectSocket :
      object->hSocket = *(socketTy *)data;
      CreateIoCompletionPort(object->hDevice, base->completionPort, 0, 1);
      break;
    default :
      break;
  }

  return object;
}


asyncOpRoot *iocpNewAsyncOp(asyncBase *base)
{
  iocpOp *op;
  op = malloc(sizeof(iocpOp));
  if (op)
    memset(op, 0, sizeof(asyncOp));

  return (asyncOpRoot*)op;
}


void iocpDeleteObject(aioObject *object)
{
  switch (object->root.type) {
    case ioObjectDevice :
    case ioObjectSocket :
    default :
      break;
  }  
  
  free(object);
}

void iocpFinishOp(iocpOp *op)
{
}

void iocpStartTimer(iocpOp *op, uint64_t usTimeout, int count)
{
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  op->signalTime.QuadPart = -(int64_t)(usTimeout*10);
  op->counter = count;
  if (count != 0)
    QueueUserAPC(timerStartProc, localBase->timerThread, (ULONG_PTR)op);  
}


void iocpStopTimer(iocpOp *op)
{
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  QueueUserAPC(timerCancelProc, localBase->timerThread, (ULONG_PTR)op);
}


void iocpActivate(iocpOp *op)
{
  iocpBase *localBase = (iocpBase*)op->info.root.base;
  PostQueuedCompletionStatus(localBase->completionPort, 0, (ULONG_PTR)op, 0);
}


void iocpAsyncConnect(iocpOp *op, const HostAddress *address, uint64_t usTimeout)
{
  struct sockaddr_in localAddress;
  localAddress.sin_family = address->family;
  localAddress.sin_addr.s_addr = address->ipv4;
  localAddress.sin_port = address->port;
  initializeOp(op);    
  startOrContinueAction(op, &localAddress);
}


void iocpAsyncAccept(iocpOp *op, uint64_t usTimeout)
{  
  // Expand internal buffer to 2*(sizeof(struct sockaddr_in)+16) if need
  const size_t acceptResultSize = 2*(sizeof(struct sockaddr_in)+16);
  
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  } else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }

  initializeOp(op);    
  startOrContinueAction(op, 0);
}


void iocpAsyncRead(iocpOp *op, uint64_t usTimeout)
{
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncWrite(iocpOp *op, uint64_t usTimeout)
{
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncReadMsg(iocpOp *op, uint64_t usTimeout)
{
  // Expand internal buffer to sizeof(struct sockaddr_in if need
  const size_t acceptResultSize = sizeof(recvFromData);
  
  if (op->info.internalBuffer == 0) {
    op->info.internalBuffer = malloc(acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  } else if (op->info.internalBufferSize < acceptResultSize) {
    op->info.internalBuffer = realloc(op->info.internalBuffer, acceptResultSize);
    op->info.internalBufferSize = acceptResultSize;
  }  
  
  initializeOp(op);  
  startOrContinueAction(op, 0);
}


void iocpAsyncWriteMsg(iocpOp *op, const HostAddress *address, uint64_t usTimeout)
{
  initializeOp(op);
  op->info.host = *address;
  startOrContinueAction(op, 0);
}
