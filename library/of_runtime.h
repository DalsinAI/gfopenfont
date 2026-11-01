/* gfopenfont.library's own runtime: of_runtime.c and of_stack.c.
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#ifndef OF_RUNTIME_H
#define OF_RUNTIME_H

#include <exec/tasks.h>

int of_runtime_init(void);
void of_runtime_exit(void);

/* Calls fn(a, b) on the stack sss describes, through exec's StackSwap,
 * and comes back to the caller's own. sss must be set up afresh for each
 * call (its pointer at the top). */
typedef long (*OFStackFn)(void *a, void *b);
long of_call_on_stack(struct StackSwapStruct *sss, OFStackFn fn, void *a, void *b);

#endif
