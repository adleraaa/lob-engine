# lob-engine

A single-threaded limit order book and matching engine in C++20. It supports limit, market, IOC and
fill-or-kill orders, cancels, and modifies, matches with price-time priority, and emits trade and
price-level update events. The book is a class template over its price-level container, so the
same matching code runs on a `std::map` level book and on a flat, array-indexed level book, and the
benchmark compares the two directly. I built it to learn how exchange matching engines are
structured and how to test one properly. Correctness is checked by a randomized differential test:
millions of generated requests go to both engines and to a deliberately simple reference book, and
every result, trade, level update and best bid/ask, plus periodic full-book snapshots, must match
exactly.

## Results

All numbers below are copied from files in [`results/`](results), which the commands in
[Reproduce](#reproduce) regenerate. Each results file records the git commit, compiler, build type
and flags it was produced with.

### Correctness

| check | result | source |
|---|---|---|
| Differential test, 5 request mixes x 3 seeds x 1,000,000 requests | 15,000,000 requests, 3,114,030 trades and 13,849,530 level updates compared one by one across the reference, map and flat books; best bid and ask compared after every request; 15,000 full-book snapshot comparisons; 0 mismatches | [`difftest.json`](results/difftest.json) |
| Mutation check: 9 hand-injected bugs (FIFO to LIFO, off-by-one at the limit price, missing event, broken hash-map deletion, ...) | 9 / 9 caught, all 9 by a result mismatch. Mutants are built with `-DNDEBUG`, so the engine's own `assert()`s cannot take credit, and the unmutated difftest must pass first | [`mutation_check.md`](results/mutation_check.md) |
| Unit tests (GoogleTest) | 79 test instances: 19 behaviour tests and 1 tick-range test run against all three books, plus engine-only, flat-book, container and CLI-parsing tests; ctest also checks that the difftest rejects malformed arguments | `tests/` |

All three books in the difftest get the same tick range, so they must also agree on which prices are
out of range. The fifth mix, `edge_range`, uses prices 1 to 300 with orders placed up to 140 ticks
from a mid that wanders over the whole range and at most 60 live ids. By construction, that puts
orders on the first and last slots of the flat book's array and leaves sparse levels, so the
best-level scan has to skip empty slots.

Of the 15,000,000 difftest requests, 2,660,058 were rejected (breakdown by request kind in
`difftest.json`):

- 2,549,404 `UnknownId`: 2,261,452 cancels and 287,952 modifies. Most of them target orders that had
  already been filled, because the generator does not see fills. A smaller part are deliberately
  unknown ids injected by the `with_invalid_requests` and `edge_range` mixes.
- 110,654 injected invalid requests: 20,769 `InvalidId`, 20,700 `InvalidQty`, 20,637 `InvalidPrice`,
  34,634 `PriceOutOfRange` (20,829 adds, 13,805 modifies of resting orders) and 13,914 `DuplicateId`.

CI runs the unit tests and a shorter difftest with gcc and clang, in Release and in Debug with
AddressSanitizer + UndefinedBehaviorSanitizer, plus the mutation check.

### Performance

2,000,000 requests per run, the same deterministic stream for both books. Throughput is the median
of 11 runs per book (map and flat runs alternate), with the min-max range. Latency reads the CPU
timestamp counter around every request; each percentile is the median over 5 such runs.

Laptop: Intel Core i9-14900HX, Windows 11, on AC power, Windows "Balanced" power plan, GCC 16.2.0
(MinGW-w64), `-O3 -DNDEBUG -std=c++20`, statically linked, no `-march=native`, benchmark thread
pinned to logical CPU 4, commit `57a3ddc`. The run started once total CPU load had stayed below 15%
for 16 seconds; load during the run was 16.7% median and 19.2% max, of which one busy logical CPU
out of 32 (about 3%) is the benchmark itself
([`bench_conditions.json`](results/bench_conditions.json)).

| workload | book | median Mreq/s (min-max) | p50 ns | p99 ns | p99.9 ns |
|---|---|---|---|---|---|
| add_cancel | map | 19.16 (18.74-20.06) | 51 | 122 | 203 |
| add_cancel | flat | 28.88 (27.61-29.12) | 38 | 105 | 169 |
| balanced | map | 20.07 (19.42-20.78) | 49 | 168 | 268 |
| balanced | flat | 30.56 (29.76-31.48) | 32 | 126 | 198 |
| match_heavy | map | 19.46 (17.59-20.24) | 49 | 184 | 278 |
| match_heavy | flat | 31.10 (28.50-31.97) | 33 | 125 | 184 |

Full output: [`bench.md`](results/bench.md), [`bench.csv`](results/bench.csv),
[`bench.json`](results/bench.json).

The same benchmark on a GitHub-hosted runner (AMD EPYC 9V74, 4 vCPU, Ubuntu, GCC 13.3, commit
`c449771`, which has the same engine and benchmark code; run via the manual
[`bench.yml`](.github/workflows/bench.yml) workflow), for a second, independent data point:

| workload | map Mreq/s | flat Mreq/s | flat / map |
|---|---|---|---|
| add_cancel | 14.95 | 21.42 | 1.43 |
| balanced | 15.09 | 22.65 | 1.50 |
| match_heavy | 14.94 | 23.44 | 1.57 |

Source: [`results/bench_ci/`](results/bench_ci). Its min-max ranges span less than 0.4% of the median.

**What the stream contains.** The generator remembers each limit order id until it cancels it and
does not see fills, so on its own it emits many cancels and modifies of orders that already traded
away. The book rejects those (`UnknownId`) after one hash lookup, so they would count as very cheap
"requests" and make the cancel share mostly no-ops. The benchmark therefore builds its stream by
running the generator through an untimed map book and dropping every request that came back
`UnknownId`. Such a request never changes the book, so the remaining requests behave exactly as
before. Resulting mix, from `bench.json` (identical on both machines):

| workload | generated | dropped (`UnknownId`) | limit add | cancel | modify | market | IOC | FOK filled / killed | trades |
|---|---|---|---|---|---|---|---|---|---|
| add_cancel | 2,156,587 | 156,587 (7.3%) | 0.540 | 0.460 | 0 | 0 | 0 | 0 / 0 | 155,424 |
| balanced | 2,430,035 | 430,035 (17.7%) | 0.546 | 0.346 | 0.031 | 0.029 | 0.029 | 23,818 / 14,378 | 508,860 |
| match_heavy | 2,808,598 | 808,598 (28.8%) | 0.541 | 0.171 | 0.017 | 0.108 | 0.108 | 39,781 / 68,354 | 1,061,780 |

Every kept request returned `Ok`. A killed FOK is still an `Ok` request that did real work (a walk
over the opposite side to add up the liquidity), but it does not trade; in `match_heavy` 63% of FOKs
are killed. At the end of a run the book holds 1,387 (`match_heavy`) to 3,714 (`add_cancel`)
resting orders; the generator caps live ids at 4,096, and the pool and id map are pre-sized to that
so their growth stays out of the timed region.

How to read this:

- The flat book's median throughput is 1.51x (`add_cancel`), 1.52x (`balanced`) and 1.60x
  (`match_heavy`) the map book's on the laptop, and 1.43x to 1.57x on the cloud runner. In every
  workload on both machines, the flat book's slowest run is faster than the map book's fastest.
- p50 latency is 25-35% lower for the flat book. These percentiles include the timer (the measured
  `rdtsc` pair costs 6.6 ns here, `timer_overhead_ns` in `bench.json`); subtracting it, the map
  book's p50 is 1.44x to 1.66x the flat book's, close to the throughput ratio.
- The three workloads run at about the same rate within each book, even though `match_heavy`
  produces 7 times the trades of `add_cancel`. I have not profiled why. My guess is that a fill
  costs about as much as the passive add or cancel it replaces, since each is one level update plus
  a few pointer writes.
- My guess for why the flat book is not further ahead (not measured separately): most activity is
  within a few dozen ticks of the mid, so the map's tree of live levels stays small and
  cache-resident.
- The max values (up to 20 ms) are OS interruptions on a desktop Windows machine, not engine
  behaviour; I report p99.9 and leave max in `bench.json`.

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
|    Levels = MapLevels  : std::map<Price, PriceLevel>,      |
|                         optional [min_price, max_price]    |
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
| `tools/cli.hpp`, `tools/build_info.hpp` | strict argument parsing; commit/compiler info for results files |
| `bench/bench.cpp` | throughput and latency benchmark |
| `scripts/mutation_check.py` | injects bugs and checks the difftest catches them |
| `scripts/bench_when_idle.ps1` | Windows: waits for an idle machine, runs the benchmark, records load and power state |

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
  price outside the configured tick range) changes nothing and emits nothing.
