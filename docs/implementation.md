# Implementation

This document walks through how the allocator actually works in code , the data structures it keeps, what happens on `mem_alloc()` and `mem_free()`, and the reasoning behind the trade-offs. If `design.md` is the "what and why", this is the "how". Code snippets are lightly trimmed for readability; the real thing lives in `src/` and `include/`.

## Workflows

### Allocation

`mem_alloc()` checks the calling thread's local cache first: if the requested size rounds up to one of the fixed size classes and that class's cache bucket has a block available, it's popped and returned immediately (no mutex involved). The pop is NUMA-aware, the bucket is scanned for a block tagged with the caller's current NUMA node and that one is returned; if none matches, the bucket head is used instead and the hit is counted as remote. Only when the cache misses entirely does the allocator fall back to the shared path: lock `heap_mutex`, search the global free list using whichever strategy is configured, and either reuse/split a matching block or request fresh memory from the OS (`sbrk` below the 128 KB threshold, `mmap` above it). Newly sourced blocks are stamped with the NUMA node they came from.

### Deallocation

`mem_free()` runs a sequence of guard checks before touching the free list: a `NULL` pointer returns immediately; a corrupted block (bad magic, footer, or canary) is rejected with a diagnostic instead of being freed; a block that's already free is rejected as a double-free. Only after passing all three does it try the thread-local cache, if the block's size matches a class exactly and that cache isn't full, it's pushed there directly, again without the mutex. Everything else falls through to the shared path, where `mmap`-backed blocks are returned to the OS immediately via `munmap`, and `sbrk`-backed blocks are coalesced with their neighbours and added back to the global free list.

---

## 1. The block header

Every allocation the caller sees is really the middle slice of a larger structure. In front of the payload sits a fixed 48-byte header, and behind it sits a 16-byte trailer (an 8-byte footer plus an 8-byte canary):

```
[ HEADER (48B) ][ payload (>= 16B) ][ FOOTER (8B) ][ CANARY (8B) ]
                ^
                the pointer we hand back to the caller
```

```c
typedef struct block
{
    size_t size;                   // payload size, not counting header/trailer
    _Atomic block_state_t is_free; // ALLOCATED / FREE_GLOBAL / FREE_THREAD_LOCAL
    uint32_t magic;                // MAGIC_NUMBER (0xDEADBEEF) , corruption sentinel

    struct block *prev_free;       // only meaningful while the block is in a free list
    struct block *next_free;

    int numa_node;                 // which NUMA node this memory was sourced from
    unsigned char _reserved[12];   // pad header to a 16-byte multiple
} block_t;
```

A few decisions worth calling out:

**The caller never sees the header.** `mem_alloc()` returns `(char *)block + HEADER_SIZE`, and `mem_free()` walks back the same distance. Pointer arithmetic is centralised in two inline helpers so the offset math only exists in one place:

```c
static inline void *get_ptr_from_block(block_t *block)
{
    return block ? (void *)((char *)block + HEADER_SIZE) : NULL;
}

static inline block_t *get_block_from_ptr(void *ptr)
{
    return ptr ? (block_t *)((char *)ptr - HEADER_SIZE) : NULL;
}
```

**The footer duplicates `size`.** This is the classic boundary-tag trick: given any block, the previous block's footer sits at `(char *)block - FOOTER_SIZE`, so we can find and merge with the block behind us in O(1) without walking the heap from the start.

```c
static inline block_t *get_prev_block(block_t *block)
{
    size_t prev_size = *(size_t *)((char *)block - FOOTER_SIZE);
    char *prev_addr  = (char *)block - FOOTER_SIZE - prev_size - HEADER_SIZE;
    return (block_t *)prev_addr;
}
```

**The canary is a fixed pattern** (`0xC0FFEEC0FFEEC0FF`) written just past the footer. If a caller writes one byte too many, it clobbers the canary, and the next integrity check catches it.

**Everything is 16-byte aligned end to end**, so every payload pointer we return is already aligned enough for any normal C type , no per-call alignment fixups.

```c
static inline size_t align_size(size_t size)
{
    return (size + ALIGNMENT - 1) & ~((size_t)ALIGNMENT - 1);
}
```

### Integrity checking

