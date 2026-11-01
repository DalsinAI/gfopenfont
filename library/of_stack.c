/* of_call_on_stack: gfopenfont.library does its work on a stack of its own.
 * Programs often call with 4 KB of stack, and shaping and the rasteriser
 * want more than that. Every call into the core holds
 * the library's lock, so one stack serves them all.
 *
 * The stack the caller passes, as the routine reads it after saving six
 * registers (24 bytes): 24(sp) the return, 28(sp) sss, 32(sp) fn, 36(sp) a,
 * 40(sp) b. StackSwap changes only the stack pointer and the task's bounds;
 * d2-d4/a2-a3 survive it and the call, as the C convention keeps them.
 *
 * Copyright (c) 2026 AmiCradle Team. OpenFont, MIT licence (LICENSE). */
#include "of_runtime.h"

__asm(
    "\t.text\n"
    "\t.even\n"
    "\t.globl _of_call_on_stack\n"
    "_of_call_on_stack:\n"
    "\tmovem.l d2-d4/a2-a3/a6,-(sp)\n"
    "\tmove.l 28(sp),a2\n"          /* sss */
    "\tmove.l 32(sp),a3\n"          /* fn */
    "\tmove.l 36(sp),d2\n"          /* a */
    "\tmove.l 40(sp),d3\n"          /* b */
    "\tmove.l 4.w,a6\n"
    "\tmove.l a2,a0\n"
    "\tjsr -732(a6)\n"              /* StackSwap: on the library's stack now */
    "\tmove.l d3,-(sp)\n"
    "\tmove.l d2,-(sp)\n"
    "\tjsr (a3)\n"
    "\taddq.l #8,sp\n"
    "\tmove.l d0,d4\n"
    "\tmove.l 4.w,a6\n"
    "\tmove.l a2,a0\n"
    "\tjsr -732(a6)\n"              /* StackSwap: back on the caller's */
    "\tmove.l d4,d0\n"
    "\tmovem.l (sp)+,d2-d4/a2-a3/a6\n"
    "\trts\n");
