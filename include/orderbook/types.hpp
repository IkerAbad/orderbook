#pragma once

#include <cstdint>

namespace ob {

using OrderId = std::uint64_t;
using Price = std::int64_t;   // integer ticks, never floating point
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };

struct Order {
    OrderId id{};
    Side side{};
    Price price{};
    Quantity quantity{};
};

struct Trade {
    OrderId taker{};
    OrderId maker{};
    Price price{};
    Quantity quantity{};
};

}  // namespace ob
