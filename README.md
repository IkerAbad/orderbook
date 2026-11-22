# orderbook

[![ci](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml/badge.svg)](https://github.com/IkerAbad/orderbook/actions/workflows/ci.yml)
[![license](https://img.shields.io/badge/license-MIT-333?style=flat-square)](LICENSE)
![version](https://img.shields.io/badge/version-0.2.0-333?style=flat-square)

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

## Benchmark

```
$ ./build/orderbook bench --events 1000000 --seed 42 --runs 3 --impl all
workload  1000000 events, seed 42, ladder [9809, 10339]
map    141.048 ns/event   7089800 events/s   457777 trades
array  120.061 ns/event   8329099 events/s   457777 trades
pool    42.1251 ns/event   23738816 events/s   457777 trades
```

Measured on a Ryzen 9 5900X, Windows 11, GCC 14.2 in `Release` mode (`-O3`),
single thread, best of three runs. On this run `PoolBook` is 3.3 times faster
than `Book` and 2.9 times faster than `ArrayBook`. Ten consecutive runs of the
same command gave 138.9-154.5 ns/event for `map`, 118.3-134.9 for `array` and
41.7-53.3 for `pool`.

The workload comes from the same binary:

```
$ ./build/orderbook generate --events 1000000 --seed 42 --out trace.csv
wrote 1000000 events to trace.csv (650012 limit, 199987 cancel, 150001 market)
```

The generator draws 65 % limit orders, 20 % cancels and 15 % market orders. It
assigns an id to every limit and market order, so the cancel ids stay aligned
with the book. It does not know which orders the book has filled, so some cancel
events reference orders that already executed and the book rejects them. The
rare event where the generator has no id left becomes a market order, which
explains the small drift from the exact percentages.

### Where the time goes

- `map` to `array` saves 21.0 ns/event, 14.9 % of the map time. Price-level
  lookup stops walking a red-black tree and stops allocating a node for every
  new price.
- `array` to `pool` saves 77.9 ns/event, 64.9 % of the array time. A resting
  limit order stops allocating a `std::list` node and an `unordered_map` node,
  and matching stops chasing two pointers per order.
- The second change is about 3.7 times larger than the first. That is the
  measured answer to why the array alone did not stand out: both earlier books
  paid the per-order allocation, and 65 % of the events are limit orders.
- The remaining 42 ns/event are the match loop, the callback, the trade
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

## Use

```
# write a synthetic session to CSV
./build/orderbook generate --events 100000 --seed 7 --out trace.csv

# replay it and print the final state of the book
./build/orderbook replay trace.csv

# compare the three implementations
./build/orderbook bench --events 1000000 --seed 42 --runs 3 --impl all

# export depth snapshots for the plot
./build/orderbook depth --events 200000 --seed 7 --frames 60 --out depth.csv
python tools/visualize.py --input depth.csv --png docs/depth.png --gif docs/depth.gif
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

`ArrayBook` and `PoolBook` take the price ladder range in the constructor.
`PoolBook` also takes the number of orders that can rest at once:

```cpp
#include <orderbook/array_book.hpp>
#include <orderbook/pool_book.hpp>

ob::ArrayBook array_book(9900, 10100, on_trade);
ob::PoolBook pool_book(9900, 10100, 500000, on_trade);
```

A price outside the ladder throws `std::out_of_range`. If the remainder of an
order would need a slot and the pool is full, `add_limit` throws
`std::overflow_error`. A non-crossing order in that situation leaves the book
unchanged. A crossing order keeps the trades it already executed, because they
were matched before the remainder looked for a slot.

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

**One thread.** Determinism is worth more than parallelism here. A benchmark
with threads measures the scheduler as much as the book.

## Limitations

- No self-trade prevention, iceberg orders, hidden liquidity or auction phases.
- No persistence and no network gateway.
- The synthetic generator is a Poisson-like flow, not real market data. The CSV
  format exists so real captures can be replayed later.
- `ArrayBook` and `PoolBook` need a fixed price range. Outside it, the price is
  rejected.
- `PoolBook` needs a bound on resting orders. In the benchmark the bound is the
  number of limit events, which is safe but conservative. A production
  deployment would size the pool from its peak live orders plus a margin.
- Per-event latency percentiles are not published because the Windows clock
  ticks at 100 ns, which is the same order as the operation itself. The
  throughput figure does not suffer from that limit.

## Tests

34 cases. The original 19 stay unchanged and cover priority rules, partial
fills, market orders walking several levels, cancel and modify semantics, the
spread invariant under a random workload, trace reproducibility and the array
ladder range. Running `./build/orderbook_tests` reports those 19 cases.

The 15 new cases cover the pool: slot reuse, a middle cancel that must not break
the intrusive list, a full pool, invalid ladders, capacities and a span that
overflows `Price`, depth, modify with an invalid price that must keep the
original order, and a counter that replaces global `operator new` and asserts
that 50 rounds of adding, matching, modifying and cancelling do not allocate.
They also check that every limit and market event consumes one id, so the
cancels in a generated trace reference the order the book actually assigned. A
differential test runs 20,000 events through `Book`, `ArrayBook` and `PoolBook`
and compares the best prices, live orders, level counts, every trade and the
executed volume. Running `./build/orderbook_pool_tests` reports the 15 new
cases. Two CLI checks pass `--runs 0` and `--frames 0` and require the error
message instead of a crash.

CI runs the tests on Linux and Windows with GCC and MSVC, and runs them again
under AddressSanitizer and UndefinedBehaviorSanitizer on Ubuntu.

## Español

Libro de órdenes y motor de matching en C++23, con prioridad precio-tiempo,
precios enteros y benchmark reproducible. Sin dependencias externas.

Hay tres implementaciones del mismo contrato: una con niveles en `std::map`,
otra con un array contiguo indexado por tick y una tercera que añade un pool de
órdenes. Las dos primeras reservan memoria por cada orden que descansa en el
libro. `PoolBook` reserva todo al construirse y enlaza las órdenes con índices
enteros, así que cancelar y casar no hacen ninguna reserva.

Sobre un millón de eventos, `PoolBook` es 3,3 veces más rápido que la versión
con `std::map` y 2,9 veces más rápido que la versión con array. El cambio de
`map` a array ahorra 21,0 ns por evento. El cambio de array a pool ahorra 77,9
ns por evento, porque elimina la reserva del nodo de lista y del nodo del índice
por cada orden. Esa era la parte que impedía que el array destacara.

Los tests cubren las reglas de prioridad, las ejecuciones parciales, el recorrido
de niveles por parte de las órdenes a mercado, la semántica de `cancel` y
`modify`, el rango del array, el pool lleno, la reutilización de huecos, la
ausencia de reservas en régimen y una comparación diferencial entre las tres
implementaciones con la misma carga.

## License

MIT. See [LICENSE](LICENSE).
