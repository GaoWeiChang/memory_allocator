#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <pthread.h>

#include "../include/heap.h"
#include "../include/numa_topology.h"

static void test_cache_prefers_local_node(void)
{
    heap_reset();
    numa_fake_enable(4);

    printf("\n=> Test 1: TLS cache prefers a block from the caller's NUMA node\n\n");

    /* Acquire three same-size blocks, each tagged to a different node. */
    numa_fake_set_alloc_node(2);
    void *a = mem_alloc(64);
    numa_fake_set_alloc_node(0);
    void *b = mem_alloc(64);
    numa_fake_set_alloc_node(1);
    void *c = mem_alloc(64);
    assert(a && b && c);
    assert(a != b && b != c && a != c);

    memset(a, 1, 64);
    memset(b, 1, 64);
    memset(c, 1, 64);

    /* Free them -> all land in the same size-class bucket of the TLS cache. */
    mem_free(a);
    mem_free(b);
    mem_free(c);
    assert(heap_tls_cache_count() == 3);

    heap_numa_cache_stats_reset();

    /* Running on node 0 -> must get b back, not the list head. */
    numa_fake_set_current_node(0);
    void *p0 = mem_alloc(64);
    printf("current node 0 -> got %p (expected b = %p)\n", p0, b);
    assert(p0 == b);

    /* Running on node 2 -> must get a back. */
    numa_fake_set_current_node(2);
    void *p2 = mem_alloc(64);
    printf("current node 2 -> got %p (expected a = %p)\n", p2, a);
    assert(p2 == a);

    heap_numa_stats_t s = heap_numa_cache_stats();
    printf("stats: local=%lu remote=%lu misses=%lu\n",
           s.local_hits, s.remote_hits, s.cache_misses);
    assert(s.local_hits == 2);
    assert(s.remote_hits == 0);

    /* Running on node 3 -> only c (node 1) left, no local match -> fallback. */
    numa_fake_set_current_node(3);
    void *p3 = mem_alloc(64);
    printf("current node 3 -> got %p (expected c = %p, remote hit)\n", p3, c);
    assert(p3 == c);

    s = heap_numa_cache_stats();
    assert(s.remote_hits == 1);

    mem_free(p0);
    mem_free(p2);
    mem_free(p3);

    assert(heap_check_consistency().heap_is_consistent);

    numa_fake_disable();
    printf("--------\n  PASS  \n--------\n");
}

static void test_single_node_still_works(void)
{
    heap_reset();
    numa_fake_disable();

    printf("\n=> Test 2: allocator works with no fake topology (single node)\n\n");

    void *ptrs[64];
    for (int i = 0; i < 64; i++)
    {
        ptrs[i] = mem_alloc(96);
        assert(ptrs[i] != NULL);
        memset(ptrs[i], 0xAB, 96);
    }
    for (int i = 0; i < 64; i++)
        mem_free(ptrs[i]);

    /* Re-allocating should be served by the cache and counted as local. */
    heap_numa_cache_stats_reset();
    void *q = mem_alloc(96);
    assert(q != NULL);
    heap_numa_stats_t s = heap_numa_cache_stats();
    assert(s.local_hits == 1 && s.remote_hits == 0);
    mem_free(q);

    assert(heap_check_consistency().heap_is_consistent);
    printf("--------\n  PASS  \n--------\n");
}

int main(void)
{
    test_cache_prefers_local_node();
    test_single_node_still_works();
    printf("\nAll NUMA tests passed.\n");
    return 0;
}
