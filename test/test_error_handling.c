#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../include/heap.h"

static void test_normal_operations_stay_consistent(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("=> Test 1: heap consistency\n");

    void* ptrs[50];
    for (int i = 0; i < 50; i++) {
        ptrs[i] = mem_alloc((size_t)(16 + i * 8));
    }
    for (int i = 0; i < 50; i += 2) {
        mem_free(ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 0; i < 20; i++) {
        void* p = mem_alloc(64);
        mem_free(p);
    }

    heap_check_result_t result = heap_check_consistency();
    printf("checked %zu block (allocated=%zu, free=%zu), corrupted=%zu, mismatch=%zu\n",
           result.block_checked, result.allocated_blocks_found, result.free_blocks_found,
           result.corrupted_blocks, result.free_list_mismatches);

    assert(result.heap_is_consistent);
    assert(result.corrupted_blocks == 0);
    assert(result.free_list_mismatches == 0);

    for (int i = 1; i < 50; i += 2) 
        mem_free(ptrs[i]);
        
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_footer_overflow_detected(void){
    heap_reset();
    printf("\n");
    printf("\n");
    printf("=> Test 2: Overflow ovelap the footer\n");

    void* p = mem_alloc(32);
    assert(p != NULL);

    block_t* block = get_block_from_ptr(p);
    printf("  malloc(32), footer before overlap = %zu (aligned with block->size=%zu)\n", *get_footer_ptr(block), block->size);

    // write directly overlapped footer
    *get_footer_ptr(block) = 0xBADBAD;

    mem_free(p);

    /* check block didn't add in free list */
    bool found_in_list = false;
    block_t* cur = heap.free_list_head;
    while (cur) { if (cur == block) { found_in_list = true; break; } cur = cur->next_free; }
    assert(!found_in_list);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_canary_overflow_detected(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("=> Test 3: Overflow ovelap the canary \n");

    void* p = mem_alloc(32);
    assert(p != NULL);
    block_t* block = get_block_from_ptr(p);

    assert(*get_footer_ptr(block) == block->size);   /* footer normal */
    *get_canary_ptr(block) = 0xDEADDEAD;              /* canary overlap */

    mem_free(p);

    bool found_in_list = false;
    block_t* cur = heap.free_list_head;
    while (cur) { if (cur == block) { found_in_list = true; break; } cur = cur->next_free; }
    assert(!found_in_list);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_use_after_free_poisoning(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("Test 4: use-after-free poisoning: payload overlapped by 0xDD after free()\n");

    void* p = mem_alloc(64);
    memset(p, 0xAB, 64);
    mem_free(p);

    unsigned char* bytes = (unsigned char*)p;
    bool all_poisoned = true;
    for (int i = 0; i < 64; i++) {
        if (bytes[i] != POISON_BYTE) { all_poisoned = false; break; }
    }
    assert(all_poisoned);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_use_after_free_write_detected_on_reuse(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("Test 5: use-after-free: write data after free()\n");

    void* p = mem_alloc(64);
    mem_free(p);

    memset(p, 0x11, 64);

    void* p2 = mem_alloc(64); // receive the warning
    assert(p2 == p);

    mem_free(p2);
    
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_consistency_check_catches_deliberate_corruption(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("Test 6: Heap corruption test\n");

    void* a = mem_alloc(64);
    void* guard = mem_alloc(16);    // barrier for avoid a get coalesce
    void* b = mem_alloc(64);
    (void)b;

    heap_check_result_t before = heap_check_consistency();
    assert(before.heap_is_consistent);

    /* corrupt memory */
    block_t* block_a = get_block_from_ptr(a);
    block_a->magic = 0x12345678;

    heap_check_result_t after = heap_check_consistency();
    assert(!after.heap_is_consistent);
    assert(after.corrupted_blocks >= 1);

    block_a->magic = MAGIC_NUMBER;
    mem_free(a);
    mem_free(guard);
    
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_heap_dump_runs_without_crash(void) {
    heap_reset();
    printf("\n");
    printf("\n");
    printf("Test 7: heap_dump() without crash\n");

    void* a = mem_alloc(64);
    void* b = mem_alloc(128);
    mem_free(a);

    heap_dump();

    mem_free(b);
    
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

int main(void) {

    test_normal_operations_stay_consistent();
    test_footer_overflow_detected();
    test_canary_overflow_detected();
    test_use_after_free_poisoning();
    test_use_after_free_write_detected_on_reuse();
    test_consistency_check_catches_deliberate_corruption();
    test_heap_dump_runs_without_crash();

    return 0;
}