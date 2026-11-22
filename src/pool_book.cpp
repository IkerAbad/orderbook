#include "orderbook/pool_book.hpp"

#include <algorithm>
#include <cstdint>
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

std::size_t mix(OrderId id) {
    id += 0x9E3779B97F4A7C15ULL;
    id = (id ^ (id >> 30)) * 0xBF58476D1CE4E5B9ULL;
    id = (id ^ (id >> 27)) * 0x94D049BB133111EBULL;
    id ^= id >> 31;
    return static_cast<std::size_t>(id);
}

}  // namespace

PoolBook::PoolBook(Price min_price, Price max_price, std::size_t max_orders,
                   TradeCallback on_trade)
    : min_price_(min_price),
      max_price_(max_price),
      bids_(ladder_size(min_price, max_price)),
      asks_(ladder_size(min_price, max_price)),
      orders_(max_orders),
      next_(max_orders, kNone),
      prev_(max_orders, kNone),
      on_trade_(std::move(on_trade)) {
    if (max_orders == 0) {
        throw std::invalid_argument("order pool capacity must be positive");
    }
    if (max_orders > static_cast<std::size_t>(kNone) / 2) {
        throw std::invalid_argument("order pool is too large");
    }

    for (std::size_t i = 0; i + 1 < max_orders; ++i) {
        next_[i] = static_cast<std::uint32_t>(i + 1);
    }
    free_head_ = 0;

    std::size_t table = 16;
    const std::size_t wanted = max_orders + max_orders / 2 + 1;
    while (table < wanted) {
        table <<= 1;
    }
    keys_.assign(table, 0);
    slots_.assign(table, 0);
    mask_ = table - 1;
}

bool PoolBook::in_range(Price price) const {
    return price >= min_price_ && price <= max_price_;
}

std::size_t PoolBook::index_of(Price price) const {
    if (!in_range(price)) {
        throw std::out_of_range("price " + std::to_string(price) +
                                " outside the ladder [" + std::to_string(min_price_) +
                                ", " + std::to_string(max_price_) + "]");
    }
    return static_cast<std::size_t>(price - min_price_);
}

std::uint32_t PoolBook::alloc_slot() {
    if (free_head_ == kNone) {
        throw std::overflow_error("order pool is full");
    }
    const std::uint32_t slot = free_head_;
    free_head_ = next_[slot];
    return slot;
}

void PoolBook::free_slot(std::uint32_t slot) {
    next_[slot] = free_head_;
    free_head_ = slot;
}

void PoolBook::index_insert(OrderId id, std::uint32_t slot) {
    std::size_t i = mix(id) & mask_;
    while (keys_[i] != 0) {
        if (keys_[i] == id) {
            slots_[i] = slot;
            return;
        }
        i = (i + 1) & mask_;
    }
    keys_[i] = id;
    slots_[i] = slot;
}

std::size_t PoolBook::index_find(OrderId id) const {
    std::size_t i = mix(id) & mask_;
    while (keys_[i] != 0) {
        if (keys_[i] == id) {
            return i;
        }
        i = (i + 1) & mask_;
    }
    return kMissing;
}

void PoolBook::index_erase_at(std::size_t pos) {
    keys_[pos] = 0;

    // Reinsert the rest of the probe cluster so no lookup loses its chain.
    std::size_t i = (pos + 1) & mask_;
    while (keys_[i] != 0) {
        const OrderId id = keys_[i];
        const std::uint32_t slot = slots_[i];
        keys_[i] = 0;
        index_insert(id, slot);
        i = (i + 1) & mask_;
    }
}

void PoolBook::index_erase(OrderId id) {
    const std::size_t pos = index_find(id);
    if (pos != kMissing) {
        index_erase_at(pos);
    }
}

void PoolBook::push_back(Level& level, std::uint32_t slot) {
    next_[slot] = kNone;
    prev_[slot] = level.tail;
    if (level.tail != kNone) {
        next_[level.tail] = slot;
    } else {
        level.head = slot;
    }
    level.tail = slot;
}

