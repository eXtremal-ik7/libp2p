#include "macro.h"
#include "LiteFlatHashTable.h"
#include "p2putils/HttpRequestParse.h"
#include "p2putils/uriParse.h"
#include <string.h>
#include <algorithm>

static LiteFlatHashTable *methodNames = nullptr;
static LiteFlatHashTable *headerNames = nullptr;

static Terminal httpMethods[] = {
  {"GET", hmGet},
  {"HEAD", hmHead},
  {"POST", hmPost},
  {"PUT", hmPut},
  {"DELETE", hmDelete},
  {"CONNECT", hmConnect},
  {"OPTIONS", hmOptions},
  {"TRACE", hmTrace},
  {"PATCH", hmPatch}
};

static Terminal httpHeaders[] = {
  {"Host", hhHost},
  {"User-Agent", hhUserAgent},
  {"Accept", hhAccept},
  {"Transfer-Encoding", hhTransferEncoding},
  {"Content-Length", hhContentLength}
};

struct UriArg {
  httpRequestParseCb *callback;
  void *arg;
};

static int isDigit(char s)
{
  return (s >= '0' && s <= '9');
}

static inline int canRead(const char *ptr, const char *end, size_t size)
{
  return size <= static_cast<size_t>(end-ptr);
}

static inline ParserResultTy compareUnchecked(const char **ptr, const char *end, const char *substr, size_t size)
{
  __UNUSED(end);
  if (memcmp(*ptr, substr, size) == 0) {
    (*ptr) += size;
    return ParserResultOk;
  } else {
    return ParserResultError;
  }
}

static ParserResultTy skipSPCharacters(const char **ptr, const char *end)
{
  if (*ptr < end) {
    if (**ptr != ' ')
      return ParserResultError;
    while (*ptr < end && **ptr == ' ')
      (*ptr)++;
    return ParserResultOk;
  } else {
   return ParserResultNeedMoreData;
  }
}

static ParserResultTy readUntilCRLF(const char **ptr, const char *end)
{
  if (*ptr >= end-2)
    return ParserResultNeedMoreData;

  while (*ptr < end-1) {
    if (**ptr == 0x0D && *(*ptr + 1) == 0x0A)
      break;
    (*ptr)++;
  }

  (*ptr) += 2;
  return ParserResultOk;
}

static inline ParserResultTy readDec(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return ParserResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size *= 10;
      *size += static_cast<unsigned char>(*p-'0');
    } else {
      return ParserResultError;
    }

    p++;
  }

  return ParserResultNeedMoreData;
}

static inline ParserResultTy readHex(const char **ptr, const char *end, size_t *size)
{
  *size = 0;
  const char *p = *ptr;
  while (p < end-2) {
    if (*p == 0x0D && *(p+1) == 0x0A) {
      *ptr = p+2;
      return ParserResultOk;
    } else if (*p >= '0' && *p <= '9') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'0');
    } else if (*p >= 'A' && *p <= 'F') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'A'+10);
    } else if (*p >= 'a' && *p <= 'f') {
      *size <<= 4; *size += static_cast<unsigned char>(*p-'a'+10);
    } else {
      return ParserResultError;
    }

    p++;
  }

  return ParserResultNeedMoreData;
}

void httpRequestParserInit(HttpRequestParserState *state)
{
  state->buffer = nullptr;
  state->end = nullptr;
  state->ptr = nullptr;
  state->state = httpRequestMethod;
  state->haveBody = 0;
  state->chunked = 0;
  state->dataRemaining = 0;
  state->firstFragment = true;
  if (!headerNames)
    headerNames = buildLiteFlatHashTable(httpHeaders, sizeof(httpHeaders) / sizeof(Terminal));
  if (!methodNames)
    methodNames = buildLiteFlatHashTable(httpMethods, sizeof(httpMethods)/ sizeof(Terminal));
}

void httpRequestSetBuffer(HttpRequestParserState *state, const void *buffer, size_t size)
{
  state->ptr = state->buffer = static_cast<const char*>(buffer);
  state->end = state->buffer + size;
}

