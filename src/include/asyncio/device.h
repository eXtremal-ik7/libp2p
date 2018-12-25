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

int deviceSyncRead(iodevTy hDevice, void *buffer, size_t size, int waitAll, size_t *bytesTransferred);
int deviceSyncWrite(iodevTy hDevice, const void *buffer, size_t size, int waitAll, size_t *bytesTransferred);

#ifdef __cplusplus
}
#endif
