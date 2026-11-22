#include "orderbook/array_book.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace ob {
namespace {

std::size_t ladder_size(Price min_price, Price max_price) {
    if (max_price < min_price) {
        throw std::invalid_argument("max_price is below min_price");
    }
    constexpr std::size_t kMaxLevels = 50'000'000;
    const auto span = static_cast<std::uint64_t>(max_price) -
                      static_cast<std::uint64_t>(min_price);
    if (span >= kMaxLevels) {
        throw std::invalid_argument("price ladder is too large");
    }
    return static_cast<std::size_t>(span) + 1;
}

}  // namespace

ArrayBook::ArrayBook(Price min_price, Price max_price, TradeCallback on_trade)
    : min_price_(min_price),
      max_price_(max_price),
      bids_(ladder_size(min_price, max_price)),
      asks_(ladder_size(min_price, max_price)),
      on_trade_(std::move(on_trade)) {}

bool ArrayBook::in_range(Price price) const {
    return price >= min_price_ && price <= max_price_;
}

std::size_t ArrayBook::index_of(Price price) const {
    if (!in_range(price)) {
        throw std::out_of_range("price " + std::to_string(price) +
                                " outside the ladder [" + std::to_string(min_price_) +
                                ", " + std::to_string(max_price_) + "]");
    }
    return static_cast<std::size_t>(price - min_price_);
}

void ArrayBook::advance_bid() {
    if (!has_bid_) {
        return;
    }
    while (bids_[best_bid_].orders.empty()) {
        if (best_bid_ == 0) {
            has_bid_ = false;
            return;
        }
        --best_bid_;
    }
}

void ArrayBook::advance_ask() {
    if (!has_ask_) {
        return;
    }
    while (asks_[best_ask_].orders.empty()) {
        if (best_ask_ + 1 >= asks_.size()) {
            has_ask_ = false;
            return;
        }
        ++best_ask_;
    }
}

void ArrayBook::rest(Order order) {
    const std::size_t i = index_of(order.price);
    Location loc{order.side, order.price, {}};

    if (order.side == Side::Buy) {
        auto& level = bids_[i];
        if (order.quantity > std::numeric_limits<Quantity>::max() - level.total) {
            throw std::overflow_error("level quantity overflow");
        }
        level.orders.push_back(order);
        level.total += order.quantity;
        loc.it = std::prev(level.orders.end());
        if (!has_bid_ || i > best_bid_) {
            best_bid_ = i;
            has_bid_ = true;
        }
    } else {
        auto& level = asks_[i];
        if (order.quantity > std::numeric_limits<Quantity>::max() - level.total) {
            throw std::overflow_error("level quantity overflow");
        }
        level.orders.push_back(order);
        level.total += order.quantity;
        loc.it = std::prev(level.orders.end());
        if (!has_ask_ || i < best_ask_) {
            best_ask_ = i;
            has_ask_ = true;
        }
    }

    index_.emplace(order.id, loc);
}

Quantity ArrayBook::match_asks(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && has_ask_) {
        if (has_limit && min_price_ + static_cast<Price>(best_ask_) > limit) {
            break;
        }

        Level& level = asks_[best_ask_];
        while (taker.quantity > 0 && !level.orders.empty()) {
            Order& maker = level.orders.front();
            const Quantity q = std::min(taker.quantity, maker.quantity);

            if (on_trade_) {
                on_trade_(Trade{taker.id, maker.id, maker.price, q});
            }

            filled += q;
            taker.quantity -= q;
            maker.quantity -= q;
            level.total -= q;

            if (maker.quantity == 0) {
                index_.erase(maker.id);
                level.orders.pop_front();
            }
        }

        advance_ask();
    }

    return filled;
}

Quantity ArrayBook::match_bids(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && has_bid_) {
        if (has_limit && min_price_ + static_cast<Price>(best_bid_) < limit) {
            break;
        }

        Level& level = bids_[best_bid_];
        while (taker.quantity > 0 && !level.orders.empty()) {
            Order& maker = level.orders.front();
            const Quantity q = std::min(taker.quantity, maker.quantity);

            if (on_trade_) {
                on_trade_(Trade{taker.id, maker.id, maker.price, q});
            }

            filled += q;
            taker.quantity -= q;
            maker.quantity -= q;
            level.total -= q;

            if (maker.quantity == 0) {
                index_.erase(maker.id);
                level.orders.pop_front();
            }
        }

        advance_bid();
    }

    return filled;
}

