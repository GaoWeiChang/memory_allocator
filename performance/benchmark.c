/*
 * build/run: make benchmark
 *
 * Runs a fixed-size alloc/free churn workload across a matrix of
 * (allocation size, thread count) combinations and reports:
 *
 *   allocations per second   - total allocs / wall-clock time of the timed phase
 *   p50 latency ns           - median mem_alloc() latency
 *   p99 latency ns           - 99th percentile mem_alloc() latency
 */
#define _GNU_SOURCE
#include "heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ITERS_PER_THREAD 10000   // timed allocs per thread
#define LIVE_WINDOW 128          // blocks kept live during churn
#define WARMUP_ITERS 2000        // untimed allocs before timing starts

static const size_t SIZES[] = {16, 64, 256, 512}; // bytes
static const int THREADS[] = {1, 2, 4, 8};

static const strategy_t STRATEGIES[] = {
    STRATEGY_FIRST_FIT,
    STRATEGY_BEST_FIT,
    STRATEGY_WORST_FIT,
};

static const char *strategy_name(strategy_t s)
{
    switch (s)
    {
    case STRATEGY_FIRST_FIT: return "first-fit";
    case STRATEGY_BEST_FIT:  return "best-fit";
    case STRATEGY_WORST_FIT: return "worst-fit";
    default:                 return "unknown";
    }
}


static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double percentile(double *sorted, size_t n, double p)
{
    if (n == 0)
        return 0.0;
    size_t idx = (size_t)(p * (double)(n - 1) + 0.5);
    if (idx >= n)
        idx = n - 1;
    return sorted[idx];
}


typedef struct
{
    size_t size;
    double *lat;   // ITERS_PER_THREAD slot slice, filled by this thread
    pthread_barrier_t *start_barrier;
} job_t;

static void *worker(void *arg)
{
    job_t *j = arg;
    void *live[LIVE_WINDOW] = {0};
    int idx = 0;

    for (int i = 0; i < WARMUP_ITERS; i++)
    {
        void *p = mem_alloc(j->size);
        if (live[idx])
            mem_free(live[idx]);
        live[idx] = p;
        idx = (idx + 1) % LIVE_WINDOW;
    }

    pthread_barrier_wait(j->start_barrier);

    for (int i = 0; i < ITERS_PER_THREAD; i++)
    {
        double t0 = now_ns();
        void *p = mem_alloc(j->size);
        double t1 = now_ns();
        j->lat[i] = t1 - t0;

        if (live[idx])
            mem_free(live[idx]);
        live[idx] = p;
        idx = (idx + 1) % LIVE_WINDOW;
    }

    for (int i = 0; i < LIVE_WINDOW; i++)
        if (live[i])
            mem_free(live[i]);

    return NULL;
}

/* ---- one (size, threads) run ---- */

typedef struct
{
    size_t size;
    int threads;
    double aps;
    double p50;
    double p99;
} result_t;

static result_t run_case(size_t size, int nthreads)
{
    heap_reset();

    pthread_t tid[64];
    job_t jobs[64];
    pthread_barrier_t start_barrier;
    pthread_barrier_init(&start_barrier, NULL, (unsigned)nthreads + 1);

    size_t total_iters = (size_t)nthreads * ITERS_PER_THREAD;
    double *lat = malloc(total_iters * sizeof(double));

    for (int t = 0; t < nthreads; t++)
    {
        jobs[t].size = size;
        jobs[t].lat = lat + (size_t)t * ITERS_PER_THREAD;
        jobs[t].start_barrier = &start_barrier;
        pthread_create(&tid[t], NULL, worker, &jobs[t]);
    }

    pthread_barrier_wait(&start_barrier); /* release workers */
    double t0 = now_ns();

    for (int t = 0; t < nthreads; t++)
        pthread_join(tid[t], NULL);

    double elapsed = now_ns() - t0;
    pthread_barrier_destroy(&start_barrier);

    qsort(lat, total_iters, sizeof(double), cmp_double);

    result_t r;
    r.size = size;
    r.threads = nthreads;
    r.aps = (double)total_iters / (elapsed / 1e9);
    r.p50 = percentile(lat, total_iters, 0.50);
    r.p99 = percentile(lat, total_iters, 0.99);
    free(lat);

    return r;
}

int main(void)
{
    size_t n_sizes = sizeof(SIZES) / sizeof(SIZES[0]);
    size_t n_threads = sizeof(THREADS) / sizeof(THREADS[0]);
    size_t n_strategies = sizeof(STRATEGIES) / sizeof(STRATEGIES[0]);

    printf("allocation throughput / latency  (iters/thread=%d, live window=%d)\n\n",
            ITERS_PER_THREAD, LIVE_WINDOW);

    for (size_t st = 0; st < n_strategies; st++)
    {
        heap_set_strategy(STRATEGIES[st]);

        printf("================ strategy: %s ================\n",
               strategy_name(STRATEGIES[st]));
        printf("%-10s %8s %16s %10s %10s\n",
                "size", "threads", "allocs/sec", "p50 ns", "p99 ns");
        printf("--------------------------------------------------------------\n");

        for (size_t si = 0; si < n_sizes; si++)
        {
            for (size_t ti = 0; ti < n_threads; ti++)
            {
                result_t r = run_case(SIZES[si], THREADS[ti]);

                printf("%-10zu %8d %16.0f %10.0f %10.0f\n",
                       r.size, r.threads, r.aps, r.p50, r.p99);

                fflush(stdout);
            }
            printf("\n");
        }
        printf("\n");
    }

    return 0;
}
