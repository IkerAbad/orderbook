#pragma once

#include "orderbook/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

// One line of a recorded market session.
// op is 'L' (limit), 'M' (market) or 'C' (cancel).
struct TraceEvent {
    std::uint64_t ts_ns{};
    char op{};
    Side side{};
    Price price{};
    Quantity quantity{};
    OrderId id{};  // order id for L and M, referenced by C
};

struct GeneratorOptions {
    std::uint64_t events{1'000'000};
    std::uint64_t seed{42};
    Price mid{10'000};
    Quantity max_quantity{100};
};

std::vector<TraceEvent> generate(const GeneratorOptions& options);

std::vector<TraceEvent> parse_csv(std::string_view text);
bool write_csv(const std::string& path, const std::vector<TraceEvent>& events);
std::vector<TraceEvent> read_csv(const std::string& path);

}  // namespace ob
