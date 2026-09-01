#include "../include/heap.h"
#include "../include/numa_topology.h"
#include <stdio.h>
#include <string.h>

/* thread-local cache */
#define NUM_SIZE_CLASSES 8
#define MAX_CACHE_PER_CLASS 32

static const size_t SIZE_CLASSES[NUM_SIZE_CLASSES] = {16, 32, 48, 64, 96, 128, 192, 256};

typedef struct
{
    block_t *head;
    int count;
} tls_bucket_t;

static _Thread_local tls_bucket_t tls_cache[NUM_SIZE_CLASSES];

/* NUMA-aware cache accounting (aggregated across threads) */
static _Atomic unsigned long numa_local_hits = 0;
static _Atomic unsigned long numa_remote_hits = 0;
static _Atomic unsigned long numa_cache_misses = 0;

heap_numa_stats_t heap_numa_cache_stats(void)
{
    heap_numa_stats_t s;
    s.local_hits = atomic_load(&numa_local_hits);
    s.remote_hits = atomic_load(&numa_remote_hits);
    s.cache_misses = atomic_load(&numa_cache_misses);
    return s;
}

void heap_numa_cache_stats_reset(void)
{
    atomic_store(&numa_local_hits, 0);
    atomic_store(&numa_remote_hits, 0);
    atomic_store(&numa_cache_misses, 0);
}

/*
    Pop a block from a thread-local cache bucket, preferring one whose memory
        lives on the caller's current NUMA node. 
    Falls back to the head of the list (any node) when no local block is cached. 
    Returns NULL when the bucket is empty.
*/
static block_t *tls_bucket_pop_numa_aware(tls_bucket_t *bucket)
{
    if (!bucket->head)
        return NULL;

    int want = numa_current_node();

    block_t *prev = NULL;
    block_t *match_prev = NULL;
    block_t *match = NULL;

    // find block that match current NUMA node 
    for (block_t *cur = bucket->head; cur; prev = cur, cur = cur->next_free)
    {
        if (cur->numa_node == want)
        {
            match = cur;
            match_prev = prev;
            break;
        }
    }

    block_t *block;
    if (match)
    {
        block = match;
        if (match_prev)
            match_prev->next_free = match->next_free;
        else
            bucket->head = match->next_free;
        atomic_fetch_add(&numa_local_hits, 1);
    }
    else
    {
        // fall back
        block = bucket->head;
        bucket->head = block->next_free;
        if (block->numa_node == want || want == NUMA_NODE_UNKNOWN)
            atomic_fetch_add(&numa_local_hits, 1);
        else
            atomic_fetch_add(&numa_remote_hits, 1);
    }

    bucket->count--;
    block->next_free = NULL;
    return block;
}

// use when free()/push in cache only
static int size_class_index_exact(size_t size)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    {
        if (size == SIZE_CLASSES[i])
            return i;
    }
    return -1;
}

// use when malloc()/pop from cache only
static int size_class_index_ceil(size_t size)
{
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    {
        if (size <= SIZE_CLASSES[i])
            return i;
    }
    return -1;
}

heap_state_t heap = {
    .free_list_head = NULL,
    .heap_mutex = PTHREAD_MUTEX_INITIALIZER,
    .strategy = STRATEGY_FIRST_FIT,
    .next_fit_cursor = NULL,
    .total_allocated = 0,
    .allocation_count = 0,
    .free_count = 0};

void heap_set_strategy(strategy_t strategy)
{
    pthread_mutex_lock(&heap.heap_mutex);
    heap.strategy = strategy;
    heap.next_fit_cursor = NULL;
    pthread_mutex_unlock(&heap.heap_mutex);
}

void remove_from_free_list(block_t *block)
{
    /*
        if the next-fit cursor points to this block,
        advance it to another free block before removal
    */
    if (heap.next_fit_cursor == block)
    {
        heap.next_fit_cursor = block->next_free ? block->next_free : heap.free_list_head;
        if (heap.next_fit_cursor == block)
        {
            heap.next_fit_cursor = NULL;
        }
    }

    // remove block
    if (block->prev_free)
    {
        block->prev_free->next_free = block->next_free;
    }
    else
    {
        heap.free_list_head = block->next_free;
    }

    if (block->next_free)
    {
        block->next_free->prev_free = block->prev_free;
    }

    block->prev_free = NULL;
    block->next_free = NULL;
}

