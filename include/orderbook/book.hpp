#pragma once

#include "orderbook/types.hpp"

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ob {

// Limit order book with price-time priority.
//
// Prices are integer ticks. Matching always executes at the resting order's
// price (the maker decides), which is the standard rule in equity and futures
// markets. The book is single-threaded and allocation happens on insert only:
// cancels and matches never allocate.
class Book {
public:
    // Called for every fill. It must not throw or re-enter the book.
    using TradeCallback = std::function<void(const Trade&)>;
    using Depth = std::vector<std::pair<Price, Quantity>>;

    explicit Book(TradeCallback on_trade = {});

    // Returns the id assigned to the incoming order. If the order crosses the
    // spread it is matched first and only the remainder rests in the book.
    OrderId add_limit(Side side, Price price, Quantity quantity);

    // Consumes the opposite side until quantity is exhausted or the book is
    // empty. Returns the executed quantity.
    Quantity add_market(Side side, Quantity quantity);

    bool cancel(OrderId id);

    // A price change or an increase in quantity loses time priority, as in a
    // real exchange. A decrease keeps the original position. The id is kept.
    bool modify(OrderId id, Price new_price, Quantity new_quantity);

    [[nodiscard]] std::optional<Price> best_bid() const;
    [[nodiscard]] std::optional<Price> best_ask() const;
    [[nodiscard]] Quantity quantity_at(Side side, Price price) const;
    [[nodiscard]] Depth depth(Side side, std::size_t levels) const;
    [[nodiscard]] std::size_t order_count() const { return index_.size(); }
    [[nodiscard]] std::size_t level_count(Side side) const;

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

    using BidLevels = std::map<Price, Level, std::greater<Price>>;
    using AskLevels = std::map<Price, Level>;

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Location> index_;
    TradeCallback on_trade_;
    OrderId next_id_{1};

    void rest(Order order);
    void remove(OrderId id);
    Quantity match_asks(Order& taker, Price limit, bool has_limit);
    Quantity match_bids(Order& taker, Price limit, bool has_limit);
    Quantity match(Order& taker, Price limit, bool has_limit);
};

}  // namespace ob
