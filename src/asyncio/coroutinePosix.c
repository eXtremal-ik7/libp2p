#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include "asyncio/coroutine.h"
#include "config.h"

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
  int finished;
  int counter;
  coroutineCbTy *cbProc;
  void *cbArg;
} coroutineTy;

static __thread coroutineTy *mainCoroutine;
static __thread coroutineTy *currentCoroutine;

void switchContext(contextTy *from, contextTy *to);
void x86InitFPU(contextTy *context);

static void fiberEntryPoint(coroutineTy *coroutine)
{
  coroutine->entryPoint(coroutine->arg);
  coroutine->finished = 1;
  currentCoroutine = currentCoroutine->prev;
  switchContext(&coroutine->context, &currentCoroutine->context);
}

static void fiberInit(coroutineTy *coroutine, size_t stackSize)
{
#ifdef __i386__
  // x86 arch
  // EIP = fiberEntryPoint
  // ESP = stack + stackSize - 4
  // [ESP] = coroutine
  posix_memalign(&coroutine->stack, 16, stackSize);
  uintptr_t *esp = ((uintptr_t*)coroutine->stack) + (stackSize - 4)/sizeof(uintptr_t);
  *esp = (uintptr_t)coroutine;
  coroutine->context.registers[CTX_EIP_INDEX] = (uintptr_t)fiberEntryPoint;
  coroutine->context.registers[CTX_ESP_INDEX] = (uintptr_t)esp;
  x86InitFPU(&coroutine->context);
#elif __x86_64__
  // x86_64 arch
  // RIP = fiberEntryPoint
  // RSP = stack + stackSize - 128 - 16
  // RDI = coroutine
  posix_memalign(&coroutine->stack, 32, stackSize);
  uintptr_t *rsp = ((uintptr_t*)coroutine->stack) + (stackSize - 128 - 8)/sizeof(uintptr_t);
  coroutine->context.registers[CTX_RIP_INDEX] = (uintptr_t)fiberEntryPoint;
  coroutine->context.registers[CTX_RSP_INDEX] = (uintptr_t)rsp;
  x86InitFPU(&coroutine->context);
#else
#error "Platform not supported"
#endif
}

int coroutineIsMain()
{
  return currentCoroutine == mainCoroutine;
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
  
  // Create main fiber if it not exists
  if (currentCoroutine == 0)
    mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
  
  coroutineTy *coroutine;
  posix_memalign((void**)&coroutine, 8, sizeof(coroutineTy));
  fiberInit(coroutine, stackSize);
  coroutine->entryPoint = entry;
  coroutine->arg = arg;
  coroutine->prev = currentCoroutine;
  coroutine->finished = 0;
  coroutine->counter = 0;
  coroutine->cbProc = 0;
  coroutine->cbArg = 0;
  sigprocmask(SIG_SETMASK, &old, 0);  
  return coroutine;
}

void coroutineDelete(coroutineTy *coroutine)
{
  free(coroutine->stack);
  free(coroutine);
}

int coroutineCall(coroutineTy *coroutine)
{
  assert(__sync_fetch_and_add(&coroutine->counter, 1) == 0 &&
         "Call coroutine from multiple threads detected!");
  if (!coroutineFinished(coroutine)) {
    // Create main fiber if it not exists
    if (currentCoroutine == 0)
      mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
    coroutine->prev = currentCoroutine;
    currentCoroutine = coroutine;

    switchContext(&coroutine->prev->context, &coroutine->context);

    if (coroutine->cbProc) {
      coroutine->cbProc(coroutine->cbArg);
      coroutine->cbProc = 0;
    }

    int finished = coroutine->finished;
    if (finished) {
      free(coroutine->stack);
      free(coroutine);
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
    currentCoroutine = currentCoroutine->prev;
    assert(__sync_fetch_and_add(&old->counter, -1) == 1 &&
           "Multiple yield from one coroutine detected!");
    switchContext(&old->context, &currentCoroutine->context);
  }
}

void coroutineSetYieldCallback(coroutineCbTy proc, void *arg)
{
  // Create main fiber if it not exists
  if (currentCoroutine == 0)
    mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
  currentCoroutine->cbProc = proc;
  currentCoroutine->cbArg = arg;
}
