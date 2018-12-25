#include "asyncio/coroutine.h"
#include "asyncio/asyncioTypes.h"
#include "atomic.h"
#include <windows.h>
#include <assert.h>

__tls coroutineTy *currentCoroutine;
__tls coroutineTy *mainCoroutine;

typedef struct coroutineTy {
  struct coroutineTy *prev;
  LPVOID fiber;
  coroutineProcTy *entryPoint;
  void *arg;
  int finished;
  unsigned counter;
  coroutineCbTy *cbProc;
  void *cbArg;
} coroutineTy;

static VOID __stdcall fiberEntryPoint(LPVOID lpParameter)
{
  coroutineTy *coro = (coroutineTy*)lpParameter;
  coro->entryPoint(coro->arg);
  coro->finished = 1;
  currentCoroutine = coro->prev;
  SwitchToFiber(currentCoroutine->fiber);
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

coroutineTy *coroutineNew(coroutineProcTy entry, void *arg, unsigned stackSize)
{
  coroutineTy *coroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
  coroutine->fiber = CreateFiber(stackSize, fiberEntryPoint, coroutine);
  coroutine->entryPoint = entry;
  coroutine->arg = arg;
  return coroutine;
}

void coroutineDelete(coroutineTy *coroutine)
{
  DeleteFiber(coroutine->fiber);
  free(coroutine);
}

int coroutineCall(coroutineTy *coroutine)
{
  assert(__uint_atomic_fetch_and_add(&coroutine->counter, 1) == 0 &&
         "Call coroutine from multiple threads detected!");

  if (!currentCoroutine) {
    mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
    currentCoroutine->fiber = ConvertThreadToFiber(0);    
  }
  
  coroutine->prev = currentCoroutine;
  currentCoroutine = coroutine;
  SwitchToFiber(coroutine->fiber);

  if (coroutine->cbProc) {
    coroutine->cbProc(coroutine->cbArg);
    coroutine->cbProc = 0;
  }

  int finished = coroutine->finished;
  if (finished) {
    DeleteFiber(coroutine->fiber);
    free(coroutine);
  }
  
  return finished;
}

void coroutineYield()
{
  if (currentCoroutine->prev) {
    coroutineTy *old = currentCoroutine;
    currentCoroutine = currentCoroutine->prev;
    assert(__uint_atomic_fetch_and_add(&old->counter, -1) == 1 &&
           "Multiple yield from one coroutine detected!");
    SwitchToFiber(currentCoroutine->fiber);
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
