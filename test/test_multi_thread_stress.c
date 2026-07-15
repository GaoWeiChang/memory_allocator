#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>
#include "../include/heap.h"

#define NUM_THREADS   8
#define ITERATIONS    3000


typedef struct {
    int thread_id;
    int corruption_count;
    unsigned int seed;
} thread_arg_t;

static void* worker(void* arg) {
    thread_arg_t* targ = (thread_arg_t*)arg;
    targ->corruption_count = 0;

    for (int i = 0; i < ITERATIONS; i++) {
        size_t size = 32 + (rand_r(&targ->seed) % 200);
        void* p = mem_alloc(size);
        if (!p) continue;

        /* Write a unique signature for this specific (thread_id, iteration) across the entire block */
        uint32_t signature = (uint32_t)(targ->thread_id * 1000000 + i);
        uint32_t* words = (uint32_t*)p;
        size_t nwords = size / sizeof(uint32_t);
        for (size_t w = 0; w < nwords; w++) words[w] = signature;

        /* Delay slightly to widen the "time window", giving other threads a chance to intercept 
         * and claim the same pointer if a race window actually exists in the allocator code. */
        for (volatile int y = 0; y < 30; y++) { /* Short busy-wait */ }

        /* Verify that the signature is still intact before freeing — if it isn't, it means someone else 
         * overwrote this memory while we were still holding it (a clear sign of a race condition). */
        bool intact = true;
        for (size_t w = 0; w < nwords; w++) {
            if (words[w] != signature) { intact = false; break; }
        }
        if (!intact) targ->corruption_count++;

        mem_free(p);
    }

    return NULL;
}

int main(void) {
    printf("======================================================\n");
    printf(" Thread Safety Stress Test\n");
    printf(" (%d threads x %d iterations = %d total malloc/free calls)\n",
           NUM_THREADS, ITERATIONS, NUM_THREADS * ITERATIONS);
    printf("======================================================\n\n");

    heap_reset();

    pthread_t threads[NUM_THREADS];
    thread_arg_t args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].thread_id = i;
        args[i].seed = (unsigned int)(1234 + i * 97);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_create(&threads[i], NULL, worker, &args[i]);
    }
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    int total_corruption = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        printf("  thread %d: corruption_count = %d\n", i, args[i].corruption_count);
        total_corruption += args[i].corruption_count;
    }

    printf("\nTotal corruption detected: %d times out of %d allocations\n",
           total_corruption, NUM_THREADS * ITERATIONS);

    heap_check_result_t result = heap_check_consistency();
    printf("heap_check_consistency() after stress test: consistent=%d, corrupted=%zu, mismatch=%zu\n",
           result.heap_is_consistent, result.corrupted_blocks, result.free_list_mismatches);

    if (total_corruption == 0 && result.heap_is_consistent) {
        printf("\n*** PASS: No race conditions found — allocator is thread-safe ***\n");
        return 0;
    } else {
        printf("\n*** FAIL: Race condition detected! Allocator is NOT thread-safe ***\n");
        return 1;
    }
}