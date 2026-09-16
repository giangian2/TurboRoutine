/*
 * Demo and self-check of the coroutine library. Four independent parts,
 * each printing what it verified; the exit status is non-zero if any check
 * fails, so `make run` doubles as a smoke test.
 *
 *   1. generator   values flow both ways through co_yield/co_resume
 *   2. slicing     co_checkpoint splits a long loop into budgeted slices
 *   3. migration   many coroutines on a worker pool, resumed by any thread
 *   4. cost        nanoseconds per context switch
 */
#define _GNU_SOURCE /* gettid(), clock_gettime() */

#include "../include/Coro.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

/* ---- 1. generator -------------------------------------------------------- */

#define GEN_COUNT 5

/* Yields the first n squares, then returns their count. */
static uint64_t squares(void* arg, uint64_t first)
{
    uint64_t n = (uint64_t)(uintptr_t)arg;
    printf("  generator started, first resume passed %lu\n", (unsigned long)first);
    for (uint64_t i = 0; i < n; i++)
        (void)co_yield (i * i);
    return n;
}

static int demo_generator(CoPool* pool)
{
    puts("1. generator");

    TurboCo* co = co_create(pool, squares, (void*)(uintptr_t)GEN_COUNT);
    if (!co)
        return 1;

    int      ok = 1;
    uint64_t v  = co_resume(co, 42);
    for (uint64_t i = 0; co->state != CO_STATE_DONE; i++)
    {
        printf("  resumer got %lu\n", (unsigned long)v);
        ok &= v == i * i;
        v = co_resume(co, 0);
    }
    ok &= v == GEN_COUNT;
    printf("  generator returned %lu -- %s\n", (unsigned long)v, ok ? "ok" : "FAILED");

    co_destroy(pool, co);
    return !ok;
}

/* ---- 2. slicing ---------------------------------------------------------- */

#define SLICE_ITERATIONS 100000

/* A long loop with one checkpoint: the only change a pure algorithm needs. */
static uint64_t long_sum(void* arg, uint64_t unused)
{
    (void)arg;
    (void)unused;
    uint64_t sum = 0;
    for (uint64_t i = 1; i <= SLICE_ITERATIONS; i++)
    {
        co_checkpoint();
        sum += i;
    }
    return sum;
}

static int demo_slicing(CoPool* pool)
{
    puts("2. slicing");

    TurboCo* co = co_create(pool, long_sum, NULL);
    if (!co)
        return 1;

    uint64_t sum    = 0;
    int      slices = 0;
    do
    {
        sum = co_resume(co, 0); /* one slice: returns at the next checkpoint */
        slices++;
    } while (co->state != CO_STATE_DONE);

    uint64_t expect = (uint64_t)SLICE_ITERATIONS * (SLICE_ITERATIONS + 1) / 2;
    int      ok     = sum == expect && slices == SLICE_ITERATIONS / CO_BUDGET + 1;
    printf("  %d iterations, budget %d: %d slices, sum %lu -- %s\n", SLICE_ITERATIONS, CO_BUDGET,
           slices, (unsigned long)sum, ok ? "ok" : "FAILED");

    co_destroy(pool, co);
    return !ok;
}

/* ---- 3. migration on a worker pool --------------------------------------- */

#define JOBS 256
#define WORKERS 4
#define JOB_ITERATIONS 50000

typedef struct
{
    uint64_t sum;
    double   fp;           /* floating point value carried across yields */
    int      threads_seen; /* how many times the executing thread changed */
    pid_t    last_tid;
} Job;

static uint64_t job_run(void* arg, uint64_t unused)
{
    (void)unused;
    Job*   job = arg;
    double x   = 0.5; /* lives in an xmm register or on the coroutine stack */
    for (uint64_t i = 1; i <= JOB_ITERATIONS; i++)
    {
        job->sum += i;
        x = x * 1.0000001 + 1e-9;

        /* A system call per iteration would dwarf the work: sampling the
         * thread once per budget is enough to observe every migration. */
        if ((i & (CO_BUDGET - 1)) == 0)
        {
            pid_t tid = gettid();
            if (tid != job->last_tid)
            {
                job->threads_seen++;
                job->last_tid = tid;
            }
        }
        co_checkpoint();
    }
    job->fp = x;
    return 0;
}

/* Shared run queue: a fixed ring of JOBS pointers behind one mutex. */
typedef struct
{
    TurboCo*        ring[JOBS];
    size_t          head, count, finished;
    pthread_mutex_t lock;
    pthread_cond_t  ready;
} RunQueue;

