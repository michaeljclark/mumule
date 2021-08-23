#pragma once

#undef NDEBUG
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>
#include <time.h>
#include <assert.h>

#include "mulog.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mu_mule;
typedef struct mu_mule mu_mule;
struct mu_thread;
typedef struct mu_thread mu_thread;

/*
 * mumule thread pool:
 *
 * - `mule_init(mule,nthreads,kernel,userdata)` to initialize the queue
 * - `mule_launch(mule)` to start threads
 * - `mule_shutdown(mule)` to stop threads
 * - `mule_submit(mule,n)` to queue work
 * - `mule_synchronize(mule)` to quench the queue
 * - `mule_reset(mule)` to clear counters
 *
 * mumule example program:
 *
 * {
 *     mu_mule mule;
 *
 *     mule_init(&mule, 2, w1, NULL);
 *     mule_submit(&mule, 8);
 *     mule_launch(&mule);
 *     mule_synchronize(&mule);
 *     mule_shutdown(&mule);
 *     mule_destroy(&mule);
 * }
 *
 */

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

/*
 * mumule implementation
 */

static void mule_init(mu_mule *mule, size_t num_threads, mumule_work_fn kernel, void *userdata)
{
    memset(mule, 0, sizeof(mu_mule));
    mule->userdata = userdata;
    mule->kernel = kernel;
    mule->num_threads = num_threads;
    mtx_init(&mule->mutex, mtx_plain);
    cnd_init(&mule->wake_worker);
    cnd_init(&mule->wake_dispatcher);
}

static int mule_thread(void *arg)
{
    mu_thread *thread = (mu_thread*)arg;
    mu_mule *mule = thread->mule;
    void* userdata = mule->userdata;
    const size_t thread_idx = thread->idx;
    size_t queued, processing, workitem, processed;

    debugf("mule_thread-%zu: worker-started\n", thread_idx);

    atomic_fetch_add_explicit(&mule->threads_running, 1, __ATOMIC_RELAXED);

    for (;;) {
        /* find out how many items still need processing */
        queued = atomic_load_explicit(&mule->queued, __ATOMIC_ACQUIRE);
        processing = atomic_load_explicit(&mule->processing, __ATOMIC_ACQUIRE);

        /* sleep on condition if queue empty or exit if asked to stop */
        if (processing == queued)
        {
            mtx_lock(&mule->mutex);
            if (!atomic_load(&mule->running)) {
                mtx_unlock(&mule->mutex);
                break;
            }
            debugf("mule_thread-%zu: worker-sleeping\n", thread_idx);
            cnd_wait(&mule->wake_worker, &mule->mutex);
            mtx_unlock(&mule->mutex);
            debugf("mule_thread-%zu: worker-woke\n", thread_idx);
            continue;
        }

        /* dequeue work-item using compare-and-swap, run, update processed */
        workitem = processing + 1;
        if (!atomic_compare_exchange_weak(&mule->processing, &processing,
            workitem)) continue;
        (mule->kernel)(userdata, thread_idx, workitem);
        processed = atomic_fetch_add_explicit(&mule->processed, 1, __ATOMIC_SEQ_CST);

        /* signal dispatcher precisely when the last item is processed */
        if (processed + 1 == queued) {
            debugf("mule_thread-%zu: signal-dispatcher\n", thread_idx);
            /*
             * [lost-wakeup-issue]
             *
             * dispatcher can pre-empted before cnd_wait so the dispatcher
             * uses cond_timedwait. needs edge triggered conditions.
             */
            cnd_signal(&mule->wake_dispatcher);
        }
    }

    atomic_fetch_add_explicit(&mule->threads_running, -1, __ATOMIC_RELAXED);

    debugf("mule_thread-%zu: worker-exiting\n", thread_idx);

    return 0;
}

static size_t mule_submit(mu_mule *mule, size_t count)
{
    size_t idx = atomic_fetch_add(&mule->queued, count);
    cnd_broadcast(&mule->wake_worker);
    return idx + count;
}

static int mule_launch(mu_mule *mule)
{
    if (atomic_load(&mule->running)) return 0;

    debugf("mule_launch: starting-threads\n");

    for (size_t idx = 0; idx < mule->num_threads; idx++) {
        mule->threads[idx].mule = mule;
        mule->threads[idx].idx = idx;
    }
    atomic_store(&mule->running, 1);
    atomic_thread_fence(__ATOMIC_SEQ_CST);

    for (size_t i = 0; i < mule->num_threads; i++) {
        assert(!thrd_create(&mule->threads[i].thread, mule_thread, &mule->threads[i]));
    }
    return 0;
}

static int mule_synchronize(mu_mule *mule)
{
    size_t queued, processed;

    debugf("mule_synchronize: quench-queue\n");

    cnd_broadcast(&mule->wake_worker);

    /* wait for queue to quench */
    mtx_lock(&mule->mutex);
    for (;;) {
        struct timespec abstime = { 0 };
        assert(!clock_gettime(CLOCK_REALTIME, &abstime));
        abstime.tv_nsec += 1000000;

        queued = atomic_load_explicit(&mule->queued, __ATOMIC_ACQUIRE);
        processed = atomic_load_explicit(&mule->processed, __ATOMIC_ACQUIRE);
        if (processed < queued) {
            /*
             * [lost-wakeup-issue]
             *
             * dispatcher can pre-empted before cnd_wait so the dispatcher
             * uses cond_timedwait. needs edge triggered conditions.
             */
            cnd_timedwait(&mule->wake_dispatcher, &mule->mutex, &abstime);
        } else {
            break;
        }
    };
    mtx_unlock(&mule->mutex);

    return 0;
}

static int mule_reset(mu_mule *mule)
{
    mule_synchronize(mule);

    atomic_store(&mule->queued, 0);
    atomic_store(&mule->processing, 0);
    atomic_store(&mule->processed, 0);

    cnd_broadcast(&mule->wake_worker);

    return 0;
}

static int mule_shutdown(mu_mule *mule)
{
    if (!atomic_load(&mule->running)) return 0;

    /* shutdown workers */
    debugf("mule_synchronize: stopping-threads\n");

    mtx_lock(&mule->mutex);
    atomic_store_explicit(&mule->running, 0, __ATOMIC_RELEASE);
    mtx_unlock(&mule->mutex);
    cnd_broadcast(&mule->wake_worker);

    /* join workers */
    for (size_t i = 0; i < mule->num_threads; i++) {
        int res;
        assert(!thrd_join(mule->threads[i].thread, &res));
    }

    return 0;
}

static int mule_destroy(mu_mule *mule)
{
    mule_shutdown(mule);

    mtx_destroy(&mule->mutex);
    cnd_destroy(&mule->wake_worker);
    cnd_destroy(&mule->wake_dispatcher);

    return 0;
}

#ifdef __cplusplus
}
#endif
