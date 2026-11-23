# orderbook

[![ci](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-333?style=flat-square)](LICENSE)
![version](https://img.shields.io/badge/version-1.0.0-333?style=flat-square)

Limit order book and matching engine in C++23. Price-time priority, integer
prices, three implementations of the same contract, no external dependencies,
and a benchmark you can reproduce on your own machine.

![Order book depth](docs/depth.gif)

## Three implementations, one contract

All three expose the same methods: `add_limit`, `add_market`, `cancel`,
`modify`, `best_bid`, `best_ask`, `quantity_at`, `depth`, `order_count` and
`level_count`.

| | `Book` | `ArrayBook` | `PoolBook` |
|---|---|---|---|
| Price levels | `std::map` | contiguous array by tick | contiguous array by tick |
| Resting orders | `std::list` nodes | `std::list` nodes | preallocated vector, integer links |
| Order lookup | `std::unordered_map` | `std::unordered_map` | open-addressing table |
| Heap use per resting order | list node + map node | list node + map node | none |
| Price range | unbounded | fixed ladder | fixed ladder |
| Resting capacity | unbounded | unbounded | fixed at construction |

`PoolBook` keeps orders in a `std::vector<Order>` and recycles slots through a
free list of integer indices. Each price level stores `head` and `tail` indices,
and the orders at a level form a doubly linked list through two parallel
`std::vector<std::uint32_t>` arrays. Cancel and matching only move indices.

The id lookup is preallocated too: an open-addressing table with linear probing,
sized when the book is created. Removing an order reinserts the rest of its
probe cluster, so lookups keep working without tombstones.

### Complexity

`L` is the number of price levels, `R` the ladder size and `k` the requested
depth. The fills term is the number of makers touched by a match. Cancel is
average O(1) in all three because ids go through a hash table. The difference is
what happens when a level empties.

| Operation | `Book` | `ArrayBook` | `PoolBook` |
|---|---|---|---|
| `add_limit` | O(fills + log L) | O(fills) | O(fills) |
| `add_market` | O(fills) | O(fills) | O(fills) |
| `cancel` | average O(1), O(log L) if the level empties | average O(1), O(R) if the best level empties | average O(1), O(R) if the best level empties |
| `modify` | O(1) in place, O(fills + log L) if it moves | O(1) in place, O(fills) if it moves | O(1) in place, O(fills) if it moves |
| `best_bid` / `best_ask` | O(1) | O(1) | O(1) |
| `quantity_at` | O(log L) | O(1) | O(1) |
| `depth(k)` | O(k) | O(R) | O(R) |
| `level_count` | O(1) | O(R) | O(R) |

`PoolBook` does not allocate in `add_limit`, `add_market`, `cancel` or `modify`
after construction. `depth` allocates its result vector.

## Benchmark

### Synthetic workload

```
$ ./build/orderbook bench --events 1000000 --seed 42 --runs 3 --repeat 10 --impl all
workload  1000000 events, seed 42, mixed, ladder [9809, 10339]
map    min 173.8  median 177.6  max 241.3 ns/event   457777 trades
array  min 150.7  median 157.2  max 189.4 ns/event   457777 trades
pool   min 64.3  median 74.6  max 95.0 ns/event   457777 trades
pool memory  36.8 MiB, 650012 slots
```

Measured on 2026-11-23 on a Ryzen 9 5900X, Windows 11, GCC 14.2 in `Release` mode (`-O3`),
single thread. Each repeat is the best of three runs and the table summarises
ten repeats. Using the medians, `PoolBook` is 2.4 times faster than `Book` and
2.1 times faster than `ArrayBook`. The min and max columns show the spread of
this machine. The pool line is followed by the memory reserved for its vectors:
36.8 MiB for 650,012 slots and the id table. The trace itself is not included.
When more than one implementation runs, the command also checks that they
produced the same trade count and volume before printing. A mismatch fails the
run.

### Real BTCUSDT day

`benchmarks/btcusdt-2026-11-22.csv.zst` is one UTC day of BTCUSDT aggregate
trades, 1,003,991 trades from 2026-11-22. `tools/parquet_to_trace.py` reads the
Parquet written in the `market-data-service` schema. The public trade feed has
no order ids, no resting depth and no cancels, so the converter writes one limit
order and one market order per trade: the limit order rests at the trade price
on the maker side and the market order hits it on the aggressor side. The real
prices, sizes, timestamps and aggressor side are preserved. The book is flat
between trades.

```
$ zstd -d benchmarks/btcusdt-2026-11-22.csv.zst -o data/btcusdt-2026-11-22.csv
$ ./build/orderbook bench --trace data/btcusdt-2026-11-22.csv --runs 3 --repeat 10 --impl all
workload  2007982 events, trace data/btcusdt-2026-11-22.csv, ladder [5876751, 6167099]
map    min 62.4  median 62.5  max 63.3 ns/event   1003991 trades
array  min 56.8  median 58.3  max 60.7 ns/event   1003991 trades
pool   min 52.7  median 54.1  max 58.0 ns/event   1003991 trades
pool memory  71.2 MiB, 1003991 slots
```

Measured on the same Ryzen 9 5900X, Windows 11, GCC 14.2 build on 2026-11-23,
with the same three runs and ten repeats.

| Workload | Events | Trades | `map` | `array` | `pool` | pool vs map |
|---|---|---|---|---|---|---|
| Synthetic mixed | 1,000,000 | 457,777 | 177.6 | 157.2 | 74.6 | 2.4x |
| BTCUSDT 2026-11-22 | 2,007,982 | 1,003,991 | 62.5 | 58.3 | 54.1 | 1.2x |

The real trace is faster per event for all three books because the
reconstructed book holds at most one order. There is no deep tree to walk, no
cancel to process and no 650,000-order working set. The pool's lead falls from
2.4x to 1.2x: most of its synthetic win comes from avoiding a `std::list` node
and an `unordered_map` node per resting order, and from not chasing two
pointers per fill. With one order at a time that work is small.

The real day spans 290,148 price ticks between its lowest and highest limit
price, against 330 in the synthetic mixed run. `ArrayBook` and `PoolBook` track
the number of non-empty levels, so an emptied side flips `has_bid_` or
`has_ask_` without scanning the ladder. Before that change every market order
scanned up to 290,000 empty levels and the two ladder books cost 26 to 30
microseconds per event on this trace.

The pool memory line is a conservative bound: one slot per limit event. The
real trace reserves 1,003,991 slots although at most one order is live. A
deployment should size the pool from peak live orders, as the Limitations
section says.

### What the traces contain

Both reports come from `tools/trace_report.py`, which reads the same CSV the
engine reads. The JSON outputs are in `benchmarks/`.

| Metric | Synthetic mixed | BTCUSDT 2026-11-22 |
|---|---|---|
| Events | 1,000,000 | 2,007,982 |
| Limit / cancel / market | 650,012 / 199,987 / 150,001 | 1,003,991 / 0 / 1,003,991 |
| Cancel share | 20.00 % | 0.00 % |
| Duration | 1.000 s | 86,398.570 s |
| Events per second | 1,000,001 | 23.24 |
| Trades per second | 150,001 | 11.62 |
| Max events, 10 ms / 100 ms / 1 s | 10,000 / 100,000 / 1,000,000 | 800 / 1,886 / 4,398 |
| Max trades, 1 s | 150,001 | 2,199 |
| Max inter-arrival gap | 0.001 ms | 8,412.495 ms |
| Price range | 9,909 .. 10,239 trace units | 58,768.51 .. 61,669.99 USDT |
| Size p50 / p90 / p99 / max | 50 / 91 / 99 / 100 trace units | 0.00045 / 0.02064 / 0.42534 / 42.52597 BTC |
| p99 / p50 | 1.98 | 945.20 |

The differences are the point:

- **Sizes.** Synthetic sizes are uniform 1..100, so p99/p50 is 1.98. Real BTC
  sizes are heavy-tailed: median 0.00045 BTC, p99 0.42534 BTC, p99/p50 945, max
  42.52597 BTC. The synthetic generator cannot exercise that skew.
- **Bursts.** The synthetic trace is a throughput stream: 1,000,000 events in
  one second, with a 1 microsecond gap between events. The real day averages
  23.24 events per second, peaks at 4,398 events in one second and 800 in
  10 ms, and has an 8.4 second quiet gap. The synthetic trace is a CPU
  benchmark, not a market clock.
- **Cancels.** Synthetic mixed is 20.00 % cancels. The real trace is 0.00 %:
  aggregate trades do not say whether a resting order was cancelled or filled,
  and the converter never invents a cancel. The cancel path remains covered by
  the synthetic workloads and the unit tests.
- **Depth.** The synthetic run writes 650,012 limit events, so the pool reserves
  650,012 slots and the map works over a deep book. The real trace has one live
  order at a time. That is why the absolute numbers are lower on the real trace
  and why the pool's lead shrinks.

### Workloads

`--workload` selects the generator mix:

| Workload | limit | cancel | market |
|---|---|---|---|
| `mixed` | 65 | 20 | 15 |
| `limits` | 100 | 0 | 0 |
| `cancels` | 50 | 50 | 0 |
| `markets` | 50 | 0 | 50 |

A cancel with no live id becomes a market order, so the counts printed by
`generate` can differ slightly.

Medians for the other workloads with `--repeat 5`. The `mixed` row is the
ten-repeat run above.

| Workload | `map` | `array` | `pool` |
|---|---|---|---|
| `mixed` | 177.6 | 157.2 | 74.6 |
| `limits` | 248.3 | 234.4 | 114.8 |
| `cancels` | 92.7 | 64.6 | 52.4 |
| `markets` | 94.5 | 73.5 | 61.8 |

```
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --repeat 5 --workload limits --impl all
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --repeat 5 --workload cancels --impl all
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --repeat 5 --workload markets --impl all
```

The default `mixed` workload comes from the same binary:

```
$ ./build/orderbook generate --events 1000000 --seed 42 --out trace.csv
wrote 1000000 events to trace.csv (650012 limit, 199987 cancel, 150001 market)
```

The generator draws 65 % limit orders, 20 % cancels and 15 % market orders by
default. It assigns an id to every limit and market order, so the cancel ids
stay aligned with the book. It does not know which orders the book has filled,
so some cancel events reference orders that already executed and the book
rejects them. The rare event where the generator has no id left becomes a market
order, which explains the small drift from the exact percentages.

### Where the time goes

- `map` to `array` saves 20.4 ns/event, 11.5 % of the map median. Price-level
  lookup stops walking a red-black tree and stops allocating a node for every
  new price.
- `array` to `pool` saves 82.6 ns/event, 52.5 % of the array median. A resting
  limit order stops allocating a `std::list` node and an `unordered_map` node,
  and matching stops chasing two pointers per order.
- The second change is about 4.0 times larger than the first. That is the
  measured answer to why the array alone did not stand out: both earlier books
  paid the per-order allocation, and 65 % of the events are limit orders in the
  mixed workload.
- The remaining 74.6 ns/event are the match loop, the callback, the trade
  counters, the id hash and ordinary branch and cache misses. This build does
  not profile them further.

## What it does

- Limit orders with price-time priority. The first order at a price is the first
  one filled.
- Market orders that walk the book until the quantity is exhausted or the book
  is empty.
- `cancel` in O(1) through a direct index, with no allocation in `PoolBook`.
- `modify` that follows the exchange rule: a price change or a size increase
  loses time priority, a size decrease keeps it.
- Trades always execute at the resting order's price. The maker decides.
- Deterministic: the same seed produces the same trace and the same book.
- A depth export that feeds the plot above.

## Contract

Every book exposes the same methods:

| Method | Result |
|---|---|
| `add_limit(side, price, quantity)` | order id, after matching any crossing quantity |
| `add_market(side, quantity)` | executed quantity |
| `cancel(id)` | `true` if the order was resting |
| `modify(id, price, quantity)` | `true` if the order existed, zero quantity cancels |
| `best_bid()` / `best_ask()` | optional best price |
| `quantity_at(side, price)` | resting quantity at one price |
| `depth(side, levels)` | best prices and quantities, up to `levels` |
| `order_count()` / `level_count(side)` | live orders / non-empty levels |

`ArrayBook` and `PoolBook` add `min_price()` and `max_price()`. `PoolBook`
adds `capacity()` and `memory_bytes()`.

## Build

Requires CMake 3.25+ and a compiler with C++23 support (GCC 14, Clang 18,
MSVC 19.38).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The benchmark numbers above were produced with Ninja and GCC 14.2:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++
cmake --build build
```

## Presets

`CMakePresets.json` defines the common configurations:

| Preset | Configuration |
|---|---|
| `release` | Release, warnings as errors |
| `debug` | Debug |
| `clang` | Release with `clang++` |
| `asan` | Debug with AddressSanitizer |
| `ubsan` | Debug with UndefinedBehaviorSanitizer |
| `coverage` | Debug with gcov instrumentation |
| `fuzz` | Clang libFuzzer targets, tests off |

```
cmake --preset release
cmake --build --preset release
ctest --preset release
```

The same three steps are available as a workflow:

```
cmake --workflow --preset release
```

A package workflow configures, builds and runs CPack:

```
cmake --workflow --preset package
```

The sanitizer presets need the matching runtime installed. The `fuzz` preset
also needs Clang. Debug builds define `_GLIBCXX_ASSERTIONS`, so libstdc++ checks
container bounds.

The `coverage` preset writes gcov data. With `gcovr` installed:

```
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage
gcovr -r . build/coverage --filter "src/" --exclude "src/main.cpp" --fail-under-line 90 --txt
```

CI fails if line coverage of the library sources drops below 90 %.

## Use

```
# print the version or the usage
./build/orderbook --version
./build/orderbook --help

# write a synthetic session to CSV
./build/orderbook generate --events 100000 --seed 7 --out trace.csv

# write a limit-only session
./build/orderbook generate --events 100000 --seed 7 --workload limits --out trace.csv

# replay it and print the final state of the book
./build/orderbook replay trace.csv

# replay through every implementation and check they agree
./build/orderbook replay trace.csv --impl all

# summarize a recorded trace
./build/orderbook stats trace.csv

# compare the three implementations
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --impl all

# ten repeats with min, median and max
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --repeat 10 --impl all

# benchmark a recorded trace
./build/orderbook bench --trace data/btcusdt-2026-11-22.csv --runs 3 --repeat 10 --impl all

# export depth snapshots for the plot
./build/orderbook depth --events 200000 --seed 7 --frames 60 --out depth.csv
python tools/visualize.py --input depth.csv --png docs/depth.png --gif docs/depth.gif

# export five levels per side
./build/orderbook depth --events 200000 --seed 7 --frames 60 --levels 5 --out depth.csv

# check that every implementation produces the same depth CSV
./build/orderbook depth --events 200000 --seed 7 --frames 60 --out depth.csv --impl all
```

The trace files come from the Parquet written by `market-data-service`.
`tools/binance_archive_to_parquet.py` normalizes a Binance daily archive into
that schema, `tools/parquet_to_trace.py` writes the replay CSV and
`tools/trace_report.py` produces the statistics. They need Python 3.10+ and
`pyarrow`. The exact commands, hashes and conversion model are in
`benchmarks/README.md`.

As a library:

```cpp
#include <orderbook/book.hpp>

ob::Book book([](const ob::Trade& t) {
    // t.taker, t.maker, t.price, t.quantity
});

book.add_limit(ob::Side::Buy, 10000, 25);   // rests at 10000
book.add_limit(ob::Side::Sell, 10000, 10);  // fills 10 at 10000
```

`ArrayBook` and `PoolBook` take the price ladder range in the constructor.
`PoolBook` also takes the number of orders that can rest at once:

```cpp
#include <orderbook/array_book.hpp>
#include <orderbook/pool_book.hpp>

ob::ArrayBook array_book(9900, 10100, on_trade);
ob::PoolBook pool_book(9900, 10100, 500000, on_trade);
```

Both fixed-ladder books expose `min_price()` and `max_price()`. The
`ORDERBOOK_VERSION_*` macros are in `<orderbook/version.hpp>`.
`ob::read_csv_stream(path, callback)` visits a trace event by event without
storing it.

A price outside the ladder throws `std::out_of_range`. If the remainder of an
order would need a slot and the pool is full, `add_limit` throws
`std::overflow_error`. A non-crossing order in that situation leaves the book
unchanged. A crossing order keeps the trades it already executed, because they
were matched before the remainder looked for a slot. `PoolBook::memory_bytes()`
returns the bytes reserved by its vectors.

## Install

```
cmake --install build --prefix /some/prefix
```

The install exports `orderbook::orderbook_core` for `find_package`:

```cmake
find_package(orderbook 1.0 CONFIG REQUIRED)
target_link_libraries(app PRIVATE orderbook::orderbook_core)
```

Set `-DBUILD_TESTING=OFF` to skip the tests when the project is included as a
subdirectory or packaged. Set `-DORDERBOOK_BUILD_CLI=OFF` to build only the
library and skip the command line tool. The `examples/consumer` project is
built against an installed tree in CI.

## Package

CPack builds a ZIP or TGZ from the install tree:

```
cmake --build build
cd build
cpack -G ZIP
```

The archive contains the library, the headers, the CLI, the CMake package and
the documentation. The Windows ZIP contains a statically linked
`orderbook.exe` that does not need the MinGW runtime DLLs. A source archive is
also available:

```
cd build
cpack --config CPackSourceConfig.cmake -G TGZ
```

## Project layout

```
CHANGELOG.md        release notes
include/orderbook/  public headers
src/                library sources and the CLI
tests/              doctest suites, reference model and header self-checks
fuzz/               libFuzzer targets and seed corpora
examples/consumer/  consumer built with find_package
benchmarks/         real BTCUSDT day, hashes, trace reports and provenance
tools/              trace converter, trace report and depth visualizer
```

## Design decisions

**Prices are integers.** `Price` is `std::int64_t` counted in ticks. Floating
point prices accumulate error, compare badly and make equality checks
unreliable. Every serious engine works in integer ticks.

**The maker sets the price.** A buy order that crosses an ask at 101 executes at
101, not at the buyer's limit. This is how equities and futures work, and it is
the opposite of what a naive implementation does.

**Cancel is O(1).** `Book` and `ArrayBook` keep an `unordered_map<OrderId,
Location>` where `Location` holds the level and a `std::list` iterator.
`PoolBook` maps the id to a pool slot with an open-addressing table and unlinks
the slot from its level through `prev` and `next` indices.

**The pool removes the hot-path allocation.** `PoolBook` reserves every order
slot and every link when it is constructed. Resting, cancelling and matching
never touch the heap. The price is a fixed capacity chosen by the caller, and a
full pool is an error instead of a hidden `malloc`.

**An emptied side is O(1).** `ArrayBook` and `PoolBook` keep a count of
non-empty levels. When a match or a cancel removes the last order on a side,
`has_bid_` or `has_ask_` flips without walking the fixed ladder. When another
level is still occupied, the scan resumes from the old best price and stops at
the next non-empty level. The real BTCUSDT day made this visible: a day-wide
ladder has 290,000 ticks and a flat book would otherwise scan all of them after
every trade.

**One thread.** Determinism is worth more than parallelism here. A benchmark
with threads measures the scheduler as much as the book.

**Books move, they do not copy.** A copy would duplicate every order and, in
`Book` and `ArrayBook`, leave the id index pointing at the original list nodes.
Copy and assignment are deleted. Move is defaulted, keeps the nodes alive and is
noexcept.

## Limitations

- No self-trade prevention, iceberg orders, hidden liquidity or auction phases.
- No persistence and no network gateway.
- The trade callback runs inside `add_limit`, `add_market` and `modify`. It must
  not throw or call back into the book.
- `Book` and `ArrayBook` allocate when an order rests. A `std::bad_alloc` during
  that step can leave the book partially updated. `PoolBook` has no allocation
  window after construction.
- The synthetic generator is a Poisson-like flow, not real market data. A real
  BTCUSDT day can be replayed with `bench --trace`, but the public aggregate
  trade feed has no order ids, no resting depth and no cancels. The converter
  models each trade with one limit order and one market order, so the real trace
  measures the add and match path on real prices, sizes and timing, not a full
  order-by-order book.
- Cancels and deep-book behaviour are covered by the synthetic workloads and the
  unit tests, not by the real trace.
- `ArrayBook` and `PoolBook` need a fixed price range. Outside it, the price is
  rejected.
- `PoolBook` needs a bound on resting orders. In the benchmark the bound is the
  number of limit events, which is safe but conservative. On the real day that
  reserves 1,003,991 slots although at most one order is live. A production
  deployment would size the pool from its peak live orders plus a margin.
- Per-event latency percentiles are not published because the Windows clock
  ticks at 100 ns, which is the same order as the operation itself. The
  throughput figure does not suffer from that limit.

## Tests

57 cases. The original 19 stay unchanged and cover priority rules, partial
fills, market orders walking several levels, cancel and modify semantics, the
spread invariant under a random workload, trace reproducibility and the array
ladder range. Running `./build/orderbook_tests` reports those 19 cases.

The 38 new cases cover the pool and the edges around it: slot reuse, a middle
cancel that must not break the intrusive list, 1000 full pool cycles, a full
pool, invalid ladders and capacities, spans that overflow `Price`, level totals
that overflow `Quantity`, depth, empty books, zero-depth queries, zero-size
market orders and modify no-ops, modify crossing the spread, modify fully
executing, a crossing order that frees pool slots, the memory footprint, noexcept
queries, move-only book semantics, a three-seed differential, a generator `mid`
that leaves no room for the drift, a generator workload weight check, and a
counter that replaces global `operator new` and asserts that 50 rounds of
adding, matching, modifying and cancelling do not allocate. They also check that
every limit and market event consumes one id, so the cancels in a generated
trace reference the order the book actually assigned, that `parse_csv` works
without a file, and that `read_csv_stream` visits every event. The CSV reader
round-trips a generated trace and rejects a bad header, a bad op, a bad side, a
non-numeric field, a wrong field count and an empty file, it reports missing
paths, it accepts header-only files, CRLF line endings and a UTF-8 BOM. Running
`./build/orderbook_pool_tests` reports the 38 new cases.

Both differential tests also compare against an independent reference model that
uses a map of vectors and computes totals on demand. The first runs 20,000
generated events, the second runs 5,000 random operations with modifies. They
compare best prices, live orders, level counts, depth, every trade and the
executed volume. Twenty-five CLI checks cover `--help`, `--version`, a
reproducible generate run, a trace summary, a golden replay through map, array
and pool, a depth run through all implementations, a byte-for-byte map/pool depth
comparison, the four benchmark `--impl` modes, the workload mix, a zero depth
limit, the invalid-argument cases, a repeat benchmark, zero repeats, a missing
option value and an unknown option. `ctest -L unit` runs the two doctest
binaries, `ctest -L cli` runs the command line checks and `ctest -L bench` runs
the benchmark modes.

An object library compiles `types.hpp`, `book.hpp`, `array_book.hpp`,
`pool_book.hpp` and `trace.hpp` in separate translation units, so every public
header must stand on its own.

CI runs the tests on Linux and Windows with GCC and MSVC, on Linux with Clang,
and under AddressSanitizer and UndefinedBehaviorSanitizer on Ubuntu. GCC and
Clang also build with `-Wconversion` and `-Wsign-conversion`. GCC adds its
duplicated-condition, logical-op, useless-cast and cast-qual warnings. Warnings
are errors in every compiler job. Both build jobs also install the package, run
the installed CLI, build `examples/consumer` against the installation, produce a
ZIP with CPack and compile trees with `BUILD_TESTING=OFF` and
`ORDERBOOK_BUILD_CLI=OFF`. A separate job checks that line coverage of the
library sources stays at or above 90 %.

## Fuzzing

Two libFuzzer targets are built when `ORDERBOOK_BUILD_FUZZERS=ON` is passed to
Clang:

- `fuzz_parse_csv` feeds arbitrary bytes to `parse_csv`, which must reject
  malformed input with an exception instead of reading out of bounds.
- `fuzz_book` feeds the same bytes to `Book`, `ArrayBook` and `PoolBook` and
  aborts if they disagree on state or trades.

```
cmake --preset fuzz
cmake --build --preset fuzz
./build/fuzz/fuzz_parse_csv -max_total_time=60 fuzz/corpus_parse
./build/fuzz/fuzz_book -max_total_time=60 fuzz/corpus_book
```

The CI job runs both for 60 seconds with AddressSanitizer and
UndefinedBehaviorSanitizer on every push and pull request. A weekly job runs
each target for 600 seconds. The corpus lives in `fuzz/corpus_parse` and
`fuzz/corpus_book`, and CI uploads it after every run as `corpus-parse`,
`corpus-book`, `corpus-parse-long` and `corpus-book-long`.

## Español

Libro de órdenes y motor de matching en C++23, con prioridad precio-tiempo,
precios enteros y benchmark reproducible. Sin dependencias externas.

Hay tres implementaciones del mismo contrato: una con niveles en `std::map`,
otra con un array contiguo indexado por tick y una tercera que añade un pool de
órdenes. Las dos primeras reservan memoria por cada orden que descansa en el
libro. `PoolBook` reserva todo al construirse y enlaza las órdenes con índices
enteros, así que cancelar y casar no hacen ninguna reserva.

Sobre un millón de eventos sintéticos, `PoolBook` es 2,4 veces más rápido que
la versión con `std::map` y 2,1 veces más rápido que la versión con array. El
cambio de `map` a array ahorra 20,4 ns por evento. El cambio de array a pool
ahorra 82,6 ns por evento, porque elimina la reserva del nodo de lista y del
nodo del índice por cada orden. Esa era la parte que impedía que el array
destacara.

También se puede reproducir un día real de BTCUSDT, 2026-11-22, con
`bench --trace`. El fichero comprimido y su hash están en `benchmarks/`. Como el
replay reconstruido mantiene una sola orden viva cada vez, el pool pasa a ser
solo 1,2 veces más rápido que el mapa, y el día real no contiene cancelaciones
porque el feed público de aggTrades no las publica.

Los tests cubren las reglas de prioridad, las ejecuciones parciales, el recorrido
de niveles por parte de las órdenes a mercado, la semántica de `cancel` y
`modify`, el rango del array, el pool lleno, la reutilización de huecos, la
ausencia de reservas en régimen, el parser CSV y una comparación diferencial
entre las tres implementaciones y un modelo de referencia independiente. El
parser y el motor se someten además a libFuzzer con AddressSanitizer y
UndefinedBehaviorSanitizer.

## License

MIT. See [LICENSE](LICENSE).
