#include "asyncio/http.h"
#include <string.h>

const char *httpPoolId = "HTTP";

typedef enum {
  httpOpConnect = 0,
  httpOpRequest
} HttpOpTy;

void httpConnectProc(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg);
void httpRequestproc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg);

void httpsConnectProc(SSLInfo *info);
void httpsRequestProc(SSLInfo *info);

void httpParseStart(HTTPOp *op);

static HTTPOp *allocHttpOp(HTTPClient *client,
                           int type,
                           httpParseCb parseCallback,
                           httpCb callback,
                           void *arg)
{
  HTTPOp *op = (HTTPOp*)queryObject(client->base, httpPoolId);
  if (!op)
    op = (HTTPOp*)malloc(sizeof(HTTPOp));

  op->type = type;
  op->parseCallback = parseCallback;
  op->callback = callback;
  op->next = 0;
  op->info.client = client;  
  op->info.arg = arg;
  
  op->info.resultCode = 0;
  op->info.contentType.data = 0;
  op->info.body.data = 0;
  return op;
}

static void finishHttpOp(HTTPOp *Op, AsyncOpStatus status)
{
  HTTPOp *current = Op->next;
  Op->info.client->current = current;
  Op->info.client->tail = current ? Op->info.client->tail : 0;

  Op->info.status = status;
  if (Op->callback)
    Op->callback(&Op->info);
  releaseObject(Op->info.client->base, Op, httpPoolId);
  if (current) {
    HTTPClient *client = current->info.client;
    switch (current->type) {
      case httpOpConnect :
        if (client->isHttps) {
          aioSslConnect(client->sslSocket, &current->address, current->usTimeout, httpsConnectProc, current);
        } else {
          aioConnect(client->plainSocket, &current->address, current->usTimeout, httpConnectProc, current);
        }
        break;
      case httpOpRequest :
        dynamicBufferSeek(&client->out, SeekSet, 0);
        httpInit(&client->state);
        httpParseStart(current);
        break;
    }
  }
}

void httpConnectProc(AsyncOpStatus status, asyncBase *base, aioObject *object, void *arg)
{
  finishHttpOp((HTTPOp*)arg, status);
}

void httpsConnectProc(SSLInfo *info)
{
  finishHttpOp((HTTPOp*)info->arg, info->status);
}

void httpRequestProc(AsyncOpStatus status, asyncBase *base, aioObject *object, size_t transferred, void *arg)
{
  if (status == aosSuccess) {
    HTTPOp *Op = (HTTPOp*)arg;
    HTTPClient *client = Op->info.client;
    httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
    httpParseStart((HTTPOp*)arg);
  } else {
    finishHttpOp((HTTPOp*)arg, status);
  }  
}

void httpsRequestProc(SSLInfo *info)
{
  if (info->status == aosSuccess) {
    HTTPOp *Op = (HTTPOp*)info->arg;
    HTTPClient *client = Op->info.client;
    httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+info->bytesTransferred);
    httpParseStart((HTTPOp*)info->arg);
  } else {
    finishHttpOp((HTTPOp*)info->arg, info->status);
  }
}