/* Caller holds q->lock. Never full: each coroutine is in the ring at most once. */
static void runq_push(RunQueue* q, TurboCo* co)
{
    q->ring[(q->head + q->count) % JOBS] = co;
    q->count++;
    pthread_cond_signal(&q->ready);
}

static void* worker_main(void* arg)
{
    RunQueue* q = arg;
    for (;;)
    {
        pthread_mutex_lock(&q->lock);
        while (q->count == 0 && q->finished < JOBS)
            pthread_cond_wait(&q->ready, &q->lock);
        if (q->finished == JOBS)
        {
            pthread_mutex_unlock(&q->lock);
            return NULL;
        }
        TurboCo* co = q->ring[q->head];
        q->head     = (q->head + 1) % JOBS;
        q->count--;
        pthread_mutex_unlock(&q->lock);

        co->state = CO_STATE_RUNNING;
        (void)co_resume(co, 0); /* returns at the coroutine's next checkpoint */

        /* Back on the WORKER stack: only now may another thread see `co`.
         * Pushing it from inside the coroutine, before its co_yield had
         * finished saving, would let a second worker resume a stack that
         * is still executing here. */
        pthread_mutex_lock(&q->lock);
        if (co->state == CO_STATE_DONE)
        {
            if (++q->finished == JOBS)
                pthread_cond_broadcast(&q->ready);
        }
        else
        {
            co->state = CO_STATE_READY;
            runq_push(q, co);
        }
        pthread_mutex_unlock(&q->lock);
    }
}

static int demo_migration(CoPool* pool)
{
    printf("3. migration: %d coroutines on %d worker threads\n", JOBS, WORKERS);

    static Job      jobs[JOBS];
    static TurboCo* cos[JOBS];
    static RunQueue q = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

    for (int i = 0; i < JOBS; i++)
    {
        cos[i] = co_create(pool, job_run, &jobs[i]);
        if (!cos[i])
            return 1;
        runq_push(&q, cos[i]); /* no worker running yet: no lock needed */
    }

    pthread_t threads[WORKERS];
    for (int i = 0; i < WORKERS; i++)
        pthread_create(&threads[i], NULL, worker_main, &q);
    for (int i = 0; i < WORKERS; i++)
        pthread_join(threads[i], NULL);

    /* Reference result, computed without any coroutine. */
    double ref = 0.5;
    for (int i = 1; i <= JOB_ITERATIONS; i++)
        ref = ref * 1.0000001 + 1e-9;
    uint64_t expect = (uint64_t)JOB_ITERATIONS * (JOB_ITERATIONS + 1) / 2;

    int ok = 1, migrated = 0;
    for (int i = 0; i < JOBS; i++)
    {
        ok &= jobs[i].sum == expect && jobs[i].fp == ref;
        migrated += jobs[i].threads_seen > 1;
        co_destroy(pool, cos[i]);
    }
    printf("  results match a plain loop: %s; resumed by more than one thread: %d/%d\n",
           ok ? "ok" : "FAILED", migrated, JOBS);
    return !ok;
}

/* ---- 4. switch cost ------------------------------------------------------ */

#define ROUND_TRIPS 2000000L

static uint64_t echo_plus_one(void* arg, uint64_t v)
{
    (void)arg;
    for (;;)
        v = co_yield (v + 1);
    return 0; /* unreachable */
}

static int demo_cost(CoPool* pool)
{
    puts("4. cost");

    TurboCo* co = co_create(pool, echo_plus_one, NULL);
    if (!co)
        return 1;

    struct timespec start, end;
    uint64_t        v = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (long i = 0; i < ROUND_TRIPS; i++)
        v = co_resume(co, v);
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ns =
        ((double)(end.tv_sec - start.tv_sec) * 1e9 + (double)(end.tv_nsec - start.tv_nsec)) /
        ROUND_TRIPS;
    int ok = v == (uint64_t)ROUND_TRIPS;
    printf("  %ld round trips: %.2f ns each, %.2f ns per switch -- %s\n", ROUND_TRIPS, ns, ns / 2,
           ok ? "ok" : "FAILED");

    co_destroy(pool, co); /* abandoned mid-loop: releasing the slot is enough */
    return !ok;
}

int main(void)
{
    CoPool* pool = co_pool_create(JOBS + 1);
    if (!pool)
    {
        fprintf(stderr, "coroutine pool creation failed\n");
        return 1;
    }

    int failures = 0;
    failures += demo_generator(pool);
    failures += demo_slicing(pool);
    failures += demo_migration(pool);
    failures += demo_cost(pool);

    co_pool_free(pool);
    printf("%s\n", failures ? "SOME CHECKS FAILED" : "all checks passed");
    return failures != 0;
}
