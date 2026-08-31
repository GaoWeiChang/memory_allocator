#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "../include/heap.h"

#define OPS_PER_THREAD 100000

typedef struct
{
    int thread_id;
    unsigned int seed;
} bench_arg_t;

static void *bench_worker(void *arg)
{
    bench_arg_t *a = (bench_arg_t *)arg;
    for (int i = 0; i < OPS_PER_THREAD; i++)
    {
        size_t size = 16 + (rand_r(&a->seed) % 240);
        void *p = mem_alloc(size);
        if (p)
        {
            ((char *)p)[0] = (char)i;
            mem_free(p);
        }
    }
    return NULL;
}

static double run_with_threads(int num_threads)
{
    heap_reset();

    pthread_t threads[64];
    bench_arg_t args[64];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_threads; i++)
    {
        args[i].thread_id = i;
        args[i].seed = (unsigned int)(4321 + i * 131);
        pthread_create(&threads[i], NULL, bench_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_sec = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    long total_ops = (long)num_threads * OPS_PER_THREAD;
    double ops_per_sec = total_ops / elapsed_sec;

    printf("  %2d thread(s): %8ld ops in %.3f seconds -> %12.0f ops/sec (%.0f ops/sec/thread)\n",
           num_threads, total_ops, elapsed_sec, ops_per_sec, ops_per_sec / num_threads);

    return ops_per_sec;
}

int main(void)
{
    printf("======================================================\n");
    printf(" Multi-threading test\n");
    printf(" (%d malloc+free per thread)\n", OPS_PER_THREAD);
    printf("======================================================\n\n");

    int thread_counts[] = {1, 2, 4, 8};
    int thread_counts_size = sizeof(thread_counts) / sizeof(thread_counts[0]);

    double results[thread_counts_size];

    for (int i = 0; i < thread_counts_size; i++)
    {
        results[i] = run_with_threads(thread_counts[i]);
    }

    printf("\n--- Scaling Efficiency Summary (Compared to 1 thread) ---\n");
    for (int i = 0; i < thread_counts_size; i++)
    {
        double speedup = results[i] / results[0];
        printf("  %2d thread(s): Actual speedup = %.2fx\n", thread_counts[i], speedup);
    }

    heap_reset();

    return 0;
}