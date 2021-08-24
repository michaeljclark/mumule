# mumule

> simple thread pool implementation using the C11 thread support library.

 - `mule_init(mule,nthreads,kernel,userdata)` to initialize the queue
 - `mule_launch(mule)` to start threads
 - `mule_shutdown(mule)` to stop threads
 - `mule_submit(mule,n)` to queue work
 - `mule_synchronize(mule)` to quench the queue
 - `mule_reset(mule)` to clear counters

_mumule_ is a simple thread worker pool that dispatches one dimension of work
indices to a kernel function, either in batch or incrementally with multiple
submissions, to be run by a pool of threads. _mumule_ use three queue counters:
`queued`, `processing`, and `processed` and it is up to the caller to provide
array storage for input and output.

## lock-free atomics and "the lost wakeup problem"

_mumule_ attempts to be lock-free in the common case, that is the main thread
can submit work without taking a mutex and the worker threads can accept new
work without taking a mutex, with the design goal that expensive locking and
unlocking of the mutex after each work-item can be avoided. _mumule_ achieves
this by using lock-free atomic operations on counters using primitives from
`<stdatomic.h>`.

Alas POSIX/C condition variables have a well-known design flaw called
["the lost wakeup problem"](https://docs.oracle.com/cd/E19455-01/806-5257/sync-30/index.html).
POSIX/C condition variables do not support "edge-triggered-events", instead
they require a thread to be waiting at the time an event is signaled to
detect it otherwise an event from a call to `cnd_signal` may be lost if
`cnd_wait` is called too late. The issue can be solved by operating on the
counters within a critical section guarded by the queue mutex, however, the
goal is to avoid locking and unlocking a mutex which may add up to tens of
microseconds between each work item.

### queue-complete edge signal

The issue occurs in _mumule_ while attempting to precisely `cnd_signal` the
_queue-complete_ edge from the worker processing the last item to the
dispatcher `cnd_wait` in `mule_synchronize`. The code tries to do this
precisely but the problem occurs between checking the _queue-complete_
condition and sleeping, whereby one can miss a state change if pre-empted
between checking the condition _(processed < queued)_ and calling `cnd_wait`
thereby causing a deadlock if timeouts were not used.

This design flaw in POSIX/C condition variables is remedied by _"futexes"_
which can recheck the condition in the kernel while interrupts are disabled
and atomically sleep if the condition still holds, but _"futexes"_ are not
portable to other operating systems. _mumule_ instead tries to make the race
condition as narrow as possible, immediately waiting after checking the
condition and using `cnd_timedwait` so that if a wakeup is missed, the
dispatcher thread will retry in a loop testing the condition again after 1ms.

The race condition does not appear in practice but will appear if the
`cnd_timedwait` is changed to `cnd_wait` and a yield or print statement is
inserted between evaluating the condition `(processed < queued)` and calling
`cnd_wait`, and would occasionally cause a deadlock without the timeout.

see `mule_thread`:
```
        /* signal dispatcher precisely when the last item is processed */
        if (processed + 1 == queued) {
            cnd_signal(&mule->wake_dispatcher);
        }
```

and `mule_synchronize`:
```
        /* wait for queue to quench */
        if (processed < queued) {
            cnd_timedwait(&mule->wake_dispatcher, &mule->mutex, &abstime);
        }
```

## mumule interface

These definitions give a summary of the _mumule_ API interface.

```
typedef void(*mumule_work_fn)(void *arg, size_t thr_idx, size_t item_idx);

static void mule_init(mu_mule *mule, size_t num_threads, mumule_work_fn kernel, void *userdata);
static size_t mule_submit(mu_mule *mule, size_t count);
static int mule_launch(mu_mule *mule);
static int mule_synchronize(mu_mule *mule);
static int mule_reset(mu_mule *mule);
static int mule_shutdown(mu_mule *mule);
static int mule_destroy(mu_mule *mule);

enum { mumule_max_threads = 8 };

struct mu_thread { mu_mule *mule; size_t idx; thrd_t thread; };

struct mu_mule
{
    _Atomic(bool)    running;
    _Atomic(size_t)  queued;          /* consider aligning these */
    _Atomic(size_t)  processing;      /* counters so they have */
    _Atomic(size_t)  processed;       /* dedicated cache lines. */
    mtx_t            mutex;
    cnd_t            wake_dispatcher;
    cnd_t            wake_worker;
    void*            userdata;
    mumule_work_fn   kernel;
    size_t           num_threads;
    _Atomic(size_t)  threads_running;
    mu_thread        threads[mumule_max_threads];
};
```

## example program

The following example launches two threads with eight workitems.

```
#include <assert.h>
#include <stdatomic.h>
#include "mumule.h"

_Atomic(size_t) counter = 0;

void work(void *arg, size_t thr_idx, size_t item_idx)
{
    atomic_fetch_add_explicit(&counter, 1, __ATOMIC_SEQ_CST);
}

int main(int argc, const char **argv)
{
    mu_mule mule;

    mule_init(&mule, 2, work, NULL);
    mule_submit(&mule, 8);
    mule_launch(&mule);
    mule_synchronize(&mule);
    mule_shutdown(&mule);
    mule_destroy(&mule);

    assert(atomic_load(&counter) == 8);
}
```

## build and run

Tested with Clang and GCC on Linux _(Ubuntu 20.04 LTS)_.

```
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -B build -G Ninja
cmake --build build -- --verbose
```

Run with `build/test_mumule -v` to enable verbose debug messages:

```
mule_launch: starting-threads
mule_synchronize: quench-queue
mule_thread-0: worker-started
arg=(nil) thr_idx=0 item_idx=1
arg=(nil) thr_idx=0 item_idx=2
arg=(nil) thr_idx=0 item_idx=3
arg=(nil) thr_idx=0 item_idx=4
mule_thread-1: worker-started
arg=(nil) thr_idx=1 item_idx=6
arg=(nil) thr_idx=1 item_idx=7
arg=(nil) thr_idx=1 item_idx=8
arg=(nil) thr_idx=0 item_idx=5
mule_thread-0: signal-dispatcher
mule_thread-1: worker-sleeping
mule_thread-0: worker-sleeping
mule_synchronize: stopping-threads
mule_thread-1: worker-woke
mule_thread-1: worker-exiting
mule_thread-0: worker-woke
mule_thread-0: worker-exiting

```