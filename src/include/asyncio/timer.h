#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

struct timeMark {
  uint64_t mark;
};

typedef struct timeMark timeMark;

timeMark getTimeMark();
uint64_t usDiff(timeMark first, timeMark second);

#ifdef __cplusplus
}
#endif
