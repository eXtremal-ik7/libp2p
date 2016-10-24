#undef _FORTIFY_SOURCE
#include <setjmp.h>
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

typedef struct fiberContextTy {
  coroutineTy *coroutine;
  coroutineProcTy *proc;
  void *arg;
} fiberContextTy;

__thread coroutineTy *currentCoroutine = 0;
__thread char cleanupFiberStack[4096];

static void fiberDestroy()
{
  // free stack & jump caller fiber
  free(currentCoroutine->stack);
  currentCoroutine->stack = 0; 
  
  currentCoroutine = currentCoroutine->prev;
  _longjmp(currentCoroutine->context, 1);
}

static void fiberStart(void *arg)
{
  fiberContextTy fiberArg = *((fiberContextTy*)arg);
  free(arg);
  
  if (_setjmp(fiberArg.coroutine->context) != 0) {
    fiberArg.proc(fiberArg.arg); 
    
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
    fiberContextTy *fiberArg = malloc(sizeof(fiberContextTy));
    fiberArg->coroutine = coroutine;
    fiberArg->proc = entry;
    fiberArg->arg = arg;
    coroutine->initialContext.uc_link = &callerCtx;
    makecontext(&coroutine->initialContext,
                (void(*)())fiberStart,
                sizeof(void*)/sizeof(int),
                fiberArg);
    swapcontext(&callerCtx, &coroutine->initialContext);
  }
  
  return coroutine;
}

void coroutineDelete(coroutineTy *coroutine)
{
  if (coroutine->stack)
    free(coroutine->stack);
  free(coroutine);
}

void coroutineCall(coroutineTy *coroutine)
{
  if (!coroutineFinished(coroutine)) {
    coroutine->prev = currentCoroutine;
    currentCoroutine = coroutine;
    if (_setjmp(coroutine->prev->context) == 0)
      _longjmp(coroutine->context, 1);
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
