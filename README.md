# orderbook

[![ci](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-333?style=flat-square)](LICENSE)
![C++23](https://img.shields.io/badge/C%2B%2B-23-333?style=flat-square&logo=cplusplus&logoColor=white)

Limit order book and matching engine in C++23. Price-time priority, integer
prices, no external dependencies, and a benchmark you can reproduce on your own
machine.

```
$ ./orderbook bench --events 1000000 --seed 42 --runs 3
best ns/event  203.97
best events/s  4,902,640
trades         469,797
volume         11,981,522
```

Measured on a Ryzen 9 5900X, GCC 14.2, `-O3`, single thread, with a workload of
65 % limit orders, 20 % cancels and 15 % market orders. The per-event latency
percentiles are not published because the Windows clock ticks at 100 ns, which
is the same order as the operation itself. The throughput figure does not suffer
from that limit.

## What it does

- Limit orders with price-time priority. The first order at a price is the first
  one filled.
- Market orders that walk the book until the quantity is exhausted or the book
  is empty.
- `cancel` in O(1) through an id index, with no allocation.
- `modify` that follows the exchange rule: a price change or a size increase
  loses time priority, a size decrease keeps it.
- Trades always execute at the resting order's price. The maker decides.
- Deterministic: the same seed produces the same trace and the same book.

## Build

Requires CMake 3.25+ and a compiler with C++23 support (GCC 14, Clang 18,
MSVC 19.38).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Use

```
# write a synthetic session to CSV
./build/orderbook generate --events 100000 --seed 7 --out trace.csv

# replay it and print the final state of the book
./build/orderbook replay trace.csv

# benchmark
./build/orderbook bench --events 1000000 --seed 42 --runs 3
```

As a library:

```cpp
#include <orderbook/book.hpp>

ob::Book book([](const ob::Trade& t) {
    // t.taker, t.maker, t.price, t.quantity
});

book.add_limit(ob::Side::Buy, 10000, 25);   // rests at 10000
book.add_limit(ob::Side::Sell, 10000, 10);  // fills 10 at 10000
```

## Design decisions

**Prices are integers.** `Price` is `std::int64_t` counted in ticks. Floating
point prices accumulate error, compare badly and make equality checks
unreliable. Every serious engine works in integer ticks.

**The maker sets the price.** A buy order that crosses an ask at 101 executes at
101, not at the buyer's limit. This is how equities and futures work, and it is
the opposite of what a naive implementation does.

**Cancel is O(1).** The book keeps an `unordered_map<OrderId, Location>` where
`Location` holds the level and a `std::list` iterator. Removing an order is a
list erase plus a map erase, with no search.

**Insert allocates, the hot path does not.** Levels are `std::map` nodes and the
index is an `unordered_map`, so the first touch of a price allocates. Matching a
resting order never does.

**One thread.** Determinism is worth more than parallelism here. A benchmark
with threads measures the scheduler as much as the book.

## Limitations

These are deliberate, and each one is the next step rather than an oversight.

- `std::map` gives O(log n) access to the best price. A production engine uses an
  array indexed by tick, which is O(1) and cache-friendly. That comparison is
  the next milestone.
- No self-trade prevention, iceberg orders, hidden liquidity or auction phases.
- No persistence and no network gateway.
- The synthetic generator is a Poisson-like flow, not real market data. The CSV
  format exists so real captures can be replayed later.

## Tests

15 cases covering priority rules, partial fills, market orders walking several
levels, cancel and modify semantics, the spread invariant under a random
workload, and trace reproducibility. CI runs them on Linux and Windows.

## Español

Libro de órdenes y motor de matching en C++23, con prioridad precio-tiempo,
precios enteros y benchmark reproducible. Sin dependencias externas. Los tests
cubren las reglas de prioridad, las ejecuciones parciales, el recorrido de
niveles por parte de las órdenes a mercado, la semántica de `cancel` y `modify`,
y la reproducibilidad con la misma semilla.

## License

MIT. See [LICENSE](LICENSE).
