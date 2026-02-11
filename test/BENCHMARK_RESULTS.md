# Benchmark Results: std::map vs boost::container::flat_map

**Environment:** g++-14 (Ubuntu 14.2.0), -O2, x86-64 Linux  
**Date:** 2026-02-11  
**Source:** `test/source/test_benchmark_map.cpp`

## Summary Tables

### Insertion (ns per operation)

| Size | Keys    | std::map  | flat_map  | Ratio (std/flat) | Winner   |
|------|---------|-----------|-----------|------------------|----------|
| 4    | sorted  | 90.9      | 108.7     | 0.84             | std::map |
| 8    | sorted  | 189.0     | 184.8     | 1.02             | ~tie     |
| 12   | sorted  | 291.0     | 245.5     | 1.19             | flat_map |
| 4    | random  | 96.5      | 111.3     | 0.87             | std::map |
| 8    | random  | 205.3     | 201.4     | 1.02             | ~tie     |
| 12   | random  | 290.0     | 255.9     | 1.13             | flat_map |
| 10000| sorted  | 533,264   | 187,544   | 2.84             | flat_map |
| 10000| random  | 1,365,980 | 8,660,550 | 0.16             | std::map |

### Iteration (ns per full traversal)

| Size | Keys    | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|---------|----------|----------|------------------|----------|
| 4    | sorted  | 13.1     | 2.5      | 5.33             | flat_map |
| 8    | sorted  | 27.1     | 4.1      | 6.65             | flat_map |
| 12   | sorted  | 40.9     | 5.5      | 7.44             | flat_map |
| 4    | random  | 13.4     | 2.5      | 5.43             | flat_map |
| 8    | random  | 34.6     | 4.1      | 8.40             | flat_map |
| 12   | random  | 40.1     | 5.8      | 6.91             | flat_map |
| 10000| sorted  | 53,104   | 6,100    | 8.71             | flat_map |
| 10000| random  | 84,650   | 6,029    | 14.04            | flat_map |

### Random-Order Search (ns per n lookups)

| Size | Insert Keys | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|-------------|----------|----------|------------------|----------|
| 4    | sorted      | 9.7      | 11.5     | 0.84             | std::map |
| 8    | sorted      | 24.5     | 31.0     | 0.79             | std::map |
| 12   | sorted      | 35.5     | 48.1     | 0.74             | std::map |
| 4    | random      | 9.9      | 11.2     | 0.88             | std::map |
| 8    | random      | 24.0     | 29.7     | 0.81             | std::map |
| 12   | random      | 41.3     | 49.6     | 0.83             | std::map |
| 10000| sorted      | 880,588  | 630,098  | 1.40             | flat_map |
| 10000| random      | 770,410  | 638,316  | 1.21             | flat_map |

### Sorted-Order Search (ns per n lookups)

| Size | Insert Keys | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|-------------|----------|----------|------------------|----------|
| 4    | sorted      | 9.9      | 11.5     | 0.87             | std::map |
| 8    | sorted      | 23.6     | 27.6     | 0.86             | std::map |
| 12   | sorted      | 34.7     | 49.8     | 0.70             | std::map |
| 4    | random      | 8.8      | 11.8     | 0.75             | std::map |
| 8    | random      | 22.2     | 28.5     | 0.78             | std::map |
| 12   | random      | 38.2     | 46.8     | 0.82             | std::map |
| 10000| sorted      | 399,471  | 384,337  | 1.04             | ~tie     |
| 10000| random      | 441,741  | 388,595  | 1.14             | flat_map |

## Analysis

### 1. Insertion Performance

- **Small maps (n≤12):** For very small maps (n=4), `std::map` is slightly faster
  because tree insertion with few nodes has low overhead while `flat_map` must
  shift elements in its internal vector. At n=8 they are roughly tied, and by
  n=12 `flat_map` starts to win.

- **Large sorted insertion (n=10000):** `flat_map` is **2.8× faster** because
  appending to a sorted vector is extremely efficient — each key goes at the end
  with amortized O(1) cost, and the contiguous memory layout benefits from
  hardware prefetching.

- **Large random insertion (n=10000):** `std::map` is **6.3× faster** because
  each random insert into a `flat_map` requires O(n) element shifting to
  maintain sorted order, resulting in O(n²) total cost. In contrast, `std::map`
  maintains O(n log n) for any insertion order.

### 2. Iteration Performance

- `flat_map` **dominates iteration** across all sizes, from **5× faster** at n=4
  to **14× faster** at n=10000 with random-order insertion.

- This is entirely due to **cache locality**: `flat_map` stores key-value pairs
  in a contiguous `std::vector`, enabling efficient L1/L2 cache line
  utilization and hardware prefetching. `std::map` uses a red-black tree where
  each node is a separate heap allocation, causing cache misses on every pointer
  chase.

- The advantage grows with map size because larger maps amplify the cache miss
  penalty — the tree nodes of `std::map` become increasingly scattered in
  memory.

### 3. Random-Order Search Performance

- **Small maps (n≤12):** `std::map` is **17–26% faster** for random lookups.
  At these sizes the entire tree fits in L1 cache, and the tree's O(log n)
  pointer-following costs are comparable to `flat_map`'s binary search with
  its branch-misprediction overhead. The overhead of `flat_map`'s comparator
  and index arithmetic slightly exceeds the tree traversal cost.

- **Large maps (n=10000):** `flat_map` is **21–40% faster** because binary
  search on contiguous memory benefits from cache-line prefetching, while
  `std::map`'s tree traversal causes frequent cache misses as nodes are
  scattered across the heap.

### 4. Sorted-Order Search Performance

- **Small maps (n≤12):** `std::map` is **13–30% faster**, similar to the
  random search results. Sequential access patterns don't provide much
  benefit at small sizes since everything fits in cache.

- **Large maps (n=10000):** Results are nearly **tied** when keys were
  inserted in sorted order (ratio=1.04), and `flat_map` wins by **14%**
  when keys were inserted randomly. Sorted sequential search in `std::map`
  benefits from spatial locality in the in-order traversal path through the
  tree, narrowing the gap compared to random search.

### 5. Key Takeaways

| Use Case | Recommendation |
|----------|---------------|
| Small maps (≤12 elements) with frequent lookups | **std::map** — 15-25% faster search |
| Small maps (≤12 elements) with frequent iteration | **flat_map** — 5-7× faster iteration |
| Large maps with sorted/sequential insertion | **flat_map** — faster insert (2.8×), iteration (8.7×), and search (1.4×) |
| Large maps with random insertion | **std::map** — 6× faster insert; use flat_map only if iteration/search dominate |
| Iteration-heavy workloads at any size | **flat_map** — 5-14× faster due to cache locality |
| Mixed workloads with large random inserts + searches | **Depends** — consider building flat_map from sorted data if possible |

### 6. Recommendation for gtopt

The `gtopt` project currently uses `boost::container::flat_map` as its default
(via `gtopt::flat_map` in `fmap.hpp`). This is a **good default** for the
typical optimization/modeling workload where:

- Maps are often built once (frequently from sorted data) and then read many
  times for iteration and lookup
- Cache locality during iteration over problem coefficients is critical for
  solver performance
- The 5-14× iteration speedup far outweighs the search overhead for small maps

For cases where large maps need random-order construction, consider sorting the
input data first and using ordered insertion, or building a `std::map` and
converting to `flat_map` after construction is complete.
