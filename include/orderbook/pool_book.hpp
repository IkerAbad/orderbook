#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ob {

// Same contract as Book and ArrayBook, with no allocation on the order path.
//
// Orders live in a preallocated vector and are linked into price levels through
// integer indices (head, tail, next, prev) instead of std::list nodes. Cancelling
// and matching only move indices, so they never touch the heap. The price ladder
// is fixed at construction, like ArrayBook, and the order pool has a fixed
// capacity chosen by the caller.
class PoolBook {
public:
    using TradeCallback = std::function<void(const Trade&)>;
    using Depth = std::vector<std::pair<Price, Quantity>>;

    // max_orders is the number of orders that can rest at the same time. Adding
    // one more throws std::overflow_error.
    PoolBook(Price min_price, Price max_price, std::size_t max_orders,
             TradeCallback on_trade = {});

    OrderId add_limit(Side side, Price price, Quantity quantity);
    Quantity add_market(Side side, Quantity quantity);
    bool cancel(OrderId id);
    bool modify(OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] Depth depth(Side side, std::size_t levels) const;
    [[nodiscard]] std::size_t order_count() const { return live_; }
    [[nodiscard]] std::size_t level_count(Side side) const;
    [[nodiscard]] std::size_t capacity() const { return orders_.size(); }

private:
    static constexpr std::uint32_t kNone = 0xFFFF'FFFFu;
    static constexpr std::size_t kMissing = static_cast<std::size_t>(-1);

    struct Level {
        std::uint32_t head{kNone};
        std::uint32_t tail{kNone};
        Quantity total{0};
    };

    Price min_price_{};
    Price max_price_{};
    std::vector<Level> bids_;
    std::vector<Level> asks_;
    std::size_t best_bid_{0};
    std::size_t best_ask_{0};
    bool has_bid_{false};
    bool has_ask_{false};

    std::vector<Order> orders_;
    std::vector<std::uint32_t> next_;
    std::vector<std::uint32_t> prev_;
    std::uint32_t free_head_{kNone};
    std::size_t live_{0};

    // Open-addressing map from order id to pool slot. Preallocated with the
    // pool, so a resting order never grows it.
    std::vector<OrderId> keys_;
    std::vector<std::uint32_t> slots_;
    std::size_t mask_{0};

    TradeCallback on_trade_;
    OrderId next_id_{1};

    [[nodiscard]] bool in_range(Price price) const;
    std::size_t index_of(Price price) const;
    [[nodiscard]] std::uint32_t alloc_slot();
    void free_slot(std::uint32_t slot);

    void index_insert(OrderId id, std::uint32_t slot);
    [[nodiscard]] std::size_t index_find(OrderId id) const;
    void index_erase_at(std::size_t pos);
    void index_erase(OrderId id);

    void push_back(Level& level, std::uint32_t slot);
    void unlink(Level& level, std::uint32_t slot);
    void pop_front(Level& level);
    void release(std::uint32_t slot);

    void rest(Order order);
    bool remove(OrderId id);
    void advance_bid();
    void advance_ask();
    Quantity match_asks(Order& taker, Price limit, bool has_limit);
    Quantity match_bids(Order& taker, Price limit, bool has_limit);
    Quantity match(Order& taker, Price limit, bool has_limit);
};

}  // namespace ob