- Listeners are called synchronously, in the middle of a request, and must not call back into the
  same book (that could free an order the matching loop still points at). Debug builds assert on it.
- No self-trade prevention. Order id 0 is reserved.

## Reproduce

Requires CMake 3.20+, Ninja and a C++20 compiler (tested: GCC 16.2 MinGW-w64 locally; GCC and Clang
on Ubuntu in CI). GoogleTest v1.15.2 is fetched at configure time.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure            # unit tests + quick difftest

./build/lob_difftest --ops 1000000 --seeds 3 --out results/difftest.json
./build/lob_bench --ops 2000000 --reps 11 --lat-reps 5 --cpu 4 --out results
# or, on Windows, wait for an idle machine and record load/power conditions too:
pwsh -NoProfile -File scripts/bench_when_idle.ps1
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
  when a new price level appears; the flat book's level array never allocates after construction.)
  The pool owns all order memory; pointers stay valid because chunks never move.
- **Open-addressing id map instead of `std::unordered_map`.** `unordered_map` allocates a node per
  insert, which would undo the pool. Linear probing keeps slots in one array; deletion uses backward
  shift instead of tombstones, so heavy add/cancel churn does not degrade lookups. It is tested
  against `std::unordered_map` under random churn (small table, ids from a small range), and by a
  test that picks ids with colliding home slots so that one probe cluster wraps around the end of
  the table, then erases them in random orders.