void add_to_free_list(block_t *block)
{
    block->is_free = 1;
    block->prev_free = NULL;
    block->next_free = heap.free_list_head;

    if (heap.free_list_head)
    {
        heap.free_list_head->prev_free = block;
    }
    heap.free_list_head = block;
}

block_t *split_block(block_t *block, size_t needed_size)
{
    if (!can_split_block(block, needed_size))
        return NULL;

    void *split_addr = (char *)block + HEADER_SIZE + needed_size + FOOTER_SIZE;
    block_t *new_block = (block_t *)split_addr;
    size_t remain_size = block->size - needed_size - HEADER_SIZE - FOOTER_SIZE;
    initialize_free_block(new_block, remain_size);
    new_block->numa_node = block->numa_node;

    block->size = needed_size;
    write_footer(block);

    return new_block;
}

block_t *find_free_block_first_fit(size_t size)
{
    block_t *current = heap.free_list_head;

    while (current)
    {
        if (verify_block_integrity(current) != BLOCK_VALID)
        {
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void *)current);
            return NULL;
        }

        if (current->size >= size)
        {
            return current;
        }
        current = current->next_free;
    }

    return NULL;
}

block_t *find_free_block_best_fit(size_t size)
{
    block_t *current = heap.free_list_head;
    block_t *best = NULL;

    while (current)
    {
        if (verify_block_integrity(current) != BLOCK_VALID)
        {
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void *)current);
            return NULL;
        }

        // NOTE !!!! why use || instead of &&?
        if (current->size >= size)
        {
            if (!best || current->size < best->size)
            {
                best = current;
                if (best->size == size)
                    break;
            }
        }
        current = current->next_free;
    }

    return best;
}

block_t *find_free_block_worst_fit(size_t size)
{
    block_t *current = heap.free_list_head;
    block_t *worst = NULL;

    while (current)
    {
        if (verify_block_integrity(current) != BLOCK_VALID)
        {
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void *)current);
            return NULL;
        }

        if (current->size >= size)
        {
            if (!worst || current->size > worst->size)
            {
                worst = current;
            }
        }
        current = current->next_free;
    }

    return worst;
}

block_t *find_free_block_next_fit(size_t size)
{
    if (!heap.free_list_head)
        return NULL;

    block_t *start = heap.next_fit_cursor ? heap.next_fit_cursor : heap.free_list_head;
    block_t *current = start;

    while (current)
    {
        if (verify_block_integrity(current) != BLOCK_VALID)
        {
            fprintf(stderr, "Error: found free-list corruption at %p\n", (void *)current);
            return NULL;
        }

        if (current->size >= size)
        {
            heap.next_fit_cursor = current->next_free ? current->next_free : heap.free_list_head;
            return current;
        }

        current = current->next_free;
        if (current == start)
            break;
    }

    return NULL;
}

