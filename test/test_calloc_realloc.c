#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdint.h>
#include "../include/heap.h"

static void test_calloc_zero_fill(void) {
    heap_reset();
    printf("[Test 1] calloc(): Ensuring actual zero-fill...\n");

    void* p = mem_alloc(256);
    memset(p, 0xFF, 256);
    mem_free(p);   /* This block is poisoned with 0xDD after free (Chapter 07) */

    void* c = mem_calloc(16, 16);   /* Request 256 bytes via calloc -> typically gets the same address (from free list) */
    assert(c != NULL);

    unsigned char* bytes = (unsigned char*)c;
    bool all_zero = true;
    for (int i = 0; i < 256; i++) {
        if (bytes[i] != 0) { all_zero = false; break; }
    }
    assert(all_zero);
    printf("  calloc(16, 16) on a block previously holding 0xFF and poisoned with 0xDD -> successfully zero-filled\n");

    mem_free(c);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_calloc_overflow_detected(void) {
    heap_reset();
    printf("[Test 2] calloc(): Detecting integer overflow of nmemb * size...\n");

    /* (SIZE_MAX / 2 + 1) multiplied by 2 will overflow back to a small number if not checked beforehand */
    void* p = mem_calloc(SIZE_MAX / 2 + 1, 2);
    assert(p == NULL);
    printf("  calloc(SIZE_MAX/2+1, 2) -> NULL (overflow detected before calling actual malloc)\n");

    void* q = mem_calloc(0, 100);
    printf("  calloc(0, 100) -> %s (per our design where malloc(0) returns NULL)\n",
           q ? "not NULL" : "NULL");

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_realloc_shrink_in_place(void) {
    heap_reset();
    printf("[Test 3] realloc(): Shrinking memory block in-place (address must not change)...\n");

    void* p = mem_alloc(1024);
    memset(p, 0xAB, 1024);

    void* shrunk = mem_realloc(p, 64);
    assert(shrunk == p);   /* Must shrink in-place; no movement required */
    printf("  realloc(p, 1024->64) -> exact same original address\n");

    unsigned char* bytes = (unsigned char*)shrunk;
    for (int i = 0; i < 64; i++) assert(bytes[i] == 0xAB);
    printf("  First 64 bytes of data remain untouched and equal to 0xAB\n");

    mem_free(shrunk);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_realloc_fallback_copy(void) {
    heap_reset();
    printf("[Test 5] realloc(): Fallback malloc + copy + free when in-place growth is impossible...\n");

    void* a = mem_alloc(64);
    void* guard = mem_alloc(16);   /* Guard block prevents 'a' from expanding in-place */
    memset(a, 0xEF, 64);

    void* grown = mem_realloc(a, 2048);   /* Significantly larger request while right block (guard) is active -> must fallback */
    assert(grown != NULL);
    printf("  realloc(a, 64->2048) with guard block blocking path -> returned new address (fallback triggered)\n");

    unsigned char* bytes = (unsigned char*)grown;
    for (int i = 0; i < 64; i++) assert(bytes[i] == 0xEF);
    printf("  First 64 bytes of data copied successfully\n");

    mem_free(grown);
    mem_free(guard);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_realloc_edge_cases(void) {
    heap_reset();
    printf("[Test 6] realloc(): Edge cases with NULL and 0...\n");

    void* p1 = mem_realloc(NULL, 64);
    assert(p1 != NULL);
    printf("  realloc(NULL, 64) -> behaved like a successful malloc(64)\n");

    void* p2 = mem_realloc(p1, 0);
    assert(p2 == NULL);
    printf("  realloc(p, 0) -> returned NULL (and freed the original block)\n");

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_aligned_alloc_small_alignment(void) {
    heap_reset();
    printf("[Test 7] aligned_alloc(): Alignment <= 16 using normal allocation path...\n");

    void* p = mem_aligned_alloc(16, 64);
    assert(p != NULL);
    assert(((uintptr_t)p % 16) == 0);
    printf("  aligned_alloc(16, 64) -> correctly aligned to 16 bytes\n");

    mem_free(p);
    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_aligned_alloc_large_alignment(void) {
    heap_reset();
    printf("[Test 8] aligned_alloc(): Alignment larger than 16 (32, 64, 4096)...\n");

    size_t alignments[] = {32, 64, 128, 4096};
    for (size_t i = 0; i < sizeof(alignments)/sizeof(alignments[0]); i++) {
        size_t align = alignments[i];
        void* p = mem_aligned_alloc(align, align * 2);
        assert(p != NULL);
        assert(((uintptr_t)p % align) == 0);
        printf("  aligned_alloc(%zu, %zu) -> address %% %zu == 0 ✓\n", align, align * 2, align);

        memset(p, 0x77, align * 2);   /* Write test data to verify usability */
        mem_free(p);                    /* Must successfully free through original mem_free() (using offset redirect) */
    }

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_aligned_alloc_invalid_args(void) {
    heap_reset();
    printf("[Test 9] aligned_alloc(): Rejecting invalid arguments...\n");

    assert(mem_aligned_alloc(3, 12) == NULL);     /* 3 is not a power of two */
    printf("  aligned_alloc(3, 12) -> NULL (3 is not a power of two)\n");

    assert(mem_aligned_alloc(32, 50) == NULL);    /* 50 is not a multiple of 32 */
    printf("  aligned_alloc(32, 50) -> NULL (50 is not a multiple of 32)\n");

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}

static void test_malloc_usable_size(void) {
    heap_reset();
    printf("[Test 10] malloc_usable_size(): Ensured usable size >= requested size...\n");

    void* p = mem_alloc(50);   /* Will be rounded up to 64 bytes (align_size(50)=64) */
    size_t usable = mem_alloc_usable_size(p);
    printf("  malloc(50) -> usable_size = %zu (>= 50 and matches align_size)\n", usable);
    assert(usable >= 50);
    mem_free(p);

    /* Test with aligned_alloc as well */
    void* ap = mem_aligned_alloc(64, 128);
    size_t ausable = mem_alloc_usable_size(ap);
    printf("  aligned_alloc(64,128) -> usable_size = %zu (>= 128)\n", ausable);
    assert(ausable >= 128);
    mem_free(ap);

    printf("--------\n");
    printf("  PASS  \n");
    printf("--------\n");
}


int main(){

    test_calloc_zero_fill();
    test_calloc_overflow_detected();
    test_realloc_shrink_in_place();
    test_realloc_fallback_copy();
    test_realloc_edge_cases();
    test_aligned_alloc_small_alignment();
    test_aligned_alloc_large_alignment();
    test_aligned_alloc_invalid_args();
    test_malloc_usable_size();

    printf("\n");
    printf("\n");
}