# Changelog

## 0.2.0 - 2026-11-22

### Added

- `PoolBook`, a third implementation with a preallocated order vector, a free
  list of integer slots and intrusive `head`, `tail`, `next` and `prev` indices
  per price level.
- Open-addressing id table sized with the pool, so resting, cancelling and
  matching do not allocate after construction.
- `--impl pool` and `--impl all` in the benchmark, with a fixed price ladder and
  a fixed order capacity.
- `PoolBook::memory_bytes()` and a `pool memory` line in the benchmark.
- `orderbook/version.hpp` generated from the project version.
- Benchmark consistency check: implementations must agree on trades and volume.
- `parse_csv(std::string_view)` and libFuzzer targets for the parser and the
  three books.
- CMake presets for release, debug, clang, AddressSanitizer,
  UndefinedBehaviorSanitizer, coverage and libFuzzer, with matching workflow
  presets and a package workflow.
- Install rules and a `find_package(orderbook 0.2 CONFIG REQUIRED)` package
  exporting `orderbook::orderbook_core`, with a consumer project built against
  an installed tree in CI.
- Binary and source ZIP/TGZ packages built with CPack.
- `--help` and `--version` in the CLI.
- `--repeat` in the benchmark, with min, median and max over the repeats.
- `--workload mixed|limits|cancels|markets` for the generator, benchmark and
  depth commands.
- `--levels` in `depth` to choose how many price levels are exported.
- `--impl` in `replay`, with an agreement check when several implementations
  run.
- `--impl` in `depth`, with a byte-for-byte comparison of the generated CSV
  across implementations.
- `ORDERBOOK_BUILD_CLI=OFF`, which builds only the library.
- Independent reference model used by both differential tests.
- Header self-checks, move-only book semantics and `noexcept` query methods and
  moves.
- Ladder bounds exposed by `ArrayBook` and `PoolBook` through `min_price()` and
  `max_price()`.

### Fixed

- Market orders consumed an id in the book but not in the trace generator, so
  cancels could reference the wrong order. Every limit and market event now
  consumes one id.
- Ladder spans near `INT64_MIN` and `INT64_MAX` were undefined. The span is
  computed in unsigned arithmetic before the size check.
- Level totals could overflow `Quantity`. All three books reject the addition
  before mutating the level.
- `depth --frames 0` divided by zero and `bench --runs 0` reported zeros as
  success. Both are rejected.
- The CLI accepted unknown options, missing option values and extra positional
  arguments. They are now errors.
- The CSV reader now validates the header, the op and the side, reports the line
  number, accepts CRLF and a UTF-8 BOM and rejects empty files.
- A zero `max_quantity` in the generator is rejected.
- A generator `mid` that leaves no room for the price drift is rejected instead
  of overflowing.
- doctest 2.4.11 includes `<ciso646>` under Clang, which libstdc++ 15 turns into
  a warning. The test targets suppress that diagnostic.

### Tests and CI

- 56 doctest cases, 24 CLI checks and 26 CTest entries.
- CTest labels `unit`, `cli` and `bench` for selective runs.
- Differential tests cover 20,000 generated events and 5,000 random operations
  with modifies, against the three books and the reference model.
- CI runs GCC and MSVC on Linux and Windows, Clang on Linux, ASan and UBSan on
  Ubuntu, two 60-second libFuzzer runs and a 90 % line coverage gate.
- GCC and Clang build with `-Wconversion` and `-Wsign-conversion`, GCC adds its
  extra condition, logical and cast warnings, and warnings are errors.
