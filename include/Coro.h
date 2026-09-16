#ifndef CORO_H
#define CORO_H

#include "CoroAbi.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Stackful coroutines for x86-64 Linux, with a hand-written context switch.
 *
 * Each coroutine runs on its own stack, so a plain C function running on it
 * can be suspended at any call depth with every local variable intact, and
 * resumed later by ANY thread: the stack is ordinary memory shared by the
 * whole process, and a switch reloads every register the ABI requires.
 *
 * Stacks live in a CoPool: one contiguous mapping split into fixed slots of
 * CO_SLOT_SIZE bytes, each starting at a multiple of CO_SLOT_SIZE:
 *
 *   high  +---------------------------+  slot base + CO_SLOT_SIZE
 *         | TurboCo header (64 bytes) |
 *         +---------------------------+  <- initial stack pointer
 *         | stack, grows downward     |
 *         |           ...             |
 *         +---------------------------+  slot base + CO_GUARD_SIZE
 *         | guard page, PROT_NONE     |
 *   low   +---------------------------+  slot base
 *
 * The header sits at the top, not at the base: a stack overflow runs into
 * the guard page and faults immediately, instead of silently overwriting
 * the header first.
 *
 * Only touched pages consume memory (the mapping is MAP_NORESERVE), so a
 * coroutine that uses 3 KB of stack costs one page, not 64 KB.
 */

typedef enum
{
    CO_OK        = 0,
    CO_ERR_ARG   = -1, /* invalid argument                  */
    CO_ERR_ALLOC = -2  /* out of memory / mapping refused   */
} CO_ERROR_CODES;

/*
 * Entry function of a coroutine. `arg` is the pointer given to co_create();
 * `value` is what the first co_resume() passed. The return value is handed
 * to the co_resume() call that observes the coroutine finishing.
 */
typedef uint64_t (*CoFn)(void* arg, uint64_t value);

/*
 * Per-coroutine header, the last CO_HDR_SIZE bytes of its slot. The context
 * switch reads and writes the first fields by offset (see CoroAbi.h); the
 * assertions below keep the struct and those offsets in lockstep.
 *
 * `state` is owned by the scheduler: co_resume/co_yield never touch it.
 * The one exception is co_entry, which sets CO_STATE_DONE when the entry
 * function returns.
 */
typedef struct TurboCo
{
    void*    co_sp;     /* stack pointer while suspended              */
    void*    worker_sp; /* stack pointer of whoever resumed it        */
    int64_t  budget;    /* iterations left before co_checkpoint yields */
    uint32_t state;     /* CO_STATE_*                                 */
    uint32_t slot;      /* index of this coroutine's slot in its pool */
    void*    user[4];   /* free for the scheduler: arena, event, ...  */
} TurboCo;

_Static_assert(sizeof(TurboCo) == CO_HDR_SIZE, "TurboCo size out of sync with CoroAbi.h");
_Static_assert(offsetof(TurboCo, co_sp) == CO_OFF_CO_SP, "co_sp offset");
_Static_assert(offsetof(TurboCo, worker_sp) == CO_OFF_WORKER_SP, "worker_sp offset");
_Static_assert(offsetof(TurboCo, budget) == CO_OFF_BUDGET, "budget offset");
_Static_assert(offsetof(TurboCo, state) == CO_OFF_STATE, "state offset");
_Static_assert(CO_SLOT_SIZE == (1 << CO_SLOT_SHIFT), "slot size");
_Static_assert(CO_SLOT_MASK == CO_SLOT_SIZE - 1, "slot mask");

typedef struct
{
    char*           base; /* slot 0; a multiple of CO_SLOT_SIZE       */
    size_t          nslots;
    uint32_t*       free_slots; /* stack of free slot indices               */
    size_t          free_count;
    unsigned*       vg_ids; /* valgrind stack ids, one per slot         */
    pthread_mutex_t lock;   /* guards free_slots/free_count only        */
} CoPool;

/* ---- pool --------------------------------------------------------------- */

/* Maps nslots slots. NULL on invalid argument or allocation failure. */
CoPool* co_pool_create(size_t nslots);
void    co_pool_free(CoPool* pool);

/* ---- lifecycle ------------------------------------------------------------ */

/*
 * Takes a free slot and prepares a coroutine that will run fn(arg, value)
 * on its first co_resume(). Thread-safe. NULL if the pool is full or an
 * argument is invalid. The coroutine starts in CO_STATE_READY.
 */
TurboCo* co_create(CoPool* pool, CoFn fn, void* arg);

/*
 * Gives the slot back. Thread-safe. The coroutine must not be running; it
 * does not have to be finished: a suspended coroutine is simply abandoned,
 * and whatever it had on its stack is discarded with the slot.
 */
int co_destroy(CoPool* pool, TurboCo* co);

/* ---- switching (src/coro_x86_64.S) ---------------------------------------- */

/*
 * Runs `co` until it yields or finishes. `value` becomes the return value
 * of the co_yield() it is suspended in (or the entry function's `value`
 * argument on the first resume). Returns what the coroutine yields, or the
 * entry function's return value; tell the two apart with co->state.
 *
 * Preconditions: co is not DONE and no other thread is running it. A
 * scheduler must therefore make a yielded coroutine visible to other
 * threads only AFTER co_resume has returned, never from inside it.
 */
uint64_t co_resume(TurboCo* co, uint64_t value);

/*
 * Suspends the calling coroutine and returns `value` to its resumer. Returns
 * the value passed to the co_resume() that wakes it up. Callable at any
 * depth, but only on a coroutine stack.
 */
uint64_t co_yield (uint64_t value);

/* First landing of a fresh coroutine. Not callable: exported for co_create. */
void co_entry(void);

/* ---- inline helpers ------------------------------------------------------- */

/*
 * Header of the running coroutine, from the stack pointer alone: no thread-
 * local storage, so the answer stays right after the coroutine migrates to
 * another thread, and the compiler may hoist it out of a loop. Only
 * meaningful on a coroutine stack; see co_pool_contains().
 */
static inline TurboCo* co_current(void)
{
    register uintptr_t sp __asm__("rsp");
    return (TurboCo*)((sp | CO_SLOT_MASK) - (CO_HDR_SIZE - 1));
}

/* True when the caller is running on one of this pool's coroutine stacks. */
static inline bool co_pool_contains(const CoPool* pool)
{
    register uintptr_t sp __asm__("rsp");
    return sp - (uintptr_t)pool->base < pool->nslots * (uintptr_t)CO_SLOT_SIZE;
}

/* Iterations between two cooperative yields; override with -DCO_BUDGET=n. */
#ifndef CO_BUDGET
#define CO_BUDGET 1024
#endif

/*
 * Cooperative scheduling point: place it once at the top of every unbounded
 * loop. Almost always a decrement and a not-taken branch; once every
 * CO_BUDGET calls it refills the budget and yields 0 to the resumer, which
 * decides whether to resume at once or run something else first.
 */
static inline void co_checkpoint(void)
{
    TurboCo* co = co_current();
    if (__builtin_expect(--co->budget <= 0, 0))
    {
        co->budget = CO_BUDGET;
        (void)co_yield (0);
    }
}

#endif /* CORO_H */
