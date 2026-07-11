#ifndef HEAP_H
#define HEAP_H

#include "allocator.h"
#include "memory_source.h"
#include <pthread.h>

typedef struct{
    block_t* free_list_head;
    pthread_mutex_t heap_mutex;

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

/* public api */
void* cmalloc(size_t size);
void cfree(void* ptr);

/* testing */
void heap_reset(void);

#endif