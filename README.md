# lob-engine

A single-threaded limit order book and matching engine in C++20. It supports limit, market, IOC and
fill-or-kill orders, cancels, and modifies, matches with price-time priority, and emits trade and
price-level update events. The book is a class template over its price-level container, so the
same matching code runs on a `std::map` level book and on a flat, array-indexed level book, and the
benchmark compares the two directly. I built it to learn how exchange matching engines are
structured and how to test one properly. Correctness is checked by a randomized differential test:
millions of generated requests go to both engines and to a deliberately simple reference book, and
every result, trade and level update, plus periodic full-book snapshots, must match exactly.

## Results

All numbers below are copied from files in [`results/`](results), which the commands in
[Reproduce](#reproduce) regenerate.

### Correctness

| check | result | source |
|---|---|---|
| Differential test, 4 request mixes x 3 seeds x 1,000,000 requests | 12,000,000 requests, 2,458,742 trades and 11,088,470 level updates compared one by one across the reference, map and flat books; 12,000 full-book snapshot comparisons; 0 mismatches | [`difftest.json`](results/difftest.json) |
| Mutation check: 9 hand-injected bugs (FIFO to LIFO, off-by-one at the limit price, missing event, broken hash-map deletion, ...) | 9 / 9 caught by the differential test | [`mutation_check.md`](results/mutation_check.md) |
| Unit tests (GoogleTest) | 71 test instances: 19 behaviour tests run against all three books, plus engine-only, flat-book and container tests | `tests/` |

Of the 12,000,000 difftest requests, 2,071,152 were rejected on purpose: 1,997,640 of them are
cancels of orders that had already been filled (`UnknownId`), and the rest come from the scenario
that injects invalid ids, quantities, prices and duplicate ids. CI runs the unit tests and a shorter
difftest with gcc and clang, in Release and in Debug with AddressSanitizer + UndefinedBehaviorSanitizer.

### Performance

2,000,000 generated requests per run. Throughput is the median of 11 runs per book, with map and
flat runs alternating, plus the min-max range. Latency comes from one extra run that reads the CPU
timestamp counter around every request.

| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|---|
| add_cancel | map | 7.16 (5.68-11.85) | 75 | 277 | 1180 |
| add_cancel | flat | 11.38 (8.87-19.62) | 55 | 200 | 804 |
| balanced | map | 11.31 (10.05-13.36) | 69 | 316 | 1053 |
| balanced | flat | 15.20 (12.25-19.06) | 51 | 224 | 884 |
| match_heavy | map | 10.40 (7.86-12.17) | 67 | 382 | 1254 |
| match_heavy | flat | 13.95 (8.07-18.72) | 50 | 260 | 1012 |

Request mix actually generated (fractions from `bench.json`):

| workload | limit add | cancel | modify | market | IOC | FOK | trades |
|---|---|---|---|---|---|---|---|
| add_cancel | 0.501 | 0.499 | 0 | 0 | 0 | 0 | 144,648 |
| balanced | 0.450 | 0.448 | 0.039 | 0.024 | 0.024 | 0.016 | 420,544 |
| match_heavy | 0.385 | 0.383 | 0.039 | 0.077 | 0.077 | 0.039 | 755,940 |

Setup: Intel Core i9-14900HX laptop, Windows 11, GCC 16.2.0 (MinGW-w64), `-O3 -DNDEBUG -std=c++20`,
no `-march=native`, benchmark thread pinned to logical CPU 4. The book holds roughly 1,300-3,600
resting orders at the end of a run (the generator caps live ids at 4,096). Full output:
[`bench.md`](results/bench.md), [`bench.csv`](results/bench.csv), [`bench.json`](results/bench.json).

How to read this:

- The flat book had higher median throughput than the map book in all three workloads (about 1.3x to
  1.6x) and about 25% lower p50 latency. My guess for why the gap is not larger (not measured
  separately): the generated books are dense near the top, with most activity within a few dozen
  ticks of the mid, so the map's O(log L) lookups run over a small tree that stays in cache.
- The min-max ranges are wide and overlap. The laptop was running other heavy jobs during the
  measurement, which is why I report the median and the full range instead of a single number.
- Latency percentiles include the timer itself (the measured `rdtsc` pair overhead is recorded as
  `timer_overhead_ns` in `bench.json`, 22 ns for this run) and the event-listener callbacks. I did
  not break down the p99.9 and max values by cause; on a busy Windows machine they are sensitive to
  OS interruptions.

## Architecture

```
                 add / cancel / modify
                          |
                          v
+-------------------- OrderBook<Levels> ---------------------+
|  validate -> FOK liquidity check -> match loop -> rest or  |
|  discard remainder; every change -> EventListener          |
|                                                            |
|  bids_ : Levels     asks_ : Levels                         |
|    Levels = MapLevels  : std::map<Price, PriceLevel>       |
|           | FlatLevels : std::vector<PriceLevel>, one slot |
|                         per tick in [min_price, max_price] |
|                                                            |
|  PriceLevel: head <-> Order <-> Order <-> tail  (FIFO)     |
|  ids_  : OrderIdMap     OrderId -> Order*  (O(1) cancel)   |
|  pool_ : ObjectPool<Order>  (owns all Order memory)        |
+------------------------------------------------------------+
          |                                 |
          v                                 v
   Trade{taker, maker, side,        LevelUpdate{side, price,
         price, qty}                            total_qty}
```

| path | contents |
|---|---|
| `include/lob/order_book.hpp` | matching logic, `MapOrderBook` / `FlatOrderBook` aliases |
| `include/lob/map_levels.hpp`, `flat_levels.hpp` | the two per-side level containers |
| `include/lob/price_level.hpp` | `Order` and the intrusive FIFO `PriceLevel` |
| `include/lob/order_id_map.hpp` | open-addressing id map with backward-shift deletion |
| `include/lob/object_pool.hpp` | chunked object pool with a free list |
| `tests/reference_book.hpp` | the slow, obviously-correct oracle |
| `tests/difftest.cpp` | randomized differential test driver |
| `tools/order_flow.hpp` | deterministic synthetic order-flow generator |
| `bench/bench.cpp` | throughput and latency benchmark |
| `scripts/mutation_check.py` | injects bugs and checks the difftest catches them |

### Order semantics

- Price-time priority: best price first; within a price, oldest order first. Trades execute at the
  resting (maker) order's price.
- `Limit` rests any remainder. `Market` ignores price and discards the remainder. `IOC` trades up to
  its limit price and discards the remainder. `FOK` trades its full quantity immediately or does
  nothing (no trades, no events).
- `modify(id, price, qty)`: same price and a quantity at or below the current remaining quantity
  keeps queue position. A price change or a quantity increase is a cancel plus a new limit order with
  the same id: it goes to the back of the queue and may trade immediately.
- Events: one `Trade` per fill, one `LevelUpdate` (new aggregate quantity, 0 = level gone) after
  every change to a level. A rejected request (bad id, quantity or price, duplicate id, unknown id,
  price outside a flat book's range) changes nothing and emits nothing.
- No self-trade prevention. Order id 0 is reserved.

## Reproduce

Requires CMake 3.20+, Ninja and a C++20 compiler (tested: GCC 16.2 MinGW-w64 locally; GCC and Clang
on Ubuntu in CI). GoogleTest v1.15.2 is fetched at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure            # unit tests + quick difftest

./build/lob_difftest --ops 1000000 --seeds 3 --out results/difftest.json
./build/lob_bench --ops 2000000 --reps 11 --cpu 4 --out results
python scripts/mutation_check.py --cxx g++ --ops 20000 --out results

# sanitizers (Linux/macOS; MinGW has no ASan)
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DLOB_SANITIZE=ON
cmake --build build-asan && ctest --test-dir build-asan
```

On a MinGW CMake without a CA bundle, the GoogleTest download fails with a TLS error; pass
`-DCMAKE_TLS_CAINFO=<path to ca-bundle.crt>` (for example the one shipped with Git for Windows).

## Design decisions

- **Integer tick prices.** Orders at the same price must compare exactly equal to share a level;
  floating point cannot promise that. Quantities are signed so an underflow shows up as a negative
  value that validation rejects instead of wrapping around.
- **Intrusive FIFO per level + id hash map.** Each `Order` carries its own `prev`/`next` links and a
  pointer to its level. A cancel looks up the order by id and unlinks it in O(1) without searching
  the queue or allocating list nodes.
- **Object pool for orders.** Orders are carved out of 4,096-object chunks and recycled through a
  free list, so adding or cancelling an order does not call `new`/`delete` for the order itself.
  (The pool and the id map still allocate when they grow, and the map book allocates a tree node
  when a new price level appears; the flat book's level array never allocates after construction.) The pool owns all order memory; pointers stay valid because chunks
  never move.
- **Open-addressing id map instead of `std::unordered_map`.** `unordered_map` allocates a node per
  insert, which would undo the pool. Linear probing keeps slots in one array; deletion uses backward
  shift instead of tombstones, so heavy add/cancel churn does not degrade lookups. It is tested
  against `std::unordered_map` under random churn with forced collisions.
- **Level container as a template parameter.** `std::map` accepts any price and uses memory
  proportional to live levels. The flat array makes a level lookup one subtraction and never
  allocates, but needs a bounded tick range and scans for the next best level when the top empties
  (short in practice, O(range) in the worst case). Writing the matching loop once and swapping the
  container makes the benchmark a like-for-like comparison.
- **Differential testing against a slow oracle, then testing the test.** The reference book is a
  vector with linear scans, short enough to verify by reading. The typed unit tests also run against
  it, so it is checked against hand-worked examples, and the mutation script confirms the difftest
  fails on realistic bugs rather than passing vacuously.
- **Deterministic generator.** The order-flow generator uses its own splitmix64 PRNG instead of
  `<random>` distributions (whose output is implementation-defined), so a seed produces the same
  stream on GCC, Clang, Windows and Linux, and any mismatch report can be replayed exactly.

## Limitations

- Single-threaded, in-memory only: no network protocol, persistence, or market-data feed handler.
- No stop, iceberg/hidden or pegged orders, no auctions, no self-trade prevention.
- Only synthetic order flow. I did not replay real exchange data (for example LOBSTER sample files),
  so the benchmark mix and book shape are my assumptions, not measured market behaviour.
- The flat book rejects prices outside its configured range, and its best-level scan is O(range) in
  the worst case (for example, a lone order at each end of a wide range).
- The id map never shrinks and the pool never returns memory to the OS.
- Price and quantity arithmetic is not checked for int64 overflow.
- Benchmarks are from one laptop under concurrent load, on Windows, with an unserialized `rdtsc`
  timer. Treat them as a relative comparison of the two level books, not as numbers comparable to
  production engines.
- The difftest can only catch disagreements between the engine and the oracle. A rule both
  implement the same wrong way would pass; the hand-written unit tests are the guard against that.

## License

MIT, copyright 2026 Yunlong Lu. See [LICENSE](LICENSE).
