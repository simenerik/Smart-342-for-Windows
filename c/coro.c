/* coro.c - see coro.h */
#include "coro.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
struct Coro { void *fiber; void (*fn)(void *); void *arg; };
static void WINAPI trampoline(void *p) { Coro *c = p; c->fn(c->arg); fprintf(stderr, "coroutine returned\n"); exit(3); }
Coro *coro_self_main(void) {
    Coro *c = calloc(1, sizeof *c);
    c->fiber = ConvertThreadToFiber(NULL);
    if (!c->fiber) c->fiber = GetCurrentFiber();
    return c;
}
Coro *coro_new(void (*fn)(void *), void *arg, size_t stack) {
    Coro *c = calloc(1, sizeof *c); c->fn = fn; c->arg = arg;
    c->fiber = CreateFiberEx(0, stack, FIBER_FLAG_FLOAT_SWITCH, trampoline, c);
    if (!c->fiber) { fprintf(stderr, "CreateFiber failed\n"); exit(3); }
    return c;
}
void coro_switch(Coro *from, Coro *to) { (void)from; SwitchToFiber(to->fiber); }
#else
#include <ucontext.h>
struct Coro { ucontext_t ctx; void (*fn)(void *); void *arg; void *stack; };
static void trampoline(unsigned hi, unsigned lo) {
    Coro *c = (Coro *)(((uintptr_t)hi << 32) | (uintptr_t)lo);
    c->fn(c->arg); fprintf(stderr, "coroutine returned\n"); exit(3);
}
Coro *coro_self_main(void) { Coro *c = calloc(1, sizeof *c); getcontext(&c->ctx); return c; }
Coro *coro_new(void (*fn)(void *), void *arg, size_t stack) {
    Coro *c = calloc(1, sizeof *c); c->fn = fn; c->arg = arg; c->stack = malloc(stack);
    if (!c->stack) { fprintf(stderr, "coroutine stack allocation failed\n"); exit(3); }
    getcontext(&c->ctx); c->ctx.uc_stack.ss_sp = c->stack; c->ctx.uc_stack.ss_size = stack; c->ctx.uc_link = NULL;
    uintptr_t p = (uintptr_t)c;
    makecontext(&c->ctx, (void (*)(void))trampoline, 2, (unsigned)(p >> 32), (unsigned)(p & 0xFFFFFFFFu));
    return c;
}
void coro_switch(Coro *from, Coro *to) { swapcontext(&from->ctx, &to->ctx); }
#endif
