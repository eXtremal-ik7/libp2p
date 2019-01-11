#include "asyncio/asyncioTypes.h"
#include "macro.h"

__NO_UNUSED_FUNCTION_BEGIN
static inline tag_t __tag_atomic_fetch_and_add(tag_t volatile *tag, tag_t value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(tag, value);
#else
#ifdef OS_32
  return InterlockedExchangeAdd((volatile LONG*)tag, value);
#else
  return InterlockedExchangeAdd64((volatile LONG64*)tag, value);
#endif
#endif
}

static inline unsigned __uint_atomic_fetch_and_add(unsigned volatile *tag, unsigned value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(tag, value);
#else
  return InterlockedExchangeAdd((volatile LONG*)tag, value);
#endif
}

static inline int __uint_atomic_compare_and_swap(unsigned volatile *tag, unsigned v1, unsigned v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
  return InterlockedCompareExchange((volatile LONG*)tag, v2, v1) == v1;
#endif
}

static inline int __tag_atomic_compare_and_swap(tag_t volatile *tag, tag_t v1, tag_t v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
#ifdef OS_32
  return InterlockedCompareExchange((volatile LONG*)tag, v2, v1) == v1;
#else
  return InterlockedCompareExchange64((volatile LONG64*)tag, v2, v1) == v1;
#endif
#endif
}

static inline int __pointer_atomic_compare_and_swap(void *volatile *tag, void *v1, void *v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(tag, v1, v2);
#else
  return InterlockedCompareExchangePointer(tag, v2, v1) == v1;
#endif
}


static inline void __spinlock_acquire(unsigned *lock)
{
  for (;;) {
    for (int i = 0; i < 7777; i++) {
      if (__uint_atomic_compare_and_swap(lock, 0, 1))
        return;
    }
#ifdef OS_WINDOWS
    SwitchToThread();
#else
    sched_yield();
#endif
  }
}

static inline int __spinlock_try_acquire(volatile unsigned *lock)
{
  return __uint_atomic_compare_and_swap(lock, 0, 1) ? 1 : 0;
}

static inline void __spinlock_release(volatile unsigned *lock)
{
  *lock = 0;
}

__NO_UNUSED_FUNCTION_END
