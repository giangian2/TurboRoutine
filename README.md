# TurboRoutine

Stackful coroutines for C on x86-64 Linux, built around a context switch
written in assembly.

TurboRoutine lets a plain, sequential C function pause at any call depth,
hand a value back to its caller and resume later on any thread, with every
local variable intact. It is the concurrency layer of
[TurboGraph](https://github.com/giangian2/TurboGraph): long graph traversals
are sliced into short pieces and scheduled on a worker pool, and the
algorithms themselves stay plain C.

```c
static uint64_t squares(void* arg, uint64_t first)
{
    for (uint64_t i = 0; i < (uint64_t)(uintptr_t)arg; i++)
        co_yield(i * i);                 /* pause here, hand i*i to the resumer */
    return 0;
}
```

---

## Features

- **Hand-written context switch.** A single assembly file, about 20
  instructions per direction. A round trip takes about 30 ns, roughly 70
  times faster than `ucontext` on the same machine.
- **Stackful.** A coroutine can suspend from any call depth, even inside a
  library function that knows nothing about coroutines. Callers do not need
  to become coroutines themselves.
- **Safe to move between threads.** The running coroutine is found from the
  stack pointer, never from thread-local storage. Any thread can resume a
  coroutine that another thread started.
- **Values in both directions.** `co_resume(co, x)` hands `x` to the
  coroutine, and `co_yield(y)` hands `y` back. That is enough for
  generators, pipelines and awaiting events.
- **Cooperative time slicing.** One `co_checkpoint()` line in a long loop
  yields every `CO_BUDGET` iterations. On the fast path it is five
  instructions.
- **Cheap memory.** Each coroutine gets a 64 KB slot of reserved address
  space. Only the pages it actually touches are backed by RAM.
- **Overflow protection.** Every stack has a guard page, so an overflow
  crashes immediately at the faulting instruction instead of silently
  corrupting a neighbor.
- **Debuggable.** Full unwind information makes `gdb` backtraces work
  through a switch at every instruction. Stacks are registered with
  valgrind, so `memcheck` and `callgrind` follow the switches.
- **No dependencies.** C11, POSIX threads, one static library.

---

## Quick start

```sh
git clone git@github.com:giangian2/TurboRoutine.git
cd TurboRoutine
make run        # builds bin/libcoro.a and runs the self-checking demo
```

A complete program:

```c
#include "Coro.h"
#include <stdio.h>

static uint64_t squares(void* arg, uint64_t first)
{
    printf("started with %lu\n", (unsigned long)first);
    for (uint64_t i = 0; i < (uint64_t)(uintptr_t)arg; i++)
        co_yield(i * i);
    return 99;
}

int main(void)
{
    CoPool*  pool = co_pool_create(16);                 /* room for 16 coroutines */
    TurboCo* co   = co_create(pool, squares, (void*)5); /* prepared, not started  */

    uint64_t v = co_resume(co, 42);                     /* runs until the first yield */
    while (co->state != CO_STATE_DONE)
    {
        printf("got %lu\n", (unsigned long)v);          /* 0 1 4 9 16 */
        v = co_resume(co, 0);
    }
    printf("returned %lu\n", (unsigned long)v);         /* 99 */

    co_destroy(pool, co);
    co_pool_free(pool);
    return 0;
}
```

```sh
gcc -O2 -Iinclude example.c -Lbin -lcoro -pthread -o example
```

---

## How it works

### Memory layout

`co_pool_create(n)` reserves one contiguous mapping and splits it into `n`
slots of 64 KB. Every slot starts at a multiple of 64 KB, and each one holds
a single coroutine:

```
high  +---------------------------+  slot base + 0x10000
      | TurboCo header, 64 bytes  |
      +---------------------------+  <- where the stack starts
      | stack, grows downward     |
      |           ...             |
      +---------------------------+  slot base + 0x1000
      | guard page, PROT_NONE     |
low   +---------------------------+  slot base
```

- **The stack grows down from just below the header.** On x86-64, `push`
  lowers `rsp` by 8 and writes, and `pop` reads and raises it by 8.
- **The header sits at the top, not at the base.** A stack overflow hits the
  guard page and faults immediately. With the header at the base, an
  overflow would overwrite it first, and the crash would appear somewhere
  unrelated much later.
- **Memory is committed lazily.** The mapping uses `MAP_NORESERVE`, so a
  coroutine that uses 3 KB of stack costs one physical page.
- **Alignment is forced by over-mapping.** `mmap` only guarantees page
  alignment, so the pool maps one extra slot and unmaps the two uneven ends.

### Finding the running coroutine

Every address inside a slot shares the same upper bits, and the last four
hex digits give the position inside the slot. Setting those 16 bits to 1
gives the slot's last byte. The header begins 63 bytes before it:

| Step | Value |
|------|-------|
| `rsp` somewhere inside a running coroutine | `0x7f3a40057df8` |
| OR with `0xFFFF` | `0x7f3a4005ffff` |
| minus 63 | `0x7f3a4005ffc0`, the header |

This is `co_current()`. It costs two instructions and touches no memory.
It gives the right answer on whichever thread happens to run the coroutine.
The Linux kernel used the same trick to find the current task from its
kernel stack pointer.

### The context switch

A switch is an ordinary function call, and that already does half of the
work:

- **The resume address is already saved.** The `call` into the switch
  pushed it, and the `ret` at the end of the switch jumps to wherever the
  other context stopped.
- **Most registers need no saving.** The System V ABI lets any function
  destroy `rax`, `rcx`, `rdx`, `rsi`, `rdi`, `r8`-`r11` and every `xmm`
  register, so the compiler never relies on them across a call.

What is left is the callee-saved set: `rbx`, `rbp`, `r12`-`r15`, `rsp`, and
the floating point control state (`MXCSR` and the x87 control word). The
switch pushes all of it onto the **current** stack, stores `rsp` in the
header and loads the other context's `rsp`. Every suspended context
therefore leaves the same frame:

```
resume address        <- pushed by the call into the switch
rbp
rbx
r12
r13
r14
r15
MXCSR | x87 CW        <- saved stack pointer points here
```

`co_resume(co, value)`, called by a worker:

| # | Instructions | Effect |
|---|--------------|--------|
| 1 | `push rbp` ... `push r15`, `sub rsp, 8`, `stmxcsr`, `fnstcw` | the worker's context is saved on the worker's stack |
| 2 | `mov [rdi + 8], rsp` | `co->worker_sp` records where the worker stopped |
| 3 | `mov rsp, [rdi]` | **the switch**: `rsp` now points into the coroutine's slot |
| 4 | `ldmxcsr`, `fldcw`, `add rsp, 8`, `pop r15` ... `pop rbp` | the coroutine's context is restored from its own stack |
| 5 | `mov rax, rsi` | `value` becomes the return value of the coroutine's pending `co_yield` |
| 6 | `ret` | jumps to the coroutine's resume address |

`co_yield(value)` mirrors it, with one difference: it takes no header
argument. It computes the header from `rsp`, saves `rsp` into
`co->co_sp`, loads `co->worker_sp`, restores the worker's registers and
returns `value` from the worker's `co_resume`. Because every `co_resume`
overwrites `worker_sp`, a coroutine always yields back to the thread that
resumed it most recently.

### Starting a coroutine

A new coroutine has never been suspended, so it has no frame to restore.
`co_create` writes one by hand, with exactly the shape that `co_resume`
expects:

| Address | Content | Read by |
|---------|---------|---------|
| `...ffb8` | address of `co_entry` | the `ret` |
| `...ffb0` | 0 | `pop rbp` |
| `...ffa8` | 0 | `pop rbx` |
| `...ffa0` | entry function | `pop r12` |
| `...ff98` | its argument | `pop r13` |
| `...ff90` | 0 | `pop r14` |
| `...ff88` | 0 | `pop r15` |
| `...ff80` | default FPU control state; `co->co_sp` points here | `ldmxcsr`, `fldcw` |

The first `co_resume` therefore needs no special case. Its `ret` lands on
`co_entry`, an assembly routine that:

1. moves `r13` into `rdi` and the resume value into `rsi`;
2. calls the function stored in `r12`, with the stack aligned to 16 bytes as
   the ABI requires;
3. when that function returns, sets `co->state = CO_STATE_DONE` and yields
   its return value to the resumer for good.

`r12` and `r13` carry the function and its argument because they are
callee-saved. They survive the `ret` untouched, so no per-signature
trampoline is needed.

### Cooperative slicing

```c
while (!queue_is_empty(q))
{
    co_checkpoint();            /* the only line an algorithm needs */
    int u = queue_dequeue(q);
    ...
}
```

`co_checkpoint()` decrements a budget stored in the header. Once every
`CO_BUDGET` calls (1024 by default, overridable with `-DCO_BUDGET=n`) it
refills the budget and calls `co_yield(0)`, and the resumer decides what
runs next. At `-O2`, GCC computes the header address once, outside the
loop. That is only valid because the header depends on the stack, not on
the thread. Inside the loop, the fast path is a load, a subtraction, a store
and a branch that is almost never taken.

### Running on a thread pool

A stack is ordinary memory shared by the whole process, and a switch
reloads every register the ABI requires. Any thread can therefore resume a
coroutine that another thread suspended. The one register a switch does not
touch is `fs`, the base of thread-local storage. After a migration,
`errno`, `pthread_self()` and every `_Thread_local` variable belong to the
new thread.

Two rules keep a pool correct:

1. **Publish a coroutine only after `co_resume` has returned.** If a
   coroutine put itself back in the run queue before yielding, a second
   worker could resume it while the first was still saving registers on
   the same stack. Re-enqueue from the worker's own stack, after the call.
2. **Hold no lock and no thread-local address across a yield.** The code may
   continue on another thread, where the mutex owner and the thread-local
   storage are different.

A minimal worker loop:

```c
for (;;)
{
    TurboCo* co = dequeue();            /* blocks on a shared run queue */
    co->state   = CO_STATE_RUNNING;
    co_resume(co, 0);                   /* returns at the next yield    */

    if (co->state == CO_STATE_DONE)
        co_destroy(pool, co);
    else
    {
        co->state = CO_STATE_READY;
        enqueue(co);                    /* only now may another thread see it */
    }
}
```

`src/main.c` contains a complete version: 256 coroutines on 4 worker threads,
each resumed by several threads, with integer and floating point results
checked against a plain loop.

---

## API

All declarations are in `include/Coro.h`.

| Function | Description |
|----------|-------------|
| `CoPool* co_pool_create(size_t n)` | Maps `n` slots. `NULL` on invalid argument or allocation failure. |
| `void co_pool_free(CoPool* pool)` | Unmaps every slot. No coroutine may be running. |
| `TurboCo* co_create(CoPool* pool, CoFn fn, void* arg)` | Takes a free slot and prepares `fn(arg, value)`. Thread-safe. `NULL` if the pool is full. |
| `int co_destroy(CoPool* pool, TurboCo* co)` | Returns the slot. Thread-safe. The coroutine may be finished or abandoned mid-way, but not running. |
| `uint64_t co_resume(TurboCo* co, uint64_t value)` | Runs `co` until it yields or returns. Returns the yielded value, or the entry function's return value. |
| `uint64_t co_yield(uint64_t value)` | Suspends the calling coroutine. Returns the `value` of the `co_resume` that wakes it. Only valid on a coroutine stack. |
| `void co_checkpoint(void)` | Inline. Yields 0 once every `CO_BUDGET` calls. |
| `TurboCo* co_current(void)` | Inline. Header of the running coroutine. Only meaningful on a coroutine stack. |
| `bool co_pool_contains(const CoPool* pool)` | Inline. Whether the caller runs on one of the pool's stacks. |

The entry function has the signature `uint64_t fn(void* arg, uint64_t value)`.
`value` is what the first `co_resume` passed.

**States.** `co->state` belongs to the scheduler. `co_resume` and `co_yield`
never change it. The only exception is `co_entry`, which sets
`CO_STATE_DONE`. The values `CO_STATE_READY`, `CO_STATE_RUNNING` and
`CO_STATE_WAITING` are available for a scheduler to use.

**Header.** `TurboCo` is 64 bytes: the two saved stack pointers, the budget,
the state, the slot index and four `user` pointers for scheduler data such
as an arena or a pending event. The assembly addresses the fields by
offset, and `_Static_assert`s in `Coro.h` stop the build if the struct and
`include/CoroAbi.h` ever disagree.

---

## Performance

Measured on an Intel Core i5-7200U under WSL2, with gcc 13.3 at `-O2`.

**Switch cost**, one round trip to the coroutine and back, over millions of
iterations:

| Mechanism | Round trip | Relative |
|-----------|-----------|----------|
| TurboRoutine | ~30 ns | 1x |
| `ucontext` (`swapcontext`) | ~2 µs | ~70x |
| two kernel threads handing off with a condition variable | ~83 µs | ~2700x |

`ucontext` saves and restores the signal mask with a system call on every
switch, and a thread hand-off goes through the kernel scheduler. System
calls are more expensive under WSL2 than on native Linux, so both
alternatives would do better on bare metal. They would still be orders of
magnitude slower. Most of TurboRoutine's own cost is not in its instructions:
the CPU's return address predictor always mispredicts the `ret` of a switch.
Saving the FPU control state costs less than the measurement noise.

**Real workload.** A BFS over the Bay Area road graph (321,270 reachable
vertices, CSR layout) from TurboGraph, best of 15 runs:

| Variant | Time | Slices |
|---------|------|--------|
| unchanged BFS, run to completion inside a coroutine | ~23 ms | 1 |
| BFS with one `co_checkpoint()`, budget 1024 | ~23 ms | 314 |

The two are indistinguishable within measurement noise. Each slice runs for
about 70 µs, short enough that a long traversal never starves small queries
sharing the same workers.

---

## Debugging and tooling

- **gdb.** The switch routines carry complete CFI unwind directives. Both
  sides of a switch leave the same frame shape, so the rules stay correct
  after `rsp` changes. `bt` works at every instruction, and backtraces end
  cleanly at `co_entry`.
- **valgrind.** When `<valgrind/valgrind.h>` is available at build time,
  every slot is registered as a stack. `make memcheck` runs the demo with
  zero errors.
- **Guard pages.** A stack overflow raises `SIGSEGV` with a fault address
  inside the lowest page of the slot.
- **Debug logging.** `make DEBUG=1` enables `LOG_DEBUG`/`LOG_ERROR` on pool
  and coroutine lifecycle events. They compile to nothing otherwise.
- **Control-flow Enforcement Technology (CET).** The assembly object carries
  no GNU property note, so the linker drops the shadow stack marking and the
  kernel does not enable it. That is intentional: a shadow stack would
  reject the `ret` into a frame built by hand or saved by another context.

---

## Building

```sh
make            # bin/libcoro.a and the bin/main demo (-O0, debuggable)
make OPT=-O2    # optimized build; switching OPT rebuilds from scratch
make DEBUG=1    # structured LOG_DEBUG/LOG_ERROR output on stderr
make run        # runs the demo, which exits non-zero if a check fails
make memcheck   # runs the demo under valgrind
make profile    # profiles the demo with callgrind
make clean
```

Requirements: x86-64 Linux, `gcc` with C11, `make`. The valgrind headers are
optional.

To use the library, add `include/` to the include path and link
`bin/libcoro.a` with `-pthread`.

## Profiling

`make profile` rebuilds at `-O2` and runs the demo under callgrind, which
counts executed instructions exactly instead of sampling. Two runs of the
same binary give the same numbers, so small changes can be compared.

```sh
make profile                                     # whole program
make profile PROFFLAGS="-f co_resume -c -B"      # coroutine code only, with cache and branches
make profile PROFFLAGS="-f co_resume -s before"  # store a baseline
# ...edit, rebuild...
make profile PROFFLAGS="-f co_resume -d before"  # per-function instruction delta
tools/profile.sh --help                          # every option
```

Collect inside `co_resume`. It is assembly, so it is never inlined away, and
everything a coroutine runs is reached through it. `tools/cgdiff.c` compares
two profiles function by function.

## Project layout

```
include/Coro.h          public API, header struct, inline helpers
include/CoroAbi.h       constants shared by C and assembly
include/Debug.h         LOG_DEBUG / LOG_ERROR
src/coro_x86_64.S       co_resume, co_yield, co_entry
src/coro.c              pool, slot allocation, first frame
src/main.c              demo and self-check
tools/                  profile.sh, cgdiff.c
```

## Limitations and roadmap

- **x86-64 System V only.** An AArch64 port would be a second assembly file
  behind the same three symbols.
- **Fixed 64 KB stacks.** Deep recursion inside a coroutine hits the guard
  page. The size is a single constant in `CoroAbi.h`.
- **No scheduler yet.** The library provides the mechanism. A run queue with
  per-worker affinity and `co_await` on I/O and completion events are next.
- **No signal-safe switching.** Do not switch from inside a signal handler.

## License

GNU General Public License v3.0. See [LICENSE](LICENSE).
