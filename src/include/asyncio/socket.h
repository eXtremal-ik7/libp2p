#ifndef __ASYNCIO_SOCKET_H_
#define __ASYNCIO_SOCKET_H_

#include "config.h"
#include <stdint.h>

#if defined(OS_WINDOWS)
#include <Winsock2.h>
#include <mswsock.h>
#include <Windows.h>
typedef SOCKET socketTy;
#define INVALID_SOCKET INVALID_HANDLE_VALUE
#elif defined(OS_COMMONUNIX)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#define INVALID_SOCKET -1
typedef int socketTy;
#endif

#include <stdint.h>

typedef struct HostAddress {
  int family;
  union {
    uint32_t ipv4;
    uint16_t ipv6[8];
  };
  uint16_t port;
} HostAddress;

uint32_t addrfromAscii(const char *cp);
void initializeSocketSubsystem();
socketTy socketCreate(int af, int type, int protocol, int isAsync);
void socketClose(socketTy hSocket);
int socketBind(socketTy hSocket, const HostAddress *address);
int socketListen(socketTy hSocket);
void socketReuseAddr(socketTy hSocket);

#endif //__ASYNCIO_SOCKET_H_
