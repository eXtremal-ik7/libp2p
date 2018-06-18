#undef _FORTIFY_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>
#include "asyncio/coroutine.h"
#include "config.h"

typedef struct coroutineTy {
  struct coroutineTy *prev;
  void *stack;
  ucontext_t initialContext;
  jmp_buf context;
} coroutineTy;

__thread coroutineTy *currentCoroutine = 0;
__thread char cleanupFiberStack[4096];

#include <stdio.h>
void splitPtr(void *ptr, unsigned *lo, unsigned *hi)
{
  uintptr_t p = (uintptr_t)ptr;
  *lo = p & 0xFFFFFFFF;
  *hi = p >> 32;
}

void *makePtr(unsigned lo, unsigned hi)
{
  return (void*)((((intptr_t)hi) << 32) | lo);
}

static void fiberDestroy()
{
  // free stack & jump caller fiber
  free(currentCoroutine->stack);
  currentCoroutine->stack = 0; 
  
  currentCoroutine = currentCoroutine->prev;
  _longjmp(currentCoroutine->context, 1);
}

static void fiberStart(unsigned coroPtrLo, unsigned coroPtrHi, unsigned procPtrLo, unsigned procPtrHi, unsigned argPtrLo, unsigned argPtrHi)
{
  coroutineTy *coroutine = makePtr(coroPtrLo, coroPtrHi);
  coroutineProcTy *proc = makePtr(procPtrLo, procPtrHi);
  void *arg = makePtr(argPtrLo, argPtrHi);  
  
  if (_setjmp(coroutine->context) != 0) {
    proc(arg); 
    
    // change stack frame before its release
    // switch to temporary cleanup fiber    
    ucontext_t cleanupFiberCtx;
    getcontext(&cleanupFiberCtx);
    cleanupFiberCtx.uc_stack.ss_sp = cleanupFiberStack;
    cleanupFiberCtx.uc_stack.ss_size = sizeof(cleanupFiberStack);
    cleanupFiberCtx.uc_stack.ss_flags = 0;
    cleanupFiberCtx.uc_link = 0;
    makecontext(&cleanupFiberCtx, (void(*)())fiberDestroy, 0);
    setcontext(&cleanupFiberCtx);
  }
}

coroutineTy *coroutineCurrent()
{
  return currentCoroutine;
}

int coroutineFinished(coroutineTy *coroutine)
{
  return coroutine->stack == 0;
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
    currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
  
  coroutineTy *coroutine = (coroutineTy*)malloc(sizeof(coroutineTy));
  coroutine->prev = currentCoroutine;

  // Stack allocate & fill context with stack pointer
  coroutine->stack = malloc(stackSize);
  getcontext(&coroutine->initialContext);
  coroutine->initialContext.uc_stack.ss_sp = coroutine->stack;
  coroutine->initialContext.uc_stack.ss_size = stackSize;
  coroutine->initialContext.uc_stack.ss_flags = 0;
  
  {
    // Create new fiber context with 'makecontext'
    // Switch to new fiber with 'swapcontext'
    // Get point for 'longjmp' function
    // Switch back to caller fiber    
    ucontext_t callerCtx;
    coroutine->initialContext.uc_link = &callerCtx;
    
    // TODO: now only for x86_64    
    unsigned coroPtrLo, coroPtrHi, procPtrLo, procPtrHi, argPtrLo, argPtrHi;
    splitPtr(coroutine, &coroPtrLo, &coroPtrHi);
    splitPtr(entry, &procPtrLo, &procPtrHi);
    splitPtr(arg, &argPtrLo, &argPtrHi);
    makecontext(&coroutine->initialContext, (void(*)())fiberStart, 6, coroPtrLo, coroPtrHi, procPtrLo, procPtrHi, argPtrLo, argPtrHi);
    swapcontext(&callerCtx, &coroutine->initialContext);
  }
  
  sigprocmask(SIG_SETMASK, &old, 0);  
  return coroutine;
}

void coroutineDelete(coroutineTy *coroutine)
{
  if (coroutine->stack)
    free(coroutine->stack);
  free(coroutine);
}

int coroutineCall(coroutineTy *coroutine)
{
  if (!coroutineFinished(coroutine)) {
    coroutine->prev = currentCoroutine;
    currentCoroutine = coroutine;
    if (_setjmp(coroutine->prev->context) == 0)
      _longjmp(coroutine->context, 1);
    
    return coroutineFinished(coroutine);
  }
}

void coroutineYield()
{
  if (currentCoroutine && currentCoroutine->prev) {
    coroutineTy *old = currentCoroutine;
    currentCoroutine = currentCoroutine->prev;
    if (_setjmp(old->context) == 0)
      _longjmp(currentCoroutine->context, 1);
  }
}
