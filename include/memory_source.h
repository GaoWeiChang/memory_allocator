#ifndef MEMORY_SOURCE_H
#define MEMORY_SOURCE_H

#include "./allocator.h"
#include <pthread.h>

#define MMAP_THRESHOLD  (128 * 1024)            // threshold 128 KB
#define PAGE_SIZE   4096                        // 4 KB
#define HEAP_EXTENSION_SIZE   (64 * 1024)       // expand heap by 64 KB


typedef enum{
    MEMORY_SOURCE_SBRK,
    MEMORY_SOURCE_MMAP,
    MEMORY_SOURCE_ERROR
} memory_source_t;

typedef enum {
    ALLOC_SUCCESS = 0,
    ALLOC_ERROR_OUT_OF_MEMORY,
    ALLOC_ERROR_INVALID_SIZE,
    ALLOC_ERROR_INVALID_POINTER
} alloc_error_t;

typedef struct memory_region {
    void* start;
    size_t size;
    bool is_mmap;
    struct memory_region* next;
} memory_region_t;

// global variable for last error
extern _Thread_local alloc_error_t last_error;

void* acquire_memory(size_t total_size);
void release_memory(void* ptr, size_t size);

memory_region_t* find_memory_region(void* ptr);
void memory_source_cleanup(void);

#endif