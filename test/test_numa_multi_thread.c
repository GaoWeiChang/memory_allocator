#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>

#include "../include/heap.h"
#include "../include/numa_topology.h"

/*
    Multi-threaded fake topology.

    Each worker thread is "pinned" to its own NUMA node (fake). It fills its own
    thread-local cache with blocks tagged to that node, then re-allocates them
    and checks every block came back from the local cache, proving the TLS
    cache keeps per-thread, per-node blocks and never serves a remote block
    when a local one is available.

    A barrier holds every thread until all of them have finished allocating, so
    that no thread exits (which would flush its cache into the shared global
    free list, a NUMA-agnostic path) while another is still allocating.
*/

#define MT_THREADS 4
#define MT_BLOCKS 16
#define MT_SIZE 64

static pthread_barrier_t done_allocating;

static int cmp_ptr(const void *x, const void *y)
{
    void *const *a = x;
    void *const *b = y;
    return (*a > *b) - (*a < *b);
}

static void *mt_worker(void *arg)
{
    int node = (int)(long)arg;

    /* this thread runs on `node` and allocates node-local memory */
    numa_fake_set_current_node(node);
    numa_fake_set_alloc_node(node);

    void *freed[MT_BLOCKS];
    for (int i = 0; i < MT_BLOCKS; i++)
    {
        freed[i] = mem_alloc(MT_SIZE);
        assert(freed[i] != NULL);
        memset(freed[i], node, MT_SIZE);
    }
    for (int i = 0; i < MT_BLOCKS; i++)
        mem_free(freed[i]); /* -> this thread's TLS cache, all tagged `node` */

    void *got[MT_BLOCKS];
    for (int i = 0; i < MT_BLOCKS; i++)
    {
        got[i] = mem_alloc(MT_SIZE);
        assert(got[i] != NULL);
    }

    /* every re-allocation must be one of the blocks we just cached */
    qsort(freed, MT_BLOCKS, sizeof(void *), cmp_ptr);
    qsort(got, MT_BLOCKS, sizeof(void *), cmp_ptr);
    assert(memcmp(freed, got, sizeof(freed)) == 0);

    pthread_barrier_wait(&done_allocating);

    for (int i = 0; i < MT_BLOCKS; i++)
        mem_free(got[i]);

    return NULL;
}

static void test_multi_thread_per_node_cache(void)
{
    heap_reset();
    numa_fake_enable(MT_THREADS);
    heap_numa_cache_stats_reset();
    pthread_barrier_init(&done_allocating, NULL, MT_THREADS);

    printf("\n=> per-thread NUMA-local cache under %d threads\n\n", MT_THREADS);

    pthread_t th[MT_THREADS];
    for (long i = 0; i < MT_THREADS; i++)
        assert(pthread_create(&th[i], NULL, mt_worker, (void *)i) == 0);
    for (int i = 0; i < MT_THREADS; i++)
        pthread_join(th[i], NULL);

    pthread_barrier_destroy(&done_allocating);

    heap_numa_stats_t s = heap_numa_cache_stats();
    printf("stats: local=%lu remote=%lu misses=%lu\n",
           s.local_hits, s.remote_hits, s.cache_misses);

    /* first MT_BLOCKS allocs per thread miss the cache; the rest are local */
    assert(s.remote_hits == 0);
    assert(s.local_hits == (unsigned long)MT_THREADS * MT_BLOCKS);
    assert(s.cache_misses == (unsigned long)MT_THREADS * MT_BLOCKS);

    assert(heap_check_consistency().heap_is_consistent);

    numa_fake_disable();
    printf("--------\n  PASS  \n--------\n");
}

int main(void)
{
    test_multi_thread_per_node_cache();
    printf("\nMulti-threaded NUMA test passed.\n");

    heap_reset();

    return 0;
}
