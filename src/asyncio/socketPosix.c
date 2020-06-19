#include "asyncio/socket.h"
#include <fcntl.h>
#include <unistd.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>

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
  return WSASocket(af, type, protocol, NULL, 0, isAsync ? WSA_FLAG_OVERLAPPED : 0);
#else
  int hSocket = socket(af, type, protocol);
  if (isAsync) {
    int current = fcntl(hSocket, F_GETFL);
    fcntl(hSocket, F_SETFL, O_NONBLOCK | current);
  }
  
  int optval = 1;
  setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (char *)&optval, sizeof(optval) );
  return hSocket;
#endif
}

void socketClose(socketTy hSocket)
{
  close(hSocket);
}

int socketBind(socketTy hSocket, const HostAddress *address)
{
  struct sockaddr_in localAddr;
  localAddr.sin_family = address->family;
  localAddr.sin_addr.s_addr = address->ipv4;
  localAddr.sin_port = address->port;
  return bind(hSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));
}


int socketListen(socketTy hSocket)
{
  return listen(hSocket, SOMAXCONN);
}

int socketShutdown(socketTy hSocket, int how)
{
  return shutdown(hSocket, how);
}

void socketReuseAddr(socketTy hSocket)
{
  int optval = 1;
  setsockopt(hSocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));
}

uint32_t addrfromAscii(const char *cp)
{
  uint32_t res = inet_addr(cp);
  return (res != INADDR_NONE) ? res : 0;
}

int socketSyncRead(socketTy hSocket, void *buffer, size_t size, int waitAll, size_t *bytesTransferred)
{
  if (!waitAll) {
    ssize_t result = recv(hSocket, buffer, size, 0);
    if (result > 0) {
      *bytesTransferred = (size_t)result;
      return 1;
    } else {
      return 0;
    }
  } else {
    size_t transferred = 0;
    ssize_t result;
    while (transferred != size && (result = recv(hSocket, (uint8_t*)buffer + transferred, size - transferred, 0)) > 0)
      transferred += (size_t)result;
    *bytesTransferred = transferred;
    return transferred == size;
  }
}

int socketSyncWrite(socketTy hSocket, const void *buffer, size_t size, int waitAll, size_t *bytesTransferred)
{
#ifdef OS_LINUX
  int flags = MSG_NOSIGNAL;
#else
  int flags = 0;
#endif
  if (!waitAll) {
    ssize_t result = send(hSocket, buffer, size, flags);
    if (result > 0) {
      *bytesTransferred = (size_t)result;
      return 1;
    } else {
      return 0;
    }
  } else {
    size_t transferred = 0;
    ssize_t result;
    while (transferred != size && (result = send(hSocket, (uint8_t*)buffer + transferred, size - transferred, flags)) > 0)
      transferred += (size_t)result;
    *bytesTransferred = transferred;
    return transferred == size;
  }
}
