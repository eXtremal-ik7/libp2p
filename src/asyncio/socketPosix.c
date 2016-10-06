#include "asyncio/socket.h"
#include <fcntl.h>
#include <unistd.h>

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
