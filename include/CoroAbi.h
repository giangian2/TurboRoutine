#ifndef COROABI_H
#define COROABI_H

/*
 * Constants shared by the C side (include/Coro.h, src/coro.c) and the
 * context switch (src/coro_x86_64.S).
 *
 * Plain #defines and nothing else: this header is also run through the
 * preprocessor for an assembly file, which knows nothing about C types,
 * casts or suffixes. The C side _Static_asserts every offset below against
 * the real TurboCo struct, so the two can never drift apart silently.
 */

/*
 * Slot geometry. Every coroutine owns one slot of CO_SLOT_SIZE bytes, and
 * every slot starts at a multiple of CO_SLOT_SIZE. Setting the low
 * CO_SLOT_SHIFT bits of any address inside a slot therefore yields the
 * slot's last byte, and the header sits right below it:
 *
 *     header = (rsp | CO_SLOT_MASK) - (CO_HDR_SIZE - 1)
 *
 * This is the trick the Linux kernel used to find the current task from
 * its stack pointer. It costs two instructions, touches no memory and
 * does not depend on which thread the coroutine is running on.
 */
#define CO_SLOT_SHIFT 16
#define CO_SLOT_SIZE 0x10000
#define CO_SLOT_MASK 0xFFFF

/* Lowest page of every slot: PROT_NONE, so a stack overflow faults. */
#define CO_GUARD_SIZE 0x1000

/* ---- TurboCo header: size and field offsets --------------------------- */

#define CO_HDR_SIZE 64
#define CO_OFF_CO_SP 0     /* coroutine stack pointer while suspended     */
#define CO_OFF_WORKER_SP 8 /* stack pointer of whoever resumed it         */
#define CO_OFF_BUDGET 16   /* iterations left before co_checkpoint yields */
#define CO_OFF_STATE 24    /* one of CO_STATE_*                           */

/* ---- coroutine states -------------------------------------------------- */

#define CO_STATE_READY 0   /* created or suspended, may be resumed        */
#define CO_STATE_RUNNING 1 /* some thread is executing on its stack       */
#define CO_STATE_WAITING 2 /* parked on an event, must not be resumed     */
#define CO_STATE_DONE 3    /* entry function returned; set by co_entry    */

/*
 * Floating point control state a fresh coroutine starts with: MXCSR in
 * the low 4 bytes (0x1F80: round to nearest, all SSE exceptions masked),
 * x87 control word in bytes 4-5 (0x037F: the same, for the x87 FPU). These
 * are the values every C program starts with.
 */
#define CO_FPU_DEFAULT 0x037F00001F80

#endif /* COROABI_H */
