#ifdef __cplusplus
extern "C" {
#endif

#include "asyncio/asyncioTypes.h"

iodevTy serialPortOpen(const char *name);

void serialPortClose(iodevTy port);

int serialPortSetConfig(iodevTy port,
                        int speed,
                        int dataBits,
                        int stopBits,
                        int parity);

void serialPortFlush(iodevTy port);


#ifdef __cplusplus
}
#endif
