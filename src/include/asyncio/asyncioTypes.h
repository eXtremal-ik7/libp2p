#ifndef __ASYNCTYPES_H_
#define __ASYNCTYPES_H_

#include "config.h"
#include <stdint.h>

#if defined(OS_WINDOWS)
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
typedef HANDLE iodevTy;
typedef SOCKET socketTy;
typedef HANDLE timerTy;
#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#elif defined(OS_COMMONUNIX)
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
typedef int iodevTy;
typedef int socketTy;
typedef timer_t timerTy;
#define INVALID_SOCKET -1
#endif

typedef struct HostAddress {
  int family;
  union {
    uint32_t ipv4;
    uint16_t ipv6[8];
  };
  uint16_t port;
} HostAddress;


// typedef struct aioObject aioObject;
// typedef struct asyncOp asyncOp;
// typedef struct coroutineTy coroutineTy;

#endif //__ASYNCTYPES_H_
