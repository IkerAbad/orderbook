#include "orderbook/book.hpp"
#include "orderbook/trace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage() {
    std::cout <<
        "orderbook - limit order book and matching engine\n"
        "\n"
        "usage:\n"
        "  orderbook generate --events N --seed S --out FILE\n"
        "  orderbook replay FILE\n"
        "  orderbook bench --events N --seed S [--runs R]\n";
}

double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double idx = p * static_cast<double>(sorted.size() - 1);
    const auto lo = static_cast<std::size_t>(idx);
    const auto hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

int run_generate(std::uint64_t events, std::uint64_t seed, const std::string& out) {
    ob::GeneratorOptions options{};
    options.events = events;
    options.seed = seed;

    const auto trace = ob::generate(options);
    if (!ob::write_csv(out, trace)) {
        std::cerr << "cannot write " << out << '\n';
        return 1;
    }

    std::cout << "wrote " << trace.size() << " events to " << out << '\n';
    return 0;
}

int run_replay(const std::string& path) {
    const auto trace = ob::read_csv(path);

    std::uint64_t trades = 0;
    std::uint64_t volume = 0;
    ob::Book book([&](const ob::Trade& t) {
        ++trades;
        volume += t.quantity;
    });

    std::uint64_t cancels = 0;
    std::uint64_t cancelled = 0;

    for (const ob::TraceEvent& e : trace) {
        switch (e.op) {
            case 'L':
                book.add_limit(e.side, e.price, e.quantity);
                break;
            case 'M':
                book.add_market(e.side, e.quantity);
                break;
            case 'C':
                ++cancels;
                if (book.cancel(e.id)) {
                    ++cancelled;
                }
                break;
            default:
                break;
        }
    }

    std::cout << "events     " << trace.size() << '\n'
              << "trades     " << trades << '\n'
              << "volume     " << volume << '\n'
              << "cancels    " << cancelled << " / " << cancels << " found\n"
              << "resting    " << book.order_count() << " orders\n"
              << "levels     " << book.level_count(ob::Side::Buy) << " bid, "
              << book.level_count(ob::Side::Sell) << " ask\n";

    if (const auto bid = book.best_bid()) {
        std::cout << "best bid   " << *bid << '\n';
    }
    if (const auto ask = book.best_ask()) {
        std::cout << "best ask   " << *ask << '\n';
    }
    return 0;
}

int run_bench(std::uint64_t events, std::uint64_t seed, int runs) {
    ob::GeneratorOptions options{};
    options.events = events;
    options.seed = seed;

    const auto trace = ob::generate(options);
    std::vector<double> samples;
    samples.reserve(events);

    double best_ns_per_event = 0.0;
    std::uint64_t trades = 0;
    std::uint64_t volume = 0;

    for (int run = 0; run < runs; ++run) {
        trades = 0;
        volume = 0;
        samples.clear();

        ob::Book book([&](const ob::Trade& t) {
            ++trades;
            volume += t.quantity;
        });

        const auto total_start = std::chrono::steady_clock::now();

        for (const ob::TraceEvent& e : trace) {
            const auto start = std::chrono::steady_clock::now();
            switch (e.op) {
                case 'L':
                    book.add_limit(e.side, e.price, e.quantity);
                    break;
                case 'M':
                    book.add_market(e.side, e.quantity);
                    break;
                case 'C':
                    book.cancel(e.id);
                    break;
                default:
                    break;
            }
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration<double, std::nano>(end - start).count());
        }

        const auto total_end = std::chrono::steady_clock::now();
        const double total_ns =
            std::chrono::duration<double, std::nano>(total_end - total_start).count();
        const double ns_per_event = total_ns / static_cast<double>(trace.size());

        if (run == 0 || ns_per_event < best_ns_per_event) {
            best_ns_per_event = ns_per_event;
        }

        std::sort(samples.begin(), samples.end());

        std::cout << "run " << (run + 1) << '\n'
                  << "  total        " << total_ns / 1'000'000.0 << " ms\n"
                  << "  ns/event     " << ns_per_event << '\n'
                  << "  events/s     " << 1e9 / ns_per_event << '\n'
                  << "  p50          " << percentile(samples, 0.50) << " ns\n"
                  << "  p99          " << percentile(samples, 0.99) << " ns\n"
                  << "  p99.9        " << percentile(samples, 0.999) << " ns\n";
    }

    std::cout << "best ns/event  " << best_ns_per_event << '\n'
              << "best events/s  " << 1e9 / best_ns_per_event << '\n'
              << "trades         " << trades << '\n'
              << "volume         " << volume << '\n';
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

    std::uint64_t events = 1'000'000;
    std::uint64_t seed = 42;
    int runs = 3;
    std::string out;
    std::string input;

    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--events" && i + 1 < args.size()) {
            events = std::stoull(std::string(args[++i]));
        } else if (args[i] == "--seed" && i + 1 < args.size()) {
            seed = std::stoull(std::string(args[++i]));
        } else if (args[i] == "--runs" && i + 1 < args.size()) {
            runs = std::stoi(std::string(args[++i]));
        } else if (args[i] == "--out" && i + 1 < args.size()) {
            out = std::string(args[++i]);
        } else if (input.empty()) {
            input = std::string(args[i]);
        }
    }

    if (command == "generate") {
        if (out.empty()) {
            std::cerr << "--out is required\n";
            return 1;
        }
        return run_generate(events, seed, out);
    }
    if (command == "replay") {
        if (input.empty()) {
            std::cerr << "a trace file is required\n";
            return 1;
        }
        return run_replay(input);
    }
    if (command == "bench") {
        return run_bench(events, seed, runs);
    }

    usage();
    return 1;
}