`verify_block_integrity()` is called on every `free()` and during every free-list scan. It's a cheap set of sanity checks that turn "undefined behaviour later" into "a clear error message now":

```c
static inline block_status_t verify_block_integrity(block_t *block)
{
    if (!validate_block_address(block))            return BLOCK_MISALIGNED;
    if (block->magic != MAGIC_NUMBER)              return BLOCK_CORRUPT_MAGIC;
    if (block->is_free > BLOCK_FREE_THREAD_LOCAL)  return BLOCK_INVALID_FREE_STATE;
    if (*get_footer_ptr(block) != block->size)     return BLOCK_FOOTER_MISMATCH;
    if (*get_canary_ptr(block) != CANARY_PATTERN)  return BLOCK_CANARY_CORRUPTED;
    return BLOCK_VALID;
}
```

Each failure mode maps to a human-readable diagnosis in `mem_free()` , a footer mismatch or a broken canary means "you overflowed a buffer", a bad magic number means "this probably isn't a heap pointer, or the block in front of you overflowed into this header".

---

## 2. Where raw memory comes from

`memory_source.c` is the only file that talks to the OS. It picks between two sources based on size:

```c
static memory_source_t select_memory_source(size_t size)
{
    if (align_size(size) >= MMAP_THRESHOLD)   // 128 KB
        return MEMORY_SOURCE_MMAP;
    return MEMORY_SOURCE_SBRK;
}
```

**Small requests grow the heap with `sbrk`.** Because `sbrk` is a syscall, we don't call it once per allocation , we grab it in 64 KB chunks (`HEAP_EXTENSION_SIZE`) and carve later requests out of that pool until it runs dry:

```c
// enough left in the current pool , just bump the pointer
if (heap_extension_pool && pool_remaining >= aligned_size)
{
    void *result = heap_extension_pool;
    heap_extension_pool = (char *)heap_extension_pool + aligned_size;
    pool_remaining -= aligned_size;
    if (current_sbrk_region)
        current_sbrk_region->carved_size += aligned_size;
    return result;
}

// pool exhausted , one syscall to refill
size_t extension_size = (aligned_size > HEAP_EXTENSION_SIZE)
                            ? aligned_size : HEAP_EXTENSION_SIZE;
void *new_memory = sbrk((intptr_t)extension_size);
```

**Large requests go straight to `mmap`** and are handed back to the OS with `munmap` the moment they're freed , no point holding a 200 KB region hostage in a free list.

### The region registry

Every chunk we get from the OS `sbrk` or `mmap` , is recorded in a linked list of `memory_region_t`:

```c
typedef struct memory_region
{
    void *start;
    size_t size;        // total capacity from the OS
    size_t carved_size; // how much of it we've handed out as blocks
    bool is_mmap;
    struct memory_region *next;
} memory_region_t;
```

This matters for two things:

1. **`free()` needs to know the block's origin** , an `mmap` block gets `munmap`'d, an `sbrk` block gets coalesced and returned to the free list. `find_memory_region()` answers "which region owns this pointer?".
2. **Coalescing must not cross a region boundary.** Two blocks that happen to be adjacent in memory but live in different `sbrk` extensions are not really neighbours, and merging them would be a bug. Every coalesce candidate is bounds-checked against its owning region first.

---

## 3. The free list and fit strategies

Free `sbrk`-backed blocks live in one global doubly-linked list, threaded through the `prev_free` / `next_free` pointers in the header. Insertion is LIFO , new frees go on the head, which is O(1) and tends to keep recently-used memory hot:

```c
void add_to_free_list(block_t *block)
{
    block->is_free = 1;
    block->prev_free = NULL;
    block->next_free = heap.free_list_head;
    if (heap.free_list_head)
        heap.free_list_head->prev_free = block;
    heap.free_list_head = block;
}
```

Finding a block to reuse is pluggable. All three strategies are a single linear scan; they differ only in which candidate they keep:

```c
// first-fit: stop at the first block big enough
if (current->size >= size)
    return current;

// best-fit: smallest block that still fits (exact match wins immediately)
if (current->size >= size && (!best || current->size < best->size))
{
    best = current;
    if (best->size == size) break;
}

// worst-fit: largest block available
if (current->size >= size && (!worst || current->size > worst->size))
    worst = current;
```

