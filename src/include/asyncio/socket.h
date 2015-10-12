#include "config.h"
#include <stdint.h>

#if defined(OS_WINDOWS)
#include <Winsock2.h>
#include <mswsock.h>
#include <Windows.h>
typedef SOCKET socketTy;
#elif defined(OS_COMMONUNIX)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
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
int socketBind(socketTy hSocket, const HostAddress *address);
int socketListen(socketTy hSocket);
void socketReuseAddr(socketTy hSocket);
