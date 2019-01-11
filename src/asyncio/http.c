#include "asyncio/http.h"

#include "asyncio/asyncio.h"
#include "asyncio/coroutine.h"
#include <string.h>

static const char *httpPoolId = "HTTP";
static const char *httpPoolTimerId = "HTTPTimer";

typedef enum {
  httpOpConnect = 0,
  httpOpRequest
} HttpOpTy;

typedef struct ioHttpRequestArg {
  HTTPClient *client;
  HTTPOp *op;
  const char *request;
  size_t requestSize;
} ioHttpRequestArg;

typedef struct coroReturnStruct {
  coroutineTy *coroutine;
  AsyncOpStatus status;
  int resultCode;
} coroReturnStruct;

static AsyncOpStatus httpParseStart(asyncOpRoot *opptr);

static asyncOpRoot *alloc()
{
  return (asyncOpRoot*)malloc(sizeof(HTTPOp));
}

static int cancel(asyncOpRoot *opptr)
{
  HTTPClient *client = (HTTPClient*)opptr->object;
  cancelIo(client->isHttps ? (aioObjectRoot*)client->sslSocket : (aioObjectRoot*)client->plainSocket);
  return 0;
}

static void connectFinish(asyncOpRoot *opptr)
{
  ((httpConnectCb*)opptr->callback)(opGetStatus(opptr), (HTTPClient*)opptr->object, opptr->arg);
}

static void requestFinish(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)opptr->object;
  client->contentType = op->contentType;
  client->body = op->body;
  ((httpRequestCb*)opptr->callback)(opGetStatus(opptr), client, op->resultCode, opptr->arg);
}


static void coroutineConnectCb(AsyncOpStatus status, HTTPClient *client, void *arg)
{
  __UNUSED(client)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  coroutineCall(r->coroutine);
}

static void coroutineRequestCb(AsyncOpStatus status, HTTPClient *client, int resultCode, void *arg)
{
  __UNUSED(client)
  coroReturnStruct *r = (coroReturnStruct*)arg;
  r->status = status;
  r->resultCode = resultCode;
  coroutineCall(r->coroutine);
}

void httpRequestProc(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  asyncOpRoot *opptr = (asyncOpRoot*)arg;
  HTTPClient *client = (HTTPClient*)opptr->object;
  httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
  resumeParent(opptr, status);
}

void httpsRequestProc(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
{
  __UNUSED(object)
  asyncOpRoot *opptr = (asyncOpRoot*)arg;
  HTTPClient *client = (HTTPClient*)opptr->object;
  httpSetBuffer(&client->state, client->inBuffer, client->inBufferOffset+transferred);
  resumeParent(opptr, status);
}

void httpConnectProc(AsyncOpStatus status, aioObject *object, void *arg)
{
  __UNUSED(object)
  resumeParent((asyncOpRoot*)arg, status);
}

void httpsConnectProc(AsyncOpStatus status, SSLSocket *object, void *arg)
{
  __UNUSED(object)
  resumeParent((asyncOpRoot*)arg, status);
}

static AsyncOpStatus httpConnectStart(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)opptr->object;
  if (op->state == 0) {
    op->state = 1;
    if (client->isHttps)
      aioSslConnect(client->sslSocket, &op->address, 0, httpsConnectProc, op);
    else
      aioConnect(client->plainSocket, &op->address, 0, httpConnectProc, op);
    return aosPending;
  } else {
    return aosSuccess;
  }
}

static AsyncOpStatus httpParseStart(asyncOpRoot *opptr)
{
  HTTPOp *op = (HTTPOp*)opptr;
  HTTPClient *client = (HTTPClient*)op->root.object;

  if (op->state == 0) {
    dynamicBufferSeek(&client->out, SeekSet, 0);
    httpInit(&client->state);
    op->state = 1;
  }

  switch (httpParse(&client->state, op->parseCallback, op)) {
    case httpResultOk :
      return aosSuccess;
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
                   0,
                   httpsRequestProc,
                   op);
      else
        aioRead(client->plainSocket,
                client->inBuffer+offset,
                client->inBufferSize-offset,
                afNone,
                0,
                httpRequestProc,
                op);

      client->inBufferOffset = offset;
      return aosPending;
    }
    case httpResultError :
      return aosUnknownError;
  }

  return aosUnknownError;
}


