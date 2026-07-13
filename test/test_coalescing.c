#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "../include/heap.h"

static void test_forward_coalescing(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 1: Forward coalescing\n");
    printf("\n");

    void* a = mem_alloc(64);
    void* b = mem_alloc(64);
    void* c = mem_alloc(64);
    assert(a && b && c);

    block_t* block_a = get_block_from_ptr(a);
    block_t* block_b = get_block_from_ptr(b);
    size_t size_a_before = block_a->size;
    size_t size_b_before = block_b->size;

    mem_free(b);
    mem_free(a);

    // check block a coalesce with b
    block_t* merged = get_block_from_ptr(a);
    size_t expected_min = size_a_before + FOOTER_SIZE + HEADER_SIZE + size_b_before;

    assert(merged->size >= expected_min);
    assert(merged->is_free == 1);

    mem_free(c);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_backward_coalescing(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 2: Backward coalescing\n");
    printf("\n");

    void* a = mem_alloc(64);
    void* b = mem_alloc(64);
    void* c = mem_alloc(64);
    assert(a && b && c);

    block_t* block_a = get_block_from_ptr(a);
    block_t* block_b = get_block_from_ptr(b);
    size_t size_a = block_a->size;
    size_t size_b = block_b->size;

    mem_free(a);
    mem_free(b);
    
    
    block_t* merged = get_block_from_ptr(a);   /* ผลลัพธ์ควรมี a เป็นจุดเริ่มต้น เพราะ prev ชนะ */
    size_t expected_min = size_a + FOOTER_SIZE + HEADER_SIZE + size_b;
    assert(merged->size >= expected_min);

    mem_free(c);
    
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_triple_coalescing_and_reuse(void){
    heap_reset();
    
    printf("\n");
    printf("\n");

    printf("=> Test 3: Coalesce 3 blocks and reuse\n");
    printf("\n");

    void* a = mem_alloc(128);
    void* b = mem_alloc(128);
    void* c = mem_alloc(128);
    assert(a && b && c);

    block_t* block_a = get_block_from_ptr(a);
    block_t* block_b = get_block_from_ptr(b);
    block_t* block_c = get_block_from_ptr(c);

    /* Capture the sizes before free(), because coalescing immediately
     * mutates the original block's ->size in place once the merge succeeds.
     */
    size_t size_a = block_a->size;
    size_t size_b = block_b->size;
    size_t total_payload = size_a + size_b + block_c->size;

    mem_free(a);
    mem_free(c);   /* a and c are free, but b (the middle block) is still allocated,
                       * so a and c remain as two separate free blocks.
                       */

    mem_free(b);   /* Once b is also freed, a, b, and c should merge
                       * into one large contiguous block.
                       */

    block_t* merged = get_block_from_ptr(a);

    printf("  Merged block size = %zu, original total payload (excluding the removed headers/footers) = %zu\n",
           merged->size, total_payload);

    /* Demonstrate that fragmentation has been reduced:
     * request a block larger than any individual block could satisfy,
     * but small enough to fit in the merged block.
     */
    size_t big_request = size_a + size_b + 32;  /* Larger than either a or b individually. */

    void* big = mem_alloc(big_request);
    assert(big != NULL);
    assert(big == a);   /* First-fit should return the newly merged block. */

    printf("  malloc(%zu) succeeded using the original address, proving that the blocks were successfully coalesced.\n",
           big_request);

    mem_free(big);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}


static void test_no_out_of_bounds_read_at_heap_edge(void){
    heap_reset();

    printf("\n");
    printf("\n");

    printf("=> Test 4: Boundary check\n");
    printf("\n");

    /* จองก้อนเดียวแล้ว free ทันที มันจะกลายเป็นก้อนแรก (หรือก้อนเดียว) ใน region
     * ตอน free ต้องพยายาม coalesce ทั้งซ้ายและขวา ซึ่งอาจชนขอบ region ได้
     * ถ้า boundary check ผิดพลาด โปรแกรมจะ segfault ทันทีตรงนี้ */
    void* p = mem_alloc(32);
    assert(p != NULL);
    mem_free(p);   /* ถ้าไม่ crash แปลว่า is_valid_neighbor() ป้องกันการอ่านเลยขอบเขตได้จริง */

    printf("  free() ที่ขอบ region ไม่ crash -> boundary check ทำงานถูกต้อง\n");

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

int main(){

    test_forward_coalescing();
    test_backward_coalescing();
    test_triple_coalescing_and_reuse();
    test_no_out_of_bounds_read_at_heap_edge();

    printf("\n");
    printf("\n");


    return 0;
}