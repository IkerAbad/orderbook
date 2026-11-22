#pragma once

#include "orderbook/types.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace test {

// Independent reference model for the contract shared by the three books.
// It favours clarity over speed: levels are std::map and each level is a plain
// vector scanned in order. The tests use it to catch bugs that the three books
// could share.
class ReferenceBook {
public:
    using TradeCallback = std::function<void(const ob::Trade&)>;
    using Depth = std::vector<std::pair<ob::Price, ob::Quantity>>;

    explicit ReferenceBook(TradeCallback on_trade = {}) : on_trade_(std::move(on_trade)) {}

    ob::OrderId add_limit(ob::Side side, ob::Price price, ob::Quantity quantity) {
        ob::Order order{next_id_++, side, price, quantity};
        if (quantity == 0) {
            return order.id;
        }
        match(order, price, true);
        if (order.quantity > 0) {
            rest(order);
        }
        return order.id;
    }

    ob::Quantity add_market(ob::Side side, ob::Quantity quantity) {
        ob::Order order{next_id_++, side, 0, quantity};
        return match(order, 0, false);
    }

    bool cancel(ob::OrderId id) {
        const auto loc = find_order(id);
        if (!loc) {
            return false;
        }
        erase_order(*loc);
        return true;
    }

    bool modify(ob::OrderId id, ob::Price new_price, ob::Quantity new_quantity) {
        const auto loc = find_order(id);
        if (!loc) {
            return false;
        }

        ob::Order& order = level_at(*loc)[loc->index];
        if (new_quantity == 0) {
            erase_order(*loc);
            return true;
        }

        if (new_price == order.price && new_quantity <= order.quantity) {
            order.quantity = new_quantity;
            return true;
        }

        ob::Order updated = order;
        erase_order(*loc);
        updated.price = new_price;
        updated.quantity = new_quantity;
        match(updated, new_price, true);
        if (updated.quantity > 0) {
            rest(updated);
        }
        return true;
    }

    [[nodiscard]] std::optional<ob::Price> best_bid() const {
        if (bids_.empty()) {
            return std::nullopt;
        }
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<ob::Price> best_ask() const {
        if (asks_.empty()) {
            return std::nullopt;
        }
        return asks_.begin()->first;
    }

    [[nodiscard]] ob::Quantity quantity_at(ob::Side side, ob::Price price) const {
        if (side == ob::Side::Buy) {
            const auto it = bids_.find(price);
            return it == bids_.end() ? 0 : sum(it->second);
        }
        const auto it = asks_.find(price);
        return it == asks_.end() ? 0 : sum(it->second);
    }

    [[nodiscard]] Depth depth(ob::Side side, std::size_t levels) const {
        Depth out;
        if (side == ob::Side::Buy) {
            for (auto it = bids_.begin(); it != bids_.end() && out.size() < levels; ++it) {
                out.emplace_back(it->first, sum(it->second));
            }
        } else {
            for (auto it = asks_.begin(); it != asks_.end() && out.size() < levels; ++it) {
                out.emplace_back(it->first, sum(it->second));
            }
        }
        return out;
    }

    [[nodiscard]] std::size_t order_count() const { return count_; }

    [[nodiscard]] std::size_t level_count(ob::Side side) const {
        return side == ob::Side::Buy ? bids_.size() : asks_.size();
    }

private:
    using BidLevels =
        std::map<ob::Price, std::vector<ob::Order>, std::greater<ob::Price>>;
    using AskLevels = std::map<ob::Price, std::vector<ob::Order>>;

    struct Location {
        ob::Side side{};
        ob::Price price{};
        std::size_t index{};
    };

    BidLevels bids_;
    AskLevels asks_;
    TradeCallback on_trade_;
    ob::OrderId next_id_{1};
    std::size_t count_{0};

    static ob::Quantity sum(const std::vector<ob::Order>& orders) {
        ob::Quantity total = 0;
        for (const ob::Order& order : orders) {
            total += order.quantity;
        }
        return total;
    }

    void rest(const ob::Order& order) {
        if (order.side == ob::Side::Buy) {
            bids_[order.price].push_back(order);
        } else {
            asks_[order.price].push_back(order);
        }
        ++count_;
    }

    std::vector<ob::Order>& level_at(const Location& loc) {
        return loc.side == ob::Side::Buy ? bids_.at(loc.price) : asks_.at(loc.price);
    }

    std::optional<Location> find_order(ob::OrderId id) const {
        for (auto it = bids_.begin(); it != bids_.end(); ++it) {
            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (it->second[i].id == id) {
                    return Location{ob::Side::Buy, it->first, i};
                }
            }
        }
        for (auto it = asks_.begin(); it != asks_.end(); ++it) {
            for (std::size_t i = 0; i < it->second.size(); ++i) {
                if (it->second[i].id == id) {
                    return Location{ob::Side::Sell, it->first, i};
                }
            }
        }
        return std::nullopt;
    }

    void erase_order(const Location& loc) {
        if (loc.side == ob::Side::Buy) {
            auto it = bids_.find(loc.price);
            it->second.erase(it->second.begin() +
                             static_cast<std::ptrdiff_t>(loc.index));
            if (it->second.empty()) {
                bids_.erase(it);
            }
        } else {
            auto it = asks_.find(loc.price);
            it->second.erase(it->second.begin() +
                             static_cast<std::ptrdiff_t>(loc.index));
            if (it->second.empty()) {
                asks_.erase(it);
            }
        }
        --count_;
    }

    ob::Quantity match(ob::Order& taker, ob::Price limit, bool has_limit) {
        ob::Quantity filled = 0;

        while (taker.quantity > 0) {
            if (taker.side == ob::Side::Buy) {
                if (asks_.empty()) {
                    break;
                }
                auto it = asks_.begin();
                if (has_limit && it->first > limit) {
                    break;
                }
                ob::Order& maker = it->second.front();
                const ob::Quantity q = std::min(taker.quantity, maker.quantity);

                if (on_trade_) {
                    on_trade_(ob::Trade{taker.id, maker.id, maker.price, q});
                }

                filled += q;
                taker.quantity -= q;
                maker.quantity -= q;

                if (maker.quantity == 0) {
                    it->second.erase(it->second.begin());
                    --count_;
                    if (it->second.empty()) {
                        asks_.erase(it);
                    }
                }
            } else {
                if (bids_.empty()) {
                    break;
                }
                auto it = bids_.begin();
                if (has_limit && it->first < limit) {
                    break;
                }
                ob::Order& maker = it->second.front();
                const ob::Quantity q = std::min(taker.quantity, maker.quantity);

                if (on_trade_) {
                    on_trade_(ob::Trade{taker.id, maker.id, maker.price, q});
                }

                filled += q;
                taker.quantity -= q;
                maker.quantity -= q;

                if (maker.quantity == 0) {
                    it->second.erase(it->second.begin());
                    --count_;
                    if (it->second.empty()) {
                        bids_.erase(it);
                    }
                }
            }
        }

        return filled;
    }
};

}  // namespace test
