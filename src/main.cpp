#include "orderbook/array_book.hpp"
#include "orderbook/book.hpp"
#include "orderbook/pool_book.hpp"
#include "orderbook/trace.hpp"
#include "orderbook/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout <<
        "orderbook " ORDERBOOK_VERSION_STRING " - limit order book and matching engine\n"
        "\n"
        "usage:\n"
        "  orderbook --version\n"
        "  orderbook --help\n"
        "  orderbook generate --events N --seed S [--workload mixed|limits|cancels|markets] --out FILE\n"
        "  orderbook replay FILE [--impl map|array|pool|both|all]\n"
        "  orderbook bench --events N --seed S [--runs R] [--repeat R] [--workload mixed|limits|cancels|markets] [--impl map|array|pool|both|all]\n"
        "  orderbook depth --events N --seed S --frames K [--levels K] [--workload mixed|limits|cancels|markets] --out FILE [--impl map|array|pool|both|all]\n";
}

template <class BookT>
void apply(BookT& book, const ob::TraceEvent& event) {
    switch (event.op) {
        case 'L':
            book.add_limit(event.side, event.price, event.quantity);
            break;
        case 'M':
            book.add_market(event.side, event.quantity);
            break;
        case 'C':
            book.cancel(event.id);
            break;
        default:
            break;
    }
}

struct ImplSelection {
    bool map{};
    bool array{};
    bool pool{};
};

ImplSelection select_impl(const std::string& impl) {
    return ImplSelection{
        impl == "map" || impl == "both" || impl == "all",
        impl == "array" || impl == "both" || impl == "all",
        impl == "pool" || impl == "all",
    };
}

bool apply_workload(const std::string& name, ob::GeneratorOptions& options) {
    if (name == "mixed") {
        options.limit_percent = 65;
        options.cancel_percent = 20;
    } else if (name == "limits") {
        options.limit_percent = 100;
        options.cancel_percent = 0;
    } else if (name == "cancels") {
        options.limit_percent = 50;
        options.cancel_percent = 50;
    } else if (name == "markets") {
        options.limit_percent = 50;
        options.cancel_percent = 0;
    } else {
        return false;
    }
    return true;
}

int run_generate(std::uint64_t events, std::uint64_t seed, const std::string& workload,
                 const std::string& out) {
    ob::GeneratorOptions options{};
    options.events = events;
    options.seed = seed;
    if (!apply_workload(workload, options)) {
        std::cerr << "--workload must be mixed, limits, cancels or markets\n";
        return 1;
    }

    const auto trace = ob::generate(options);
    if (!ob::write_csv(out, trace)) {
        std::cerr << "cannot write " << out << '\n';
        return 1;
    }

    std::uint64_t limits = 0;
    std::uint64_t cancels = 0;
    std::uint64_t markets = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op == 'L') {
            ++limits;
        } else if (e.op == 'C') {
            ++cancels;
        } else if (e.op == 'M') {
            ++markets;
        }
    }

    std::cout << "wrote " << trace.size() << " events to " << out << " (" << limits
              << " limit, " << cancels << " cancel, " << markets << " market)\n";
    return 0;
}

struct ReplayResult {
    std::uint64_t trades{};
    std::uint64_t volume{};
    std::uint64_t cancels{};
    std::uint64_t cancelled{};
    std::size_t resting{};
    std::size_t bid_levels{};
    std::size_t ask_levels{};
    std::optional<ob::Price> best_bid{};
    std::optional<ob::Price> best_ask{};
};

template <class BookT, class Factory>
ReplayResult replay_once(const std::vector<ob::TraceEvent>& trace, Factory factory) {
    ReplayResult result{};
    BookT book = factory([&](const ob::Trade& t) {
        ++result.trades;
        result.volume += t.quantity;
    });

    for (const ob::TraceEvent& e : trace) {
        if (e.op == 'C') {
            ++result.cancels;
            if (book.cancel(e.id)) {
                ++result.cancelled;
            }
        } else {
            apply(book, e);
        }
    }

    result.resting = book.order_count();
    result.bid_levels = book.level_count(ob::Side::Buy);
    result.ask_levels = book.level_count(ob::Side::Sell);
    result.best_bid = book.best_bid();
    result.best_ask = book.best_ask();
    return result;
}

