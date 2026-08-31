#ifndef HEAP_H
#define HEAP_H

#include "allocator.h"
#include "memory_source.h"
#include <pthread.h>

#define POISON_BYTE 0xDD

typedef enum
{
    STRATEGY_FIRST_FIT,
    STRATEGY_BEST_FIT,
    STRATEGY_WORST_FIT,
    STRATEGY_NEXT_FIT
} strategy_t;

typedef struct
{
    block_t *free_list_head;
    pthread_mutex_t heap_mutex;

    strategy_t strategy;
    block_t *next_fit_cursor; // cursor for next-fit strategy

    size_t total_allocated;
    size_t allocation_count;
    size_t free_count;
} heap_state_t;

typedef struct
{
    size_t block_checked;
    size_t free_blocks_found;
    size_t allocated_blocks_found;
    size_t thread_cache_blocks_found;
    size_t corrupted_blocks;
    size_t free_list_mismatches;
    bool heap_is_consistent;
} heap_check_result_t;

extern heap_state_t heap;

/* free list management */
void add_to_free_list(block_t *block);
void remove_from_free_list(block_t *block);

block_t *find_free_block(size_t size);
block_t *split_block(block_t *block, size_t needed_size);

/* search strategies */
void heap_set_strategy(strategy_t strategy);
block_t *find_free_block_first_fit(size_t size);
block_t *find_free_block_best_fit(size_t size);
block_t *find_free_block_worst_fit(size_t size);
block_t *find_free_block_next_fit(size_t size);

/* check consistency */
heap_check_result_t heap_check_consistency(void);

/* public api */
void *mem_alloc(size_t size);
void mem_free(void *ptr);
size_t mem_alloc_usable_size(void *ptr);

/* thread-local cache */
size_t heap_tls_cache_count(void);
void heap_tls_cache_flush(void);

/* for testing & debugging */
void heap_reset(void);
void heap_dump(void);

#endif