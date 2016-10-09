#include "asyncio/http.h"
#include "asyncio/coroutine.h"
#include <string.h>

const char *httpPoolId = "HTTP";
const char *httpPoolTimerId = "HTTPTimer";

typedef enum {
  httpOpConnect = 0,
  httpOpRequest
} HttpOpTy;

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  int resultCode;
} coroReturnStruct;

void httpParseStart(HTTPOp *op);

static asyncOpRoot *alloc(asyncBase *base)
{
  return (asyncOpRoot*)malloc(sizeof(HTTPOp));
}

static void start(asyncOpRoot *op)
{
  HTTPClient *client = (HTTPClient*)op->object;
  switch (op->opCode) {
    case httpOpConnect :
      // TODO: return error
      break;
    case httpOpRequest :
       dynamicBufferSeek(&client->out, SeekSet, 0);
       httpInit(&client->state);
       httpParseStart((HTTPOp*)op);   
  }
}

static void finish(asyncOpRoot *root, int status)
{
  HTTPOp *op = (HTTPOp*)root;
  HTTPClient *client = (HTTPClient*)root->object; 
  
    // cleanup child operation after timeout
  if (status == aosTimeout)
    cancelIoForParentOp(client->isHttps ? (aioObjectRoot*)client->sslSocket : (aioObjectRoot*)client->plainSocket, root);  
  
  if (root->callback) {
     switch (root->opCode) {
       case httpOpConnect :
         ((httpConnectCb*)root->callback)(status, root->base, client, root->arg);
         break;
       case httpOpRequest :
         client->contentType = op->contentType;
         client->body = op->body;
         ((httpRequestCb*)root->callback)(status, root->base, client, op->resultCode, root->arg);
         break;
     }
  }
}

static void coroutineConnectCb(AsyncOpStatus status, asyncBase *base, HTTPClient *client, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineRequestCb(AsyncOpStatus status, asyncBase *base, HTTPClient *client, int resultCode, void *arg)
{
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->resultCode = resultCode;
  coroutineCall(r->coroutine);
}

static asyncOpRoot *allocHttpOp(asyncBase *base,
                                HTTPClient *client,
                                int type,
                                httpParseCb parseCallback,
                                void *callback,
                                void *arg,
                                uint64_t timeout)
{
  HTTPOp *op = (HTTPOp*)
    initAsyncOpRoot(base, httpPoolId, httpPoolTimerId, alloc, start, finish, &client->root, callback, arg, 0, type, timeout);

  op->parseCallback = parseCallback;
  op->resultCode = 0;
  op->contentType.data = 0;
  op->body.data = 0;
  return (asyncOpRoot*)op;
}

void httpConnectProc(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  finishOperation((asyncOpRoot*)arg, status, 1);
}

void httpsConnectProc(AsyncOpStatus status, asyncBase *base, SSLSocket *object, void *arg)
{
  finishOperation((asyncOpRoot*)arg, status, 1);
}

void httpRequestProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  HTTPOp *op = (HTTPOp*)arg;  
  if (status == aosSuccess) {  
    HTTPClient *client = (HTTPClient*)op->root.object;
    httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
    httpParseStart(op);
  } else {
    finishOperation(&op->root, status, 1);
  }  
}

void httpsRequestProc(AsyncOpStatus status, asyncBase *base, SSLSocket *object, size_t transferred, void *arg)
{
  HTTPOp *op = (HTTPOp*)arg;
  if (status == aosSuccess) {
    HTTPClient *client = (HTTPClient*)op->root.object;
    httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
    httpParseStart(op);
  } else {
    finishOperation(&op->root, status, 1);
  }
}

void httpParseDefault(HttpComponent *component, void *arg)
{
  HTTPOp *op = (HTTPOp*)arg;
  HTTPClient *client = (HTTPClient*)op->root.object;
  switch (component->type) {
    case httpDtStartLine : {
      op->resultCode = component->startLine.code;
      break;
    }
    
    case httpDtHeaderEntry : {
      switch (component->header.entryType) {
        case hhContentType : {
          char *out = (char*)dynamicBufferAlloc(&client->out, component->header.stringValue.size+1);
          memcpy(out, component->header.stringValue.data, component->header.stringValue.size);
          out[component->header.stringValue.size] = 0;
          
          op->contentType.data = out;
          op->contentType.size = component->header.stringValue.size;
          break;
        }
      }
      
      break;
    }
    
    case httpDtData :
    case httpDtDataFragment : {
      char *out = (char*)dynamicBufferAlloc(&client->out, component->data.size+1);
      dynamicBufferSeek(&client->out, SeekCur, -1);
      memcpy(out, component->data.data, component->data.size);
      out[component->data.size] = 0;
          
      if (!op->body.data)
        op->body.data = out;
      op->body.size += component->data.size;
      break;
    }    
  }
}

