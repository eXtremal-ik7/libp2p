#include "asyncio/socket.h"

void initializeSocketSubsystem()
{
#ifdef OS_WINDOWS
  WSADATA wsadata;
  WSAStartup(MAKEWORD(2, 2), &wsadata);
#endif
}


socketTy socketCreate(int af, int type, int protocol, int isAsync)
{
#ifdef OS_WINDOWS
  SOCKET hSocket = WSASocket(af, type, protocol, NULL, 0, isAsync ? WSA_FLAG_OVERLAPPED : 0);
  if (isAsync) {
    u_long arg = 1;
    ioctlsocket(hSocket, FIONBIO, &arg);
  }

  return hSocket;
#else
  return socket(af, type, protocol);
#endif
}

void socketClose(socketTy hSocket)
{
  closesocket(hSocket);
}

int socketBind(socketTy hSocket, const HostAddress *address)
{
#ifdef OS_WINDOWS
  struct sockaddr_in localAddr;
  localAddr.sin_family = address->family;
  localAddr.sin_addr.s_addr = address->ipv4;
  localAddr.sin_port = address->port;
  return bind(hSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));
#else
  return 0;
#endif
}


int socketListen(socketTy hSocket)
{
  return listen(hSocket, SOMAXCONN);
}


void socketReuseAddr(socketTy hSocket)
{
  char optval = 1;
  setsockopt(hSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
}


uint32_t addrfromAscii(const char *cp)
{
  uint32_t res = inet_addr(cp);
  return (res != INADDR_NONE) ? res : 0;
}

int socketSyncRead(socketTy hSocket, void *buffer, size_t size, int waitAll, size_t *bytesTransferred)
{
  DWORD bytesNum = 0;
  WSABUF wsabuf;
  if (!waitAll) {
    // TODO: correct processing >4Gb data blocks
    wsabuf.buf = buffer;
    wsabuf.len = (ULONG)size;
    DWORD flags = 0;
    if (WSARecv(hSocket, &wsabuf, 1, &bytesNum, &flags, 0, 0) == 0 && bytesNum != 0) {
      *bytesTransferred = bytesNum;
      return 1;
    } else {
      return 0;
    }
  } else {
    size_t transferred = 0;
    DWORD flags;
    do {
      flags = 0;
      transferred += (size_t)bytesNum;
      wsabuf.buf = (uint8_t*)buffer + transferred;
      // TODO: correct processing >4Gb data blocks
      wsabuf.len = (ULONG)(size - transferred);
    } while (transferred != size && WSARecv(hSocket, &wsabuf, 1, &bytesNum, &flags, 0, 0) == 0);
    *bytesTransferred = transferred;
    return transferred == size;
  }
}

int socketSyncWrite(socketTy hSocket, const void *buffer, size_t size, int waitAll, size_t *bytesTransferred)
{
  DWORD bytesNum = 0;
  WSABUF wsabuf;
  if (!waitAll) {
    // TODO: correct processing >4Gb data blocks
    wsabuf.buf = (char*)buffer;
    wsabuf.len = (ULONG)size;
    if (WSASend(hSocket, &wsabuf, 1, &bytesNum, 0, 0, 0) == 0 && bytesNum != 0) {
      *bytesTransferred = bytesNum;
      return 1;
    }
    else {
      return 0;
    }
  } else {
    size_t transferred = 0;
    do {
      transferred += (size_t)bytesNum;
      wsabuf.buf = (uint8_t*)buffer + transferred;
      // TODO: correct processing >4Gb data blocks
      wsabuf.len = (ULONG)(size - transferred);
    } while (transferred != size && WSASend(hSocket, &wsabuf, 1, &bytesNum, 0, 0, 0) == 0);
    *bytesTransferred = transferred;
    return transferred == size;
  }
}
