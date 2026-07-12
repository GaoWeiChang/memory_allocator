#ifndef HEAP_H
#define HEAP_H

#include "allocator.h"
#include "memory_source.h"
#include <pthread.h>

typedef enum {
    STRATEGY_FIRST_FIT,
    STRATEGY_BEST_FIT,
    STRATEGY_WORST_FIT,
    STRATEGY_NEXT_FIT
} strategy_t;

typedef struct {
    block_t* free_list_head;
    pthread_mutex_t heap_mutex;

    strategy_t strategy;
    block_t* next_fit_cursor;

    size_t total_allocated;
    size_t allocation_count;
    size_t free_count;
} heap_state_t;

extern heap_state_t heap;


/* free list management */
void add_to_free_list(block_t* block);
void remove_from_free_list(block_t* block);

block_t* find_free_block(size_t size);
block_t* split_block(block_t* block, size_t needed_size);

/* search strategies */
void heap_set_strategy(strategy_t strategy);
block_t* find_free_block_first_fit(size_t size);
block_t* find_free_block_best_fit(size_t size);
block_t* find_free_block_worst_fit(size_t size);
block_t* find_free_block_next_fit(size_t size);

/* public api */
void* cmalloc(size_t size);
void cfree(void* ptr);

/* reset heap for testing */
void heap_reset(void);

#endif