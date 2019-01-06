#include "p2putils/CommonParse.h"
#include "p2putils/HttpParse.h"
#include <string.h>
#include <algorithm>

extern StateElement httpHeaderFSM[128];

HttpParserResultTy simpleTableParse(const char **ptr, const char *end, StateElement *entry, int *token)
{
  const char *p = *ptr;
  StateElement *currentState = entry;
  while (p != end) {
    StateElement &current = currentState[(int)*p];
    switch (current.state) {
      case stFinish :
        *token = current.token;
        *ptr = p;
        return httpResultOk;
      case stError:
        return httpResultError;
      case stMemcmp:
        *token = current.token;
        if (memcmp(p+1, current.ptr, current.intValue) == 0) {
          *ptr = p+1+current.intValue;
          return httpResultOk;
        } else {
          return httpResultError;
        }
      case stSwitchTable:
        currentState = (StateElement*)current.ptr;
        break;
    }
      
    p++;
  }
  
  return httpResultNeedMoreData;
}

static int isDigit(char s)
{
  return (s >= '0' && s <= '9');
}

static inline int canRead(const char *ptr, const char *end, size_t size)
{
  return size <= (size_t)(end-ptr);
}

static HttpParserResultTy compareUnchecked(const char **ptr, const char *end, const char *substr, size_t size)
{
  if (memcmp(*ptr, substr, size) == 0) {
    (*ptr) += size;
    return httpResultOk;
  } else {
    return httpResultError;
  }
}

static HttpParserResultTy skipSPCharacters(const char **ptr, const char *end)
{
  if (*ptr < end) {
    if (**ptr != ' ')
      return httpResultError;
    while (*ptr < end && **ptr == ' ')
      (*ptr)++;
    return httpResultOk;
  } else {
   return httpResultNeedMoreData; 
  }
}

static HttpParserResultTy readUntilCRLF(const char **ptr, const char *end)
{
  if (*ptr >= end-2)
    return httpResultNeedMoreData;
  
  while (*ptr < end-1) {
    if (**ptr == 0x0D && *(*ptr + 1) == 0x0A)
      break;
    (*ptr)++;
  }
  
  (*ptr) += 2;
  return httpResultOk;
}

static inline HttpParserResultTy readDec(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return httpResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size *= 10; *size += *p-'0';
    } else {
      return httpResultError;
    }
    
    p++;
  }
  
  return httpResultNeedMoreData;
}

static inline HttpParserResultTy readHex(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return httpResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size <<= 4; *size += *p-'0';
    } else if (*p >= 'A' && *p <= 'F') {
      *size <<= 4; *size += *p-'A'+10;
    } else if (*p >= 'a' && *p <= 'f') {
      *size <<= 4; *size += *p-'a'+10;
    } else {
      return httpResultError;
    }
    
    p++;
  }
  
  return httpResultNeedMoreData;
}

static HttpParserResultTy httpParseStartLine(HttpParserState *state, httpParseCb callback, void *arg)
{
  const char startLine[] = "HTTP/";
  
  HttpParserResultTy result;
  HttpComponent component;
  const char *ptr = state->ptr;
  if (!canRead(ptr, state->end, sizeof(startLine)-1 + 3))
    return httpResultNeedMoreData;
  
  // HTTP Protocol version
  if ( (result = compareUnchecked(&ptr, state->end, startLine, sizeof(startLine)-1)) != httpResultOk )
    return result;
  if ( !(isDigit(ptr[0]) && ptr[1] == '.' && isDigit(ptr[2])) )
    return httpResultError;
  component.startLine.majorVersion = ptr[0]-'0';
  component.startLine.minorVersion = ptr[2]-'0';
  ptr += 3;
  
  // HTTP response code
  skipSPCharacters(&ptr, state->end);
  if (!canRead(ptr, state->end, 3))
    return httpResultNeedMoreData;
  component.startLine.code = 100*(ptr[0]-'0') + 10*(ptr[1]-'0') + (ptr[2]-'0');
  ptr += 3;
  
  // associated textual phrase
  skipSPCharacters(&ptr, state->end);
  component.startLine.description.data = ptr;
  if ( ( result = readUntilCRLF(&ptr, state->end)) != httpResultOk )
    return result;
  component.startLine.description.size = ptr-component.startLine.description.data-2;
  component.type = httpDtStartLine;
  callback(&component, arg);
  
  state->ptr = (char*)ptr;
  return httpResultOk;
}

void httpInit(HttpParserState *state)
{
  state->state = httpStStartLine;
  state->chunked = false;
  state->dataRemaining = 0;
  state->firstFragment = true;  
}

void httpSetBuffer(HttpParserState *state, void *buffer, size_t size)
{
  state->ptr = state->buffer = (char*)buffer;
  state->end = state->buffer + size;
}

