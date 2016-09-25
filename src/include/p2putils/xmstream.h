#ifndef __XMSTREAM_H_
#define __XMSTREAM_H_

#include "p2putils/strExtras.h"
#include <string.h>

class xmstream {
private:
  uint8_t *_m;
  uint8_t *_p;
  size_t _size;
  size_t _msize;
  
  int _eof;
  int _own;
  
public:
  xmstream(void *data, size_t size) : _m((uint8_t*)data), _p((uint8_t*)data), _size(size), _msize(size), _eof(0), _own(0) {}
  xmstream(size_t size = 64) : _size(0), _msize(size), _eof(0), _own(1) {
    _m = (uint8_t*)malloc(size);
    _p = _m;
  }
  
  ~xmstream() {
    if (_own)
      free(_m);
  }
  
  size_t offsetOf() { return _p - _m; }
  size_t sizeOf() { return _size; }
  size_t remaining() { return _size - offsetOf(); }
  int eof() { return _eof; }
  
  void *data() { return _m; }
  template<typename T> T *data() { return (T*)_m; }
  template<typename T> T *ptr() { return (T*)_p; }
  
  void *alloc(size_t size) {
    _eof = 0;
    size_t offset = offsetOf();
    size_t required = offset + size;
    if (required > _msize) {
      size_t newSize = _msize;
      do {
        newSize *= 2;
      } while (newSize < required);
      
      _msize = newSize;
      if (_own) {
        _m = (uint8_t*)realloc(_m, _msize);
      } else {
        uint8_t *newData = (uint8_t*)malloc(_msize);
        memcpy(newData, _m, _size);
        _m = newData;
      }
      
      _size = required;
      uint8_t *p = _m + offset;
      _p = p + size;
      _own = 1;
      return p;
    } else {
      void *p = _p;
      _p += size;
      _size = required < _size ? _size : required;
      return p;
    }
  }
  
  template<typename T> T *alloc(size_t size) { return (T*)alloc(size); }
  
  // pointer move functions
  void reset() {
    _p = _m;
    _size = 0;
    _eof = 0;
  }
  
  void seekSet(size_t offset) {
    _p = _m + ((offset < _size) ? offset : _size);
  }
  
  // read functions
  template<typename T> T *jumpOver(size_t size) {
    if (size <= remaining()) {
      T *p = (T*)_p;
      _p += size;
      return p;
    } else {
      _eof = 1;
      return 0;
    }
  }
  
  void read(void *data, size_t size) {
    if (void *p = jumpOver<void>(size))
      memcpy(data, p, size);
  }
  
  template<typename T> T read() {
    T *p = jumpOver<T>(sizeof(T));
    return p ? *p : T();
  }
  
  template<typename T> T readNetworkByteOrder() {
    T *p = jumpOver<T>(sizeof(T));
    return p ? xhton<T>(*p) : T();
  }
  
  // write functions
  void write(const void *data, size_t size) { memcpy(alloc<void>(size), data, size); }
  template<typename T>
    void write(const T& data) { *(T*)alloc<T>(sizeof(T)) = data; }
  template<typename T>
    void writeNetworkByteOrder(T data) { *(T*)alloc<T>(sizeof(T)) = xhton(data); }
};

#endif //__XMSTREAM_H_
