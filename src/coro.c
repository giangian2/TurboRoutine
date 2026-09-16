/*
 * Coroutine pool: the aligned mapping that holds every stack, slot
 * allocation, and the hand-built first frame of a new coroutine.
 */
#define _DEFAULT_SOURCE /* MAP_ANONYMOUS, MAP_NORESERVE */

#include "../include/Coro.h"
#include "../include/Debug.h"
#include <stdlib.h>
#include <sys/mman.h>

/*
 * Stack registration for valgrind. Without it memcheck sees rsp jump into
 * an mmap'd block and reports every push as an invalid write. The client
 * requests are a handful of no-op instructions outside valgrind, so they
 * stay in every build when the header is available.
 */
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#else
#define VALGRIND_STACK_REGISTER(lo, hi) ((unsigned)0)
#define VALGRIND_STACK_DEREGISTER(id) ((void)(id))
#endif

static inline char* slot_base(const CoPool* pool, size_t slot)
{
    return pool->base + slot * CO_SLOT_SIZE;
}

/* The header is the last CO_HDR_SIZE bytes of the slot. */
static inline TurboCo* slot_header(const CoPool* pool, size_t slot)
{
    return (TurboCo*)(slot_base(pool, slot) + CO_SLOT_SIZE - CO_HDR_SIZE);
}

/*
 * mmap only promises page alignment, and the rsp mask needs every slot to
 * start at a multiple of CO_SLOT_SIZE. So map one extra slot, find the first
 * aligned address inside the mapping, and give back the two ragged ends.
 * They always add up to exactly the extra slot:
 *
 *   raw                                              raw + bytes + SLOT
 *   | head |<------------- bytes, aligned ------------->| tail |
 *          base                                         base + bytes
 */
static char* map_aligned(size_t bytes)
{
    char* raw = mmap(NULL, bytes + CO_SLOT_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;

    uintptr_t base = ((uintptr_t)raw + CO_SLOT_MASK) & ~(uintptr_t)CO_SLOT_MASK;
    size_t    head = base - (uintptr_t)raw;
    size_t    tail = CO_SLOT_SIZE - head;

    if (head)
        munmap(raw, head);
    if (tail)
        munmap((char*)base + bytes, tail);
    return (char*)base;
}

CoPool* co_pool_create(size_t nslots)
{
    if (nslots == 0 || nslots > UINT32_MAX || nslots > SIZE_MAX / CO_SLOT_SIZE - 1)
    {
        LOG_ERROR("invalid slot count (nslots=%zu)", nslots);
        return NULL;
    }

    CoPool* pool = calloc(1, sizeof *pool);
    if (!pool)
    {
        LOG_ERROR("allocation failed for CoPool header");
        return NULL;
    }
    pool->nslots     = nslots;
    pool->free_slots = malloc(nslots * sizeof *pool->free_slots);
    pool->vg_ids     = malloc(nslots * sizeof *pool->vg_ids);
    pool->base       = map_aligned(nslots * CO_SLOT_SIZE);
    if (!pool->free_slots || !pool->vg_ids || !pool->base ||
        pthread_mutex_init(&pool->lock, NULL) != 0)
    {
        LOG_ERROR("allocation failed for pool of %zu slots", nslots);
        if (pool->base)
            munmap(pool->base, nslots * CO_SLOT_SIZE);
        free(pool->free_slots); /* free(NULL) is legal */
        free(pool->vg_ids);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < nslots; i++)
    {
        char* lo = slot_base(pool, i);

        /* A failure here only loses overflow detection for this slot. */
        if (mprotect(lo, CO_GUARD_SIZE, PROT_NONE) != 0)
            LOG_ERROR("guard page not installed for slot %zu", i);

        pool->vg_ids[i] = VALGRIND_STACK_REGISTER(lo + CO_GUARD_SIZE, (char*)slot_header(pool, i));

        /* Pushed in reverse so slot 0 is handed out first. */
        pool->free_slots[nslots - 1 - i] = (uint32_t)i;
    }
    pool->free_count = nslots;

    LOG_DEBUG("pool of %zu slots mapped at %p (%zu KB virtual)", nslots, (void*)pool->base,
              nslots * CO_SLOT_SIZE / 1024);
    return pool;
}

void co_pool_free(CoPool* pool)
{
    if (!pool)
        return;
    for (size_t i = 0; i < pool->nslots; i++)
        VALGRIND_STACK_DEREGISTER(pool->vg_ids[i]);
    munmap(pool->base, pool->nslots * CO_SLOT_SIZE);
    pthread_mutex_destroy(&pool->lock);
    free(pool->free_slots);
    free(pool->vg_ids);
    free(pool);
    LOG_DEBUG("pool freed");
}

TurboCo* co_create(CoPool* pool, CoFn fn, void* arg)
{
    if (!pool || !fn)
    {
        LOG_ERROR("invalid argument (pool=%p, fn=%p)", (void*)pool, (void*)(uintptr_t)fn);
        return NULL;
    }

    pthread_mutex_lock(&pool->lock);
    if (pool->free_count == 0)
    {
        pthread_mutex_unlock(&pool->lock);
        LOG_ERROR("pool full (%zu slots)", pool->nslots);
        return NULL;
    }
    uint32_t slot = pool->free_slots[--pool->free_count];
    pthread_mutex_unlock(&pool->lock);

    TurboCo* co = slot_header(pool, slot);

    /*
     * Build by hand the frame a suspended coroutine would have left, so the
     * first co_resume() needs no special case: its pops load r12 = fn and
     * r13 = arg, and its ret lands on co_entry. `*--sp = x` is a push written
     * in C. The header address is a multiple of 16, so after that ret rsp is
     * aligned exactly as the ABI wants before co_entry calls fn.
     */
    uint64_t* sp = (uint64_t*)co;
    *--sp        = (uint64_t)(uintptr_t)co_entry; /* resume address       */
    *--sp        = 0;                             /* rbp                  */
    *--sp        = 0;                             /* rbx                  */
    *--sp        = (uint64_t)(uintptr_t)fn;       /* r12                  */
    *--sp        = (uint64_t)(uintptr_t)arg;      /* r13                  */
    *--sp        = 0;                             /* r14                  */
    *--sp        = 0;                             /* r15                  */
    *--sp        = CO_FPU_DEFAULT;                /* MXCSR | x87 CW       */

    co->co_sp     = sp;
    co->worker_sp = NULL;
    co->budget    = CO_BUDGET;
    co->state     = CO_STATE_READY;
    co->slot      = slot;
    for (size_t i = 0; i < sizeof co->user / sizeof co->user[0]; i++)
        co->user[i] = NULL;

    LOG_DEBUG("coroutine created in slot %u (header=%p)", slot, (void*)co);
    return co;
}

int co_destroy(CoPool* pool, TurboCo* co)
{
    if (!pool || !co || co->slot >= pool->nslots || slot_header(pool, co->slot) != co)
    {
        LOG_ERROR("invalid argument (pool=%p, co=%p)", (void*)pool, (void*)co);
        return CO_ERR_ARG;
    }

    pthread_mutex_lock(&pool->lock);
    pool->free_slots[pool->free_count++] = co->slot;
    pthread_mutex_unlock(&pool->lock);

    LOG_DEBUG("coroutine destroyed, slot %u released", co->slot);
    return CO_OK;
}