HttpParserResultTy httpParse(HttpParserState *state, httpParseCb callback, void *arg)
{
  HttpParserResultTy result;
  HttpComponent component;

  if (state->state == httpStStartLine) {
    if ( (result = httpParseStartLine(state, callback, arg)) != httpResultOk)
      return result;
    state->state = httpStHeader;
  }

  if (state->state == httpStHeader) {
    if (canRead(state->ptr, state->end, 2)) {
      component.type = httpDtHeaderEntry;
      for (;;) {
        if (state->ptr[0] == '\r' && state->ptr[1] == '\n') {
          state->ptr += 2;
          state->state = httpStBody;
          break;
        } else {
          int token;
          const char *p = state->ptr;
          HttpParserResultTy result = simpleTableParse(&p, state->end, httpHeaderFSM, &token);
          if (result == httpResultOk) {
            component.header.entryType = token;
            skipSPCharacters(&p, state->end);

            switch (token) {
              case hhContentLength : {
                size_t contentSize;
                if ( ( result = readDec(&p, state->end, &contentSize)) != httpResultOk )
                  return result;
                component.header.intValue = contentSize;
                state->dataRemaining = contentSize;
                callback(&component, arg);
                break;
              }

              case hhTransferEncoding : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
                  return result;
                component.header.stringValue.size = p-component.header.stringValue.data-2;
                callback(&component, arg);

                state->chunked = (component.header.stringValue.size == 7) &&
                                 (memcmp(component.header.stringValue.data, "chunked", 7) == 0);
                break;
              }

              case hhContentType :
              case hhConnection :
              case hhDate :
              case hhServer : {
                component.header.stringValue.data = p;
                if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
                  return result;
                component.header.stringValue.size = p-component.header.stringValue.data-2;
                callback(&component, arg);
                break;
              }
            }

            state->ptr = (char*)p;
          } else if (result == httpResultNeedMoreData) {
            return result;
          } else if (result == httpResultError) {
            component.header.entryType = 0;
            component.header.entryName.data = p;
            const char *entryNameEnd = 0;
            while (p < state->end) {
              if (*p == ':') {
                entryNameEnd = p;
                p++;
                break;
              }
              p++;
            }

            component.header.entryName.size = entryNameEnd-component.header.entryName.data;
            skipSPCharacters(&p, state->end);
            component.header.stringValue.data = p;
            if ( ( result = readUntilCRLF(&p, state->end)) != httpResultOk )
              return result;
            component.header.stringValue.size = p-component.header.stringValue.data-2;
            callback(&component, arg);

            state->ptr = (char*)p;
          }
        }
      }
    } else {
      return httpResultNeedMoreData;
    }
  }

  if (state->state == httpStBody) {
    if (state->chunked) {
      const char *readyChunk = 0;
      size_t readyChunkSize = 0;
      const char *p = state->ptr;

      for (;;) {
        if (state->dataRemaining) {
          bool needMoreData = false;
          // need read /httpDataRemaining/ bytes
          if (canRead(p, state->end, state->dataRemaining)) {
            readyChunk = p;
            readyChunkSize = state->dataRemaining;
            p += state->dataRemaining;
            state->dataRemaining = 0;
            state->firstFragment = false;
          } else {
            readyChunk = p;
            readyChunkSize = std::min(state->dataRemaining, (size_t)(state->end - p));
            p += readyChunkSize;
            state->dataRemaining -= readyChunkSize;
            needMoreData = true;
          }

          if (readyChunkSize) {
            component.type = httpDtDataFragment;
            component.data.data = readyChunk;
            component.data.size = readyChunkSize;
            callback(&component, arg);
            state->ptr = (char*)p;
          }

          if (needMoreData)
            return httpResultNeedMoreData;
        } else {
          // we at begin of next chunk
          if (!state->firstFragment) {
            // skip CRLF for non-first chunk
            if (!canRead(p, state->end, 2))
              return httpResultNeedMoreData;
            p += 2;
          }

          size_t chunkSize;
          if ( ( result = readHex(&p, state->end, &chunkSize)) != httpResultOk )
            return result;

          if (chunkSize == 0) {
            component.type = httpDtData;
            component.data.data = p;
            component.data.size = 0;
            callback(&component, arg);
            state->state = httpStLast;
            state->ptr = (char*)p+2;
            break;
          } else {
            state->dataRemaining = chunkSize;
          }
        }
      }
    } else {
      const char *p = state->ptr;
      if (canRead(p, state->end, state->dataRemaining)) {
        component.type = state->firstFragment ? httpDtData : httpDtDataFragment;
        component.data.data = p;
        component.data.size = state->dataRemaining;
        callback(&component, arg);
        state->ptr = (char*)p + state->dataRemaining;
        state->state = httpStLast;
      } else if (p != state->end) {
        size_t size = std::min(state->dataRemaining, (size_t)(state->end - p));
        component.type = httpDtDataFragment;
        component.data.data = p;
        component.data.size = size;
        callback(&component, arg);
        state->ptr = (char*)p + size;
        state->firstFragment = false;
        state->dataRemaining -= size;
        return httpResultNeedMoreData;
      }
    }
  }

  return httpResultOk;
}

void *httpDataPtr(HttpParserState *state)
{
  return state->ptr;
}

size_t httpDataRemaining(HttpParserState *state)
{
  return state->end - state->ptr;
}
