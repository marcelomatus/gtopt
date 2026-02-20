# Benchmark Results: std::map vs boost::container::flat_map

**Environment:** g++-14 (Ubuntu 14.2.0), Release (-O3), x86-64 Linux
**Date:** 2026-02-20
**Source:** `test/source/test_benchmark_map.cpp`

> **Note on `std::flat_map`:** The `<flat_map>` header (C++23) is not yet shipped
> by GCC 14's libstdc++; it will be available in GCC 15+. The benchmarks below
> therefore compare `std::map` (node-based) vs `boost::container::flat_map`
> (contiguous vector). The project's `fmap.hpp` abstraction already supports
> switching to `std::flat_map` via `GTOPT_USE_STD_FLAT_MAP` once available.

## Summary Tables

### Insertion (ns per operation)

| Size | Keys   | std::map  | flat_map  | Ratio (std/flat) | Winner   |
|------|--------|-----------|-----------|------------------|----------|
| 4    | sorted | 88        | 101       | 0.87             | std::map |
| 8    | sorted | 194       | 190       | 1.02             | ~tie     |
| 12   | sorted | 315       | 225       | 1.40             | flat_map |
| 500  | sorted | 20,897    | 8,555     | 2.44             | flat_map |
| 4    | random | 170       | 101       | 1.69             | flat_map |
| 8    | random | 218       | 168       | 1.30             | flat_map |
| 12   | random | 358       | 219       | 1.64             | flat_map |
| 500  | random | 23,253    | 25,583    | 0.91             | std::map |

### Iteration (ns per full traversal)

| Size | Keys   | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|--------|----------|----------|------------------|----------|
| 4    | sorted | 16       | 1.6      | 9.8×             | flat_map |
| 8    | sorted | 32       | 2.2      | 14.6×            | flat_map |
| 12   | sorted | 49       | 2.8      | 17.3×            | flat_map |
| 500  | sorted | 2,326    | 85       | 27.4×            | flat_map |
| 4    | random | 16       | 1.6      | 10.0×            | flat_map |
| 8    | random | 32       | 2.2      | 14.6×            | flat_map |
| 12   | random | 48       | 2.8      | 17.2×            | flat_map |
| 500  | random | 2,337    | 84       | 27.7×            | flat_map |

### Random-Order Search (ns per n lookups)

| Size | Insert Keys | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|-------------|----------|----------|------------------|----------|
| 4    | sorted      | 11       | 9.4      | 1.17             | flat_map |
| 8    | sorted      | 34       | 27       | 1.26             | flat_map |
| 12   | sorted      | 51       | 38       | 1.34             | flat_map |
| 500  | sorted      | 6,429    | 5,966    | 1.08             | flat_map |
| 4    | random      | 13       | 9.3      | 1.37             | flat_map |
| 8    | random      | 30       | 27       | 1.12             | flat_map |
| 12   | random      | 47       | 46       | 1.02             | ~tie     |
| 500  | random      | 6,770    | 5,874    | 1.15             | flat_map |

### Sorted-Order Search (ns per n lookups)

| Size | Insert Keys | std::map | flat_map | Ratio (std/flat) | Winner   |
|------|-------------|----------|----------|------------------|----------|
| 4    | sorted      | 21       | 9.1      | 2.33             | flat_map |
| 8    | sorted      | 49       | 43       | 1.15             | flat_map |
| 12   | sorted      | 50       | 38       | 1.34             | flat_map |
| 500  | sorted      | 6,434    | 5,310    | 1.21             | flat_map |
| 4    | random      | 14       | 9.0      | 1.52             | flat_map |
| 8    | random      | 31       | 29       | 1.07             | flat_map |
| 12   | random      | 47       | 37       | 1.26             | flat_map |
| 500  | random      | 6,054    | 5,120    | 1.18             | flat_map |

## Analysis

### 1. Insertion Performance

- **Small sorted maps (n≤12):** At n=4 `std::map` is 13% faster; they are tied at
  n=8; `flat_map` pulls ahead by 40% at n=12. When keys arrive in sorted order,
  `flat_map` appends to the end of its internal vector at essentially O(1) amortised
  cost, so the gap widens steadily with size.

- **Small random maps (n≤12):** Counter-intuitively, `flat_map` is **30–70% faster**
  even for random insertion at these sizes. The physical element shifts needed to
  maintain sort order are only a few bytes at n≤12 and fit entirely in L1 cache,
  while `std::map`'s heap-allocated tree nodes incur pointer-chasing overhead that
  dominates at small counts.

- **Large sorted (n=500):** `flat_map` is **2.4× faster** — sequential appends are
  cheap and benefit from hardware prefetching.

