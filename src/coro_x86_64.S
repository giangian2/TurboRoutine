/*
 * Context switch for x86-64, System V ABI (Linux).
 *
 * A switch is an ordinary function call. That does half of the work for
 * free:
 *
 *   - the resume address is already on the stack: the caller's `call` put
 *     it there, and the `ret` at the end of the routine is the jump into
 *     the other context;
 *   - caller-saved registers (rax, rcx, rdx, rsi, rdi, r8-r11, every xmm)
 *     need no saving: the compiler already treats them as destroyed by
 *     any call.
 *
 * What is left is the callee-saved set: rbx, rbp, r12-r15, rsp, and the
 * floating point control bits (MXCSR and the x87 control word). The six
 * registers and the control bits are pushed onto the CURRENT stack; only
 * rsp is written somewhere else, into the TurboCo header. Every suspended
 * context therefore has the same frame shape, from high to low addresses:
 *
 *     resume address      <- pushed by the call into the switch
 *     rbp
 *     rbx
 *     r12
 *     r13
 *     r14
 *     r15
 *     MXCSR | x87 CW      <- saved stack pointer points here
 *
 * co_create() in src/coro.c builds exactly this shape by hand for a
 * coroutine that has never run, with co_entry as its resume address.
 *
 * CFI: because both sides of a switch have the same frame shape, the
 * unwind rules written for the push half stay true after rsp is swapped;
 * they simply describe the other context's frame. gdb can therefore
 * backtrace through a switch at any instruction.
 *
 * CET: this file carries no GNU property note, so the linker drops the
 * IBT/SHSTK markings from the final binary and the kernel does not enable
 * the shadow stack. That is required: a shadow stack would reject every
 * `ret` into a frame that was built by hand or saved by another context.
 */

#include "CoroAbi.h"

        .intel_syntax noprefix
        .text

/* ------------------------------------------------------------------------
 * uint64_t co_resume(TurboCo* co, uint64_t value)
 *
 *   rdi = co      header of the coroutine to run
 *   rsi = value   what the coroutine's pending co_yield() returns
 *
 * Returns the value the coroutine passes to its next co_yield(), or the
 * entry function's return value when it finishes.
 * ---------------------------------------------------------------------- */
        .globl  co_resume
        .type   co_resume, @function
        .p2align 4
co_resume:
        .cfi_startproc
        /* 1. save the caller's context on the caller's stack */
        push    rbp
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset rbp, 0
        push    rbx
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset rbx, 0
        push    r12
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r12, 0
        push    r13
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r13, 0
        push    r14
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r14, 0
        push    r15
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r15, 0
        sub     rsp, 8
        .cfi_adjust_cfa_offset 8
        stmxcsr [rsp]                           /* 4 bytes at rsp   */
        fnstcw  [rsp + 4]                       /* 2 bytes at rsp+4 */

        /* 2. publish where the caller stopped, load where the coroutine stopped */
        mov     [rdi + CO_OFF_WORKER_SP], rsp   /* co->worker_sp = rsp */
        mov     rsp, [rdi + CO_OFF_CO_SP]       /* rsp = co->co_sp: THE SWITCH */

        /* 3. restore the coroutine's context from the coroutine's stack */
        ldmxcsr [rsp]
        fldcw   [rsp + 4]
        add     rsp, 8
        .cfi_adjust_cfa_offset -8
        pop     r15
        .cfi_adjust_cfa_offset -8
        .cfi_restore r15
        pop     r14
        .cfi_adjust_cfa_offset -8
        .cfi_restore r14
        pop     r13
        .cfi_adjust_cfa_offset -8
        .cfi_restore r13
        pop     r12
        .cfi_adjust_cfa_offset -8
        .cfi_restore r12
        pop     rbx
        .cfi_adjust_cfa_offset -8
        .cfi_restore rbx
        pop     rbp
        .cfi_adjust_cfa_offset -8
        .cfi_restore rbp

        /* 4. hand over the value and jump to the coroutine's resume address */
        mov     rax, rsi
        ret
        .cfi_endproc
        .size   co_resume, .-co_resume