void httpParseStart(HTTPOp *op)
{
  HTTPClient *client = (HTTPClient*)op->root.object;
  switch (httpParse(&client->state, op->parseCallback, op)) {
    case httpResultOk :
      finishOperation(&op->root, aosSuccess, 1);
      break;
    case httpResultNeedMoreData : {
      // copy 'tail' to begin of buffer
      size_t offset = httpDataRemaining(&client->state);
      if (offset)
        memcpy(client->inBuffer, httpDataPtr(&client->state), offset);
      
      if (client->isHttps)
        aioSslRead(op->root.base,
                   client->sslSocket,
                   client->inBuffer+offset,
                   client->inBufferSize-offset,
                   afNone,
                   0,
                   httpsRequestProc,
                   op);
      else
        aioRead(op->root.base,
                client->plainSocket,
                client->inBuffer+offset,
                client->inBufferSize-offset,
                afNone,
                0,
                httpRequestProc,
                op);
        
      client->inBufferOffset = offset;
      break;
    }
    case httpResultError :
      finishOperation(&op->root, aosUnknownError, 1);
      break;
  }
}

static void httpClientDestructor(aioObjectRoot *root)
{
  HTTPClient *client = (HTTPClient*)root;
  dynamicBufferFree(&client->out);
  free(client->inBuffer);
  free(client);
}

HTTPClient *httpClientNew(asyncBase *base, aioObject *socket)
{
  HTTPClient *client = (HTTPClient*)initObjectRoot(ioObjectUserDefined, sizeof(HTTPClient), httpClientDestructor); 
  client->isHttps = 0;
  client->inBuffer = (uint8_t*)malloc(65536);
  client->inBufferSize = 65536;
  client->inBufferOffset = 0;
  dynamicBufferInit(&client->out, 65536);
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->plainSocket = socket;
  return client;
}

HTTPClient *httpsClientNew(asyncBase *base, SSLSocket *socket)
{
  HTTPClient *client = (HTTPClient*)initObjectRoot(ioObjectUserDefined, sizeof(HTTPClient), httpClientDestructor);  
  client->isHttps = 1;
  client->inBuffer = (uint8_t*)malloc(65536);
  client->inBufferSize = 65536;
  client->inBufferOffset = 0;
  dynamicBufferInit(&client->out, 65536);
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->sslSocket = socket;
  return client;
}

void httpClientDelete(HTTPClient *client)
{
  client->root.links--; // TODO: atomic
  if (client->isHttps)
    sslSocketDelete(client->sslSocket);
  else
    deleteAioObject(client->plainSocket);
  checkForDeleteObject(&client->root);
}

void aioHttpConnect(asyncBase *base,
                    HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpConnectCb callback,
                    void *arg)
{
  asyncOpRoot *op = allocHttpOp(base, client, httpOpConnect, 0, callback, arg, usTimeout);
  if (addToExecuteQueue(&client->root, op, 1)) {
    if (client->isHttps)
      aioSslConnect(base, client->sslSocket, address, usTimeout, httpsConnectProc, op);
    else
      aioConnect(base, client->plainSocket, address, usTimeout, httpConnectProc, op);
  }
}

void aioHttpRequest(asyncBase *base,
                    HTTPClient *client,
                    const char *request,
                    size_t requestSize,
                    uint64_t usTimeout,
                    httpParseCb parseCallback,
                    httpRequestCb callback,
                    void *arg)
{
  asyncOpRoot *op = allocHttpOp(base, client, httpOpRequest, parseCallback, callback, arg, usTimeout);
  
  if (client->isHttps)
    aioSslWrite(base, client->sslSocket, (void*)request, requestSize, afNone, 0, 0, 0);
  else
    aioWrite(base, client->plainSocket, (void*)request, requestSize, afNone, 0, 0, 0);  
  
  if (addToExecuteQueue(&client->root, op, 0))
    start(op);
}


int ioHttpConnect(asyncBase *base, HTTPClient *client, const HostAddress *address, uint64_t usTimeout)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioHttpConnect(base, client, address, usTimeout, coroutineConnectCb, &r);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -1;
}


int ioHttpRequest(asyncBase *base,
                  HTTPClient *client,
                  const char *request,
                  size_t requestSize,
                  uint64_t usTimeout,
                  httpParseCb parseCallback)
{
  coroReturnStruct r = {coroutineCurrent()};
  aioHttpRequest(base, client, request, requestSize, usTimeout, parseCallback, coroutineRequestCb, &r);
  coroutineYield();
  return r.resultCode;
}
