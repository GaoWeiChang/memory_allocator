#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include "../include/heap.h"


// display free list
static void print_free_list(const char* label) {
    printf("  %s: [", label);
    block_t* cur = heap.free_list_head;
    bool first = true;
    while (cur) {
        printf("%s%zu", first ? "" : ", ", cur->size);
        first = false;
        cur = cur->next_free;
    }
    printf("]\n");
}

static void test_first_fit_picks_first_sufficient(void) {
    heap_reset();

    printf("\n");
    printf("\n");

    heap_set_strategy(STRATEGY_FIRST_FIT);
    printf("=> Test 1: First-fit strategy\n");
    printf("\n");

    /* Important note:
     * Always insert a "guard" block (a block that remains allocated)
     * between the blocks used in the test. Otherwise, the coalescing
     * logic implemented in Chapter 05 will merge adjacent free blocks
     * into a single larger block, leaving no separate free blocks for
     * the allocation strategy to choose from.
     */
    void* g0 = mem_alloc(16);
    void* p1 = mem_alloc(256);   /* Freed first  -> ends up at the tail of the free list */
    void* g1 = mem_alloc(16);
    void* p2 = mem_alloc(64);
    void* g2 = mem_alloc(16);
    void* p3 = mem_alloc(512);
    void* g3 = mem_alloc(16);
    void* p4 = mem_alloc(64);    /* Freed last -> becomes the head of the free list */
    void* g4 = mem_alloc(16);

    mem_free(p1);
    mem_free(p2);
    mem_free(p3);
    mem_free(p4);

    print_free_list("current free list (head -> tail)");

    /* Request 32 bytes.
     * First-Fit should return the first block in the free list (p4, 64 bytes),
     * even though p2 is also a 64-byte block.
     * The key behavior is that First-Fit stops searching as soon as it finds
     * the first block that is large enough.
     */
    void* result = mem_alloc(32);
    block_t* result_block = get_block_from_ptr(result);
    assert(result_block == get_block_from_ptr(p4));

    mem_free(result);
    mem_free(g0);
    mem_free(g1);
    mem_free(g2);
    mem_free(g3);
    mem_free(g4);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_best_fit_picks_tightest(void){
    heap_reset();
    
    printf("\n");
    printf("\n");

    heap_set_strategy(STRATEGY_BEST_FIT);
    printf("=> Test 2: Best-fit strategy\n");
    printf("\n");

    void* g0 = mem_alloc(16);
    void* p1 = mem_alloc(256);
    void* g1 = mem_alloc(16);
    void* p2 = mem_alloc(512);
    void* g2 = mem_alloc(16);
    void* p3 = mem_alloc(80);    /* best-fit */
    void* g3 = mem_alloc(16);
    void* p4 = mem_alloc(1024);
    void* g4 = mem_alloc(16);

    mem_free(p1);
    mem_free(p2);
    mem_free(p3);
    mem_free(p4);

    print_free_list("current free list (head -> tail)");

    void* result = mem_alloc(64);
    block_t* result_block = get_block_from_ptr(result);
    assert(result_block == get_block_from_ptr(p3));

    mem_free(result);
    mem_free(g0); 
    mem_free(g1); 
    mem_free(g2); 
    mem_free(g3); 
    mem_free(g4);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_worst_fit_picks_largest(void){
    heap_reset();

    printf("\n");
    printf("\n");

    heap_set_strategy(STRATEGY_WORST_FIT);
    printf("=> Test 3: Worst-fit strategy\n");
    printf("\n");

    void* g0 = mem_alloc(16);
    void* p1 = mem_alloc(256);
    void* g1 = mem_alloc(16);
    void* p2 = mem_alloc(1024);   /* largest -> Worst-Fit */
    void* g2 = mem_alloc(16);
    void* p3 = mem_alloc(80);
    void* g3 = mem_alloc(16);
    void* p4 = mem_alloc(512);
    void* g4 = mem_alloc(16);

    mem_free(p1);
    mem_free(p2);
    mem_free(p3);
    mem_free(p4);

    print_free_list("current free list (head -> tail)");

    void* result = mem_alloc(64);
    block_t* result_block = get_block_from_ptr(result);
    assert(result_block == get_block_from_ptr(p2));

    mem_free(result);
    mem_free(g0); 
    mem_free(g1); 
    mem_free(g2); 
    mem_free(g3); 
    mem_free(g4);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");    
}

static void test_next_fit_distributes_across_heap(void){
    heap_reset();
    
    printf("\n");
    printf("\n");

    heap_set_strategy(STRATEGY_NEXT_FIT);
    printf("=> Test 4: Next-fit strategy\n");
    printf("\n");

    void* ptrs[5];
    void* guards[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = mem_alloc(64);
        guards[i] = mem_alloc(16);
    }
    for (int i = 0; i < 5; i++){
        mem_free(ptrs[i]);
    }

    /* First allocation:
    * Should return the block at the initial cursor position
    * (the head of the free list).
    */
    void* r1 = mem_alloc(64);
    block_t* cursor_after_r1 = heap.next_fit_cursor;
    printf("  malloc(64) #1 -> returned %p, cursor advanced to %p\n", get_block_from_ptr(r1), (void*)cursor_after_r1);

    /* Second allocation:
    * The cursor should continue from its current position
    * instead of restarting at the head of the free list.
    */
    void* r2 = mem_alloc(64);
    printf("  malloc(64) #2 -> returned %p (a different block from the first; did not wrap back to the beginning)\n", get_block_from_ptr(r2));
    assert(get_block_from_ptr(r2) != get_block_from_ptr(r1));

    mem_free(r1);
    mem_free(r2);
    for (int i = 0; i < 5; i++){
        mem_free(guards[i]);
    }

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");    
}

static void test_search_speed_comparison(void) {
    printf("=> Test 6: Speed comparison First-Fit vs Best-Fit\n");

    strategy_t strategies[] = {STRATEGY_FIRST_FIT, STRATEGY_BEST_FIT, STRATEGY_WORST_FIT};
    const char* names[] = {"First-Fit", "Best-Fit ", "Worst-Fit"};

    for (int s = 0; s < 3; s++) {
        heap_reset();
        heap_set_strategy(strategies[s]);

        /* Build a free list containing 2,000 blocks of varying sizes. */
        void* ptrs[2000];
        for (int i = 0; i < 2000; i++) {
            ptrs[i] = mem_alloc((size_t)(16 + (i % 100) * 8));
        }
        for (int i = 0; i < 2000; i++) {
            mem_free(ptrs[i]);
        }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        /* Perform 1,000 allocation searches.
        * Each allocation requests a small block, forcing the allocator
        * to traverse the free list to find a suitable block.
        */
        for (int i = 0; i < 1000; i++) {
            void* p = mem_alloc(50);
            if (p) mem_free(p);
        }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e3;

        printf("[%s] 1000 malloc+free on free list %.1f us (%.3f us/op)\n", names[s], us, us / 1000.0);
    }
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");    
}

int main(void) {

    test_first_fit_picks_first_sufficient();
    test_best_fit_picks_tightest();
    test_worst_fit_picks_largest();
    test_next_fit_distributes_across_heap();
    test_search_speed_comparison();

    return 0;
}
