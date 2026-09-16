# TurboGraph Coroutines

Stackful coroutines for x86-64 Linux in C, with a hand-written assembly
context switch. Built as the concurrency layer of
[TurboGraph](https://github.com/giangian2/TurboGraph): it lets plain,
single-threaded graph algorithms be sliced, suspended and resumed on a pool of
worker threads without rewriting them.

---

## Motivation

Graph traversals are CPU-bound. A BFS over a large CSR graph runs a tight loop
until it is done, and a server that runs it cannot serve anybody else in the
meantime. The usual ways out all cost something the graph core refuses to pay:

- **A thread per query.** Megabytes of stack per client, and a kernel context
  switch that costs microseconds and flushes the caches the traversal had just
  warmed up.
- **An event loop.** Fine for O(1) lookups; one large traversal freezes every
  other client.
- **Stackless coroutines, state machines, "step" APIs.** Fast, but every local
  variable of every algorithm has to move into a hand-written struct. The
  algorithms stop being plain C.

## The idea

**Give every query its own small stack, and switch between stacks in user
space.** A C function running on its own stack can be suspended at any call
depth, with all of its locals intact, and resumed later by any thread. The
algorithm does not know it happened.

Design principles:

- **The algorithm stays pure.** The only change a loop needs is one
  `co_checkpoint()` line, which compiles to a decrement and a branch.
- **Nothing hidden.** The whole switch is 23 instructions of assembly in one
  file, commented line by line.
- **Thread-independent by construction.** The running coroutine is found from
  the stack pointer, never from thread-local storage, so migrating between
  worker threads is always safe.
- **Debuggable.** gdb backtraces through a switch at any instruction; valgrind
  follows every stack.

## Architecture

### Slots

A `CoPool` maps one contiguous region split into 64 KB slots, one per
coroutine. Every slot starts at a multiple of 64 KB:

```
high  +---------------------------+  slot base + 64 KB
      | TurboCo header (64 bytes) |
      +---------------------------+  <- initial stack pointer
      | stack, grows downward     |
      |           ...             |
      +---------------------------+  slot base + 4 KB
      | guard page, PROT_NONE     |
low   +---------------------------+  slot base
```

- **Header from the stack pointer.** Setting the low 16 bits of any address in
  the slot gives its last byte, and the header starts 63 bytes before it: two
  instructions, no memory access, no thread-local storage.
- **Header at the top.** A stack overflow hits the guard page and faults
  immediately instead of corrupting the header.
- **Only touched pages cost memory.** The mapping is `MAP_NORESERVE`; a
  coroutine using 3 KB of stack costs one page.

### The switch

A switch is an ordinary function call, so the resume address is already on
the stack and the caller-saved registers need no saving. `co_resume` and
`co_yield` push the six callee-saved registers and the floating point control
bits onto the current stack, store `rsp` in the header, load the other
context's `rsp`, pop its registers and `ret` into it. Every suspended context
has the same frame shape:

```
resume address      <- pushed by the call into the switch
rbp
rbx
r12
r13
r14
r15
MXCSR | x87 CW      <- saved stack pointer
```

`co_create` builds this frame by hand for a new coroutine, with `co_entry` as
its resume address, so the first resume needs no special case.

### API

| Function | Purpose |
|----------|---------|
| `co_pool_create(n)` / `co_pool_free(p)` | map / unmap `n` slots |
| `co_create(p, fn, arg)` | take a slot, prepare `fn(arg, value)`; thread-safe |
| `co_destroy(p, co)` | give the slot back, finished or abandoned; thread-safe |
| `co_resume(co, value)` | run `co` until it yields or returns |
| `co_yield(value)` | suspend, hand `value` to the resumer |
| `co_checkpoint()` | yield once every `CO_BUDGET` calls (default 1024) |
| `co_current()` | header of the running coroutine, from `rsp` |
| `co_pool_contains(p)` | whether the caller runs on one of `p`'s stacks |

### Usage

```c
static uint64_t squares(void* arg, uint64_t first)
{
    for (uint64_t i = 0; i < (uint64_t)(uintptr_t)arg; i++)
        co_yield(i * i);              /* the resumer's co_resume returns i*i */
    return 0;
}

CoPool*  pool = co_pool_create(16);
TurboCo* co   = co_create(pool, squares, (void*)5);

uint64_t v = co_resume(co, 0);
while (co->state != CO_STATE_DONE)
{
    printf("%lu\n", v);               /* 0 1 4 9 16 */
    v = co_resume(co, 0);
}
co_destroy(pool, co);
co_pool_free(pool);
```

On a worker pool, the one rule is to make a yielded coroutine visible to
other threads only **after** `co_resume` has returned: see `worker_main` in
`src/main.c`.

### Measured

On an Intel Core i5-7200U under WSL2, `-O2`:

| Measure | Value |
|---------|-------|
| one context switch | ~15 ns |
| resume + yield round trip | ~30 ns |
| `co_checkpoint` fast path | 5 instructions, header hoisted out of the loop |

The cost is dominated not by the instructions but by the `ret`, which the
CPU's return predictor always gets wrong across a switch.

## Building

```sh
make            # builds bin/libcoro.a and the bin/main demo (-O0, debuggable)
make OPT=-O2    # optimized build; switching OPT rebuilds from scratch
make DEBUG=1    # structured LOG_DEBUG/LOG_ERROR output on stderr
make run        # runs the demo, which is also a self-check
make memcheck   # runs the demo under valgrind
make profile    # profiles the demo with callgrind (see below)
make clean
```

Requirements: x86-64 Linux, a C11 compiler (`gcc`) and `make`. The valgrind
headers are optional; when present, every coroutine stack is registered so
memcheck and callgrind follow the switches.

## Profiling

Same workflow as TurboGraph core. `make profile` rebuilds at `-O2` and runs
the demo under callgrind, which counts executed instructions exactly instead
of sampling.

```sh
make profile                                     # whole program
make profile PROFFLAGS="-f co_resume -c -B"      # coroutine code only, cache and branches
make profile PROFFLAGS="-f co_resume -s before"  # store the baseline
# ...edit, rebuild...
make profile PROFFLAGS="-f co_resume -d before"  # per-function instruction delta
tools/profile.sh --help                          # every option
```

Collect inside `co_resume`: it is assembly, so it is never inlined away, and
everything a coroutine runs is reached through it.

## Project layout

```
include/     public headers (Coro.h, CoroAbi.h, Debug.h)
src/         context switch (coro_x86_64.S), pool (coro.c) + main.c demo
tools/       developer tooling (profile.sh, cgdiff.c)
bin/         built library (libcoro.a) and demo binary
build/       object files
out/         everything generated: out/profile/ callgrind data
```

## Status

Early stage. The switch, the pool and the demo are in place and verified
under valgrind and gdb. Next: a run queue with per-worker affinity, `co_await`
on I/O and completion events, and wiring `co_checkpoint` into the TurboGraph
traversals. x86-64 System V only; an AArch64 switch would be a second
assembly file behind the same interface.

## License

See [LICENSE](LICENSE).