bool same_replay(const ReplayResult& a, const ReplayResult& b) {
    return a.trades == b.trades && a.volume == b.volume && a.cancels == b.cancels &&
           a.cancelled == b.cancelled && a.resting == b.resting &&
           a.bid_levels == b.bid_levels && a.ask_levels == b.ask_levels &&
           a.best_bid == b.best_bid && a.best_ask == b.best_ask;
}

void print_replay(const ReplayResult& r, std::size_t events) {
    std::cout << "events     " << events << '\n'
              << "trades     " << r.trades << '\n'
              << "volume     " << r.volume << '\n'
              << "cancels    " << r.cancelled << " / " << r.cancels << " found\n"
              << "resting    " << r.resting << " orders\n"
              << "levels     " << r.bid_levels << " bid, " << r.ask_levels << " ask\n";

    if (r.best_bid) {
        std::cout << "best bid   " << *r.best_bid << '\n';
    }
    if (r.best_ask) {
        std::cout << "best ask   " << *r.best_ask << '\n';
    }
}

int run_replay(const std::string& path, const std::string& impl) {
    const auto trace = ob::read_csv(path);
    const ImplSelection selection = select_impl(impl);
    if (!selection.map && !selection.array && !selection.pool) {
        std::cerr << "--impl must be map, array, pool, both or all\n";
        return 1;
    }

    ob::Price lo = 0;
    ob::Price hi = 0;
    bool seen = false;
    std::size_t limit_events = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op != 'L') {
            continue;
        }
        ++limit_events;
        if (!seen) {
            lo = hi = e.price;
            seen = true;
        } else {
            lo = std::min(lo, e.price);
            hi = std::max(hi, e.price);
        }
    }

    if ((selection.array || selection.pool) && !seen) {
        std::cerr << "the trace has no limit orders\n";
        return 1;
    }

    if (seen) {
        lo -= 100;
        hi += 100;
    }

    std::optional<ReplayResult> first;
    const auto check = [&](const ReplayResult& r) {
        if (!first) {
            first = r;
            return true;
        }
        return same_replay(*first, r);
    };

    if (selection.map) {
        const ReplayResult r =
            replay_once<ob::Book>(trace, [](ob::Book::TradeCallback cb) {
                return ob::Book(std::move(cb));
            });
        if (!check(r)) {
            std::cerr << "replay implementations disagree\n";
            return 1;
        }
    }

    if (selection.array) {
        const ReplayResult r = replay_once<ob::ArrayBook>(
            trace, [lo, hi](ob::ArrayBook::TradeCallback cb) {
                return ob::ArrayBook(lo, hi, std::move(cb));
            });
        if (!check(r)) {
            std::cerr << "replay implementations disagree\n";
            return 1;
        }
    }

    if (selection.pool) {
        const ReplayResult r = replay_once<ob::PoolBook>(
            trace, [lo, hi, limit_events](ob::PoolBook::TradeCallback cb) {
                return ob::PoolBook(lo, hi, limit_events, std::move(cb));
            });
        if (!check(r)) {
            std::cerr << "replay implementations disagree\n";
            return 1;
        }
    }

    print_replay(*first, trace.size());

    const int count = (selection.map ? 1 : 0) + (selection.array ? 1 : 0) +
                      (selection.pool ? 1 : 0);
    if (count > 1) {
        std::cout << "implementations  ";
        if (selection.map && selection.array && selection.pool) {
            std::cout << "map, array and pool agree\n";
        } else if (selection.map && selection.array) {
            std::cout << "map and array agree\n";
        } else if (selection.map && selection.pool) {
            std::cout << "map and pool agree\n";
        } else {
            std::cout << "array and pool agree\n";
        }
    }

    return 0;
}

struct BenchResult {
    double ns_per_event{};
    double events_per_second{};
    std::uint64_t trades{};
    std::uint64_t volume{};
};

template <class BookT, class Factory>
BenchResult bench_impl(const std::vector<ob::TraceEvent>& trace, int runs, Factory factory) {
    BenchResult best{};
    bool first = true;

    for (int run = 0; run < runs; ++run) {
        std::uint64_t trades = 0;
        std::uint64_t volume = 0;
        auto callback = [&](const ob::Trade& t) {
            ++trades;
            volume += t.quantity;
        };

        BookT book = factory(callback);

        const auto start = std::chrono::steady_clock::now();
        for (const ob::TraceEvent& event : trace) {
            apply(book, event);
        }
        const auto end = std::chrono::steady_clock::now();

        const double total_ns =
            std::chrono::duration<double, std::nano>(end - start).count();
        const double ns_per_event = total_ns / static_cast<double>(trace.size());

        if (first || ns_per_event < best.ns_per_event) {
            best.ns_per_event = ns_per_event;
            best.events_per_second = 1e9 / ns_per_event;
            best.trades = trades;
            best.volume = volume;
            first = false;
        }
    }

    return best;
}

