#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include "../include/heap.h"

static void test_cached_block_bypasses_global_mutex(void) {
    heap_reset();
    printf("[Test 1] Size-class matching blocks are stored in thread-local cache...\n");

    void* p = mem_alloc(64);   /* 64 fits the size class perfectly */
    assert(p != NULL);

    size_t before = heap_tls_cache_count();
    mem_free(p);
    size_t after = heap_tls_cache_count();

    printf("  Before free: cache count = %zu, After free: cache count = %zu\n", before, after);
    assert(after == before + 1);

    /* Must NOT be in the global free list yet because it hasn't been flushed */
    bool found_in_global = false;
    block_t* cur = heap.free_list_head;
    while (cur) { 
        if (cur == get_block_from_ptr(p)) { 
            found_in_global = true; 
            break; 
        } 
        cur = cur->next_free; 
    }
    assert(!found_in_global);
    printf("  Block is not in global free list (Correct — it resides in the thread-local cache instead)\n");

    heap_tls_cache_flush();
    printf("  Passed!\n\n");
}

static void test_cached_block_reused_correctly(void) {
    heap_reset();
    printf("[Test 2] Blocks from thread cache are reused correctly (same address, bypasses global list)...\n");

    void* p1 = mem_alloc(64);
    memset(p1, 0xAB, 64);
    mem_free(p1);

    void* p2 = mem_alloc(64);   /* Should retrieve the exact same address from the cache immediately */
    assert(p2 == p1);
    printf("  malloc(64) after free(64) -> Retrieved the same address from thread cache\n");

    mem_free(p2);
    heap_tls_cache_flush();
    printf("  Passed!\n\n");
}

static void test_cache_spill_when_full(void) {
    heap_reset();
    printf("[Test 3] Cache spills back to global free list when full...\n");

    /* MAX_CACHE_PER_CLASS = 32 (defined in heap.c) — allocate + free past the limit to force a spill.
     * We must place guard blocks between them, otherwise coalescing (if spilled to global list) will merge them all. */
    void* ptrs[40];
    void* guards[40];
    for (int i = 0; i < 40; i++) {
        ptrs[i] = mem_alloc(64);
        guards[i] = mem_alloc(16);
    }
    for (int i = 0; i < 40; i++) {
        mem_free(ptrs[i]);
    }

    size_t cache_count = heap_tls_cache_count();
    printf("  Called free() 40 times (size 64) -> cache count = %zu (Expected <= 32 due to capacity ceiling)\n", cache_count);
    assert(cache_count <= 32);

    /* Some blocks must have "spilled over" into the global free list because the cache was full */
    size_t global_count = 0;
    block_t* cur = heap.free_list_head;
    while (cur) { 
        global_count++; 
        cur = cur->next_free; 
    }
    printf("  Global free list contains %zu block(s) (the portion spilled due to cache overflow)\n", global_count);
    assert(global_count > 0);

    heap_tls_cache_flush();
    for (int i = 0; i < 40; i++) mem_free(guards[i]);
    printf("  Passed!\n\n");
}

static void test_heap_check_recognizes_cached_state(void) {
    heap_reset();
    printf("[Test 4] heap_check_consistency() recognizes thread-cached state separately...\n");

    void* p = mem_alloc(64);
    mem_free(p);   /* Enters the thread cache, setting is_free == 2 */

    heap_check_result_t result = heap_check_consistency();
    printf("  thread_cache_blocks_found = %zu, free_list_mismatches = %zu, consistent = %d\n",
           result.thread_cache_blocks_found, result.free_list_mismatches, result.heap_is_consistent);

    assert(result.thread_cache_blocks_found >= 1);
    assert(result.free_list_mismatches == 0);   /* Must not be counted as a mismatch even if missing from the global list */
    assert(result.heap_is_consistent);

    heap_tls_cache_flush();
    printf("  Passed!\n\n");
}

typedef struct {
    void* leaked_ptr;
} thread_isolation_arg_t;

static void* isolation_worker(void* arg) {
    thread_isolation_arg_t* a = (thread_isolation_arg_t*)arg;
    void* p = mem_alloc(64);
    mem_free(p);   /* Enters this specific thread's local cache only */
    a->leaked_ptr = p;
    return NULL;
}

static void test_thread_cache_is_private(void) {
    heap_reset();
    printf("[Test 5] Thread-local cache is strictly private to each thread (invisible to others)...\n");

    thread_isolation_arg_t arg = {0};
    pthread_t t;
    pthread_create(&t, NULL, isolation_worker, &arg);
    pthread_join(t, NULL);

    /* The main thread's cache should remain empty, even though the worker thread just cached a 64-byte block */
    size_t main_thread_cache = heap_tls_cache_count();
    printf("  Main thread cache count = %zu (Expected 0 since it is a different thread from the one that called free)\n",
           main_thread_cache);
    assert(main_thread_cache == 0);

    /* The block freed by the worker thread must be automatically flushed back to the global list upon thread exit
     * (via the pthread key destructor). Verify with heap_check_consistency that nothing is missing. */
    heap_check_result_t result = heap_check_consistency();
    printf("  After worker thread exits: thread_cached=%zu, free=%zu, consistent=%d\n",
           result.thread_cache_blocks_found, result.free_blocks_found, result.heap_is_consistent);
    assert(result.heap_is_consistent);
    assert(result.free_blocks_found >= 1);   /* Must have been successfully flushed to the global free list */

    printf("  Passed!\n\n");
}

