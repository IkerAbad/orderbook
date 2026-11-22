#include "orderbook/book.hpp"

#include <algorithm>
#include <iterator>

namespace ob {

Book::Book(TradeCallback on_trade) : on_trade_(std::move(on_trade)) {}

void Book::rest(Order order) {
    Location loc{order.side, order.price, {}};

    if (order.side == Side::Buy) {
        auto& level = bids_[order.price];
        level.orders.push_back(order);
        level.total += order.quantity;
        loc.it = std::prev(level.orders.end());
    } else {
        auto& level = asks_[order.price];
        level.orders.push_back(order);
        level.total += order.quantity;
        loc.it = std::prev(level.orders.end());
    }

    index_.emplace(order.id, loc);
}

Quantity Book::match_asks(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && !asks_.empty()) {
        auto level_it = asks_.begin();
        if (has_limit && level_it->first > limit) {
            break;
        }

        Level& level = level_it->second;
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

        if (level.orders.empty()) {
            asks_.erase(level_it);
        }
    }

    return filled;
}

Quantity Book::match_bids(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && !bids_.empty()) {
        auto level_it = bids_.begin();
        if (has_limit && level_it->first < limit) {
            break;
        }

        Level& level = level_it->second;
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

        if (level.orders.empty()) {
            bids_.erase(level_it);
        }
    }

    return filled;
}

Quantity Book::match(Order& taker, Price limit, bool has_limit) {
    return taker.side == Side::Buy ? match_asks(taker, limit, has_limit)
                                   : match_bids(taker, limit, has_limit);
}

OrderId Book::add_limit(Side side, Price price, Quantity quantity) {
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

Quantity Book::add_market(Side side, Quantity quantity) {
    Order order{next_id_++, side, 0, quantity};
    return match(order, 0, false);
}

void Book::remove(OrderId id) {
    const auto it = index_.find(id);
    if (it == index_.end()) {
        return;
    }

    const Location loc = it->second;
    if (loc.side == Side::Buy) {
        auto level_it = bids_.find(loc.price);
        level_it->second.total -= loc.it->quantity;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) {
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(loc.price);
        level_it->second.total -= loc.it->quantity;
        level_it->second.orders.erase(loc.it);
        if (level_it->second.orders.empty()) {
            asks_.erase(level_it);
        }
    }

    index_.erase(it);
}

bool Book::cancel(OrderId id) {
    if (index_.find(id) == index_.end()) {
        return false;
    }
    remove(id);
    return true;
}

bool Book::modify(OrderId id, Price new_price, Quantity new_quantity) {
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
        if (loc.side == Side::Buy) {
            bids_.find(loc.price)->second.total -= delta;
        } else {
            asks_.find(loc.price)->second.total -= delta;
        }
        return true;
    }

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

std::optional<Price> Book::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> Book::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity Book::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total;
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total;
}

Book::Depth Book::depth(Side side, std::size_t levels) const {
    Depth out;
    out.reserve(levels);

    if (side == Side::Buy) {
        for (auto it = bids_.begin(); it != bids_.end() && out.size() < levels; ++it) {
            out.emplace_back(it->first, it->second.total);
        }
    } else {
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < levels; ++it) {
            out.emplace_back(it->first, it->second.total);
        }
    }

    return out;
}

std::size_t Book::level_count(Side side) const {
    return side == Side::Buy ? bids_.size() : asks_.size();
}

}  // namespace ob