`heap.strategy` selects between them at runtime via `heap_set_strategy()`. **Best-fit is the default** , the fragmentation numbers in `result.md` show it keeps the largest contiguous free block by a wide margin, and its extra scan cost is mostly hidden once the thread-local cache is absorbing the common sizes.

### Splitting

If the block we found is meaningfully bigger than what was asked for, we split off the tail and return it to the free list, rather than waste the remainder:

```c
block_t *split_block(block_t *block, size_t needed_size)
{
    if (!can_split_block(block, needed_size))   // remainder must hold a usable block
        return NULL;

    void *split_addr   = (char *)block + HEADER_SIZE + needed_size + FOOTER_SIZE;
    block_t *new_block  = (block_t *)split_addr;
    size_t remain_size  = block->size - needed_size - HEADER_SIZE - FOOTER_SIZE;

    initialize_free_block(new_block, remain_size);
    new_block->numa_node = block->numa_node;  // inherit origin node

    block->size = needed_size;
    write_footer(block);
    return new_block;
}
```

`can_split_block()` refuses to split unless the leftover can hold a header, a trailer, and at least `MIN_ALLOC_SIZE` of payload , a sliver too small to ever reuse is worse than internal fragmentation.

### Coalescing

The mirror image happens on `free()`: we immediately try to merge with the physical neighbours on both sides so the free list doesn't slowly shatter into unusable fragments.

```c
static bool coalesce_forward(block_t *block, memory_region_t *region)
{
    block_t *next = get_next_block(block);
    if (!is_valid_neighbor(block, region))    return false;
    if (next->is_free != BLOCK_FREE_GLOBAL)   return false;

    remove_from_free_list(next);
    block->size += FOOTER_SIZE + HEADER_SIZE + next->size;
    write_footer(block);
    return true;
}
```

`coalesce_backward()` is the same idea in the other direction, using the previous block's footer to find its header, and it returns the (possibly new) start of the merged block. Note the guard: we only merge with blocks in `BLOCK_FREE_GLOBAL` state , a block sitting in some thread's local cache is off-limits.

---

## 4. Allocation path

`mem_alloc()` has a fast path and a slow path.

```c
void *mem_alloc(size_t size)
{
    if (size == 0) return NULL;

    size_t aligned_size = align_size((size < MIN_ALLOC_SIZE) ? MIN_ALLOC_SIZE : size);

    // --- fast path: thread-local cache, no mutex ---
    int class_idx = size_class_index_ceil(aligned_size);
    if (class_idx >= 0)
    {
        ensure_tls_registered();
        block_t *block = tls_bucket_pop_numa_aware(&tls_cache[class_idx]);
        if (block)
        {
            block->is_free = BLOCK_ALLOCATED;
            return get_ptr_from_block(block);
        }
        atomic_fetch_add(&numa_cache_misses, 1);
    }

    // --- slow path: shared free list, under heap_mutex ---
    pthread_mutex_lock(&heap.heap_mutex);
    block_t *block = find_free_block(aligned_size);
    if (block != NULL)
    {
        void *ptr = allocate_from_free_block(block, aligned_size);
        pthread_mutex_unlock(&heap.heap_mutex);
        return ptr;
    }
    pthread_mutex_unlock(&heap.heap_mutex);

    // --- nothing reusable: ask the OS ---
    return allocate_new_memory(aligned_size);
}
```

Step by step:

1. **Round the request up** to at least `MIN_ALLOC_SIZE` (so a freed block always has room for the two free-list pointers) and to a 16-byte multiple.
2. **Try the thread-local cache.** If `aligned_size` rounds up to one of the eight size classes (`{16, 32, 48, 64, 96, 128, 192, 256}`) and that bucket has a block, pop it and return , no lock, no free-list walk. The pop is NUMA-aware (section 6).
3. **Fall back to the global free list.** Take `heap_mutex`, run the configured fit strategy, and if we get a block, remove it from the list, split off any excess, mark it allocated, and return it:

