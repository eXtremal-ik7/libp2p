#include "asyncio/coroutine.h"
#include "asyncio/asyncioTypes.h"
#include <windows.h>

__tls coroutineTy *currentCoroutine;
__tls coroutineTy *mainCoroutine;

typedef struct coroutineTy {
  struct coroutineTy *prev;
  LPVOID fiber;
  coroutineProcTy *entryPoint;
  void *arg;
  int finished;
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
  if (!currentCoroutine) {
    mainCoroutine = currentCoroutine = (coroutineTy*)calloc(sizeof(coroutineTy), 1);
    currentCoroutine->fiber = ConvertThreadToFiber(0);    
  }
  
  coroutine->prev = currentCoroutine;
  currentCoroutine = coroutine;
  SwitchToFiber(coroutine->fiber);
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
    currentCoroutine = currentCoroutine->prev;
    SwitchToFiber(currentCoroutine->fiber);
  }
}
