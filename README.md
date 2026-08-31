# Memory Allocator
A thread-safe memory allocator built from scratch in C to demonstrate systems programming concepts and memory management techniques — hybrid `sbrk`/`mmap` sourcing, boundary-tag coalescing, pluggable allocation strategies, corruption detection, and a thread-local caching layer that removes global-lock contention under concurrent load.

## Table of Contents

- [Architecture Overview](#architecture-overview)
- [Memory Block Layout](#memory-block-layout)
- [Workflows Diagram](#workflows-diagram)
  - [Allocation](#allocation)
  - [Deallocation](#deallocation)
- [Multithreading](#multithreading)
  - [Performance](#performance)
- [Getting Started](#getting-started)
- [Project Structure](#project-structure)
- [Limitation](#limitation)

## Architecture Overview
- **Memory sourcing** — acquires raw memory from the OS via `sbrk()` for small requests (< 128KB) and `mmap()` for large ones, tracked through a region registry so the allocator always knows which address ranges it owns.
- **Block management** — every allocation is wrapped in a fixed-size header plus a footer/canary pair, giving the allocator O(1) neighbor lookups for coalescing and a lightweight corruption check on every free.
- **Free-list allocation** — free blocks live in a doubly-linked list searched by one of four pluggable strategies (First-Fit, Best-Fit, Worst-Fit, Next-Fit).
- **Coalescing** — adjacent free blocks are merged immediately on `free()` to keep external fragmentation low.
- **Integrity checking** — magic numbers, boundary-tag self-checks, and a redzone canary catch heap corruption and buffer overflows early; a full heap walk (`heap_check_consistency()`) can independently verify every block against the free list.
- **Thread safety** — a global mutex protects the shared free list; a per-thread cache layer removes that mutex from the hot path for common allocation sizes.

## Memory Block Layout
<p align="center">
  <img width="832" height="378" alt="memory_block_layout drawio"  src="https://github.com/user-attachments/assets/acd1422f-e3b2-4996-b046-4491ad7e39ff" />
</p>
&emsp;Every block is a contiguous run of four regions: a 32-byte HEADER holding the block's metadata, the payload the caller actually reads and writes, an 8-byte FOOTER that duplicates the block's size (used to walk backward to the previous block during coalescing), and an 8-byte CANARY that detects writes past the end of the payload. <br /> <br />

&emsp;The `mem_alloc()` return value points at the start of the payload, not the header — the header sits invisibly *before* the pointer the caller holds. Internally, the 32-byte header breaks down into five fields: `size` (8 bytes), `is_free` (4 bytes, `_Atomic` — see [Multithreading](#multithreading)), `magic` (4 bytes, corruption sentinel), and the `prev_free`/`next_free` pointers (8 bytes each) used only while the block sits in a free list. The whole block is kept 16-byte aligned end-to-end so every payload address satisfies common alignment requirements without extra work.


## Workflows Diagram
### Allocation
<p align="center">
  <img width="924" height="1144" alt="alloc drawio" src="https://github.com/user-attachments/assets/b90db5b0-ba33-44c2-8cb0-c65452d85f59" />
</p>
`mem_alloc()` checks the calling thread's local cache first: if the requested size rounds up to one of the fixed size classes and that class's cache bucket has a block available, it's popped and returned immediately (no mutex involved). Only when the cache misses does the allocator fall back to the shared path: lock `heap_mutex`, search the global free list using whichever strategy is configured, and either reuse/split a matching block or request fresh memory from the OS (`sbrk` below the 128KB threshold, `mmap` above it).

### Deallocation
<p align="center">
  <img width="1626" height="1723" alt="free drawio" src="https://github.com/user-attachments/assets/4af3c1f2-7206-446c-bec4-5f7242c041ae" />
</p>
`mem_free()` runs a sequence of guard checks before touching the free list: a `NULL` pointer returns immediately; a corrupted block (bad magic, footer, or canary) is rejected with a diagnostic instead of being freed; a block that's already free is rejected as a double-free. Only after passing all three does it try the thread-local cache — if the block's size matches a class exactly and that cache isn't full, it's pushed there directly, again without the mutex. Everything else falls through to the shared path, where `mmap`-backed blocks are returned to the OS immediately via `munmap`, and `sbrk`-backed blocks are coalesced with their neighbors and added back to the global free list.


## Multithreading
<p align="center">
  <img width="641" height="421" alt="thread drawio" src="https://github.com/user-attachments/assets/398b5671-44e7-4239-9ccf-13bbdebff9ca" />
</p>

Each thread owns a private cache covering a handful of common size classes. `mem_alloc()`/`mem_free()` pop and push blocks from this cache directly, bypassing `heap_mutex` entirely for the common case. The cache only touches the shared free list when it's empty (needs a refill) or full (needs to spill excess blocks back), turning what would otherwise be a mutex acquisition on every single allocation into an occasional, amortized cost.
 
A block sitting in a thread's cache is tagged with a third allocation state (distinct from "allocated" and "free-in-global-list") so the consistency checker can recognize it correctly instead of flagging it as lost. When a thread exits, a `pthread_key` destructor automatically flushes its cache back to the global free list, so no memory is orphaned.

### Performance
Multi-threaded stress test: 100,000 `malloc`+`free` operations per thread.
 
| Threads | Without local cache | With local cache |
|---|---|---|
| 1 | 507,306 ops/sec (1.00x) | 669,265 ops/sec (1.00x) |
| 2 | 170,179 ops/sec (0.34x) | 1,762,787 ops/sec (2.63x) |
| 4 | 117,986 ops/sec (0.23x) | 2,459,729 ops/sec (3.68x) |
| 8 | 101,057 ops/sec (0.20x) | 2,663,483 ops/sec (3.98x) |

Without a thread-local cache, every allocation and free contends for the same global mutex — throughput actually *degrades* as threads are added (down to 0.20x at 8 threads) because threads spend most of their time waiting instead of doing useful work. 
<p align="center">
  <img width="958" height="357" alt="without_local_cache" src="https://github.com/user-attachments/assets/520fcb3c-ed29-46eb-a2eb-5b783cb6b835" />
</p>


With the cache, common-size allocations almost never touch the mutex, and throughput scales to nearly 4x at 8 threads — a direct, measured demonstration of why lock-free (or lock-light) fast paths matter in a concurrent allocator.
<p align="center">
  <img width="958" height="365" alt="with_local_cache" src="https://github.com/user-attachments/assets/7a211921-ba69-4050-9ad8-d4966663bad9" />
</p>


## Getting Started
 
```bash
git clone <repository-url>
cd memory_allocator

# Build project
make

# Build and run eveyr test cases
make test

# Check memory leak for every test cases
make valgrind

# Check segmentation fault
make debug TEST=test/test_malloc

# Clean build artifacts
make clean
```

## Project Structure
```
memory_allocator/
├── include/
│   ├── allocator.h        # block header, alignment math, boundary tags, canary
│   ├── memory_source.h    # sbrk/mmap hybrid sourcing, region tracking
│   └── heap.h             # heap state, free list, public allocator API
├── src/
│   ├── memory_source.c
│   └── heap.c
└── tests/
    └── test_*.c           # test cases
```

## Limitation
- No NUMA awareness: the thread-local cache reduces lock contention but doesn't account for memory locality across NUMA nodes.
