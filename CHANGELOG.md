# Changelog

## Unreleased

### Added

- `bench --trace FILE` runs the same timing harness on a recorded CSV instead
  of the synthetic generator.
- `tools/binance_archive_to_parquet.py` normalizes a Binance daily aggTrades
  archive into the `market-data-service` Parquet schema.
- `tools/parquet_to_trace.py` converts that Parquet into a replay CSV, with one
  limit order and one market order per aggregate trade.
- `tools/trace_report.py` measures event mix, size distribution, bursts and
  cancel share from the CSV that the engine reads.
- `benchmarks/` holds one full UTC day of BTCUSDT, 2026-11-22, compressed with
  zstd, with the SHA256 of the archive, the Parquet and the CSV.
- README comparison between the synthetic mixed workload and the real day.

### Fixed

- `ArrayBook` and `PoolBook` track the number of non-empty levels, so an emptied
  side does not scan the whole price ladder to find the next best price. On the
  real BTCUSDT day this removed a 26 to 30 microsecond scan per market order.

### Tests and CI

- 28 CTest entries. The new `bench_trace` check runs the benchmark from
  `tests/data/sample.csv` through all three books.

## 1.0.0 - 2026-11-23

First public release. The engine and tooling changes are listed under 0.2.0.
This release adds the publication and continuous fuzzing pipeline.

### Added

- GitHub release workflow that builds and tests the tag on Linux and Windows
  and attaches the CPack ZIP packages and the source TGZ.
- Weekly long libFuzzer job, 600 seconds per target, in addition to the
  60-second run on every push.
- Corpus artifacts published from the fuzzing jobs.
- The Windows package links the MinGW runtime statically, so the downloaded
  binary runs without extra DLLs.

### Tests and CI

- 57 doctest cases, 25 CLI checks and 27 CTest entries.
- The release workflow runs the full test suite and the installed binary on
  both platforms before publishing.
- The benchmark section records hardware, date and the exact command.

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
- `read_csv_stream` and a streaming `replay`, so large traces are not stored
  twice.
- `stats` command for a recorded trace.
- CMake project description and homepage metadata, reused by CPack.
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

- 57 doctest cases, 25 CLI checks and 27 CTest entries.
- CTest labels `unit`, `cli` and `bench` for selective runs.
- Differential tests cover 20,000 generated events and 5,000 random operations
  with modifies, against the three books and the reference model.
- CI runs GCC and MSVC on Linux and Windows, Clang on Linux, ASan and UBSan on
  Ubuntu, two 60-second libFuzzer runs and a 90 % line coverage gate.
- GCC and Clang build with `-Wconversion` and `-Wsign-conversion`, GCC adds its
  extra condition, logical and cast warnings, and warnings are errors.

## 0.1.0 - 2026-11-22

### Added

- `Book` with `std::map` price levels and `std::list` orders.
- `ArrayBook` with a fixed price ladder.
- `generate`, `replay`, `bench` and `depth` commands.
- 19 doctest cases and CI on Linux and Windows.
