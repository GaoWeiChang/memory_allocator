#include "../include/heap.h"
#include <stdio.h>
#include <string.h>

// marker that tell this pointer came from aligned_alloc() not malloc()
#define ALIGN_MARKER ((size_t)0xA11CEDA11C0FFEEULL)

heap_state_t heap = {
    .free_list_head = NULL,
    .heap_mutex = PTHREAD_MUTEX_INITIALIZER,
    .strategy = STRATEGY_FIRST_FIT,
    .next_fit_cursor = NULL,
    .total_allocated = 0,
    .allocation_count = 0,
    .free_count = 0
};

void heap_set_strategy(strategy_t strategy){
    pthread_mutex_lock(&heap.heap_mutex);
    heap.strategy = strategy;
    heap.next_fit_cursor = NULL;
    pthread_mutex_unlock(&heap.heap_mutex);
}

void remove_from_free_list(block_t* block){
    /*
        if the next-fit cursor points to this block,
        advance it to another free block before removal
    */
    if(heap.next_fit_cursor == block){
        heap.next_fit_cursor = block->next_free ? block->next_free : heap.free_list_head;
        if(heap.next_fit_cursor == block){
            heap.next_fit_cursor = NULL;
        }
    }

    // remove block
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

block_t* find_free_block_first_fit(size_t size){
    block_t* current = heap.free_list_head;

    while(current){
        if(verify_block_integrity(current) != BLOCK_VALID){
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void*)current);
            return NULL;
        }

        if(current->size >= size){
            return current;
        }
        current = current->next_free;
    }

    return NULL;
}

block_t* find_free_block_best_fit(size_t size){
    block_t* current = heap.free_list_head;
    block_t* best = NULL;

    while(current){
        if(verify_block_integrity(current) != BLOCK_VALID){
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void*)current);
            return NULL;
        }

        // NOTE !!!! why use || instead of &&?
        if(current->size >= size){
            if(!best || current->size < best->size){
                best = current;
                if(best->size == size)
                    break;
            }
        }
        current = current->next_free;
    }

    return best;
}

block_t* find_free_block_worst_fit(size_t size){
    block_t* current = heap.free_list_head;
    block_t* worst = NULL;

    while(current){
        if(verify_block_integrity(current) != BLOCK_VALID){
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void*)current);
            return NULL;
        }

        if(current->size >= size){
            if(!worst || current->size > worst->size){
                worst = current;
            }
        }
        current = current->next_free;
    }

    return worst;
}

block_t* find_free_block_next_fit(size_t size){
    if(!heap.free_list_head)
        return NULL;
    
    block_t* start = heap.next_fit_cursor ? heap.next_fit_cursor : heap.free_list_head;
    block_t* current = start;

    while(current){
        if(verify_block_integrity(current) != BLOCK_VALID){
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void*)current);
            return NULL;
        }

        if(current->size >= size){
            heap.next_fit_cursor = current->next_free ? current->next_free : heap.free_list_head;
            return current;
        }

        current = current->next_free;
        if(current == start)
            break;
    }

    return NULL;
}

block_t* find_free_block(size_t size){
    block_t* result;
    switch (heap.strategy) {
        case STRATEGY_BEST_FIT:
            result = find_free_block_best_fit(size);
            break;
        case STRATEGY_WORST_FIT:
            result = find_free_block_worst_fit(size);
            break;
        case STRATEGY_NEXT_FIT:
            result = find_free_block_next_fit(size);
            break;
        default:
            result = find_free_block_first_fit(size);
            break;
    }

    return result;
}

// use-after-free poisoning
static void poison_block_payload(block_t* block){
    memset(get_ptr_from_block(block), POISON_BYTE, block->size);
}

static bool is_poison_intact(block_t* block, size_t len){
    unsigned char* ptr = (unsigned char*)get_ptr_from_block(block);
    for(size_t i=0; i<len; i++){
        if(ptr[i] != POISON_BYTE)
            return false;
    }

    return true;
}

