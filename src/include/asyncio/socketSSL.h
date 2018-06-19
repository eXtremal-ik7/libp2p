#ifdef __cplusplus
extern "C" {
#endif

#ifndef __ASYNCIO_SOCKETSSL_H_
#define __ASYNCIO_SOCKETSSL_H_

#include "asyncio/api.h"
#include "openssl/bio.h"

typedef struct SSLOp SSLOp;
typedef struct SSLSocket SSLSocket;

typedef void sslConnectCb(AsyncOpStatus status, asyncBase *base, SSLSocket *object, void *arg);
typedef void sslCb(AsyncOpStatus status, asyncBase *base, SSLSocket *object, size_t transferred, void *arg);

typedef struct SSLSocket {
  aioObjectRoot root;
  
  aioObject *object;  
  SSL_CTX *sslContext;
  SSL *ssl;
  BIO *bioIn;
  BIO *bioOut;
  size_t sslReadBufferSize;
  uint8_t *sslReadBuffer;
} SSLSocket;

typedef struct SSLOp {
  asyncOpRoot root;
  int state;    
  size_t sslBufferSize;
  uint8_t *sslBuffer;
  
  void *buffer;
  size_t transactionSize;
  size_t bytesTransferred;  
} SSLOp;


SSLSocket *sslSocketNew(asyncBase *base);
void sslSocketDelete(SSLSocket *socket);

socketTy sslGetSocket(const SSLSocket *socket);

void aioSslConnect(asyncBase *base,
                   SSLSocket *socket,
                   const HostAddress *address,
                   uint64_t usTimeout,
                   sslConnectCb callback,
                   void *arg);

void aioSslRead(asyncBase *base,
                SSLSocket *socket,
                void *buffer,
                size_t size,
                AsyncFlags flags,
                uint64_t usTimeout,
                sslCb callback,
                void *arg);

void aioSslWrite(asyncBase *base,
                 SSLSocket *socket,
                 void *buffer,
                 size_t size,
                 AsyncFlags flags,
                 uint64_t usTimeout,
                 sslCb callback,
                 void *arg);


int ioSslConnect(asyncBase *base, SSLSocket *socket, const HostAddress *address, uint64_t usTimeout);
ssize_t ioSslRead(asyncBase *base, SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);
ssize_t ioSslWrite(asyncBase *base, SSLSocket *socket, void *buffer, size_t size, AsyncFlags flags, uint64_t usTimeout);

#endif //__ASYNCIO_SOCKETSSL_H_

#ifdef __cplusplus
}
#endif
