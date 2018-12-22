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
