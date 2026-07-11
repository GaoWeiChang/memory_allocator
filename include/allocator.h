#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
    Block layout
        [ HEADER (32B) ][ payload (size bytes) ][ FOOTER (8B) = size ]
         store metadata   actual user data size   copy of the block size, used to find the previous block during coalescing
*/

/* block config*/
#define MAGIC_NUMBER 0xDEADBEEF
#define ALIGNMENT 16
#define MIN_ALLOC_SIZE (sizeof(void*) * 2)  // minimum space for prev_free/next_free

/* block header */
typedef struct block{
    size_t size;
    uint32_t is_free;
    uint32_t magic;

    struct block* prev_free;
    struct block* next_free;
} block_t;

#define HEADER_SIZE sizeof(block_t)
#define FOOTER_SIZE 16

/* block status */
typedef enum{
    BLOCK_VALID,
    BLOCK_CORRUPT_MAGIC,
    BLOCK_INVALID_SIZE,
    BLOCK_MISALIGNED,
    BLOCK_INVALID_FREE_STATE
} block_status_t;

/* memory alignment */
static inline size_t align_size(size_t size){
    return (size + ALIGNMENT - 1) & ~((size_t)ALIGNMENT - 1);
}

static inline bool is_aligned(const void* ptr){
    return ((uintptr_t)ptr % ALIGNMENT) == 0;
}

/* address convertion */
// return pointer after malloc to user
static inline void* get_ptr_from_block(block_t* block){
    return block ? (void*)((char*)block + HEADER_SIZE) : NULL;
}

// return block starting point
static inline block_t* get_block_from_ptr(void* ptr){
    return ptr ? (block_t*)((char*)ptr - HEADER_SIZE) : NULL;
}


/* block footer */
static inline size_t* get_footer_ptr(block_t* block){
    return (size_t*)((char*)block + HEADER_SIZE + block->size);
}

static inline void write_footer(block_t* block){
    size_t* footer = get_footer_ptr(block);
    *footer = block->size;
}


/* Initialize block */
static inline void initialize_allocated_block(block_t* block, size_t size){
    block->size = size;
    block->is_free = 0;
    block->magic = MAGIC_NUMBER;
    write_footer(block);
}

static inline void initialize_free_block(block_t* block, size_t size){
    block->size = size;
    block->is_free = 1;
    block->magic = MAGIC_NUMBER;
    block->next_free = NULL;
    block->prev_free = NULL;
    write_footer(block);
}

/* block management */
static inline bool can_split_block(block_t* block, size_t needed_size){
    if(!block)
        return false;
    
    size_t total_size = HEADER_SIZE + FOOTER_SIZE + needed_size + MIN_ALLOC_SIZE;
    if(block->size < total_size)
        return false;
    
    return true;
}

static inline block_t* get_next_block(block_t* block){
    if(!block)
        return NULL;
    
    char* next_addr = (char*)block + HEADER_SIZE + block->size + FOOTER_SIZE;

    return (block_t*)next_addr;
}

static inline block_t* get_prev_block(block_t* block){
    if(!block)
        return NULL;
    
    size_t prev_size = *(size_t*)((char*)block - FOOTER_SIZE);
    char* prev_addr = (char*)block - FOOTER_SIZE - prev_size - HEADER_SIZE;

    return (block_t*)prev_addr;
}

/* block correctness */
static inline bool validate_block_address(block_t* block){
    if(!is_aligned(block))
        return false;
    
    void* user_ptr = get_ptr_from_block(block);
    if(!is_aligned(user_ptr))
        return false;
    
    return true;
}

static inline block_status_t verify_block_integrity(block_t* block){
    if(!validate_block_address(block)){
        return BLOCK_MISALIGNED;
    }

    if(block->magic != MAGIC_NUMBER){
        return BLOCK_CORRUPT_MAGIC;
    }

    if((block->is_free != 0) && (block->is_free != 1)){
        return BLOCK_INVALID_FREE_STATE;
    }

    return BLOCK_VALID;
}

#endif