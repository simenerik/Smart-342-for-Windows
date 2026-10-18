/* coro.h - minimal coroutines (one host stack per emulated Mac process).
 * Exactly one coroutine runs at a time, so cooperative multitasking stays deterministic.
 * POSIX: ucontext; Windows: fibers. */
#ifndef CORO_H
#define CORO_H
#include <stddef.h>

typedef struct Coro Coro;
Coro *coro_self_main(void);                                   /* wrap the calling thread */
Coro *coro_new(void (*fn)(void *), void *arg, size_t stack);  /* not started until switched to */
void coro_switch(Coro *from, Coro *to);

#endif