struct BenchSet {
    BenchResult map{};
    BenchResult array{};
    BenchResult pool{};
};

BenchSet run_bench_set(const std::vector<ob::TraceEvent>& trace, int runs,
                       const ImplSelection& selection, ob::Price lo, ob::Price hi,
                       std::size_t limit_events) {
    BenchSet set;

    if (selection.map) {
        set.map = bench_impl<ob::Book>(
            trace, runs, [](ob::Book::TradeCallback cb) { return ob::Book(std::move(cb)); });
    }
    if (selection.array) {
        set.array = bench_impl<ob::ArrayBook>(
            trace, runs, [lo, hi](ob::ArrayBook::TradeCallback cb) {
                return ob::ArrayBook(lo, hi, std::move(cb));
            });
    }
    if (selection.pool) {
        set.pool = bench_impl<ob::PoolBook>(
            trace, runs, [lo, hi, limit_events](ob::PoolBook::TradeCallback cb) {
                return ob::PoolBook(lo, hi, limit_events, std::move(cb));
            });
    }

    const auto mismatch = [](const BenchResult& a, const BenchResult& b) {
        return a.trades != b.trades || a.volume != b.volume;
    };
    if ((selection.map && selection.array && mismatch(set.map, set.array)) ||
        (selection.map && selection.pool && mismatch(set.map, set.pool)) ||
        (selection.array && selection.pool && mismatch(set.array, set.pool))) {
        throw std::runtime_error(
            "benchmark implementations disagree on trades or volume");
    }
    return set;
}

void print_stats(const char* name, std::vector<double> values, std::uint64_t trades) {
    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    const double middle =
        n % 2 == 1 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;

    std::cout << name << "min " << values.front() << "  median " << middle
              << "  max " << values.back() << " ns/event   " << trades
              << " trades\n";
}

int run_bench(std::uint64_t events, std::uint64_t seed, int runs, int repeat,
              const std::string& workload, const std::string& impl) {
    if (runs < 1) {
        std::cerr << "--runs must be at least 1\n";
        return 1;
    }
    if (repeat < 1) {
        std::cerr << "--repeat must be at least 1\n";
        return 1;
    }

    ob::GeneratorOptions options{};
    options.events = events;
    options.seed = seed;
    if (!apply_workload(workload, options)) {
        std::cerr << "--workload must be mixed, limits, cancels or markets\n";
        return 1;
    }

    const auto trace = ob::generate(options);
    const ImplSelection selection = select_impl(impl);
    if (!selection.map && !selection.array && !selection.pool) {
        std::cerr << "--impl must be map, array, pool, both or all\n";
        return 1;
    }

    ob::Price lo = 0;
    ob::Price hi = 0;
    bool seen = false;
    std::size_t limit_events = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op != 'L') {
            continue;
        }
        ++limit_events;
        if (!seen) {
            lo = hi = e.price;
            seen = true;
        } else {
            lo = std::min(lo, e.price);
            hi = std::max(hi, e.price);
        }
    }

    if (!seen) {
        std::cerr << "the trace has no limit orders\n";
        return 1;
    }

    lo -= 100;
    hi += 100;

    std::cout << "workload  " << trace.size() << " events, seed " << seed << ", "
              << workload << ", ladder [" << lo << ", " << hi << "]\n";

    if (repeat == 1) {
        const BenchSet set =
            run_bench_set(trace, runs, selection, lo, hi, limit_events);

        const auto print = [](const char* name, const BenchResult& r) {
            std::cout << name << r.ns_per_event << " ns/event   "
                      << static_cast<std::uint64_t>(r.events_per_second)
                      << " events/s   " << r.trades << " trades\n";
        };

        if (selection.map) {
            print("map    ", set.map);
        }
        if (selection.array) {
            print("array  ", set.array);
        }
        if (selection.pool) {
            print("pool   ", set.pool);
        }
    } else {
        std::vector<double> map_ns;
        std::vector<double> array_ns;
        std::vector<double> pool_ns;
        std::uint64_t trades = 0;

        for (int r = 0; r < repeat; ++r) {
            const BenchSet set =
                run_bench_set(trace, runs, selection, lo, hi, limit_events);
            if (selection.map) {
                map_ns.push_back(set.map.ns_per_event);
            }
            if (selection.array) {
                array_ns.push_back(set.array.ns_per_event);
            }
            if (selection.pool) {
                pool_ns.push_back(set.pool.ns_per_event);
            }
            if (selection.map) {
                trades = set.map.trades;
            } else if (selection.array) {
                trades = set.array.trades;
            } else {
                trades = set.pool.trades;
            }
        }

        std::cout << std::fixed << std::setprecision(1);
        if (selection.map) {
            print_stats("map    ", map_ns, trades);
        }
        if (selection.array) {
            print_stats("array  ", array_ns, trades);
        }
        if (selection.pool) {
            print_stats("pool   ", pool_ns, trades);
        }
    }

    if (selection.pool) {
        const ob::PoolBook probe(lo, hi, limit_events);
        const double mib =
            static_cast<double>(probe.memory_bytes()) / (1024.0 * 1024.0);
        std::cout << "pool memory  " << mib << " MiB, " << limit_events
                  << " slots\n";
    }

    return 0;
}

