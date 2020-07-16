#ifndef __LIBP2P_HTTPREQUESTPARSE_H_
#define __LIBP2P_HTTPREQUESTPARSE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "HttpParseCommon.h"
#include "CommonParse.h"
#include <stddef.h>

typedef enum HttpRequestParserStateTy {
  httpRequestMethod = 0,
  httpRequestUriPath,
  httpRequestUriQueryBegin,
  httpRequestUriQuery,
  httpRequestUriFragment,
  httpRequestVersion,
  httpRequestHeader,
  httpRequestBody,
  httpRequestStLast
} HttpRequestParserStateTy;

typedef enum HttpRequestParserDataTy {
  httpRequestDtInitialize = 0,
  httpRequestDtMethod,
  httpRequestDtUriPathElement,
  httpRequestDtUriQueryElement,
  httpRequestDtUriFragment,
  httpRequestDtVersion,
  httpRequestDtHeaderEntry,
  httpRequestDtData,
  httpRequestDtDataLast,
} HttpRequestParserDataTy;

typedef struct HttpRequestParserState {
  HttpRequestParserStateTy state;
  const char *buffer;
  const char *ptr;
  const char *end;
  int haveBody;
  int chunked;
  int firstFragment;
  size_t dataRemaining;
} HttpRequestParserState;

typedef struct HttpRequestComponent {
  HttpRequestParserDataTy type;
  union {
    int method;
    Raw data;

    // Version
    struct {
      unsigned majorVersion;
      unsigned minorVersion;
    } version;

    // Header
    struct {
      int entryType;
      Raw entryName;
      union {
        Raw stringValue;
        size_t sizeValue;
      };
    } header;
  };
  Raw data2;
} HttpRequestComponent;

typedef int httpRequestParseCb(HttpRequestComponent *component, void *arg);

void httpRequestParserInit(HttpRequestParserState *state);
void httpRequestSetBuffer(HttpRequestParserState *state, const void *buffer, size_t size);
ParserResultTy httpRequestParse(HttpRequestParserState *state, httpRequestParseCb callback, void *arg);

const void *httpRequestDataPtr(HttpRequestParserState *state);
size_t httpRequestDataRemaining(HttpRequestParserState *state);

#ifdef __cplusplus
}
#endif

#endif //__LIBP2P_HTTPREQUESTPARSE_H_