- **Level container as a template parameter.** `std::map` accepts any price (a range is optional,
  so it can mirror the flat book's validation) and uses memory proportional to live levels. The
  flat array makes a level lookup one subtraction and never allocates, but needs a bounded tick
  range and scans for the next best level when the top empties (short in practice, O(range) in the
  worst case). Writing the matching loop once and swapping the container makes the benchmark a
  like-for-like comparison.
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
- The flat book needs a bounded tick range (the map book can take one too), and its best-level scan
  is O(range) in the worst case (for example, a lone order at each end of a wide range).
- The id map never shrinks and the pool never returns memory to the OS.
- Price and quantity arithmetic is not checked for int64 overflow.
- Benchmarks come from one laptop (Balanced power plan; total CPU load 15-19% during the run,
  including the benchmark itself) and one shared cloud VM, with an unserialized `rdtsc` timer.
  Treat them as a relative comparison of the two level books, not as numbers comparable to production engines.
- The benchmark drops the generator's cancels/modifies of already-filled orders (see above); the
  difftest keeps them, since rejecting them correctly is part of what it checks.
- The git SHA in results files is read when CMake configures. Configure re-runs when HEAD or the git
  index changes, but a file edited after the last configure and never staged is not reflected in
  the `-dirty` marker.
- The difftest can only catch disagreements between the engine and the oracle. A rule both
  implement the same wrong way would pass; the hand-written unit tests are the guard against that.

## License

MIT, copyright 2026 Yunlong Lu. See [LICENSE](LICENSE).