static HTTPOp *allocHttpOp(aioExecuteProc executeProc,
                           aioFinishProc finishProc,
                           HTTPClient *client,
                           int type,           
                           httpParseCb parseCallback,
                           void *callback,
                           void *arg,
                           uint64_t timeout)
{
  HTTPOp *op = (HTTPOp*)
    initAsyncOpRoot(httpPoolId, httpPoolTimerId, alloc, executeProc, cancel, finishProc, &client->root, callback, arg, 0, type, timeout);

  op->parseCallback = parseCallback;
  op->resultCode = 0;
  op->contentType.data = 0;
  op->body.data = 0;
  op->state = 0;
  return op;
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

static void httpClientDestructor(aioObjectRoot *root)
{
  HTTPClient *client = (HTTPClient*)root;
  dynamicBufferFree(&client->out);
  free(client->inBuffer);
  if (client->isHttps)
    sslSocketDelete(client->sslSocket);
  else
    deleteAioObject(client->plainSocket);
  free(client);
}

HTTPClient *httpClientNew(asyncBase *base, aioObject *socket)
{
  HTTPClient *client = (HTTPClient*)malloc(sizeof(HTTPClient));
  initObjectRoot(&client->root, base, ioObjectUserDefined, httpClientDestructor);
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
  HTTPClient *client = (HTTPClient*)malloc(sizeof(HTTPClient));
  initObjectRoot(&client->root, base, ioObjectUserDefined, httpClientDestructor);
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
  objectDelete(&client->root);
}

void aioHttpConnect(HTTPClient *client,
                    const HostAddress *address,
                    uint64_t usTimeout,
                    httpConnectCb callback,
                    void *arg)
{
  HTTPOp *op = allocHttpOp(httpConnectStart, connectFinish, client, httpOpConnect, 0, (void*)callback, arg, usTimeout);
  op->address = *address;
  opStart(&op->root);
}

void writeCb(AsyncOpStatus status, aioObject *object, size_t transferred, void *arg)
{
  __UNUSED(object);
  __UNUSED(transferred);
  asyncOpRoot *op = (asyncOpRoot*)arg;
  if (status == aosSuccess)
    opStart(op);
  else
    opCancel(op, opGetGeneration(op), status);
}

void sslWriteCb(AsyncOpStatus status, SSLSocket *object, size_t transferred, void *arg)
{
  __UNUSED(object);
  __UNUSED(transferred);
  asyncOpRoot *op = (asyncOpRoot*)arg;
  if (status == aosSuccess)
    opStart(op);
  else
    opCancel(op, opGetGeneration(op), status);
}

void aioHttpRequest(HTTPClient *client,
                    const char *request,
                    size_t requestSize,
                    uint64_t usTimeout,
                    httpParseCb parseCallback,
                    httpRequestCb callback,
                    void *arg)
{
  HTTPOp *op = allocHttpOp(httpParseStart, requestFinish, client, httpOpConnect, parseCallback, (void*)callback, arg, usTimeout);
  if (client->isHttps)
    aioSslWrite(client->sslSocket, request, requestSize, afSerialized, 0, sslWriteCb, op);
  else
    aioWrite(client->plainSocket, request, requestSize, afSerialized, 0, writeCb, op);
}


int ioHttpConnect(HTTPClient *client, const HostAddress *address, uint64_t usTimeout)
{
  combinerCallArgs ccArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  HTTPOp *op = allocHttpOp(httpConnectStart, connectFinish, client, httpOpConnect, 0, (void*)coroutineConnectCb, &r, usTimeout);
  op->address = *address;
  combinerCallDelayed(&ccArgs, &client->root, 1, &op->root, aaStart, 1);
  coroutineYield();
  return r.status == aosSuccess ? 0 : -(int)r.status;
}

void ioHttpRequestStart(void *arg)
{
  ioHttpRequestArg *hrArgs = (ioHttpRequestArg*)arg;
  if (hrArgs->client->isHttps)
    aioSslWrite(hrArgs->client->sslSocket, hrArgs->request, hrArgs->requestSize, afSerialized, 0, sslWriteCb, hrArgs->op);
  else
    aioWrite(hrArgs->client->plainSocket, hrArgs->request, hrArgs->requestSize, afSerialized, 0, writeCb, hrArgs->op);
}

int ioHttpRequest(HTTPClient *client,
                  const char *request,
                  size_t requestSize,
                  uint64_t usTimeout,
                  httpParseCb parseCallback)
{
  ioHttpRequestArg hrArgs;
  coroReturnStruct r = {coroutineCurrent(), aosPending, 0};
  HTTPOp *op = allocHttpOp(httpParseStart, requestFinish, client, httpOpRequest, parseCallback, (void*)coroutineRequestCb, &r, usTimeout);
  hrArgs.client = client;
  hrArgs.op = op;
  hrArgs.request = request;
  hrArgs.requestSize = requestSize;
  coroutineSetYieldCallback(ioHttpRequestStart, &hrArgs);
  coroutineYield();
  return r.status == aosSuccess ? r.resultCode : -r.status;
}