void PoolBook::unlink(Level& level, std::uint32_t slot) {
    const std::uint32_t p = prev_[slot];
    const std::uint32_t n = next_[slot];

    if (p != kNone) {
        next_[p] = n;
    } else {
        level.head = n;
    }
    if (n != kNone) {
        prev_[n] = p;
    } else {
        level.tail = p;
    }
}

void PoolBook::pop_front(Level& level) {
    const std::uint32_t slot = level.head;
    level.head = next_[slot];
    if (level.head != kNone) {
        prev_[level.head] = kNone;
    } else {
        level.tail = kNone;
    }
}

void PoolBook::release(std::uint32_t slot) {
    index_erase(orders_[slot].id);
    free_slot(slot);
    --live_;
}

void PoolBook::advance_bid() {
    if (!has_bid_) {
        return;
    }
    while (bids_[best_bid_].head == kNone) {
        if (best_bid_ == 0) {
            has_bid_ = false;
            return;
        }
        --best_bid_;
    }
}

void PoolBook::advance_ask() {
    if (!has_ask_) {
        return;
    }
    while (asks_[best_ask_].head == kNone) {
        if (best_ask_ + 1 >= asks_.size()) {
            has_ask_ = false;
            return;
        }
        ++best_ask_;
    }
}

void PoolBook::rest(Order order) {
    const std::size_t i = index_of(order.price);
    Level& level = order.side == Side::Buy ? bids_[i] : asks_[i];

    if (order.quantity > std::numeric_limits<Quantity>::max() - level.total) {
        throw std::overflow_error("level quantity overflow");
    }

    const std::uint32_t slot = alloc_slot();
    orders_[slot] = order;
    push_back(level, slot);
    level.total += order.quantity;

    if (order.side == Side::Buy) {
        if (!has_bid_ || i > best_bid_) {
            best_bid_ = i;
            has_bid_ = true;
        }
    } else {
        if (!has_ask_ || i < best_ask_) {
            best_ask_ = i;
            has_ask_ = true;
        }
    }

    index_insert(order.id, slot);
    ++live_;
}

Quantity PoolBook::match_asks(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && has_ask_) {
        if (has_limit && min_price_ + static_cast<Price>(best_ask_) > limit) {
            break;
        }

        Level& level = asks_[best_ask_];
        while (taker.quantity > 0 && level.head != kNone) {
            const std::uint32_t slot = level.head;
            Order& maker = orders_[slot];
            const Quantity q = std::min(taker.quantity, maker.quantity);

            if (on_trade_) {
                on_trade_(Trade{taker.id, maker.id, maker.price, q});
            }

            filled += q;
            taker.quantity -= q;
            maker.quantity -= q;
            level.total -= q;

            if (maker.quantity == 0) {
                pop_front(level);
                release(slot);
            }
        }

        advance_ask();
    }

    return filled;
}

Quantity PoolBook::match_bids(Order& taker, Price limit, bool has_limit) {
    Quantity filled = 0;

    while (taker.quantity > 0 && has_bid_) {
        if (has_limit && min_price_ + static_cast<Price>(best_bid_) < limit) {
            break;
        }

        Level& level = bids_[best_bid_];
        while (taker.quantity > 0 && level.head != kNone) {
            const std::uint32_t slot = level.head;
            Order& maker = orders_[slot];
            const Quantity q = std::min(taker.quantity, maker.quantity);

            if (on_trade_) {
                on_trade_(Trade{taker.id, maker.id, maker.price, q});
            }

            filled += q;
            taker.quantity -= q;
            maker.quantity -= q;
            level.total -= q;

            if (maker.quantity == 0) {
                pop_front(level);
                release(slot);
            }
        }

        advance_bid();
    }

    return filled;
}

Quantity PoolBook::match(Order& taker, Price limit, bool has_limit) {
    return taker.side == Side::Buy ? match_asks(taker, limit, has_limit)
                                   : match_bids(taker, limit, has_limit);
}