```c
static void *allocate_from_free_block(block_t *block, size_t size)
{
    remove_from_free_list(block);
    if (!is_poison_intact(block, block->size))
        fprintf(stderr, "Heap warning: use-after-free write at %p\n", ...);

    block_t *remainder = split_block(block, size);
    if (remainder) add_to_free_list(remainder);

    initialize_allocated_block(block, block->size);
    heap.total_allocated += block->size;
    heap.allocation_count++;
    return get_ptr_from_block(block);
}
```

4. **Ask the OS.** `allocate_new_memory()` computes the total footprint (header + payload + trailer, aligned), calls `acquire_memory()`, stamps the new block with its NUMA node, and returns it.

---

## 5. Deallocation path

`mem_free()` is deliberately paranoid before it touches any shared state:

```c
void mem_free(void *ptr)
{
    if (!ptr) return;

    block_t *block = get_block_from_ptr(ptr);

    // 1. reject corruption with a specific diagnosis
    block_status_t status = verify_block_integrity(block);
    if (status != BLOCK_VALID) { /* print what kind of corruption, then bail */ return; }

    // 2. reject double free
    if (block->is_free != BLOCK_ALLOCATED) {
        fprintf(stderr, "Error: block double free at %p\n", ptr);
        return;
    }

    // 3. fast path: push back into the thread-local cache
    int class_idx = size_class_index_exact(block->size);
    if (class_idx >= 0)
    {
        tls_bucket_t *bucket = &tls_cache[class_idx];
        if (bucket->count < MAX_CACHE_PER_CLASS)
        {
            poison_block_payload(block);
            block->is_free = BLOCK_FREE_THREAD_LOCAL;
            block->next_free = bucket->head;
            bucket->head = block;
            bucket->count++;
            /* update stats under the mutex, then return */
            return;
        }
    }

    // 4. slow path
    memory_region_t *region = find_memory_region(block);
    if (region && region->is_mmap) {            // give big blocks straight back
        release_memory(block, region->size);
        return;
    }

    pthread_mutex_lock(&heap.heap_mutex);        // sbrk block: coalesce + relist
    return_block_to_global_free_list(block, region);
    pthread_mutex_unlock(&heap.heap_mutex);
}
```

Notes:

**The guard checks come first, always.** A double free or an overflowed block never makes it into the free list, where it could corrupt the allocator's own bookkeeping and produce a crash far from the real bug.

**Cache push requires an *exact* size-class match** (`size_class_index_exact`), whereas the alloc-side pop rounds up (`size_class_index_ceil`). A 20-byte request is served from the 32-byte bucket, but when freed it carries `size == 32` (it was rounded at alloc time), so it lands back in the same bucket. Blocks that were split to an odd size just skip the cache.

**Freed payloads are poisoned** with `0xDD`. On the next alloc from that block we check the poison is intact; if it isn't, someone wrote through a dangling pointer and we say so.

**`return_block_to_global_free_list()`** re-initialises the block as free, coalesces forward and backward within its region, re-poisons the merged payload, and pushes it on the list head.

---

## 6. Thread-local cache

The global free list is guarded by a single `heap_mutex`. Under one thread that's free; under eight threads doing nothing but `malloc`/`free`, it becomes the entire program , throughput actually *drops* as you add cores (see `result.md`).

The fix is a per-thread cache that sits in front of the lock:

```c
#define NUM_SIZE_CLASSES 8
#define MAX_CACHE_PER_CLASS 32
static const size_t SIZE_CLASSES[NUM_SIZE_CLASSES] = {16, 32, 48, 64, 96, 128, 192, 256};

typedef struct { block_t *head; int count; } tls_bucket_t;
static _Thread_local tls_bucket_t tls_cache[NUM_SIZE_CLASSES];
```

Each thread keeps up to 32 blocks per size class in its own singly-linked buckets. Because `tls_cache` is `_Thread_local`, push and pop touch memory only this thread can see no atomics, no lock. The mutex only comes back into play when a bucket is empty (alloc has to refill from the global list or the OS) or full (free spills the block to the global path).

For the benchmark workload lots of small, short-lived allocations , this means the vast majority of calls never take the lock at all, and throughput scales to ~4x at 8 threads instead of collapsing.

### Not losing memory

A cached block is physically "allocated" from the global allocator's point of view, but logically free. That's why `block_state_t` has a third value:

