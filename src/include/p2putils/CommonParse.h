#ifndef __LIBP2P_COMMONPARSE_H_
#define __LIBP2P_COMMONPARSE_H_

#include <string>

enum StateTy {
  stFinish = 0,
  stMemcmp,
  stError,
  stSwitchTable
};


struct StateElement {
  StateTy state;
  int token;
  const void *ptr;
  int intValue;
};


#endif //__LIBP2P_COMMONPARSE_H_
