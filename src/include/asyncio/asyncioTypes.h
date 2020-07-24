#ifndef __ASYNCTYPES_H_
#define __ASYNCTYPES_H_

#include "libp2pconfig.h"
#include <stdint.h>

#if defined(OS_WINDOWS)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
typedef HANDLE iodevTy;
typedef SOCKET socketTy;
typedef int socketLenTy;
#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif
#elif defined(OS_COMMONUNIX)
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sched.h>
typedef int iodevTy;
typedef int socketTy;
typedef socklen_t socketLenTy;
#define INVALID_SOCKET -1
#endif

// Thread local storage
#ifdef _MSC_VER
#define __tls __declspec(thread)
#else
#define __tls __thread
#endif

typedef struct HostAddress {
  union {
    uint32_t ipv4;
    uint16_t ipv6[8];
  };
  uint16_t port;
  uint16_t family;
} HostAddress;

#endif //__ASYNCTYPES_H_
