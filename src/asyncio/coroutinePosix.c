#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include "asyncio/coroutine.h"
#include "libp2pconfig.h"

typedef struct contextTy {
#ifdef __i386__
#define CTX_EIP_INDEX 0
#define CTX_ESP_INDEX 1
  uint32_t registers[8];
#elif __x86_64__
#define CTX_RIP_INDEX 4
#define CTX_RSP_INDEX 5
  uint64_t registers[9];
#else
#error "Platform not supported"
#endif
} contextTy;

typedef struct coroutineTy {
  contextTy context;
  struct coroutineTy *prev;
  void *stack;
  coroutineProcTy *entryPoint;
  void *arg;
  coroutineCbTy *finishCb;
  void *finishArg;
  int finished;
  int counter;
} coroutineTy;

static __thread coroutineTy *mainCoroutine;
static __thread coroutineTy *currentCoroutine;

void switchContext(contextTy *from, contextTy *to);
void x86InitFPU(contextTy *context);

static void fiberEntryPoint(coroutineTy *coroutine)
{
  coroutine->entryPoint(coroutine->arg);
  coroutine->finished = 1;
  __sync_fetch_and_add(&coroutine->counter, -1);
  currentCoroutine = currentCoroutine->prev;
  assert(currentCoroutine && "Try exit from main coroutine");
  switchContext(&coroutine->context, &currentCoroutine->context);
}

static int fiberInit(coroutineTy *coroutine, size_t stackSize)
{
#ifdef __i386__
  // x86 arch
  // EIP = fiberEntryPoint
  // ESP = stack + stackSize - 4
  // [ESP] = coroutine
  if (posix_memalign(&coroutine->stack, 16, stackSize) == 0) {
    uintptr_t *esp = ((uintptr_t*)coroutine->stack) + (stackSize - 4)/sizeof(uintptr_t);
    *esp = (uintptr_t)coroutine;
    coroutine->context.registers[CTX_EIP_INDEX] = (uintptr_t)fiberEntryPoint;
    coroutine->context.registers[CTX_ESP_INDEX] = (uintptr_t)esp;
    x86InitFPU(&coroutine->context);
    return 1;
  } else {
    return 0;
  }
#elif __x86_64__
  // x86_64 arch
  // RIP = fiberEntryPoint
  // RSP = stack + stackSize - 128 - 16
  // RDI = coroutine
  if (posix_memalign(&coroutine->stack, 32, stackSize) == 0) {
    uintptr_t *rsp = ((uintptr_t*)coroutine->stack) + (stackSize - 128 - 8)/sizeof(uintptr_t);
    coroutine->context.registers[CTX_RIP_INDEX] = (uintptr_t)fiberEntryPoint;
    coroutine->context.registers[CTX_RSP_INDEX] = (uintptr_t)rsp;
    x86InitFPU(&coroutine->context);
    return 1;
  } else {
    return 0;
  }
#else
#error "Platform not supported"
#endif
}

int coroutineIsMain()
{
  return !currentCoroutine || currentCoroutine->prev == 0;
}

coroutineTy *coroutineCurrent()
{
  return currentCoroutine;
}

int coroutineFinished(coroutineTy *coroutine)
{
  return coroutine->finished;
}

/// coroutineNew - create coroutine
coroutineTy *coroutineNew(coroutineProcTy entry, void *arg, unsigned stackSize)
{
  sigset_t old;
  sigset_t all;
  sigfillset(&all);
  sigprocmask(SIG_SETMASK, &all, &old);
  coroutineTy *result = 0;

  // Create main fiber if it not exists
  if (currentCoroutine == 0)
    mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);

  coroutineTy *coroutine;
  if (posix_memalign((void**)&coroutine, 8, sizeof(coroutineTy)) == 0) {
    if (fiberInit(coroutine, stackSize)) {
      coroutine->entryPoint = entry;
      coroutine->arg = arg;
      coroutine->prev = currentCoroutine;
      coroutine->finished = 0;
      coroutine->counter = 0;
      coroutine->finishCb = 0;
      coroutine->finishArg = 0;
      sigprocmask(SIG_SETMASK, &old, 0);
      return coroutine;
    }
  } else {
    return 0;
  }

  sigprocmask(SIG_SETMASK, &old, 0);
  return result;
}

coroutineTy *coroutineNewWithCb(coroutineProcTy entry, void *arg, unsigned stackSize, coroutineCbTy finishCb, void *finishArg)
{
  coroutineTy *coroutine = coroutineNew(entry, arg, stackSize);
  coroutine->finishCb = finishCb;
  coroutine->finishArg = finishArg;
  return coroutine;
}

void coroutineDelete(coroutineTy *coroutine)
{
  free(coroutine->stack);
  free(coroutine);
}

int coroutineCall(coroutineTy *coroutine)
{
  if (!coroutineFinished(coroutine)) {
    if (__sync_fetch_and_add(&coroutine->counter, 2) != 0) {
      // Don't call active coroutine
      return 1;
    }

    // Create main fiber if it not exists
    if (currentCoroutine == 0)
      mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);

    do {
      coroutine->prev = currentCoroutine;
      currentCoroutine = coroutine;
      switchContext(&coroutine->prev->context, &coroutine->context);
    } while (__sync_fetch_and_add(&coroutine->counter, -1) != 1);

    int finished = coroutine->finished;
    if (finished) {
      coroutineCbTy *finishCb = coroutine->finishCb;
      void *finishArg = coroutine->finishArg;
      free(coroutine->stack);
      free(coroutine);
      if (finishCb)
        finishCb(finishArg);
    }

    return finished;
  } else {
    return 1;
  }
}

void coroutineYield()
{
  if (currentCoroutine && currentCoroutine->prev) {
    coroutineTy *old = currentCoroutine;
    unsigned counter = __sync_fetch_and_add(&old->counter, -1);
    assert(counter >= 2 && "Double yield detected");
    if (counter != 2) {
      // Other thread tried call this coroutine before
      __sync_fetch_and_add(&old->counter, -1);
      return;
    }


    currentCoroutine = currentCoroutine->prev;
    switchContext(&old->context, &currentCoroutine->context);
  }
}