ParserResultTy httpRequestParse(HttpRequestParserState *state, httpRequestParseCb callback, void *arg)
{
  ParserResultTy localResult;
  HttpRequestComponent component;
  UriArg uriArg;
  uriArg.callback = callback;
  uriArg.arg = arg;

  if (state->state == httpRequestMethod) {
    int token;
    char space = ' ';
    ParserResultTy result = searchLiteFlatHashTable(&state->ptr, state->end, &space, 1, methodNames, &token);
    if (result == ParserResultOk) {
      if (token == hmPost)
        state->haveBody = 1;

      component.type = httpRequestDtMethod;
      component.method = token;
      if (!callback(&component, arg))
        return ParserResultCancelled;
    } else {
      return result;
    }

    state->state = httpRequestUriPath;
  }

  if (state->state == httpRequestUriPath) {
    if ((localResult = skipSPCharacters(&state->ptr, state->end)) != ParserResultOk)
      return localResult;

    // Parse URI path
    localResult = uriParsePath(&state->ptr, state->end, false, [](URIComponent *source, void *arg) {
      HttpRequestComponent component;
      UriArg *uriArg = static_cast<UriArg*>(arg);
      if (source->type == uriCtPathElement) {
        component.type = httpRequestDtUriPathElement;
        component.data.data = source->raw.data;
        component.data.size = source->raw.size;
        return uriArg->callback(&component, uriArg->arg);
      } else {
        return 1;
      }
    }, &uriArg);
    if (localResult != ParserResultOk)
      return localResult;
    state->state = httpRequestUriQueryBegin;
  }

  if (state->state == httpRequestUriQueryBegin) {
    if (state->ptr == state->end)
      return ParserResultNeedMoreData;

    if (*state->ptr == '?') {
      state->state = httpRequestUriQuery;
      state->ptr++;
    } else {
      state->state = httpRequestUriFragment;
    }
  }

  // Parse URI query
  if (state->state == httpRequestUriQuery) {
    localResult = uriParseQuery(&state->ptr, state->end, false, [](URIComponent *source, void *arg) {
      HttpRequestComponent component;
      UriArg *uriArg = static_cast<UriArg*>(arg);
      if (source->type == uriCtQueryElement) {
        component.type = httpRequestDtUriQueryElement;
        component.data.data = source->raw.data;
        component.data.size = source->raw.size;
        component.data2.data = source->raw2.data;
        component.data2.size = source->raw2.size;
        return uriArg->callback(&component, uriArg->arg);
      } else {
        return 1;
      }
    }, &uriArg);
    if (localResult != ParserResultOk)
      return localResult;
    state->state = httpRequestUriFragment;
  }

  // Parse URI fragment
  if (state->state == httpRequestUriFragment) {
    if (state->ptr == state->end)
      return ParserResultNeedMoreData;

    if (*state->ptr == '#') {
      state->ptr++;
      localResult = uriParseFragment(&state->ptr, state->end, false, [](URIComponent *source, void *arg) {
        HttpRequestComponent component;
        UriArg *uriArg = static_cast<UriArg*>(arg);
        if (source->type == uriCtFragment) {
          component.type = httpRequestDtUriFragment;
          component.data.data = source->raw.data;
          component.data.size = source->raw.size;
          return uriArg->callback(&component, uriArg->arg);
        } else {
          return 1;
        }
      }, &uriArg);
      if (localResult != ParserResultOk)
        return localResult;
    }

    state->state = httpRequestVersion;
  }

  if (state->state == httpRequestVersion) {
    if ((localResult = skipSPCharacters(&state->ptr, state->end)) != ParserResultOk)
      return localResult;

    const char version[] = "HTTP/";
    if (!canRead(state->ptr, state->end, sizeof(version)-1 + 3 + 2))
      return ParserResultNeedMoreData;

    if ( (localResult = compareUnchecked(&state->ptr, state->end, version, sizeof(version)-1)) != ParserResultOk )
      return localResult;
    if ( !(isDigit(state->ptr[0]) && state->ptr[1] == '.' && isDigit(state->ptr[2]) && state->ptr[3] == '\r' && state->ptr[4] == '\n') )
      return ParserResultError;

    component.type = httpRequestDtVersion;
    component.version.majorVersion = static_cast<unsigned char>(state->ptr[0]-'0');
    component.version.minorVersion = static_cast<unsigned char>(state->ptr[2]-'0');
    if (!callback(&component, arg))
      return ParserResultCancelled;
    state->ptr += 5;
    state->state = httpRequestHeader;
  }

  if (state->state == httpRequestHeader) {
    if (!canRead(state->ptr, state->end, 2))
      return ParserResultNeedMoreData;
    component.type = httpRequestDtHeaderEntry;

    for (;;) {
      if (state->ptr[0] == '\r' && state->ptr[1] == '\n') {
        state->ptr += 2;
        if (state->haveBody) {
          state->state = httpRequestBody;
        } else {
          component.type = httpRequestDtDataLast;
          component.data.data = nullptr;
          component.data.size = 0;
          if (!callback(&component, arg))
            return ParserResultCancelled;
          state->state = httpRequestStLast;
        }
        break;
      }

      int token;
      component.header.entryName.data = state->ptr;
      const char *p = state->ptr;
      char colon = ':';
      ParserResultTy result = searchLiteFlatHashTable(&p, state->end, &colon, 1, headerNames, &token);
      if (result == ParserResultOk) {
        component.header.entryName.size = p - component.header.entryName.data;
        component.header.entryType = token;
        ++p;
        skipSPCharacters(&p, state->end);

        switch (token) {
          case hhTransferEncoding : {
            component.header.stringValue.data = p;
            if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
              return result;
            component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
            if (!callback(&component, arg))
              return ParserResultCancelled;

            state->chunked = (component.header.stringValue.size == 7) &&
                             (memcmp(component.header.stringValue.data, "chunked", 7) == 0);
            break;
          }

          case hhContentLength : {
            size_t contentSize;
            if ( ( result = readDec(&p, state->end, &contentSize)) != ParserResultOk )
              return result;
            component.header.sizeValue = contentSize;
            state->dataRemaining = contentSize;
            if (!callback(&component, arg))
              return ParserResultCancelled;
            break;
          }

          default : {
            component.header.stringValue.data = p;
            if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
              return result;
            component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
            if (!callback(&component, arg))
              return ParserResultCancelled;
            break;
          }
        }

        state->ptr = p;
      } else if (result == ParserResultNeedMoreData) {
        return result;
      } else {
        component.header.entryType = 0;
        component.header.entryName.data = p;
        const char *entryNameEnd = nullptr;
        while (p < state->end) {
          if (*p == ':') {
            entryNameEnd = p;
            p++;
            break;
          }
          p++;
        }

        component.header.entryName.size = static_cast<size_t>(entryNameEnd-component.header.entryName.data);
        skipSPCharacters(&p, state->end);
        component.header.stringValue.data = p;
        if ( ( result = readUntilCRLF(&p, state->end)) != ParserResultOk )
          return result;
        component.header.stringValue.size = static_cast<size_t>(p-component.header.stringValue.data-2);
        if (!callback(&component, arg))
          return ParserResultCancelled;

        state->ptr = p;
      }
    }
  }

  if (state->state == httpRequestBody) {
    if (state->chunked) {
      const char *readyChunk = nullptr;
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
            readyChunkSize = std::min(state->dataRemaining, static_cast<size_t>(state->end - p));
            p += readyChunkSize;
            state->dataRemaining -= readyChunkSize;
            needMoreData = true;
          }

          if (readyChunkSize) {
            component.type = httpRequestDtData;
            component.data.data = readyChunk;
            component.data.size = readyChunkSize;
            if (!callback(&component, arg))
              return ParserResultCancelled;
            state->ptr = p;
          }

          if (needMoreData)
            return ParserResultNeedMoreData;
        } else {
          // we at begin of next chunk
          if (!state->firstFragment) {
            // skip CRLF for non-first chunk
            if (!canRead(p, state->end, 2))
              return ParserResultNeedMoreData;
            p += 2;
          }

          size_t chunkSize;
          if ( ( localResult = readHex(&p, state->end, &chunkSize)) != ParserResultOk )
            return localResult;

          if (chunkSize == 0) {
            component.type = httpRequestDtDataLast;
            component.data.data = p;
            component.data.size = 0;
            if (!callback(&component, arg))
              return ParserResultCancelled;
            state->state = httpRequestStLast;
            state->ptr = p+2;
            break;
          } else {
            state->dataRemaining = chunkSize;
          }
        }
      }
    } else {
      const char *p = state->ptr;
      if (canRead(p, state->end, state->dataRemaining)) {
        component.type = httpRequestDtDataLast;
        component.data.data = p;
        component.data.size = state->dataRemaining;
        if (!callback(&component, arg))
          return ParserResultCancelled;
        state->ptr = p + state->dataRemaining;
        state->state = httpRequestStLast;
      } else {
        if (p != state->end) {
          size_t size = std::min(state->dataRemaining, static_cast<size_t>(state->end - p));
          component.type = httpRequestDtData;
          component.data.data = p;
          component.data.size = size;
          if (!callback(&component, arg))
            return ParserResultCancelled;
          state->ptr = p + size;
          state->dataRemaining -= size;
        }
        return ParserResultNeedMoreData;
      }
    }
  }

  return ParserResultOk;
}

const void *httpRequestDataPtr(HttpRequestParserState *state)
{
  return state->ptr;
}

size_t httpRequestDataRemaining(HttpRequestParserState *state)
{
  return static_cast<size_t>(state->end - state->ptr);
}
