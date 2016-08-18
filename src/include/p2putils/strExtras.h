#ifndef __LIBP2P_STREXTRAS_H_
#define __LIBP2P_STREXTRAS_H_

#include <algorithm>

template<typename IntType> IntType xhton(IntType X)
{
  if (!is_bigendian()) {
    IntType lX = X;
    IntType result = 0;

    for (size_t i = 0; i < sizeof(IntType); i++) {
      result |= (lX & 0xFF) << (8 * (sizeof(IntType) - i - 1));
      lX >>= 8;
    }

    return result;
  } else {
    return X;
  }
}

template<typename IntType> IntType xntoh(IntType X)
{
  return xhton(X);
}

template<typename Type> size_t xitoa(Type value, char *out)
{
  Type lvalue = value;
  char *lout = out;
  char *pout = out, *pOutEnd;
  
  if (lvalue < 0) {
    lvalue = -lvalue;
    *pout++ = '-';
    lout++;
  }
  
  do {
    *pout++ = '0' + lvalue % 10;
    lvalue /= 10;
  } while (lvalue);
  
  pOutEnd = pout;
  *pOutEnd = 0;
  
  pout--;
  while (lout < pout)
    std::swap(*lout++, *pout--); 
  
  return pOutEnd - out;
}

template<typename Type> Type xatoi(const char *in)
{
  Type lvalue = 0;
  const char *p = in;
  while (*p)
    lvalue = (lvalue*10) + (*p++ - '0');
  
  return lvalue;
}

#endif //__LIBP2P_STREXTRAS_H_
