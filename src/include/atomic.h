#ifndef __ATOMIC_H_
#define __ATOMIC_H_

#include "asyncio/asyncioTypes.h"
#include "macro.h"

__NO_UNUSED_FUNCTION_BEGIN
static inline unsigned __uint_atomic_fetch_and_add(unsigned volatile *ptr, unsigned value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(ptr, value);
#else
  return InterlockedExchangeAdd((volatile LONG*)ptr, value);
#endif
}

static inline int __uint_atomic_compare_and_swap(unsigned volatile *ptr, unsigned v1, unsigned v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(ptr, v1, v2);
#else
  return InterlockedCompareExchange((volatile LONG*)ptr, v2, v1) == v1;
#endif
}

static inline uintptr_t __uintptr_atomic_fetch_and_add(uintptr_t volatile *ptr, uintptr_t value)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_fetch_and_add(ptr, value);
#else
#ifdef OS_32
  return InterlockedExchangeAdd((volatile LONG*)ptr, value);
#else
  return InterlockedExchangeAdd64((volatile LONG64*)ptr, value);
#endif
#endif
}

static inline int __uintptr_atomic_compare_and_swap(uintptr_t volatile *ptr, uintptr_t v1, uintptr_t v2)
{
#ifndef _MSC_VER // Not Microsoft compiler
  return __sync_bool_compare_and_swap(ptr, v1, v2);
#else
#ifdef OS_32
  return InterlockedCompareExchange((volatile LONG*)ptr, v2, v1) == v1;
#else
  return InterlockedCompareExchange64((volatile LONG64*)ptr, v2, v1) == v1;
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

static inline void *__pointer_atomic_exchange(void *volatile *pointer, void *v1)
{
#ifndef _MSC_VER
  return __atomic_exchange_n(pointer, v1, __ATOMIC_SEQ_CST);
#else
  return InterlockedExchangePointer(pointer, v1);
#endif
}

static inline void __spinlock_acquire(unsigned *lock)
{
  for (;;) {
    int i;
    for (i = 0; i < 7777; i++) {
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
#endif //__ATOMIC_H_