- **Large random (n=500):** `std::map` is **9% faster** (effectively a tie). Each
  random insert into `flat_map` requires shifting up to n/2 elements on average,
  making total cost O(n²). At n=500 that overhead becomes measurable; at n>1000 it
  grows significantly. Build with sorted data or use `std::map` → `flat_map`
  conversion when random large-scale construction is needed.

### 2. Iteration Performance

`flat_map` **dominates iteration at every size and key order**, ranging from
**10× faster** at n=4 to **27× faster** at n=500. The advantage is entirely due to
**cache locality**: `flat_map` stores all key-value pairs in a contiguous vector,
allowing the CPU prefetcher to stream data efficiently. `std::map`'s red-black tree
scatters each node across the heap, causing a cache miss on every pointer dereference.
This gap is the single most important performance characteristic for gtopt.

### 3. Search Performance

`flat_map` is **faster across the board** in this run — contrary to older benchmarks
collected in Debug mode that showed `std::map` winning for small random lookups.
In Release mode, binary search on contiguous memory outperforms tree traversal even
for n=4 (1.2–1.4× faster), and the advantage grows to 1.1–1.3× at n=12 and
n=500. The branch predictor benefits from the regular access pattern of binary
search, while tree traversal suffers from data-dependent branches.

### 4. `reserve()` Effect on `flat_map` Insertion

| Size  | Keys   | No Reserve (ns) | With Reserve (ns) | Speedup |
|-------|--------|-----------------|-------------------|---------|
| 4     | sorted | 99              | 34                | **2.9×** |
| 8     | sorted | 169             | 72                | **2.3×** |
| 12    | sorted | 226             | 96                | **2.3×** |
| 4     | random | 101             | 33                | **3.1×** |
| 8     | random | 169             | 67                | **2.5×** |
| 12    | random | 236             | 98                | **2.4×** |
| 500   | sorted | 8,613           | 8,058             | 1.07     |
| 500   | random | 26,656          | 25,306            | 1.05     |

- **Small maps (n≤12):** `reserve()` delivers a **2.3–3.1× speedup** regardless of
  key order. Pre-allocating the internal vector eliminates the repeated reallocations
  and element copies that occur during organic growth. For n=4 a flat_map without
  reserve typically reallocates 2–3 times (capacity 1→2→4); reserving the exact size
  reduces this to zero, which more than halves the total cost.

- **Large maps (n=500):** The gain is marginal (~5–7%). At n=500, the vector grows
  logarithmically (≈9 doublings), so reallocation overhead is already amortised. The
  bottleneck for sorted insertion is memory bandwidth and for random insertion is
  element shifting — neither is improved by `reserve()`.

**Conclusion: always call `reserve()` for small maps when the final size is known.**
The `gtopt::map_reserve()` wrapper in `fmap.hpp` handles this uniformly across all
map backends (boost, std::flat_map, unordered_map).

## Recommendation for gtopt

**`boost::container::flat_map` (the current default) is the right choice** for this
project. The analysis is unambiguous across all use cases present in gtopt:

| Use case in gtopt | Typical size | Access pattern | Winner |
|---|---|---|---|
| `SparseRow::cmap_t` — LP row coefficients | 2–12 | Random insert; iterated at solve | **flat_map** |
| `Collection::uid_map_t` — UID→index | 10–500 | Built once; read-many lookups | **flat_map** |
| `IndexHolder::index_map_t` — scenario/stage/block | 4–50 | Built once; iterated | **flat_map** |
| `state_variable_map_t` — per-phase variables | 4–50 | Built once; iterated | **flat_map** |

Key reasons:

1. **Iteration is the dominant cost in LP solve** (coefficient accumulation, row
   traversal, output writing). `flat_map` is **10–27× faster** here at all sizes.

2. **Search is faster across the board** in Release mode (1.1–2.3×).

3. **Insertion is faster or equal** for the small random maps typical of LP rows
   (1.3–1.7× faster at n=8–12).

4. **`reserve()` is transformative** for small maps (2.3–3.1×): the project already
   uses `gtopt::map_reserve()` throughout to exploit this.

The only case where `std::map` wins is **large random-order insertion (n≥500)**,
which does not represent a common pattern in gtopt — LP rows are small, and larger
maps (UID lookups) are built sequentially from parsed input.

### When `std::flat_map` becomes available (GCC 15+)

`std::flat_map` stores keys and values in two separate vectors (unlike
`boost::flat_map`'s single pair-vector), which gives better iteration performance
for value-only traversals and enables `extract()` / `replace()` for O(1) bulk
operations. The project's `map_reserve()` overload for `std::flat_map` already
uses `extract()`/`replace()` correctly. Switch by defining `GTOPT_USE_STD_FLAT_MAP`
once GCC 15 is available in CI — no other code changes are required.
