#include "../include/heap.h"
#include <stdio.h>
#include <string.h>

heap_state_t heap = {
    .free_list_head = NULL,
    .heap_mutex = PTHREAD_MUTEX_INITIALIZER,
    .total_allocated = 0,
    .allocation_count = 0,
    .free_count = 0
};

void heap_reset(void){
    pthread_mutex_lock(&heap.heap_mutex);
    heap.free_list_head = NULL;
    heap.total_allocated = 0;
    heap.allocation_count = 0;
    heap.free_count = 0;
    pthread_mutex_unlock(&heap.heap_mutex);

    memory_source_cleanup();
}

void remove_from_free_list(block_t* block){
    if(block->prev_free){
        block->prev_free->next_free = block->next_free;
    } else {
        heap.free_list_head = block->next_free;
    }

    if(block->next_free){
        block->next_free->prev_free = block->prev_free;
    }

    block->prev_free = NULL;
    block->next_free = NULL;
}

void add_to_free_list(block_t* block){
    block->is_free = 1;
    block->prev_free = NULL;
    block->next_free = heap.free_list_head;

    if(heap.free_list_head){
        heap.free_list_head->prev_free = block;
    }
    heap.free_list_head = block;
}

block_t* split_block(block_t* block, size_t needed_size){
    if(!can_split_block(block, needed_size))
        return NULL;
    
    void* split_addr = (char*)block + HEADER_SIZE + needed_size + FOOTER_SIZE;
    block_t* new_block = (block_t*)split_addr;
    size_t remain_size = block->size - needed_size - HEADER_SIZE - FOOTER_SIZE;
    initialize_free_block(new_block, remain_size);

    block->size = needed_size;
    write_footer(block);

    return new_block;
}

block_t* find_free_block(size_t size){
    pthread_mutex_lock(&heap.heap_mutex);

    block_t* current = heap.free_list_head;
    block_t* result = NULL;
    while(current){
        if(verify_block_integrity(current) != BLOCK_VALID){
            fprintf(stderr, "Error: free list corrupt at %p\n", (void*)current);
            pthread_mutex_unlock(&heap.heap_mutex);
            return NULL;
        }

        // first fit
        if(current->size >= size){
            result = current;
            break;
        }
        current = current->next_free;
    }

    pthread_mutex_unlock(&heap.heap_mutex);
    return result;
}

static void* allocate_from_free_block(block_t* block, size_t size){
    // remove block from free list
    pthread_mutex_lock(&heap.heap_mutex);
    remove_from_free_list(block);
    pthread_mutex_unlock(&heap.heap_mutex);

    block_t* remainder = split_block(block, size);
    if(remainder){
        pthread_mutex_lock(&heap.heap_mutex);
        add_to_free_list(remainder);
        pthread_mutex_unlock(&heap.heap_mutex);
    }

    initialize_allocated_block(block, block->size);

    pthread_mutex_lock(&heap.heap_mutex);
    heap.total_allocated += block->size;
    heap.allocation_count++;
    pthread_mutex_unlock(&heap.heap_mutex);

    return get_ptr_from_block(block);
}

static void* allocate_new_memory(size_t size){
    size_t total_size = HEADER_SIZE + size + FOOTER_SIZE;
    size_t aligned_total = align_size(total_size);
    size_t padding = aligned_total - total_size;
    size_t payload_size = size + padding;

    void* memory = acquire_memory(aligned_total);
    if(!memory)
        return NULL;
    
    block_t* block = (block_t*)memory;
    initialize_allocated_block(block, payload_size);

    pthread_mutex_lock(&heap.heap_mutex);
    heap.total_allocated += payload_size;
    heap.allocation_count++;
    pthread_mutex_unlock(&heap.heap_mutex);

    return get_ptr_from_block(block);
}

static bool is_valid_neighbor(block_t* candidate, memory_region_t* owner_region){
    if(!candidate || !owner_region)
        return false;
    
    char* region_start = (char*)owner_region->start;
    char* region_end = region_start + owner_region->size;

    if((char*)candidate < region_start)
        return false;
    
    if((char*)candidate + HEADER_SIZE > region_end)
        return false;
    
    if(verify_block_integrity(candidate) != BLOCK_VALID)
        return false;
    
    if((char*)candidate + HEADER_SIZE + candidate->size + FOOTER_SIZE > region_end)
        return false;

    return true;
}

// merge with the next free block if possible
static bool coalesce_forward(block_t* block, memory_region_t* region){
    block_t* next = get_next_block(block);

    if(!is_valid_neighbor(block, region))
        return false;
    
    if(!next->is_free)
        return false;
    
    remove_from_free_list(next);

    block->size = block->size + FOOTER_SIZE + HEADER_SIZE + next->size;
    write_footer(block);

    return true;
}

// merge with the previous free block and return the new starting block
static block_t* coalesce_backward(block_t* block, memory_region_t* region){
    char* region_start = (char*)region->start;

    if((char*)block - FOOTER_SIZE < region_start)
        return block;

    block_t* prev = get_prev_block(block);
    if(!is_valid_neighbor(prev, region) || !prev->is_free)
        return block;

    remove_from_free_list(prev);
    
    prev->size = prev->size + FOOTER_SIZE + HEADER_SIZE + block->size;
    write_footer(prev);

    return prev;
}

void* cmalloc(size_t size){
    if(size == 0)
        return NULL;
    
    // alignment
    size_t actual_size = (size < MIN_ALLOC_SIZE) ? MIN_ALLOC_SIZE : size;
    size_t aligned_size = align_size(actual_size);

    // search in free list
    block_t* block = find_free_block(aligned_size);
    if(block != NULL){
        return allocate_from_free_block(block, aligned_size);
    }

    // if not found in free list, allocate new memory
    return allocate_new_memory(aligned_size);
}

void cfree(void* ptr){
    if(!ptr)
        return;
    
    // locate and verift block
    block_t* block = get_block_from_ptr(ptr);
    block_status_t status = verify_block_integrity(block);
    if(status != BLOCK_VALID){
        fprintf(stderr, "Error: Invalid block (status=%d) at %p\n", status, ptr);
        return;
    }

    if(block->is_free){
        fprintf(stderr, "Error: block double free at %p\n", ptr);
        return;
    }

    // check memory allocate from sbrk or mmap
    memory_region_t* region = find_memory_region(block);

    // block from mmap
    if(region && region->is_mmap){
        pthread_mutex_lock(&heap.heap_mutex);
        heap.total_allocated -= block->size;
        heap.free_count++;
        pthread_mutex_unlock(&heap.heap_mutex);

        release_memory(block, region->size);
        return;
    }

    // block from sbrk
    pthread_mutex_lock(&heap.heap_mutex);
    heap.total_allocated -= block->size;
    heap.free_count++;
    initialize_free_block(block, block->size);

    // coalesce free block
    if(region){
        coalesce_forward(block, region);
        block = coalesce_backward(block, region);
    }

    add_to_free_list(block);

    pthread_mutex_unlock(&heap.heap_mutex);
}