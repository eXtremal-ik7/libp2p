#ifdef __cplusplus
extern "C" {
#endif

#ifndef __ASYNCIO_SOCKETSSL_H_
#define __ASYNCIO_SOCKETSSL_H_

#include "asyncio/asyncio.h"
#include "openssl/bio.h"
#include "cstl.h"

typedef struct SSLInfo SSLInfo;
typedef struct SSLOp SSLOp;
  
typedef void sslCb(SSLInfo *info);  


typedef struct SSLSocket {
  asyncBase *base;
  aioObject *object;  
  socketTy S;  
  
  SSL_CTX *sslContext;
  SSL *ssl;
  BIO *bioIn;
  BIO *bioOut;
  
  size_t sslReadBufferSize;
  uint8_t *sslReadBuffer;
  
  SSLOp *current;
  SSLOp *tail;
} SSLSocket;

struct SSLInfo {
  SSLSocket *socket;
  AsyncOpStatus status;
  
  sslCb *callback;  
  void *arg;    
  
  AsyncFlags flags;
  void *buffer;
  size_t transactionSize;
  size_t bytesTransferred;
};

typedef struct SSLOp {
  struct SSLInfo info;
  
  int type;
  int state;    
  size_t sslBufferSize;
  uint8_t *sslBuffer;
  
  SSLOp *next;
  HostAddress address;
  uint64_t usTimeout;
  size_t writeSize;
} SSLOp;


SSLSocket *sslSocketNew(asyncBase *base);
void sslSocketDelete(SSLSocket *socket);

socketTy sslGetSocket(const SSLSocket *socket);

void aioSslConnect(SSLSocket *socket,
                const HostAddress *address,
                uint64_t usTimeout,
                sslCb callback,
                void *arg);

void aioSslRead(SSLSocket *socket,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             sslCb callback,
             void *arg);

void aioSslWrite(SSLSocket *socket,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              sslCb callback,
              void *arg);


int ioSslConnect(SSLSocket *socket, const HostAddress *address, uint64_t usTimeout);
ssize_t ioSslRead(SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioSslWrite(SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);

#endif //__ASYNCIO_SOCKETSSL_H_

#ifdef __cplusplus
}
#endif
