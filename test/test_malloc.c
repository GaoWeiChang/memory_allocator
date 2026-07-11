#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <stdlib.h>
#include "../include/heap.h"


static void test_basic_malloc(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 1: Basic allocation\n");
    printf("\n");

    void* p1 = cmalloc(64);
    assert(p1 != NULL);
    assert(is_aligned(p1));
    printf("malloc(64) -> %p (aligned)\n", p1);

    void* p2 = cmalloc(0);
    assert(p2 == NULL);
    printf("malloc(0) -> NULL\n");

    void* p3 = cmalloc(256 * 1024);
    assert(p3 != NULL);
    assert(p1 != p3);
    printf("malloc(256KB) -> %p\n", p3);

    cfree(p1);
    cfree(p3);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_alignment_all_sizes(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 2: Check alignment\n");
    printf("\n");

    for(size_t size=1; size<=1000; size++){
        void* ptr = cmalloc(size);
        assert(ptr != NULL);
        assert(is_aligned(ptr));
        memset(ptr, 0xAA, size);
        cfree(ptr);
    }

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_free_list_reuse(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 3: Free list reuse\n");
    printf("\n");

    void* ptrs[10];
    for(int i=0; i<10; i++){
        ptrs[i] = cmalloc(64);
        assert(ptrs[i] != NULL);
    }
    printf("Allocate 10 blocks (64 byte each)\n");

    for(int i=0; i<10; i+=2){
        cfree(ptrs[i]);
    }

    void* reused = cmalloc(64);
    assert(reused != NULL);

    bool found_reuse = false;
    for (int i = 0; i < 10; i += 2) {
        if (reused == ptrs[i]) { found_reuse = true; break; }
    }
    assert(found_reuse);

    cfree(reused);
    for (int i = 1; i < 10; i += 2) {
        cfree(ptrs[i]);
    }

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_block_splitting(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 4: Block splitting\n");
    printf("\n");

    void* big_block = cmalloc(2048);
    assert(big_block != NULL);
    cfree(big_block);

    void* small_block = cmalloc(64);
    assert(small_block != NULL);
    assert(small_block == big_block);
    assert(heap.free_list_head != NULL);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_stress(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 5: Stress test\n");
    printf("\n");

    const int iterations = 10000;
    void* allocations[iterations];

    srand(42);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < iterations; i++) {
        size_t size = (rand() % 512) + 1;
        allocations[i] = cmalloc(size);
        assert(allocations[i] != NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double alloc_us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3;

    for (int i = 0; i < iterations; i++) {
        cfree(allocations[i]);
    }

    printf("Successfully allocated %d blocks, average time %.3f us/malloc\n", iterations, alloc_us / iterations);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

int main(){

    test_basic_malloc();
    test_alignment_all_sizes();
    test_free_list_reuse();
    test_block_splitting();
    test_stress();

    printf("\n");
    printf("\n");

    return 0;
}