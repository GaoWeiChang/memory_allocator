/*
 * build/run: make fragmentation
 *
 * External fragmentation analysis for the allocator.
 *
 * Runs a randomized alloc/free churn workload against a fixed-size pool of
 * live allocations and, after every cycle, measures external fragmentation:
 *
 *     fragmentation% = (total_free - largest_free_block) / total_free * 100
 *
 * 0%  = every free byte is in one contiguous block (ideal)
 * 90% = the heap has lots of free bytes but no large contiguous run
 *
 * The same workload is replayed against all four fit strategies s
 * (first-fit, best-fit, worst-fit)
 */
#define _GNU_SOURCE
#include "heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define POOL_SIZE  3000    // live allocations kept in the pool
#define CYCLES     300     // churn cycles measured
#define CHURN_FRAC 0.25    // fraction of the pool replaced each cycle
#define SEED       20260902u

/* fragmentation snapshot */

typedef struct
{
    size_t total_free;
    size_t largest_free;
    size_t free_blocks;
} frag_t;

static frag_t snapshot(void)
{
    frag_t f = {0, 0, 0};

    pthread_mutex_lock(&heap.heap_mutex);
    for (block_t *b = heap.free_list_head; b; b = b->next_free)
    {
        f.total_free += b->size;
        if (b->size > f.largest_free)
            f.largest_free = b->size;
        f.free_blocks++;
    }
    pthread_mutex_unlock(&heap.heap_mutex);

    return f;
}

static double frag_pct(frag_t f)
{
    if (f.total_free == 0)
        return 0.0;
    return (double)(f.total_free - f.largest_free) / (double)f.total_free * 100.0;
}

/* workload */

// size mix: mostly small, occasionally large 
static size_t rand_size(unsigned *seed)
{
    if (rand_r(seed) % 100 < 85)
        return 16 + rand_r(seed) % 497;   /* 16 .. 512  */
    return 512 + rand_r(seed) % 3585;     /* 512 .. 4096 */
}

typedef struct
{
    double avg_frag;
    double peak_frag;
    double final_frag;
    size_t final_total_free;
    size_t final_largest_free;
    size_t final_free_blocks;
    size_t alloc_failures;
} run_result_t;

static run_result_t run_strategy(strategy_t strat)
{
    heap_reset();
    heap_set_strategy(strat);

    void **pool = calloc(POOL_SIZE, sizeof(void *));
    run_result_t r = {0};
    unsigned seed = SEED;

    /* initial fill */
    for (int i = 0; i < POOL_SIZE; i++)
    {
        pool[i] = mem_alloc(rand_size(&seed));
        if (!pool[i])
            r.alloc_failures++;
    }

    const int churn = (int)(POOL_SIZE * CHURN_FRAC);
    double sum_frag = 0.0;

    for (int c = 0; c < CYCLES; c++)
    {
        /* free a random subset */
        for (int k = 0; k < churn; k++)
        {
            int idx = rand_r(&seed) % POOL_SIZE;
            if (pool[idx])
            {
                mem_free(pool[idx]);
                pool[idx] = NULL;
            }
        }
        /* refill the holes with fresh random sizes */
        for (int k = 0; k < churn; k++)
        {
            int idx = rand_r(&seed) % POOL_SIZE;
            if (!pool[idx])
            {
                pool[idx] = mem_alloc(rand_size(&seed));
                if (!pool[idx])
                    r.alloc_failures++;
            }
        }

        double fp = frag_pct(snapshot());
        sum_frag += fp;
        if (fp > r.peak_frag)
            r.peak_frag = fp;
    }

    frag_t final = snapshot();
    r.avg_frag = sum_frag / CYCLES;
    r.final_frag = frag_pct(final);
    r.final_total_free = final.total_free;
    r.final_largest_free = final.largest_free;
    r.final_free_blocks = final.free_blocks;

    for (int i = 0; i < POOL_SIZE; i++)
        if (pool[i])
            mem_free(pool[i]);
    free(pool);

    return r;
}

int main(void)
{
    const struct
    {
        strategy_t s;
        const char *name;
    } strats[] = {
        {STRATEGY_FIRST_FIT, "first-fit"},
        {STRATEGY_BEST_FIT, "best-fit"},
        {STRATEGY_WORST_FIT, "worst-fit"},
    };

    printf("external fragmentation analysis  (pool=%d, cycles=%d, churn=%.0f%%)\n\n",
           POOL_SIZE, CYCLES, CHURN_FRAC * 100.0);
    printf("%-11s %8s %8s %8s %13s %13s %11s %9s\n",
           "strategy", "avg%", "peak%", "final%",
           "free bytes", "largest blk", "#free blk", "fails");
    printf("---------------------------------------------------------------"
           "----------------------------\n");

    for (size_t i = 0; i < sizeof(strats) / sizeof(strats[0]); i++)
    {
        run_result_t r = run_strategy(strats[i].s);
        printf("%-11s %8.2f %8.2f %8.2f %13zu %13zu %11zu %9zu\n",
               strats[i].name, r.avg_frag, r.peak_frag, r.final_frag,
               r.final_total_free, r.final_largest_free,
               r.final_free_blocks, r.alloc_failures);
    }

    return 0;
}