void httpParseDefault(HttpComponent *component, void *arg)
{
  HTTPOp *Op = (HTTPOp*)arg;
  HTTPClient *client = Op->info.client;
  switch (component->type) {
    case httpDtStartLine : {
      Op->info.resultCode = component->startLine.code;
      break;
    }
    
    case httpDtHeaderEntry : {
      switch (component->header.entryType) {
        case hhContentType : {
          char *out = (char*)dynamicBufferAlloc(&client->out, component->header.stringValue.size+1);
          memcpy(out, component->header.stringValue.data, component->header.stringValue.size);
          out[component->header.stringValue.size] = 0;
          
          Op->info.contentType.data = out;
          Op->info.contentType.size = component->header.stringValue.size;
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
          
      if (!Op->info.body.data)
        Op->info.body.data = out;
      Op->info.body.size += component->data.size;
      break;
    }    
  }
}

void httpParseStart(HTTPOp *op)
{
  HTTPClient *client = op->info.client;
  switch (httpParse(&client->state, op->parseCallback, op)) {
    case httpResultOk :
      finishHttpOp(op, aosSuccess);
      break;
    case httpResultNeedMoreData : {
      // copy 'tail' to begin of buffer
      size_t offset = httpDataRemaining(&client->state);
      if (offset)
        memcpy(client->inBuffer, httpDataPtr(&client->state), offset);
      
      if (client->isHttps)
        aioSslRead(client->sslSocket,
                client->inBuffer+offset,
                client->inBufferSize-offset,
                afNone,
                op->usTimeout,
                httpsRequestProc,
                op);
      else
        aioRead(client->plainSocket,
                client->inBuffer+offset,
                client->inBufferSize-offset,
                afNone,
                op->usTimeout,
                httpRequestProc,
                op);
        
      client->inBufferOffset = offset;
      break;
    }
    case httpResultError :
      finishHttpOp(op, aosUnknownError);
      break;
  }
}


HTTPClient *httpClientNew(asyncBase *base, aioObject *socket)
{
  HTTPClient *client = (HTTPClient*)malloc(sizeof(HTTPClient));
  client->base = base;
  client->isHttps = 0;
  client->inBuffer = (uint8_t*)malloc(65536);
  client->inBufferSize = 65536;
  client->inBufferOffset = 0;
  dynamicBufferInit(&client->out, 65536);
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->current = 0;
  client->tail = 0;
  client->plainSocket = socket;
  return client;
}

HTTPClient *httpsClientNew(asyncBase *base, SSLSocket *socket)
{
  HTTPClient *client = (HTTPClient*)malloc(sizeof(HTTPClient));
  client->base = base;
  client->isHttps = 1;
  client->inBuffer = (uint8_t*)malloc(65536);
  client->inBufferSize = 65536;
  client->inBufferOffset = 0;
  dynamicBufferInit(&client->out, 65536);
  httpSetBuffer(&client->state, client->inBuffer, 0);
  client->current = 0;
  client->tail = 0;
  client->sslSocket = socket;
  return client;
}

void httpClientDelete(HTTPClient *client)
{
  dynamicBufferFree(&client->out);
  free(client->inBuffer);
  free(client);
}

void aioHttpConnect(HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpCb callback,
                    void *arg)
{
  HTTPOp *newOp = allocHttpOp(client, httpOpConnect, 0, callback, arg);
  
  if (!client->tail) {
    client->current = newOp;
    client->tail = newOp;
    if (client->isHttps) {
      aioSslConnect(client->sslSocket, address, usTimeout, httpsConnectProc, newOp);
    } else {
      aioConnect(client->plainSocket, address, usTimeout, httpConnectProc, newOp);
    }
  } else {
    newOp->address = *address;
    newOp->usTimeout = usTimeout;    
    client->tail->next = newOp;
    client->tail = newOp;
  }
}

void aioHttpRequest(HTTPClient *client,
                 const char *request,
                 size_t requestSize,
                 uint64_t usTimeout,
                 httpParseCb parseCallback,
                 httpCb callback,
                 void *arg)
{
  HTTPOp *newOp = allocHttpOp(client, httpOpRequest, parseCallback, callback, arg);
  newOp->usTimeout = usTimeout;
  
  // send request immediately, wait response in queue
  if (client->isHttps)
    aioSslWrite(client->sslSocket, (void*)request, requestSize, afNone, usTimeout, 0, 0);
  else
    aioWrite(client->plainSocket, (void*)request, requestSize, afNone, usTimeout, 0, 0);

  if (!client->tail) {
    client->current = newOp;
    client->tail = newOp;    
    dynamicBufferSeek(&client->out, SeekSet, 0);
    httpInit(&client->state);
    httpParseStart(newOp);
  } else {
    newOp->usTimeout = usTimeout;
    client->tail->next = newOp;
    client->tail = newOp;
  }
}


int ioHttpConnect(HTTPClient *client, const HostAddress *address, uint64_t usTimeout)
{
  HTTPOp *newOp = allocHttpOp(client, httpOpConnect, 0, 0, 0);
  
  int result;
  if (client->isHttps)
    result = ioSslConnect(client->sslSocket, address, usTimeout);
  else
    result = ioConnect(client->plainSocket, address, usTimeout);
  
  releaseObject(newOp->info.client->base, newOp, httpPoolId);  
  return result;
}


int ioHttpRequest(HTTPClient *client,
                  const char *request,
                  size_t requestSize,
                  uint64_t usTimeout,
                  httpParseCb parseCallback)
{
  HTTPOp *newOp = allocHttpOp(client, httpOpRequest, parseCallback, 0, 0);
  
  int result;
  if (client->isHttps)
    result = ioSslWrite(client->sslSocket, (void*)request, requestSize, afNone, usTimeout);
  else
    result = ioWrite(client->plainSocket, (void*)request, requestSize, afNone, usTimeout);  
  if (result == -1) {
    releaseObject(newOp->info.client->base, newOp, httpPoolId);
    return -1;
  }

  dynamicBufferSeek(&client->out, SeekSet, 0);
  httpInit(&client->state);  
  
  for (;;) {
    switch (httpParse(&client->state, parseCallback, newOp)) {
      case httpResultOk :
        client->contentType = newOp->info.contentType;
        client->body = newOp->info.body;
        releaseObject(newOp->info.client->base, newOp, httpPoolId);
        return newOp->info.resultCode;
      case httpResultNeedMoreData : {
        // copy 'tail' to begin of buffer
        size_t offset = httpDataRemaining(&client->state);
        if (offset)
          memcpy(client->inBuffer, httpDataPtr(&client->state), offset);
      
        ssize_t bytesTransferred;
        if (client->isHttps)
          bytesTransferred =
            ioSslRead(client->sslSocket, client->inBuffer+offset, client->inBufferSize-offset, afNone, usTimeout);
        else
          bytesTransferred =
            ioRead(client->plainSocket, client->inBuffer+offset, client->inBufferSize-offset, afNone, usTimeout);
            
        client->inBufferOffset = offset;
        httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+bytesTransferred);
        break;
      }
      case httpResultError :
        releaseObject(newOp->info.client->base, newOp, httpPoolId);
        return -1;
    }
  }
}
