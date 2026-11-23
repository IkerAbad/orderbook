#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ob {

// Same contract as Book, but price levels live in a contiguous array indexed by
// tick instead of a std::map. The ladder has a fixed range, chosen when the book
// is created, and a price outside it is rejected.
class ArrayBook {
public:
    // Called for every fill. It must not throw or re-enter the book.
    using TradeCallback = std::function<void(const Trade&)>;
    using Depth = std::vector<std::pair<Price, Quantity>>;

    ArrayBook(const ArrayBook&) = delete;
    ArrayBook& operator=(const ArrayBook&) = delete;
    ArrayBook(ArrayBook&&) noexcept = default;
    ArrayBook& operator=(ArrayBook&&) noexcept = default;

    ArrayBook(Price min_price, Price max_price, TradeCallback on_trade = {});

    OrderId add_limit(Side side, Price price, Quantity quantity);
    Quantity add_market(Side side, Quantity quantity);
    bool cancel(OrderId id);
    bool modify(OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] std::optional<Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<Price> best_ask() const noexcept;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept;
    [[nodiscard]] Depth depth(Side side, std::size_t levels) const;
    [[nodiscard]] std::size_t order_count() const noexcept { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;

private:
    struct Level {
        std::list<Order> orders;
        Quantity total{0};
    };

    struct Location {
        Side side{};
        Price price{};
        std::list<Order>::iterator it{};
    };

    Price min_price_{};
    Price max_price_{};
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::size_t best_bid_{0};
    std::size_t best_ask_{0};
    bool has_bid_{false};
    bool has_ask_{false};
    std::unordered_map<OrderId, Location> index_;
    TradeCallback on_trade_;
    OrderId next_id_{1};

    [[nodiscard]] bool in_range(Price price) const;
    std::size_t index_of(Price price) const;
    void rest(Order order);
    void remove(OrderId id);
    void advance_bid();
    void advance_ask();
    Quantity match_asks(Order& taker, Price limit, bool has_limit);
    Quantity match_bids(Order& taker, Price limit, bool has_limit);
    Quantity match(Order& taker, Price limit, bool has_limit);
};

}  // namespace ob
