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

    void* p1 = mem_alloc(64);
    assert(p1 != NULL);
    assert(is_aligned(p1));
    printf("malloc(64) -> %p (aligned)\n", p1);

    void* p2 = mem_alloc(0);
    assert(p2 == NULL);
    printf("malloc(0) -> NULL\n");

    void* p3 = mem_alloc(256 * 1024);
    assert(p3 != NULL);
    assert(p1 != p3);
    printf("malloc(256KB) -> %p\n", p3);

    mem_free(p1);
    mem_free(p3);

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
        void* ptr = mem_alloc(size);
        assert(ptr != NULL);
        assert(is_aligned(ptr));
        memset(ptr, 0xAA, size);
        mem_free(ptr);
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
        ptrs[i] = mem_alloc(64);
        assert(ptrs[i] != NULL);
    }
    printf("Allocate 10 blocks (64 byte each)\n");

    for(int i=0; i<10; i+=2){
        mem_free(ptrs[i]);
    }

    void* reused = mem_alloc(64);
    assert(reused != NULL);

    bool found_reuse = false;
    for (int i = 0; i < 10; i += 2) {
        if (reused == ptrs[i]) { found_reuse = true; break; }
    }
    assert(found_reuse);

    mem_free(reused);
    for (int i = 1; i < 10; i += 2) {
        mem_free(ptrs[i]);
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

    void* big_block = mem_alloc(2048);
    assert(big_block != NULL);
    mem_free(big_block);

    void* small_block = mem_alloc(64);
    assert(small_block != NULL);
    assert(small_block == big_block);
    assert(heap.free_list_head != NULL);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

int main(){

    test_basic_malloc();
    test_alignment_all_sizes();
    test_free_list_reuse();
    test_block_splitting();

    heap_reset();
    
    printf("\n");
    printf("\n");

    return 0;
}