OrderId PoolBook::add_limit(Side side, Price price, Quantity quantity) {
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

Quantity PoolBook::add_market(Side side, Quantity quantity) {
    Order order{next_id_++, side, 0, quantity};
    return match(order, 0, false);
}

bool PoolBook::remove(OrderId id) {
    const std::size_t pos = index_find(id);
    if (pos == kMissing) {
        return false;
    }

    const std::uint32_t slot = slots_[pos];
    const Order& order = orders_[slot];
    const std::size_t i = index_of(order.price);
    const Side side = order.side;
    Level& level = side == Side::Buy ? bids_[i] : asks_[i];

    level.total -= order.quantity;
    unlink(level, slot);
    index_erase_at(pos);
    free_slot(slot);
    --live_;

    if (level.head == kNone) {
        if (side == Side::Buy) {
            if (has_bid_ && i == best_bid_) {
                advance_bid();
            }
        } else {
            if (has_ask_ && i == best_ask_) {
                advance_ask();
            }
        }
    }

    return true;
}

bool PoolBook::cancel(OrderId id) {
    return remove(id);
}

bool PoolBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    const std::size_t pos = index_find(id);
    if (pos == kMissing) {
        return false;
    }

    const std::uint32_t slot = slots_[pos];
    const Order& order = orders_[slot];
    if (new_quantity == 0) {
        remove(id);
        return true;
    }

    if (new_price == order.price && new_quantity <= order.quantity) {
        const Quantity delta = order.quantity - new_quantity;
        orders_[slot].quantity = new_quantity;
        const std::size_t i = index_of(order.price);
        if (order.side == Side::Buy) {
            bids_[i].total -= delta;
        } else {
            asks_[i].total -= delta;
        }
        return true;
    }

    index_of(new_price);  // reject before removing the original

    Order updated = order;
    updated.price = new_price;
    updated.quantity = new_quantity;
    remove(id);

    match(updated, new_price, true);
    if (updated.quantity > 0) {
        rest(updated);
    }
    return true;
}

std::optional<Price> PoolBook::best_bid() const {
    if (!has_bid_) {
        return std::nullopt;
    }
    return min_price_ + static_cast<Price>(best_bid_);
}

std::optional<Price> PoolBook::best_ask() const {
    if (!has_ask_) {
        return std::nullopt;
    }
    return min_price_ + static_cast<Price>(best_ask_);
}

Quantity PoolBook::quantity_at(Side side, Price price) const {
    if (!in_range(price)) {
        return 0;
    }
    const std::size_t i = index_of(price);
    return side == Side::Buy ? bids_[i].total : asks_[i].total;
}

PoolBook::Depth PoolBook::depth(Side side, std::size_t levels) const {
    Depth out;
    out.reserve(levels);

    if (side == Side::Buy) {
        if (!has_bid_) {
            return out;
        }
        for (std::size_t i = best_bid_ + 1; i-- > 0 && out.size() < levels;) {
            if (bids_[i].head != kNone) {
                out.emplace_back(min_price_ + static_cast<Price>(i), bids_[i].total);
            }
        }
    } else {
        if (!has_ask_) {
            return out;
        }
        for (std::size_t i = best_ask_; i < asks_.size() && out.size() < levels; ++i) {
            if (asks_[i].head != kNone) {
                out.emplace_back(min_price_ + static_cast<Price>(i), asks_[i].total);
            }
        }
    }

    return out;
}

std::size_t PoolBook::level_count(Side side) const {
    const auto& levels = side == Side::Buy ? bids_ : asks_;
    return static_cast<std::size_t>(std::count_if(
        levels.begin(), levels.end(),
        [](const Level& level) { return level.head != kNone; }));
}

std::size_t PoolBook::memory_bytes() const {
    return orders_.capacity() * sizeof(Order) +
           next_.capacity() * sizeof(std::uint32_t) +
           prev_.capacity() * sizeof(std::uint32_t) +
           keys_.capacity() * sizeof(OrderId) +
           slots_.capacity() * sizeof(std::uint32_t) +
           bids_.capacity() * sizeof(Level) +
           asks_.capacity() * sizeof(Level);
}

}  // namespace ob
