/* ==========================================================================
 * threadpool.h — minimal persistent worker pool for splitting a row range
 * [0,h) across N threads. Internal app helper (not part of the frozen ABI).
 * ========================================================================== */
#ifndef CATHODE_THREADPOOL_H
#define CATHODE_THREADPOOL_H
#include "cathode/types.h"

typedef void (*BandFn)(void *user, i32 y0, i32 y1);

typedef struct ThreadPool ThreadPool;
ThreadPool *tp_create(i32 nthreads);       /* nthreads<=0 => hw concurrency */
void        tp_destroy(ThreadPool *tp);
i32         tp_threads(const ThreadPool *tp);
/* Split [0,height) into contiguous bands, run fn(user,y0,y1) on each in
 * parallel, and block until all bands finish. */
void        tp_run_bands(ThreadPool *tp, i32 height, BandFn fn, void *user);

#endif