Quantity ArrayBook::match(Order& taker, Price limit, bool has_limit) {
    return taker.side == Side::Buy ? match_asks(taker, limit, has_limit)
                                   : match_bids(taker, limit, has_limit);
}

OrderId ArrayBook::add_limit(Side side, Price price, Quantity quantity) {
    index_of(price);  // reject out-of-range prices before touching the book

    Order order{next_id_++, side, price, quantity};
    if (quantity == 0) {
        return order.id;
    }

    match(order, price, true);
    if (order.quantity > 0) {
        rest(order);
    }
    return order.id;
}

Quantity ArrayBook::add_market(Side side, Quantity quantity) {
    Order order{next_id_++, side, 0, quantity};
    return match(order, 0, false);
}

void ArrayBook::remove(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return;
    }

    const Location loc = it->second;
    const std::size_t i = index_of(loc.price);

    if (loc.side == Side::Buy) {
        auto& level = bids_[i];
        level.total -= loc.it->quantity;
        level.orders.erase(loc.it);
        if (level.orders.empty() && has_bid_ && i == best_bid_) {
            advance_bid();
        }
    } else {
        auto& level = asks_[i];
        level.total -= loc.it->quantity;
        level.orders.erase(loc.it);
        if (level.orders.empty() && has_ask_ && i == best_ask_) {
            advance_ask();
        }
    }

    index_.erase(it);
}

bool ArrayBook::cancel(OrderId id) {
    if (index_.find(id) == index_.end()) {
        return false;
    }
    remove(id);
    return true;
}

bool ArrayBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }

    const Location loc = it->second;
    if (new_quantity == 0) {
        remove(id);
        return true;
    }

    if (new_price == loc.price && new_quantity <= loc.it->quantity) {
        const Quantity delta = loc.it->quantity - new_quantity;
        loc.it->quantity = new_quantity;
        const std::size_t i = index_of(loc.price);
        if (loc.side == Side::Buy) {
            bids_[i].total -= delta;
        } else {
            asks_[i].total -= delta;
        }
        return true;
    }

    index_of(new_price);  // reject before removing the original

    const Order order = *loc.it;
    remove(id);

    Order updated = order;
    updated.price = new_price;
    updated.quantity = new_quantity;
    match(updated, new_price, true);
    if (updated.quantity > 0) {
        rest(updated);
    }
    return true;
}

std::optional<Price> ArrayBook::best_bid() const noexcept {
    if (!has_bid_) {
        return std::nullopt;
    }
    return min_price_ + static_cast<Price>(best_bid_);
}

std::optional<Price> ArrayBook::best_ask() const noexcept {
    if (!has_ask_) {
        return std::nullopt;
    }
    return min_price_ + static_cast<Price>(best_ask_);
}

Quantity ArrayBook::quantity_at(Side side, Price price) const noexcept {
    if (!in_range(price)) {
        return 0;
    }
    const std::size_t i = index_of(price);
    return side == Side::Buy ? bids_[i].total : asks_[i].total;
}

ArrayBook::Depth ArrayBook::depth(Side side, std::size_t levels) const {
    Depth out;
    out.reserve(levels);

    if (side == Side::Buy) {
        if (!has_bid_) {
            return out;
        }
        for (std::size_t i = best_bid_ + 1; i-- > 0 && out.size() < levels;) {
            if (!bids_[i].orders.empty()) {
                out.emplace_back(min_price_ + static_cast<Price>(i), bids_[i].total);
            }
        }
    } else {
        if (!has_ask_) {
            return out;
        }
        for (std::size_t i = best_ask_; i < asks_.size() && out.size() < levels; ++i) {
            if (!asks_[i].orders.empty()) {
                out.emplace_back(min_price_ + static_cast<Price>(i), asks_[i].total);
            }
        }
    }

    return out;
}

std::size_t ArrayBook::level_count(Side side) const noexcept {
    const auto& levels = side == Side::Buy ? bids_ : asks_;
    return static_cast<std::size_t>(std::count_if(
        levels.begin(), levels.end(),
        [](const Level& level) { return !level.orders.empty(); }));
}

}  // namespace ob
