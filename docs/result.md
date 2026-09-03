## Multi-threading Performance
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


## Benchmark
`make benchmark` 
```
allocation throughput / latency  (iters/thread=10000, live window=128)

================ strategy: first-fit ================
size        threads       allocs/sec     p50 ns     p99 ns
--------------------------------------------------------------
16                1          1619862        317        411
16                2          1589072        425        613
16                4          1519602        471        782
16                8          1389781        564        965

64                1          1335350        516        585
64                2          1997965        562        660
64                4          2111466        627        854
64                8          2055761        706        955

256               1           508067       1750       1839
256               2           898907       1749       1902
256               4          1480353       1819       2078
256               8          1937060       1860       2143

512               1           252828       3459       3594
512               2           219257       3898      23395
512               4           177218       3689     106833
512               8           221549       6032     109312


================ strategy: best-fit ================
size        threads       allocs/sec     p50 ns     p99 ns
--------------------------------------------------------------
16                1          2881025        167        177
16                2          3117539        261        337
16                4          1660305        442        744
16                8          1223378        635       1100

64                1           841320        801        939
64                2           959122        894       1287
64                4          1269858       1042       1401
64                8          1217373       1117       1556

256               1           321950       2718       2861
256               2           570756       2740       2981
256               4           891689       2861       3264
256               8          1068003       2860       3396

512               1           163091       5402       5611
512               2           149753       5568      38999
512               4           232740       3691      91620
512               8           224120       4759     110038


================ strategy: worst-fit ================
size        threads       allocs/sec     p50 ns     p99 ns
--------------------------------------------------------------
16                1          3080195        166        209
16                2          3525741        211        272
16                4          2232812        326        557
16                8          1390712        564        963

64                1           963939        722        821
64                2          1228272        888       1058
64                4          1281610       1020       1371
64                8          1214987       1114       1547

256               1           326313       2709       2863
256               2           574702       2732       2987
256               4           999876       2784       3106
256               8          1155297       2888       3389

512               1           163390       5399       5626
512               2           150204       5684      38747
512               4           193825       4368      85148
512               8            90339      11679     281184
```

#### Benchmark analysis

- **Small sizes (16–64 B) ride the thread-local cache.** They hit 1.5–3.5M allocs/sec with p50 latency of 150–700 ns and barely move as threads are added, because almost every request is served from the per-thread bucket without taking `heap_mutex`.
- **`best-fit` / `worst-fit` are fastest at 16 B but degrade past ~2 threads;** `first-fit` is slower there but the most stable under contention (e.g. 256 B / 8 threads: 1.9M allocs/sec for first-fit vs ~1.1M for the others).
- **Throughput scales with thread count only while the cache absorbs the load** once the shared path dominates (512 B), adding threads mostly adds lock contention.

## Fragmentation Analysis
`make fragmentation`
```
external fragmentation analysis  (pool=3000, cycles=300, churn=25%)

strategy        avg%    peak%   final%    free bytes   largest blk   #free blk     fails
-------------------------------------------------------------------------------------------
first-fit      99.18    99.56    99.37        823152          5152         799         0
best-fit       95.89    98.64    94.80        777648         40416         178         0
worst-fit      99.82    99.84    99.84       2415872          3936        1046         0
```

`fragmentation percentage = (total_free − largest_free_block) / total_free` : higher means the heap holds free bytes but no large contiguous run. No strategy hit an allocation failure over 300 churn cycles.

#### Fragmentation analysis

- **`best-fit` fragments the least** (avg 95.9% vs ~99% for the others) and keeps by far the largest contiguous free block — ~40 KB vs ~4–5 KB — in the fewest free-list nodes (178 vs 799–1046). Placing each request in the tightest hole leaves fewer unusable slivers, matching the textbook expectation.
- **`worst-fit` is the worst here:** it always carves the biggest block, so large runs are chewed up early and the free list shatters into 1046 fragments with the largest block down to ~3.9 KB.
- **`first-fit` sits between the two** on fragmentation but is cheap to compute — it scans from the list head and stops at the first hole that fits.
- **Overall:** `best-fit` is the right default when contiguity matters; the throughput cost it pays at high thread counts is hidden anyway once the thread-local cache is handling the common sizes.
