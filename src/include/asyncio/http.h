#ifndef __ASYNCIO_HTTP_H_
#define __ASYNCIO_HTTP_H_

#ifdef __cplusplus
#include <string>
#endif

#ifdef __cplusplus
extern "C" {
#endif 
  
#include "asyncio/socketSSL.h"
#include "p2putils/HttpParse.h"
  
typedef struct HTTPInfo HTTPInfo;
typedef struct HTTPOp HTTPOp;
  
typedef void httpCb(HTTPInfo*);

typedef struct HTTPClient {
  asyncBase *base;  
  int isHttps;
  union {
    aioObject *plainSocket;
    SSLSocket *sslSocket;
  };
 
  uint8_t *inBuffer;
  size_t inBufferSize;
  size_t inBufferOffset;
  dynamicBuffer out;
  
  Raw contentType;
  Raw body;  
  
  HttpParserState state;
  HTTPOp *current;
  HTTPOp *tail;
} HTTPClient;

typedef struct HTTPInfo {
  HTTPClient *client;
  AsyncOpStatus status;  
  void *arg;
  
  int resultCode;
  Raw contentType;
  Raw body;
} HTTPInfo;

typedef struct HTTPOp {
  HTTPInfo info;

  int type;
  AsyncOpStatus status;
  httpParseCb *parseCallback;  
  httpCb *callback; 
  
  HTTPOp *next;
  HostAddress address;
  uint64_t usTimeout;  
} HTTPOp;


void httpParseDefault(HttpComponent *component, void *arg);

HTTPClient *httpClientNew(asyncBase *base, aioObject *socket);
HTTPClient *httpsClientNew(asyncBase *base, SSLSocket *socket);

void aioHttpConnect(HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpCb callback,
                    void *arg);

void aioHttpRequest(HTTPClient *client,
                    const char *request,
                    size_t requestSize,
                    uint64_t usTimeout,
                    httpParseCb parseCallback,
                    httpCb callback,
                    void *arg);

int ioHttpConnect(HTTPClient *client, const HostAddress *address, uint64_t usTimeout);
int ioHttpRequest(HTTPClient *client, const char *request, size_t requestSize, uint64_t usTimeout, httpParseCb parseCallback);
                

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus


#endif

#endif //__ASYNCIO_HTTP_H_
