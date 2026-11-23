#include "orderbook/trace.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace ob {
namespace {

constexpr char kSeparator = ',';
constexpr std::string_view kHeader = "ts_ns,op,side,price,quantity,id";

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
    if (options.max_quantity == 0) {
        throw std::invalid_argument("max_quantity must be positive");
    }

    const std::uint64_t steps = options.events / 64 + 1;
    constexpr std::uint64_t kMargin = 50;
    const auto max_price =
        static_cast<std::uint64_t>(std::numeric_limits<Price>::max());
    if (steps > max_price - kMargin) {
        throw std::invalid_argument("too many events for the price drift");
    }

    const auto drift_bound = static_cast<Price>(steps);
    if (options.mid > std::numeric_limits<Price>::max() - drift_bound - 50 ||
        options.mid < std::numeric_limits<Price>::min() + drift_bound + 50) {
        throw std::invalid_argument("mid leaves no room for the drift");
    }

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

std::vector<TraceEvent> parse_csv(std::string_view text) {
    std::vector<TraceEvent> events;
    std::size_t pos = 0;
    std::size_t line_number = 0;

    const auto next_line = [&]() -> std::optional<std::string_view> {
        if (pos >= text.size()) {
            return std::nullopt;
        }
        const std::size_t end = text.find('\n', pos);
        std::string_view line =
            end == std::string_view::npos ? text.substr(pos) : text.substr(pos, end - pos);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        return line;
    };

    const auto header = next_line();
    if (!header) {
        throw std::runtime_error("trace is empty");
    }
    ++line_number;

    std::string_view header_line = *header;
    if (header_line.size() >= 3 &&
        static_cast<unsigned char>(header_line[0]) == 0xEF &&
        static_cast<unsigned char>(header_line[1]) == 0xBB &&
        static_cast<unsigned char>(header_line[2]) == 0xBF) {
        header_line.remove_prefix(3);
    }
    if (header_line != kHeader) {
        throw std::runtime_error("unexpected trace header at line 1");
    }

    while (const auto line = next_line()) {
        ++line_number;
        if (line->empty()) {
            continue;
        }

        const auto f = split(*line, kSeparator);
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

std::vector<TraceEvent> read_csv(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open trace: " + path);
    }

    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    return parse_csv(text);
}

}  // namespace ob
