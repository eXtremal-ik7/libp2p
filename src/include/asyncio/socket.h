#ifndef __ASYNCIO_SOCKET_H_
#define __ASYNCIO_SOCKET_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/asyncioTypes.h"
#include <stdint.h>

uint32_t addrfromAscii(const char *cp);
void initializeSocketSubsystem();
socketTy socketCreate(int af, int type, int protocol, int isAsync);
void socketClose(socketTy hSocket);
int socketBind(socketTy hSocket, const HostAddress *address);
int socketListen(socketTy hSocket);
void socketReuseAddr(socketTy hSocket);

int socketSyncRead(socketTy hSocket, void *buffer, size_t size, int waitAll, size_t *bytesTransferred);
int socketSyncWrite(socketTy hSocket, void *buffer, size_t size, int waitAll, size_t *bytesTransferred);

#ifdef __cplusplus
}
#endif

#endif //__ASYNCIO_SOCKET_H_