static void* allocate_from_free_block(block_t* block, size_t size){
    // remove block from free list
    remove_from_free_list(block);

    if(!is_poison_intact(block, block->size)){
        fprintf(stderr, "Heap warning: Found write memory overlapped after free() (use-after-free) at %p\n", 
                get_ptr_from_block(block));    
    }

    block_t* remainder = split_block(block, size);
    if(remainder){
        add_to_free_list(remainder);
    }

    initialize_allocated_block(block, block->size);

    heap.total_allocated += block->size;
    heap.allocation_count++;

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
    
    pthread_mutex_lock(&heap.heap_mutex);
    initialize_allocated_block(block, payload_size);
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

// resolve aligned allocation pointer to it's original raw allocation
static void* resolve_aligned_redirect(void* ptr){
    size_t* marker_slot = (size_t*)((char*)ptr - sizeof(size_t));
    if(*marker_slot != ALIGN_MARKER)
        return NULL;

    void* raw = *(void**)((char*)ptr - 2 * sizeof(size_t));
    block_t* raw_block = get_block_from_ptr(raw);
    if(verify_block_integrity(raw_block) != BLOCK_VALID)
        return NULL;
    
    return raw;
}

void* mem_alloc(size_t size){
    if(size == 0)
        return NULL;
    
    // alignment
    size_t actual_size = (size < MIN_ALLOC_SIZE) ? MIN_ALLOC_SIZE : size;
    size_t aligned_size = align_size(actual_size);

    pthread_mutex_lock(&heap.heap_mutex);

    // search in free list
    block_t* block = find_free_block(aligned_size);
    if(block != NULL){
        void* ptr = allocate_from_free_block(block, aligned_size);
        pthread_mutex_unlock(&heap.heap_mutex);
        return ptr;
    }

    pthread_mutex_unlock(&heap.heap_mutex);

    // if not found in free list, allocate new memory
    return allocate_new_memory(aligned_size);
}

void mem_free(void* ptr){
    if(!ptr)
        return;
    
    void* redirect = resolve_aligned_redirect(ptr);
    if(redirect){
        mem_free(redirect);
        return;
    }

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

    poison_block_payload(block);
    add_to_free_list(block);

    pthread_mutex_unlock(&heap.heap_mutex);
}

void* mem_calloc(size_t nmemb, size_t size){
    if((nmemb != 0) && (nmemb * size > SIZE_MAX)){
        fprintf(stderr, "Heap calloc() error: integer overflow found (nmemb=%zu * size=%zu exceed SIZE_MAX)\n", nmemb, size);
        return NULL;
    }

    size_t total = nmemb * size;
    void* ptr = mem_alloc(total);
    if(ptr)
        memset(ptr, 0, total);

    return ptr;
}

void* mem_realloc(void* ptr, size_t new_size){
    /* Idea: 
        Allocate in the same memory address if the new size is sufficient,
        and move to a new memory address if the original size is insufficient
    */
    if(!ptr)
        return mem_alloc(new_size);
    
    if(new_size == 0){
        mem_free(ptr);
        return NULL;
    }

    /* 
        The pointer returned by mem_aligned_alloc() may not point directly to the
        beginning of the underlying block because of extra padding used to satisfy
        the requested alignment. Therefore, aligned allocations always fall back to
        the allocate-copy-free approach during realloc.
    */
    void* redirect = resolve_aligned_redirect(ptr);
    if(redirect){
        size_t old_usable = mem_alloc_usable_size(ptr);
        void* new_ptr = mem_alloc(new_size);
        if(!new_ptr)
            return NULL;
        
        size_t copy_size = (old_usable < new_size) ? old_usable : new_size;
        memcpy(new_ptr, ptr, copy_size);
        mem_free(ptr);

        return new_ptr;
    }

    block_t* block = get_block_from_ptr(ptr);
    if(verify_block_integrity(block) != BLOCK_VALID){
        fprintf(stderr, "Heap realloc() error: incorrect pointer at %p\n", ptr);
        return NULL;
    }

    size_t actual_size = (new_size < MIN_ALLOC_SIZE) ? MIN_ALLOC_SIZE : new_size;
    size_t aligned_new = align_size(actual_size);
    size_t old_size = block->size;
    
    // new size <= old size, shrink the block in place
    if(aligned_new <= old_size){
        pthread_mutex_lock(&heap.heap_mutex);

        block_t* remainder = split_block(block, aligned_new);
        if(remainder){
            memory_region_t* region = find_memory_region(block);
            if(region && !region->is_mmap){
                coalesce_forward(remainder, region);
            }
            poison_block_payload(remainder);
            add_to_free_list(remainder);
            heap.total_allocated -= (old_size - block->size);
        }

        pthread_mutex_unlock(&heap.heap_mutex);
        return ptr;
    }

    // new size > old size, try to grow the block in-place by merging with adjacent free blocks
    memory_region_t* region = find_memory_region(block);
    if(region && !region->is_mmap){
        pthread_mutex_lock(&heap.heap_mutex);

        bool merged = coalesce_forward(block, region);
        if(merged && block->size >= aligned_new){
            block_t* remainder = split_block(block, aligned_new);
            if (remainder) {
                coalesce_forward(remainder, region);
                poison_block_payload(remainder);
                add_to_free_list(remainder);
            }

            // write footer/canary at new position
            initialize_allocated_block(block, block->size);
            heap.total_allocated += (block->size - old_size);
            heap.allocation_count++;
            
            pthread_mutex_unlock(&heap.heap_mutex);
            return get_ptr_from_block(block);
        }

        pthread_mutex_unlock(&heap.heap_mutex);
    }

    // fallback to allocate-copy-free if in-place growth is impossible
    void* new_ptr = mem_alloc(new_size);
    if(!new_ptr)
        return NULL;
    
    size_t copy_size = (old_size < new_size) ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    mem_free(ptr);
    return new_ptr;
}

/*
    Standard malloc() guarantees up to 16-byte alignment (max_align_t),
    which is sufficient for all standard C data types.

    Use my_aligned_alloc() only when alignments > 16 bytes 
        eg. SIMD / AVX-2
 */
void* mem_aligned_alloc(size_t alignment, size_t size){
    if(alignment == 0 || (alignment & (alignment - 1)) != 0) {
        fprintf(stderr, "Heap aligned_alloc(): alignment=%zu must be power of 2\n", alignment);
        return NULL;
    }
    if(size % alignment != 0){
        fprintf(stderr, "[heap] aligned_alloc(): size=%zu must divisible by alignment=%zu\n",
                size, alignment);
        return NULL;
    }
    if(size == 0)
        return NULL;
    
    if(alignment <= ALIGNMENT)
        return mem_alloc(size);

    size_t extra = alignment + 2*sizeof(size_t);    // alignment + (raw pointer + ALIGN_MARKER)
    void* raw = mem_alloc(size + extra);
    if(!raw)
        return NULL;

    /*
    Allocate extra space to accommodate:
        1. 16 bytes (2 * sizeof(size_t)) for metadata (original pointer + marker)
        2. Up to (alignment - 1) bytes for shifting the pointer to the alignment boundary
     */
    uintptr_t raw_addr = (uintptr_t)raw;
    uintptr_t min_addr = raw_addr + (2 * sizeof(size_t));  // ensure there are at least 16 bytes before the aligned pointer to store the stash
    uintptr_t aligned_addr = (min_addr + alignment - 1) & ~((uintptr_t)alignment - 1);

    void* aligned_ptr = (void*)aligned_addr;

    void** stash = (void**)((char*)aligned_ptr - 2 * sizeof(size_t));
    stash[0] = raw;
    ((size_t*)stash)[1] = ALIGN_MARKER;

    return aligned_ptr;
}

// return usable size of the block (the return will more than or equal )
size_t mem_alloc_usable_size(void* ptr){
    if(!ptr)    
        return 0;
    
    void* redirect = resolve_aligned_redirect(ptr);
    if(redirect){
        block_t* raw_block = get_block_from_ptr(redirect);
        size_t offset = (size_t)((char*)ptr - (char*)redirect);
        return raw_block->size - offset;
    }

    block_t* block = get_block_from_ptr(ptr);
    if(verify_block_integrity(block) != BLOCK_VALID)
        return 0;
    
    return block->size;
}

/* Check heap consistency */
/*
    traverses every physical block in each sbrk region
    (using get_next_block() from the start to the end of the region),
    including both allocated and free blocks. It then verifies that
    the free list matches the actual heap layout, unlike find_free_block(), 
    which scans only the free list making it a more reliable debugging tool
    than scanning the free list alone.
*/
static bool block_in_free_list(block_t* target) {
    block_t* cur = heap.free_list_head;
    while(cur){
        if(cur == target)
            return true;
        cur = cur->next_free;
    }

    return false;
}

static void check_region_visitor(const memory_region_t* region, void* ctx_ptr) {
    heap_check_result_t* result = (heap_check_result_t*)ctx_ptr;

    if(region->is_mmap){
        // 1 block per region for mmap()
        block_t* block = (block_t*)region->start;
        result->block_checked++;

        if(verify_block_integrity(block) != BLOCK_VALID){
            result->corrupted_blocks++;
            return;
        }

        if(block->is_free){
            result->free_list_mismatches++;
        } else {
            result->allocated_blocks_found++;
        }
        return;
    }
    
    char* region_end = (char*)region->start + region->carved_size;
    block_t* block = (block_t*)region->start;

    while((char*)block < region_end){
        result->block_checked++;

        if(verify_block_integrity(block) != BLOCK_VALID){
            result->corrupted_blocks++;
            break;
        }

        bool in_free_list = block_in_free_list(block);
        if(block->is_free) {
            result->free_blocks_found++;
            if(!in_free_list)
                result->free_list_mismatches++;
        } else {
            result->allocated_blocks_found++;
            if(in_free_list)
                result->free_list_mismatches++;
        }

        block = get_next_block(block);
    }
}

heap_check_result_t heap_check_consistency(void){
    heap_check_result_t result = {0,0,0,0,0,true};

    pthread_mutex_lock(&heap.heap_mutex);
    memory_source_for_each_region(check_region_visitor, &result);
    pthread_mutex_unlock(&heap.heap_mutex);

    result.heap_is_consistent = (result.corrupted_blocks == 0 && result.free_list_mismatches == 0);
    return result;
}

// dump allocator state for debugging
static void dump_region_visitor(const memory_region_t* region, void* ctx){
    (void)ctx;
    printf("region %p (%zu bytes, %s)\n",
           region->start, region->size, region->is_mmap ? "mmap" : "sbrk");

    if (region->is_mmap) {
        block_t* block = (block_t*)region->start;
        printf("[%p] size=%-8zu %s\n",
               (void*)block, block->size, block->is_free ? "FREE" : "used");
        return;
    }

    char* region_end = (char*)region->start + region->carved_size;
    block_t* block = (block_t*)region->start;

    while ((char*)block < region_end) {
        if (verify_block_integrity(block) != BLOCK_VALID) {
            printf("[%p] CORRUPTED\n", (void*)block);
            break;
        }
        printf("[%p] size=%-8zu %s\n", (void*)block, block->size, block->is_free ? "FREE" : "used");
        block = get_next_block(block);
    }
}

void heap_reset(void){
    pthread_mutex_lock(&heap.heap_mutex);
    heap.free_list_head = NULL;
    heap.total_allocated = 0;
    heap.allocation_count = 0;
    heap.free_count = 0;
    pthread_mutex_unlock(&heap.heap_mutex);

    memory_source_cleanup();
}

void heap_dump(void) {
    printf("===== Heap Dump =====\n");
    pthread_mutex_lock(&heap.heap_mutex);
    memory_source_for_each_region(dump_region_visitor, NULL);
    pthread_mutex_unlock(&heap.heap_mutex);
    printf("======================\n");
}