#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>


struct cqueue_ptr {
  void **M;
  size_t allocatedSize;
  size_t begin;
  size_t end;
};

typedef struct cqueue_ptr cqueue_ptr;

void cqueue_ptr_init(cqueue_ptr *queue, size_t size);
void cqueue_ptr_push(cqueue_ptr *queue, void *data);
int cqueue_ptr_peek(cqueue_ptr *queue, void **data);
int cqueue_ptr_pop(cqueue_ptr *queue, void **data);

#ifdef __cplusplus
}
#endif