```c
typedef enum {
    BLOCK_ALLOCATED,
    BLOCK_FREE_GLOBAL,       // in the global free list
    BLOCK_FREE_THREAD_LOCAL  // parked in some thread's cache
} block_state_t;
```

`heap_check_consistency()` uses this to tell "cached" apart from "leaked" when it walks the heap.

And when a thread exits, its cache must not vanish with it. A `pthread_key` destructor flushes every bucket back to the global free list automatically:

```c
static void tls_destructor(void *unused) { heap_tls_cache_flush(); }

static void ensure_tls_registered(void)
{
    pthread_once(&tls_key_once, tls_key_init);
    if (pthread_getspecific(tls_cleanup_key) == NULL)
        pthread_setspecific(tls_cleanup_key, (void *)1); // just needs to be non-NULL
}
```

`heap_tls_cache_flush()` takes the mutex once, then runs each cached block through the normal coalesce-and-relist path.

---

## 7. NUMA-aware cache pops

On a multi-socket machine, memory attached to another socket is slower to reach. Every block already records the node it was sourced from (`numa_node`), so when we pop from a cache bucket we can prefer a block whose memory is local to the core we're running on:

```c
static block_t *tls_bucket_pop_numa_aware(tls_bucket_t *bucket)
{
    if (!bucket->head) return NULL;
    int want = numa_current_node();

    // scan the bucket for a block on our node
    block_t *match = NULL, *match_prev = NULL, *prev = NULL;
    for (block_t *cur = bucket->head; cur; prev = cur, cur = cur->next_free)
        if (cur->numa_node == want) { match = cur; match_prev = prev; break; }

    block_t *block;
    if (match) {                                  // local hit
        block = match;
        (match_prev ? match_prev->next_free : bucket->head) = match->next_free;
        atomic_fetch_add(&numa_local_hits, 1);
    } else {                                       // remote hit, take the head anyway
        block = bucket->head;
        bucket->head = block->next_free;
        atomic_fetch_add(&numa_remote_hits, 1);
    }
    bucket->count--;
    block->next_free = NULL;
    return block;
}
```

The bucket cap is 32, so the scan is bounded and cheap. Worst case (no local block) we fall back to the head exactly like a normal pop , NUMA-awareness never costs us a miss, it just improves locality when it can.

Counters (`local_hits` / `remote_hits` / `cache_misses`) are exposed through `heap_numa_cache_stats()` so tests and benchmarks can see the hit ratio.

### Topology and fake mode

`numa_topology.c` resolves "which node am I on?" , via `getcpu()` on glibc ≥ 2.29, otherwise it assumes a single node. Since most dev machines have one NUMA node, there's a fake topology for testing:

```c
void numa_fake_enable(int num_nodes);         // pretend there are N nodes
void numa_fake_set_current_node(int node);    // this thread "runs on" node X (thread-local)
void numa_fake_set_alloc_node(int node);      // new OS blocks get tagged node Y (thread-local)
```

`fake_current_node` and `fake_alloc_node` are `_Thread_local`, so a multi-threaded test can put each thread on a different node and drive both local and remote hits deterministically.

> **Limitation:** the NUMA paths are exercised only through this fake topology in tests. They have not been benchmarked on real multi-node hardware.

---

## 8. Consistency checking

`heap_check_consistency()` is the independent auditor. Instead of trusting the free list, it walks every physical block in every `sbrk` region using `get_next_block()`, and cross-checks each one:

```c
if (block->is_free == BLOCK_FREE_GLOBAL) {
    result->free_blocks_found++;
    if (!block_in_free_list(block)) result->free_list_mismatches++;  // free but not listed
}
else if (block->is_free == BLOCK_FREE_THREAD_LOCAL) {
    result->thread_cache_blocks_found++;
    if (block_in_free_list(block)) result->free_list_mismatches++;   // cached AND listed = bug
}
else {
    result->allocated_blocks_found++;
    if (block_in_free_list(block)) result->free_list_mismatches++;   // allocated but listed = bug
}
```

The heap is consistent when it finds zero corrupted blocks and zero list mismatches. This is what the test suite asserts on after every stress run.
