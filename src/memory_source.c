#include "../include/memory_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

_Thread_local alloc_error_t last_error = ALLOC_SUCCESS;

// sbrk pool status
static void* heap_extension_pool = NULL;
static size_t pool_remaining = 0;
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;

// memory region
static memory_region_t* memory_regions = NULL;
static pthread_mutex_t region_mutex = PTHREAD_MUTEX_INITIALIZER;


static memory_source_t select_memory_source(size_t size){
    size_t aligned_size = align_size(size);
    if(aligned_size >= MMAP_THRESHOLD){
        return MEMORY_SOURCE_MMAP;
    }

    return MEMORY_SOURCE_SBRK;
}

static void register_memory_region(void* start, size_t size, bool is_mmap){
    memory_region_t* region = malloc(sizeof(memory_region_t));
    if(!region)
        return;
    
    region->start = start;
    region->size = size;
    region->is_mmap = is_mmap;

    pthread_mutex_lock(&region_mutex);
    region->next = memory_regions;
    memory_regions = region;

    pthread_mutex_unlock(&region_mutex);
}

static void unregister_memory_region(void* start){
    pthread_mutex_lock(&region_mutex);

    memory_region_t** current = &memory_regions;
    while(*current){
        if((*current)->start == start){
            memory_region_t* to_remove = *current;
            *current = (*current)->next;
            free(to_remove);
            break;
        }
        current = &(*current)->next;
    }

    pthread_mutex_unlock(&region_mutex);
}

memory_region_t* find_memory_region(void* ptr){
    pthread_mutex_lock(&region_mutex);

    memory_region_t* current = memory_regions;
    while(current){
        char* start = (char*)current->start;
        char* end = start + current->size;
        if(((char*)ptr >= start) && ((char*)start < end)){
            pthread_mutex_unlock(&region_mutex);
            return current;
        }
        current = current->next;
    }

    pthread_mutex_unlock(&region_mutex);
    return NULL;
}

void memory_source_cleanup(void){
    pthread_mutex_lock(&region_mutex);
    memory_region_t* cur = memory_regions;
    while(cur){
        memory_region_t* next = cur->next;
        if(cur->is_mmap){
            munmap(cur->start, cur->size);
        }
        free(cur);
        cur = next;
    }

    memory_regions = NULL;
    pthread_mutex_unlock(&region_mutex);

    pthread_mutex_lock(&pool_mutex);
    heap_extension_pool = NULL;
    pool_remaining = 0;
    pthread_mutex_unlock(&pool_mutex);
}

/*
 * Acquire memory from the heap extension pool.
 *
 * Since sbrk() is a costly system call, memory is requested in large chunks
 * (HEAP_EXTENSION_SIZE). Small allocation requests are then served from this
 * pool until it is not enough pool for request size, reducing the number of system call.
 */

static void* acquire_memory_sbrk(size_t size){
    size_t aligned_size = align_size(size);

    pthread_mutex_lock(&pool_mutex);

    // enough pool
    if(heap_extension_pool && pool_remaining >= aligned_size){
        void* result = heap_extension_pool;
        heap_extension_pool = (char*) heap_extension_pool + aligned_size;
        pool_remaining -= aligned_size;
        pthread_mutex_unlock(&pool_mutex);

        return result;
    }

    // not enough pool
    size_t extension_size = (aligned_size > HEAP_EXTENSION_SIZE) ? aligned_size : HEAP_EXTENSION_SIZE;

    void* new_memory = sbrk((intptr_t)extension_size);
    if(new_memory == (void*)-1){
        pthread_mutex_unlock(&pool_mutex);
        last_error = ALLOC_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    memset(new_memory, 0, extension_size);
    
    void* result = new_memory;
    heap_extension_pool = (char*)new_memory + aligned_size;
    pool_remaining = extension_size - aligned_size;

    pthread_mutex_unlock(&pool_mutex);
    register_memory_region(new_memory, extension_size, false);

    return result;
}

static void* acquire_memory_mmap(size_t size){
    size_t page_aligned_size = ((size + PAGE_SIZE - 1)/PAGE_SIZE) * PAGE_SIZE;
    void* ptr = mmap(NULL, page_aligned_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
    
    if(ptr == MAP_FAILED){
        last_error = (errno == EINVAL) ? ALLOC_ERROR_INVALID_SIZE : ALLOC_ERROR_OUT_OF_MEMORY;
        return NULL;
    }

    register_memory_region(ptr, page_aligned_size, true);
    return ptr;
}

void* acquire_memory(size_t total_size){
    last_error = ALLOC_SUCCESS;
    if(total_size == 0){
        last_error = ALLOC_ERROR_INVALID_SIZE;
        return 0;
    }

    memory_source_t mem_source = select_memory_source(total_size);
    switch(mem_source){
        case MEMORY_SOURCE_SBRK:
            return acquire_memory_sbrk(total_size);
        case MEMORY_SOURCE_MMAP:
            return acquire_memory_mmap(total_size);
        default:
            last_error = ALLOC_ERROR_OUT_OF_MEMORY;
            return NULL;
    }
}

static int release_memory_mmap(void* ptr){
    memory_region_t* region = find_memory_region(ptr);
    if(!region || !region->is_mmap){
        last_error = ALLOC_ERROR_INVALID_POINTER;
        return -1;
    }

    size_t size = region->size;
    if(munmap(ptr, size) == -1){
        return -1;
    }

    unregister_memory_region(ptr);
    return 0;
}

void release_memory(void* ptr, size_t size){
    (void) size;
    if(!ptr)
        return;
    
    memory_region_t* region = find_memory_region(ptr);
    if(!region){
        last_error = ALLOC_ERROR_INVALID_POINTER;
        return;
    }

    if(region->is_mmap){
        release_memory_mmap(ptr);
    }
}