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

socketTy sslGetSocket(const SSLSocket *socket);

void sslConnect(SSLSocket *socket,
                const HostAddress *address,
                uint64_t usTimeout,
                sslCb callback,
                void *arg);

void sslRead(SSLSocket *socket,
             void *buffer,
             size_t size,
             AsyncFlags flags,
             uint64_t usTimeout,
             sslCb callback,
             void *arg);

void sslWrite(SSLSocket *socket,
              void *buffer,
              size_t size,
              AsyncFlags flags,
              uint64_t usTimeout,
              sslCb callback,
              void *arg);

#endif //__ASYNCIO_SOCKETSSL_H_

#ifdef __cplusplus
}
#endif
