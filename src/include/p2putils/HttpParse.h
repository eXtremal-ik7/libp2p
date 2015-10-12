#ifdef __cplusplus
extern "C" {
#endif 

#ifndef __LIBP2P_HTTPPARSE_H_
#define __LIBP2P_HTTPPARSE_H_

#include <stddef.h>
#include <stdint.h>
#include "p2putils/http.h"

typedef enum HttpParserStateTy {
  httpStStartLine = 0,
  httpStHeader,  
  httpStBody,
  httpStLast
} HttpParserStateTy;

typedef enum HttpParserResultTy {
  httpResultOk = 0,
  httpResultNeedMoreData,
  httpResultError
} HttpParserResultTy;

typedef enum HttpParserDataTy {
  httpDtStartLine = 0,
  httpDtHeaderEntry,
  httpDtData,
  httpDtDataFragment
} HttpParserDataTy;

typedef struct HttpParserState {
  HttpParserStateTy state;
  char *buffer;
  char *ptr;
  char *end;
  int chunked;
  int firstFragment;
  size_t dataRemaining;
} HttpParserState;

typedef struct Raw {
  const char *data;
  size_t size;
} Raw;

typedef struct HttpComponent {
  int type;
  union {
    // Start line
    struct {
      unsigned majorVersion;
      unsigned minorVersion;
      unsigned code;
      Raw description;
    } startLine;
    
    // Header
    struct {
      int entryType;
      Raw entryName;
      union {
        Raw stringValue;
        int64_t intValue;
      };
    } header;
    
    Raw data;
  };
} HttpComponent;

typedef void httpParseCb(HttpComponent *component, void *arg);

void httpInit(HttpParserState *state);
void httpSetBuffer(HttpParserState *state, void *buffer, size_t size);
HttpParserResultTy httpParse(HttpParserState *state, httpParseCb callback, void *arg);

void *httpDataPtr(HttpParserState *state);
size_t httpDataRemaining(HttpParserState *state);

#endif //__LIBP2P_HTTPPARSE_H_

#ifdef __cplusplus
}
#endif