block_t *find_free_block(size_t size)
{
    block_t *result;
    switch (heap.strategy)
    {
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
static void poison_block_payload(block_t *block)
{
    memset(get_ptr_from_block(block), POISON_BYTE, block->size);
}

static bool is_poison_intact(block_t *block, size_t len)
{
    unsigned char *ptr = (unsigned char *)get_ptr_from_block(block);
    for (size_t i = 0; i < len; i++)
    {
        if (ptr[i] != POISON_BYTE)
            return false;
    }

    return true;
}

static void *allocate_from_free_block(block_t *block, size_t size)
{
    // remove block from free list
    remove_from_free_list(block);

    if (!is_poison_intact(block, block->size))
    {
        fprintf(stderr, "Heap warning: Found write memory overlapped after free() (use-after-free) at %p\n",
                get_ptr_from_block(block));
    }

    block_t *remainder = split_block(block, size);
    if (remainder)
    {
        add_to_free_list(remainder);
    }

    initialize_allocated_block(block, block->size);

    heap.total_allocated += block->size;
    heap.allocation_count++;

    return get_ptr_from_block(block);
}

static void *allocate_new_memory(size_t size)
{
    size_t total_size = HEADER_SIZE + size + FOOTER_SIZE;
    size_t aligned_total = align_size(total_size);
    size_t padding = aligned_total - total_size;
    size_t payload_size = size + padding;

    void *memory = acquire_memory(aligned_total);
    if (!memory)
        return NULL;

    block_t *block = (block_t *)memory;

    pthread_mutex_lock(&heap.heap_mutex);
    initialize_allocated_block(block, payload_size);
    block->numa_node = numa_node_for_new_block();
    heap.total_allocated += payload_size;
    heap.allocation_count++;
    pthread_mutex_unlock(&heap.heap_mutex);

    return get_ptr_from_block(block);
}

static bool is_valid_neighbor(block_t *candidate, memory_region_t *owner_region)
{
    if (!candidate || !owner_region)
        return false;

    char *region_start = (char *)owner_region->start;
    char *region_end = region_start + owner_region->size;

    if ((char *)candidate < region_start)
        return false;

    if ((char *)candidate + HEADER_SIZE > region_end)
        return false;

    if (verify_block_integrity(candidate) != BLOCK_VALID)
        return false;

    if ((char *)candidate + HEADER_SIZE + candidate->size + FOOTER_SIZE > region_end)
        return false;

    return true;
}

// merge with the next free block if possible
static bool coalesce_forward(block_t *block, memory_region_t *region)
{
    block_t *next = get_next_block(block);

    if (!is_valid_neighbor(block, region))
        return false;

    if (next->is_free != BLOCK_FREE_GLOBAL)
        return false;

    remove_from_free_list(next);

    block->size = block->size + FOOTER_SIZE + HEADER_SIZE + next->size;
    write_footer(block);

    return true;
}

// merge with the previous free block and return the new starting block
static block_t *coalesce_backward(block_t *block, memory_region_t *region)
{
    char *region_start = (char *)region->start;

    if ((char *)block - FOOTER_SIZE < region_start)
        return block;

    block_t *prev = get_prev_block(block);
    if (!is_valid_neighbor(prev, region) || !prev->is_free)
        return block;

    if (prev->is_free != BLOCK_FREE_GLOBAL)
        return block;

    remove_from_free_list(prev);

    prev->size = prev->size + FOOTER_SIZE + HEADER_SIZE + block->size;
    write_footer(prev);

    return prev;
}

/*
    pthread key to automatically return cached blocks to the global free list upon thread termination,
    requiring no manual cleanup from the user
*/
static pthread_key_t tls_cleanup_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;

static void tls_destructor(void *unused)
{
    (void)unused;
    heap_tls_cache_flush();
}

static void tls_key_init(void)
{
    pthread_key_create(&tls_cleanup_key, tls_destructor);
}

static void ensure_tls_registered(void)
{
    pthread_once(&tls_key_once, tls_key_init);
    if (pthread_getspecific(tls_cleanup_key) == NULL)
    {
        pthread_setspecific(tls_cleanup_key, (void *)1);
    }
}

size_t heap_tls_cache_count(void)
{
    size_t total = 0;
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    {
        total += (size_t)tls_cache[i].count;
    }

    return total;
}
void *mem_alloc(size_t size)
{
    if (size == 0)
        return NULL;

    // alignment
    size_t actual_size = (size < MIN_ALLOC_SIZE) ? MIN_ALLOC_SIZE : size;
    size_t aligned_size = align_size(actual_size);

    // search in thread local cache
    int class_idx = size_class_index_ceil(aligned_size);
    if (class_idx >= 0)
    {
        ensure_tls_registered();
        tls_bucket_t *bucket = &tls_cache[class_idx];

        block_t *block = tls_bucket_pop_numa_aware(bucket);
        if (block)
        {
            // check use-after-free
            if (!is_poison_intact(block, block->size))
            {
                fprintf(stderr, "Heap warning: Found write memory overlapped after free() (use-after-free) at %p\n",
                        get_ptr_from_block(block));
            }

            block->is_free = BLOCK_ALLOCATED;
            return get_ptr_from_block(block);
        }

        atomic_fetch_add(&numa_cache_misses, 1);
    }

    pthread_mutex_lock(&heap.heap_mutex);

    // search in global free list
    block_t *block = find_free_block(aligned_size);
    if (block != NULL)
    {
        void *ptr = allocate_from_free_block(block, aligned_size);
        pthread_mutex_unlock(&heap.heap_mutex);
        return ptr;
    }

    pthread_mutex_unlock(&heap.heap_mutex);

    // if not found in free list, allocate new memory
    return allocate_new_memory(aligned_size);
}

static void return_block_to_global_free_list(block_t *block, memory_region_t *known_region)
{
    memory_region_t *region = known_region ? known_region : find_memory_region(block);
    initialize_free_block(block, block->size);

    if (region)
    {
        coalesce_forward(block, region);
        block = coalesce_backward(block, region);
    }

    poison_block_payload(block);
    add_to_free_list(block);
}

void heap_tls_cache_flush(void)
{
    pthread_mutex_lock(&heap.heap_mutex);

    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    {
        tls_bucket_t *bucket = &tls_cache[i];
        block_t *cur = bucket->head;

        while (cur)
        {
            block_t *next = cur->next_free;
            return_block_to_global_free_list(cur, NULL);
            cur = next;
        }

        bucket->head = NULL;
        bucket->count = 0;
    }

    pthread_mutex_unlock(&heap.heap_mutex);
}

void mem_free(void *ptr)
{
    if (!ptr)
        return;

    // locate and verify block
    block_t *block = get_block_from_ptr(ptr);
    block_status_t status = verify_block_integrity(block);
    if (status != BLOCK_VALID)
    {
        fprintf(stderr, "[heap] free(): Invalid block (status=%d) at %p", status, ptr);
        switch (status)
        {
        case BLOCK_FOOTER_MISMATCH:
            fprintf(stderr, " -> Heap buffer overflow detected (footer corrupted/overwritten)\n");
            break;
        case BLOCK_CANARY_CORRUPTED:
            fprintf(stderr, " -> Heap buffer overflow detected (canary corrupted/overwritten)\n");
            break;
        case BLOCK_CORRUPT_MAGIC:
            fprintf(stderr, " -> Magic number corrupted (possible invalid pointer passed to free() or overflow from previous block)\n");
            break;
        default:
            fprintf(stderr, "\n");
        }
        return;
    }

    if (block->is_free != BLOCK_ALLOCATED)
    {
        fprintf(stderr, "Error: block double free at %p\n", ptr);
        return;
    }

    // push free block in local thread cache
    int class_idx = size_class_index_exact(block->size);
    if (class_idx >= 0)
    {
        ensure_tls_registered();
        tls_bucket_t *bucket = &tls_cache[class_idx];

        // if cache didnt full push free block in cache,
        // when cache full add in global free list
        if (bucket->count < MAX_CACHE_PER_CLASS)
        {
            poison_block_payload(block);
            block->is_free = BLOCK_FREE_THREAD_LOCAL;
            block->next_free = bucket->head;
            bucket->head = block;
            bucket->count++;

            pthread_mutex_lock(&heap.heap_mutex);
            heap.total_allocated -= block->size;
            heap.free_count++;
            pthread_mutex_unlock(&heap.heap_mutex);
            return;
        }
    }

    // check memory allocate from sbrk or mmap
    memory_region_t *region = find_memory_region(block);

    // block from mmap
    if (region && region->is_mmap)
    {
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
    return_block_to_global_free_list(block, region);
    pthread_mutex_unlock(&heap.heap_mutex);
}

// return usable size of the block (the return will more than or equal )
size_t mem_alloc_usable_size(void *ptr)
{
    if (!ptr)
        return 0;

    block_t *block = get_block_from_ptr(ptr);
    if (verify_block_integrity(block) != BLOCK_VALID)
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
static bool block_in_free_list(block_t *target)
{
    block_t *cur = heap.free_list_head;
    while (cur)
    {
        if (cur == target)
            return true;
        cur = cur->next_free;
    }

    return false;
}

static void check_region_visitor(const memory_region_t *region, void *ctx_ptr)
{
    heap_check_result_t *result = (heap_check_result_t *)ctx_ptr;

    if (region->is_mmap)
    {
        // 1 block per region for mmap()
        block_t *block = (block_t *)region->start;
        result->block_checked++;

        if (verify_block_integrity(block) != BLOCK_VALID)
        {
            result->corrupted_blocks++;
            return;
        }

        if (block->is_free != BLOCK_ALLOCATED)
        {
            result->free_list_mismatches++;
        }
        else
        {
            result->allocated_blocks_found++;
        }
        return;
    }

    char *region_end = (char *)region->start + region->carved_size;
    block_t *block = (block_t *)region->start;

    while ((char *)block < region_end)
    {
        result->block_checked++;

        if (verify_block_integrity(block) != BLOCK_VALID)
        {
            result->corrupted_blocks++;
            break;
        }

        bool in_free_list = block_in_free_list(block);
        if (block->is_free == BLOCK_FREE_GLOBAL)
        {
            result->free_blocks_found++;
            if (!in_free_list)
                result->free_list_mismatches++;
        }
        else if (block->is_free == BLOCK_FREE_THREAD_LOCAL)
        {
            result->thread_cache_blocks_found++;
            if (in_free_list)
                result->free_list_mismatches++;
        }
        else
        {
            result->allocated_blocks_found++;
            if (in_free_list)
                result->free_list_mismatches++;
        }

        block = get_next_block(block);
    }
}

heap_check_result_t heap_check_consistency(void)
{
    heap_check_result_t result = {0, 0, 0, 0, 0, 0, true};

    pthread_mutex_lock(&heap.heap_mutex);
    memory_source_for_each_region(check_region_visitor, &result);
    pthread_mutex_unlock(&heap.heap_mutex);

    result.heap_is_consistent = (result.corrupted_blocks == 0 && result.free_list_mismatches == 0);
    return result;
}

// dump allocator state for debugging

static const char *block_state_label(block_t *block)
{
    if (block->is_free == BLOCK_FREE_GLOBAL)
        return "FREE";
    if (block->is_free == BLOCK_FREE_THREAD_LOCAL)
        return "cached";

    return "used";
}

static void dump_region_visitor(const memory_region_t *region, void *ctx)
{
    (void)ctx;
    printf("region %p (%zu bytes, %s)\n",
           region->start, region->size, region->is_mmap ? "mmap" : "sbrk");

    if (region->is_mmap)
    {
        block_t *block = (block_t *)region->start;
        printf("[%p] size=%-8zu %s\n",
               (void *)block, block->size, block->is_free ? "FREE" : "used");
        return;
    }

    char *region_end = (char *)region->start + region->carved_size;
    block_t *block = (block_t *)region->start;

    while ((char *)block < region_end)
    {
        if (verify_block_integrity(block) != BLOCK_VALID)
        {
            printf("[%p] CORRUPTED\n", (void *)block);
            break;
        }
        printf("[%p] size=%-8zu %s\n", (void *)block, block->size, block_state_label(block));
        block = get_next_block(block);
    }
}

void heap_reset(void)
{
    pthread_mutex_lock(&heap.heap_mutex);
    heap.free_list_head = NULL;
    heap.total_allocated = 0;
    heap.allocation_count = 0;
    heap.free_count = 0;
    pthread_mutex_unlock(&heap.heap_mutex);

    // clear thread local cache
    for (int i = 0; i < NUM_SIZE_CLASSES; i++)
    {
        tls_cache[i].head = NULL;
        tls_cache[i].count = 0;
    }

    heap_numa_cache_stats_reset();

    memory_source_cleanup();
}

void heap_dump(void)
{
    printf("===== Heap Dump =====\n");
    pthread_mutex_lock(&heap.heap_mutex);
    memory_source_for_each_region(dump_region_visitor, NULL);
    pthread_mutex_unlock(&heap.heap_mutex);
    printf("======================\n");
}