template <class BookT, class Factory>
std::size_t write_depth(const std::vector<ob::TraceEvent>& trace, std::size_t frames,
                        std::size_t levels, std::ostream& out, Factory factory) {
    BookT book = factory();
    out << "frame,side,rank,price,quantity\n";

    const std::size_t total = trace.size();
    const std::size_t step = std::max<std::size_t>(1, (total + frames - 1) / frames);
    std::size_t frame = 0;

    for (std::size_t i = 0; i < total && frame < frames; ++i) {
        apply(book, trace[i]);

        if ((i + 1) % step == 0 || i + 1 == total) {
            const auto bids = book.depth(ob::Side::Buy, levels);
            const auto asks = book.depth(ob::Side::Sell, levels);

            for (std::size_t rank = 0; rank < bids.size(); ++rank) {
                out << frame << ",B," << rank << ',' << bids[rank].first << ','
                    << bids[rank].second << '\n';
            }
            for (std::size_t rank = 0; rank < asks.size(); ++rank) {
                out << frame << ",S," << rank << ',' << asks[rank].first << ','
                    << asks[rank].second << '\n';
            }
            ++frame;
        }
    }

    return frame;
}

int run_depth(std::uint64_t events, std::uint64_t seed, std::size_t frames,
              std::size_t levels, const std::string& workload,
              const std::string& impl, const std::string& out) {
    if (frames == 0) {
        std::cerr << "--frames must be at least 1\n";
        return 1;
    }
    if (levels == 0) {
        std::cerr << "--levels must be at least 1\n";
        return 1;
    }

    ob::GeneratorOptions options{};
    options.events = events;
    options.seed = seed;
    if (!apply_workload(workload, options)) {
        std::cerr << "--workload must be mixed, limits, cancels or markets\n";
        return 1;
    }

    const auto trace = ob::generate(options);
    const ImplSelection selection = select_impl(impl);
    if (!selection.map && !selection.array && !selection.pool) {
        std::cerr << "--impl must be map, array, pool, both or all\n";
        return 1;
    }

    ob::Price lo = 0;
    ob::Price hi = 0;
    bool seen = false;
    std::size_t limit_events = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op != 'L') {
            continue;
        }
        ++limit_events;
        if (!seen) {
            lo = hi = e.price;
            seen = true;
        } else {
            lo = std::min(lo, e.price);
            hi = std::max(hi, e.price);
        }
    }

    if ((selection.array || selection.pool) && !seen) {
        std::cerr << "the trace has no limit orders\n";
        return 1;
    }

    if (seen) {
        lo -= 100;
        hi += 100;
    }

    std::ostringstream map_out;
    std::ostringstream array_out;
    std::ostringstream pool_out;
    std::optional<std::string> first_output;
    std::size_t frame_count = 0;

    const auto check = [&](std::ostringstream& stream, std::size_t count) {
        const std::string data = stream.str();
        if (!first_output) {
            first_output = data;
            frame_count = count;
            return true;
        }
        return *first_output == data;
    };

    if (selection.map) {
        const std::size_t count = write_depth<ob::Book>(trace, frames, levels, map_out,
                                                        [] { return ob::Book(); });
        if (!check(map_out, count)) {
            std::cerr << "depth implementations disagree\n";
            return 1;
        }
    }
    if (selection.array) {
        const std::size_t count = write_depth<ob::ArrayBook>(
            trace, frames, levels, array_out,
            [lo, hi] { return ob::ArrayBook(lo, hi); });
        if (!check(array_out, count)) {
            std::cerr << "depth implementations disagree\n";
            return 1;
        }
    }
    if (selection.pool) {
        const std::size_t count = write_depth<ob::PoolBook>(
            trace, frames, levels, pool_out,
            [lo, hi, limit_events] { return ob::PoolBook(lo, hi, limit_events); });
        if (!check(pool_out, count)) {
            std::cerr << "depth implementations disagree\n";
            return 1;
        }
    }

    std::ofstream file(out);
    if (!file) {
        std::cerr << "cannot write " << out << '\n';
        return 1;
    }
    file << *first_output;

    std::cout << "wrote " << frame_count << " frames to " << out << '\n';

    const int count = (selection.map ? 1 : 0) + (selection.array ? 1 : 0) +
                      (selection.pool ? 1 : 0);
    if (count > 1) {
        std::cout << "implementations  ";
        if (selection.map && selection.array && selection.pool) {
            std::cout << "map, array and pool agree\n";
        } else if (selection.map && selection.array) {
            std::cout << "map and array agree\n";
        } else if (selection.map && selection.pool) {
            std::cout << "map and pool agree\n";
        } else {
            std::cout << "array and pool agree\n";
        }
    }

    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.empty()) {
        usage();
        return 1;
    }

    const std::string_view command = args[0];

    if (command == "--help" || command == "-h") {
        usage();
        return 0;
    }
    if (command == "--version" || command == "-v") {
        std::cout << "orderbook " << ORDERBOOK_VERSION_STRING << '\n';
        return 0;
    }

    std::uint64_t events = 1'000'000;
    std::uint64_t seed = 42;
    int runs = 3;
    int repeat = 1;
    std::size_t frames = 60;
    std::size_t levels = 15;
    std::string out;
    std::string input;
    std::string impl = "all";
    bool impl_set = false;
    std::string workload = "mixed";
    bool workload_set = false;

    try {
        for (std::size_t i = 1; i < args.size(); ++i) {
            const auto value = [&](std::string_view name) -> std::string {
                if (i + 1 >= args.size()) {
                    throw std::runtime_error(std::string(name) + " needs a value");
                }
                return std::string(args[++i]);
            };

            const std::string_view arg = args[i];
            if (arg == "--events") {
                events = std::stoull(value("--events"));
            } else if (arg == "--seed") {
                seed = std::stoull(value("--seed"));
            } else if (arg == "--runs") {
                runs = std::stoi(value("--runs"));
            } else if (arg == "--repeat") {
                repeat = std::stoi(value("--repeat"));
            } else if (arg == "--frames") {
                frames = std::stoull(value("--frames"));
            } else if (arg == "--levels") {
                levels = std::stoull(value("--levels"));
            } else if (arg == "--workload") {
                workload = value("--workload");
                workload_set = true;
            } else if (arg == "--out") {
                out = value("--out");
            } else if (arg == "--impl") {
                impl = value("--impl");
                impl_set = true;
            } else if (arg.starts_with("--")) {
                throw std::runtime_error("unknown option " + std::string(arg));
            } else if (input.empty()) {
                input = std::string(arg);
            } else {
                throw std::runtime_error("unexpected argument " + std::string(arg));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "bad argument: " << e.what() << '\n';
        return 1;
    }

    try {
        if (command == "generate") {
            if (out.empty()) {
                std::cerr << "--out is required\n";
                return 1;
            }
            return run_generate(events, seed, workload, out);
        }
        if (command == "replay") {
            if (input.empty()) {
                std::cerr << "a trace file is required\n";
                return 1;
            }
            if (workload_set) {
                std::cerr << "--workload is not used by replay\n";
                return 1;
            }
            return run_replay(input, impl_set ? impl : "map");
        }
        if (command == "bench") {
            return run_bench(events, seed, runs, repeat, workload, impl);
        }
        if (command == "depth") {
            if (out.empty()) {
                std::cerr << "--out is required\n";
                return 1;
            }
            return run_depth(events, seed, frames, levels, workload,
                             impl_set ? impl : "map", out);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }

    usage();
    return 1;
}