/* ------------------------------------------------------------------------
 * uint64_t co_yield(uint64_t value)
 *
 *   rdi = value   what the resumer's co_resume() returns
 *
 * Returns the value passed to the co_resume() that resumes this coroutine.
 * Takes no header argument: it derives the header from rsp, so it works
 * at any call depth and on whatever thread the coroutine happens to be.
 * Must only be called on a coroutine stack.
 * ---------------------------------------------------------------------- */
        .globl  co_yield
        .type   co_yield, @function
        .p2align 4
co_yield:
        .cfi_startproc
        /* 1. save the coroutine's context on the coroutine's stack */
        push    rbp
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset rbp, 0
        push    rbx
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset rbx, 0
        push    r12
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r12, 0
        push    r13
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r13, 0
        push    r14
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r14, 0
        push    r15
        .cfi_adjust_cfa_offset 8
        .cfi_rel_offset r15, 0
        sub     rsp, 8
        .cfi_adjust_cfa_offset 8
        stmxcsr [rsp]
        fnstcw  [rsp + 4]

        /* 2. find the header from rsp alone */
        mov     rax, rsp
        or      rax, CO_SLOT_MASK               /* last byte of the slot */
        sub     rax, CO_HDR_SIZE - 1            /* first byte of the header */

        /* 3. publish where the coroutine stopped, load where the resumer stopped */
        mov     [rax + CO_OFF_CO_SP], rsp       /* co->co_sp = rsp */
        mov     rsp, [rax + CO_OFF_WORKER_SP]   /* rsp = co->worker_sp: THE SWITCH */

        /* 4. restore the resumer's context from the resumer's stack */
        ldmxcsr [rsp]
        fldcw   [rsp + 4]
        add     rsp, 8
        .cfi_adjust_cfa_offset -8
        pop     r15
        .cfi_adjust_cfa_offset -8
        .cfi_restore r15
        pop     r14
        .cfi_adjust_cfa_offset -8
        .cfi_restore r14
        pop     r13
        .cfi_adjust_cfa_offset -8
        .cfi_restore r13
        pop     r12
        .cfi_adjust_cfa_offset -8
        .cfi_restore r12
        pop     rbx
        .cfi_adjust_cfa_offset -8
        .cfi_restore rbx
        pop     rbp
        .cfi_adjust_cfa_offset -8
        .cfi_restore rbp

        /* 5. hand over the value and return into the resumer's co_resume() */
        mov     rax, rdi
        ret
        .cfi_endproc
        .size   co_yield, .-co_yield

/* ------------------------------------------------------------------------
 * co_entry: first instruction of every coroutine.
 *
 * Never called: reached by the `ret` of the first co_resume(), through the
 * frame co_create() built. At that point:
 *
 *   r12 = entry function    (popped from the hand-built frame)
 *   r13 = its argument      (popped from the hand-built frame)
 *   rax = value             (from the first co_resume)
 *   rsp = the header address, a multiple of 16
 *
 * r12 and r13 are callee-saved, which is why they survive the `ret` and
 * can carry the two values without a trampoline per signature.
 * ---------------------------------------------------------------------- */
        .globl  co_entry
        .type   co_entry, @function
        .p2align 4
co_entry:
        .cfi_startproc
        .cfi_undefined rip                      /* backtraces end here */
        mov     rdi, r13                        /* fn(arg, value)          */
        mov     rsi, rax
        call    r12                             /* rsp % 16 == 8 inside fn */

        mov     rdi, rax                        /* fn's result: co_resume returns it */
        mov     rax, rsp
        or      rax, CO_SLOT_MASK
        sub     rax, CO_HDR_SIZE - 1
        mov     dword ptr [rax + CO_OFF_STATE], CO_STATE_DONE
        call    co_yield                        /* back to the resumer, for good */

        ud2                                     /* resuming a DONE coroutine: trap */
        .cfi_endproc
        .size   co_entry, .-co_entry

/* Non-executable stack; see the CET note at the top of this file. */
        .section .note.GNU-stack, "", @progbits
