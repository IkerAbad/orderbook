#include "orderbook/trace.hpp"

#include <charconv>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ob {
namespace {

constexpr char kSeparator = ',';
constexpr std::string_view kHeader = "ts_ns,op,side,price,quantity,id";

void strip_cr(std::string& line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
}

std::vector<std::string_view> split(std::string_view line, char sep) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t pos = line.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

template <typename T>
T to_number(std::string_view text) {
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        throw std::runtime_error("malformed trace field");
    }
    return value;
}

}  // namespace

std::vector<TraceEvent> generate(const GeneratorOptions& options) {
    std::mt19937_64 rng(options.seed);
    std::uniform_int_distribution<int> coin(0, 99);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<Price> offset(1, 50);
    std::uniform_int_distribution<Price> drift(-1, 1);
    std::uniform_int_distribution<Quantity> size(1, options.max_quantity);

    std::vector<TraceEvent> events;
    events.reserve(options.events);

    std::vector<OrderId> live;
    live.reserve(options.events / 2);

    Price mid = options.mid;
    OrderId next_id = 1;
    std::uint64_t now = 0;

    for (std::uint64_t i = 0; i < options.events; ++i) {
        if (i % 64 == 0) {
            mid += drift(rng);
        }

        const Side side = side_pick(rng) == 0 ? Side::Buy : Side::Sell;
        TraceEvent event{};
        event.ts_ns = now;
        event.side = side;
        now += 1'000;

        const int roll = coin(rng);

        if (roll < 65) {
            event.op = 'L';
            event.price = side == Side::Buy ? mid - offset(rng) : mid + offset(rng);
            event.quantity = size(rng);
            event.id = next_id;
            live.push_back(next_id);
            ++next_id;
        } else if (roll < 85 && !live.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            const std::size_t idx = pick(rng);
            const OrderId id = live[idx];

            event.op = 'C';
            event.id = id;
            event.price = 0;
            event.quantity = 0;

            live[idx] = live.back();
            live.pop_back();
        } else {
            event.op = 'M';
            event.price = 0;
            event.quantity = size(rng);
            event.id = next_id++;
        }

        events.push_back(event);
    }

    return events;
}

bool write_csv(const std::string& path, const std::vector<TraceEvent>& events) {
    std::ofstream out(path);
    if (!out) {
        return false;
    }

    out << kHeader << '\n';
    for (const TraceEvent& e : events) {
        out << e.ts_ns << kSeparator << e.op << kSeparator
            << (e.side == Side::Buy ? 'B' : 'S') << kSeparator << e.price
            << kSeparator << e.quantity << kSeparator << e.id << '\n';
    }
    return static_cast<bool>(out);
}

std::vector<TraceEvent> read_csv(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open trace: " + path);
    }

    std::vector<TraceEvent> events;
    std::string line;
    std::size_t line_number = 0;

    if (!std::getline(in, line)) {
        throw std::runtime_error("trace is empty: " + path);
    }
    ++line_number;
    strip_cr(line);
    if (line != kHeader) {
        throw std::runtime_error("unexpected trace header at line 1");
    }

    while (std::getline(in, line)) {
        ++line_number;
        strip_cr(line);
        if (line.empty()) {
            continue;
        }

        const auto f = split(line, kSeparator);
        if (f.size() != 6) {
            throw std::runtime_error("trace line " + std::to_string(line_number) +
                                     " does not have 6 fields");
        }

        TraceEvent e{};
        try {
            e.ts_ns = to_number<std::uint64_t>(f[0]);
            e.op = f[1].empty() ? '?' : f[1][0];
            if (e.op != 'L' && e.op != 'M' && e.op != 'C') {
                throw std::runtime_error("unknown op");
            }

            if (f[2] == "B") {
                e.side = Side::Buy;
            } else if (f[2] == "S") {
                e.side = Side::Sell;
            } else {
                throw std::runtime_error("unknown side");
            }

            e.price = to_number<Price>(f[3]);
            e.quantity = to_number<Quantity>(f[4]);
            e.id = to_number<OrderId>(f[5]);
        } catch (const std::exception& ex) {
            throw std::runtime_error("trace line " + std::to_string(line_number) + ": " +
                                     ex.what());
        }
        events.push_back(e);
    }

    return events;
}

}  // namespace ob
