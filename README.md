# Memory Allocator
A thread-safe memory allocator built from scratch in C to demonstrate systems programming concepts and memory management techniques — hybrid `sbrk`/`mmap` sourcing, boundary-tag coalescing, pluggable allocation strategies, corruption detection, and a thread-local caching layer that removes global-lock contention under concurrent load.

### Documentation

go to `/docs` to see the related documents.

### Getting Started
 
```bash
git clone <repository-url>
cd memory_allocator

# Build project
make

# Build and run every test cases
make test

# Check memory leak for every test cases
make valgrind

# Check segmentation fault
make debug TEST=test/test_malloc

# Throughput / latency benchmark (per strategy, size, thread count)
make benchmark

# Fragmentation measurement
make fragmentation

# Clean build artifacts
make clean
```

### Project Structure
```
memory_allocator/
├── include/
│   ├── allocator.h        # block header, alignment math, boundary tags, canary
│   ├── memory_source.h    # sbrk/mmap hybrid sourcing, region tracking
│   ├── heap.h             # heap state, free list, public allocator API
│   └── numa_topology.h    # NUMA node query + fake topology for tests
├── src/
│   ├── memory_source.c
│   ├── heap.c
│   └── numa_topology.c
├── test/
│   └── test_*.c           # test cases (malloc, multi-thread, numa)
├── performance/
│   ├── benchmark.c        # throughput / latency matrix
│   └── fragmentation.c    # fragmentation measurement
└── docs/
    ├── design.md          # architecture, block layout, threading, NUMA
    ├── implementation.md  # allocation / deallocation workflows
    └── result.md          # benchmark results
```

### Known Limitation
- NUMA-aware caching is validated only via a fake topology in tests, not on real multi-node hardware.
- Large allocations bypass the thread-local cache. When the size classes larger than 256 bytes, it take the global locked path frequently. Under multi-threaded load this causes significant lock contention.