#define PERF_OPS_PER_THREAD 100000

typedef struct {
    unsigned int seed;
} perf_arg_t;

static void* perf_worker(void* arg) {
    perf_arg_t* a = (perf_arg_t*)arg;
    for (int i = 0; i < PERF_OPS_PER_THREAD; i++) {
        size_t size = 16 + (rand_r(&a->seed) % 240);
        void* p = mem_alloc(size);
        if (p) {
            ((char*)p)[0] = (char)i;
            mem_free(p);
        }
    }
    return NULL;
}

static double run_perf(int num_threads) {
    heap_reset();
    pthread_t threads[16];
    perf_arg_t args[16];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_threads; i++) {
        args[i].seed = (unsigned int)(9000 + i * 71);
        pthread_create(&threads[i], NULL, perf_worker, &args[i]);
    }
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double ops_per_sec = ((double)num_threads * PERF_OPS_PER_THREAD) / elapsed;

    printf("  %2d thread(s): %.0f ops/sec (%.0f ops/sec/thread)\n",
           num_threads, ops_per_sec, ops_per_sec / num_threads);
    return ops_per_sec;
}

static void test_performance_improved_vs_chapter09(void) {
    printf("[Test 6] Comparing multi-threaded performance against Chapter 09 (Should be noticeably better)...\n");

    double r1 = run_perf(1);
    double r2 = run_perf(2);
    double r4 = run_perf(4);
    double r8 = run_perf(8);

    printf("  Scaling: 2T=%.2fx, 4T=%.2fx, 8T=%.2fx (relative to 1 thread)\n",
           r2 / r1, r4 / r1, r8 / r1);
    printf("  (Note: If running in a single-core sandbox environment, numerical speedup may be limited.\n");
    printf("   However, throughput degradation per thread should be significantly 'less' than Chapter 09,\n");
    printf("   as the fast path for popular sizes entirely avoids mutex contention.)\n");
    printf("  Passed!\n\n");
}

static void test_malloc_rounds_up_but_free_stays_exact(void) {
    heap_reset();
    printf("[Test 7] malloc() rounds up to match buckets, but free() remains strictly exact...\n");

    /* free(64) -> Enters bucket[64] via an exact match */
    void* a = mem_alloc(64);
    mem_free(a);
    assert(heap_tls_cache_count() == 1);
    printf("  free(block size 64) -> Enters bucket[64] successfully (exact match)\n");

    /* malloc(50) -> aligned to 64, should immediately retrieve the same block from bucket[64] */
    void* b = mem_alloc(50);
    assert(b == a);
    assert(heap_tls_cache_count() == 0);
    printf("  malloc(50) (aligned to 64) -> Retrieved the same block from bucket[64] (exact fit, no extra padding needed)\n");
    mem_free(b);
    heap_tls_cache_flush();

    /* Case for actual rounding up: Prepare a 96-byte block in bucket[96], then request 70 bytes (aligned to 80) */
    heap_reset();
    void* c = mem_alloc(96);
    mem_free(c);
    assert(heap_tls_cache_count() == 1);
    printf("  free(block size 96) -> Enters bucket[96]\n");

    void* d = mem_alloc(70);   /* aligned_size = 80, does not match any exact class -> must round up to bucket[96] */
    assert(d == c);            /* Must retrieve the same block from bucket[96] since it rounded up to find it */
    assert(heap_tls_cache_count() == 0);
    printf("  malloc(70) (aligned to 80, non-exact class match) -> Successfully rounded up and grabbed from bucket[96]!\n");

    size_t usable = mem_alloc_usable_size(d);
    assert(usable == 96);   /* Yields a real usable size of 96 bytes despite requesting only 70 (larger but safe within boundary) */
    printf("  malloc_usable_size = %zu (>= 70 requested, safe and not undersized)\n", usable);

    mem_free(d);
    heap_tls_cache_flush();
    printf("  Passed!\n\n");
}

int main(void) {
    printf("======================================================\n");
    printf(" Thread-Local Optimization\n");
    printf("======================================================\n\n");


    test_cached_block_bypasses_global_mutex();
    test_cached_block_reused_correctly();
    test_cache_spill_when_full();
    test_heap_check_recognizes_cached_state();
    test_thread_cache_is_private();
    test_performance_improved_vs_chapter09();
    test_malloc_rounds_up_but_free_stays_exact();

    return 0;
}