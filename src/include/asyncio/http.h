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
#include "asyncio/dynamicBuffer.h"
  
typedef struct HTTPClient HTTPClient;
typedef struct HTTPInfo HTTPInfo;
typedef struct HTTPOp HTTPOp;

typedef void httpConnectCb(AsyncOpStatus, HTTPClient*, void*);
typedef void httpRequestCb(AsyncOpStatus, HTTPClient*, int, void*);

typedef struct HTTPClient {
  aioObjectRoot root;
  int isHttps;
  union {
    aioObject *plainSocket;
    SSLSocket *sslSocket;
  };
 
  uint8_t *inBuffer;
  size_t inBufferSize;
  size_t inBufferOffset;
  dynamicBuffer out; 
  HttpParserState state;
  
  // out
  Raw contentType;
  Raw body;    
} HTTPClient;


typedef struct HTTPOp {
  asyncOpRoot root;
  int state;
  HostAddress address;
  int resultCode;
  Raw contentType;
  Raw body;
  httpParseCb *parseCallback;
} HTTPOp;


void httpParseDefault(HttpComponent *component, void *arg);

HTTPClient *httpClientNew(asyncBase *base, aioObject *socket);
HTTPClient *httpsClientNew(asyncBase *base, SSLSocket *socket);
void httpClientDelete(HTTPClient *client);

void aioHttpConnect(HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpConnectCb callback,
                    void *arg);

void aioHttpRequest(HTTPClient *client,
                    const char *request,
                    size_t requestSize,
                    uint64_t usTimeout,
                    httpParseCb parseCallback,
                    httpRequestCb callback,
                    void *arg);

int ioHttpConnect(HTTPClient *client, const HostAddress *address, uint64_t usTimeout);
int ioHttpRequest(HTTPClient *client, const char *request, size_t requestSize, uint64_t usTimeout, httpParseCb parseCallback);
                

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus


#endif

#endif //__ASYNCIO_HTTP_H_
