/* ==========================================================================
 * threadpool.c  -  persistent pthread pool. Workers sleep on a condvar until a
 * batch of bands is posted, run their band, and signal completion. Reused
 * every frame to avoid thread-spawn overhead in the hot render loop.
 * ========================================================================== */
#include "threadpool.h"
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct ThreadPool {
    pthread_t      *threads;
    i32             n;
    pthread_mutex_t mtx;
    pthread_cond_t  work_ready;
    pthread_cond_t  work_done;
    /* current job */
    BandFn          fn;
    void           *user;
    i32             height;
    i32             next_band;   /* dynamic work-stealing index */
    i32             nbands;
    i32             active;      /* workers still running the current job */
    i32             generation;  /* bumped per job so workers detect new work */
    int             shutdown;
};

/* Each worker grabs bands off next_band until exhausted (work stealing keeps
 * all cores busy even when band cost is uneven, e.g. SDF hot spots). */
static void *worker_main(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;
    i32 seen_gen = 0;
    for (;;) {
        pthread_mutex_lock(&tp->mtx);
        while (!tp->shutdown && tp->generation == seen_gen)
            pthread_cond_wait(&tp->work_ready, &tp->mtx);
        if (tp->shutdown) { pthread_mutex_unlock(&tp->mtx); return NULL; }
        seen_gen = tp->generation;
        pthread_mutex_unlock(&tp->mtx);

        /* Consume bands. */
        i32 band_h = (tp->height + tp->nbands - 1) / tp->nbands;
        for (;;) {
            pthread_mutex_lock(&tp->mtx);
            i32 b = tp->next_band++;
            pthread_mutex_unlock(&tp->mtx);
            if (b >= tp->nbands) break;
            i32 y0 = b * band_h;
            i32 y1 = y0 + band_h;
            if (y1 > tp->height) y1 = tp->height;
            if (y0 < y1) tp->fn(tp->user, y0, y1);
        }

        pthread_mutex_lock(&tp->mtx);
        if (--tp->active == 0) pthread_cond_signal(&tp->work_done);
        pthread_mutex_unlock(&tp->mtx);
    }
}

ThreadPool *tp_create(i32 nthreads) {
    if (nthreads <= 0) {
        long hw = sysconf(_SC_NPROCESSORS_ONLN);
        nthreads = (hw > 0) ? (i32)hw : 4;
    }
    ThreadPool *tp = (ThreadPool *)calloc(1, sizeof(ThreadPool));
    tp->n = nthreads;
    tp->threads = (pthread_t *)calloc((size_t)nthreads, sizeof(pthread_t));
    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->work_ready, NULL);
    pthread_cond_init(&tp->work_done, NULL);
    tp->generation = 0;
    for (i32 i = 0; i < nthreads; ++i)
        pthread_create(&tp->threads[i], NULL, worker_main, tp);
    return tp;
}

void tp_destroy(ThreadPool *tp) {
    if (!tp) return;
    pthread_mutex_lock(&tp->mtx);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->work_ready);
    pthread_mutex_unlock(&tp->mtx);
    for (i32 i = 0; i < tp->n; ++i) pthread_join(tp->threads[i], NULL);
    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->work_ready);
    pthread_cond_destroy(&tp->work_done);
    free(tp->threads);
    free(tp);
}

i32 tp_threads(const ThreadPool *tp) { return tp ? tp->n : 1; }

void tp_run_bands(ThreadPool *tp, i32 height, BandFn fn, void *user) {
    if (!tp || height <= 0) return;
    if (!fn) return;
    /* More bands than threads => finer granularity for work stealing. */
    i32 nbands = tp->n * 4;
    if (nbands > height) nbands = height;
    if (nbands < 1) nbands = 1;

    pthread_mutex_lock(&tp->mtx);
    tp->fn = fn; tp->user = user; tp->height = height;
    tp->nbands = nbands; tp->next_band = 0;
    tp->active = tp->n;
    tp->generation++;
    pthread_cond_broadcast(&tp->work_ready);
    while (tp->active != 0)
        pthread_cond_wait(&tp->work_done, &tp->mtx);
    pthread_mutex_unlock(&tp->mtx);
}
