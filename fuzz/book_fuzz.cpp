#include "orderbook/array_book.hpp"
#include "orderbook/book.hpp"
#include "orderbook/pool_book.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

bool same_trades(const std::vector<ob::Trade>& a, const std::vector<ob::Trade>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].taker != b[i].taker || a[i].maker != b[i].maker ||
            a[i].price != b[i].price || a[i].quantity != b[i].quantity) {
            return false;
        }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 4) {
        return 0;
    }

    constexpr ob::Price lo = 1000;
    constexpr ob::Price hi = 2000;
    constexpr std::size_t capacity = 8192;
    constexpr int max_ops = 4096;

    std::vector<ob::Trade> map_trades;
    std::vector<ob::Trade> array_trades;
    std::vector<ob::Trade> pool_trades;

    ob::Book map_book([&](const ob::Trade& t) { map_trades.push_back(t); });
    ob::ArrayBook array_book(lo, hi,
                             [&](const ob::Trade& t) { array_trades.push_back(t); });
    ob::PoolBook pool_book(lo, hi, capacity,
                           [&](const ob::Trade& t) { pool_trades.push_back(t); });

    std::size_t pos = 0;
    const auto byte = [&]() -> std::uint8_t {
        return pos < size ? data[pos++] : 0;
    };

    std::vector<ob::OrderId> ids;
    int ops = 0;

    while (pos < size && ops < max_ops) {
        ++ops;
        const int op = byte() % 10;
        const ob::Side side = (byte() & 1) != 0 ? ob::Side::Sell : ob::Side::Buy;
        const ob::Price price =
            lo + static_cast<ob::Price>(((static_cast<unsigned>(byte()) << 8) | byte()) %
                                        1001);
        const ob::Quantity quantity = 1 + (byte() % 100);

        if (op < 5 || ids.empty()) {
            ids.push_back(map_book.add_limit(side, price, quantity));
            array_book.add_limit(side, price, quantity);
            pool_book.add_limit(side, price, quantity);
        } else if (op < 7) {
            const ob::Quantity map_filled = map_book.add_market(side, quantity);
            const ob::Quantity array_filled = array_book.add_market(side, quantity);
            const ob::Quantity pool_filled = pool_book.add_market(side, quantity);
            if (map_filled != array_filled || map_filled != pool_filled) {
                std::abort();
            }
        } else if (op < 9) {
            const ob::OrderId id = ids[byte() % ids.size()];
            const bool map_cancelled = map_book.cancel(id);
            const bool array_cancelled = array_book.cancel(id);
            const bool pool_cancelled = pool_book.cancel(id);
            if (map_cancelled != array_cancelled ||
                map_cancelled != pool_cancelled) {
                std::abort();
            }
        } else {
            const ob::OrderId id = ids[byte() % ids.size()];
            const ob::Quantity new_quantity = byte() % 101;
            const bool map_modified = map_book.modify(id, price, new_quantity);
            const bool array_modified = array_book.modify(id, price, new_quantity);
            const bool pool_modified = pool_book.modify(id, price, new_quantity);
            if (map_modified != array_modified || map_modified != pool_modified) {
                std::abort();
            }
        }

        if (ops % 64 == 0 &&
            (map_book.order_count() != array_book.order_count() ||
             map_book.order_count() != pool_book.order_count() ||
             map_book.best_bid() != array_book.best_bid() ||
             map_book.best_bid() != pool_book.best_bid() ||
             map_book.best_ask() != array_book.best_ask() ||
             map_book.best_ask() != pool_book.best_ask() ||
             map_book.depth(ob::Side::Buy, 5) != array_book.depth(ob::Side::Buy, 5) ||
             map_book.depth(ob::Side::Buy, 5) != pool_book.depth(ob::Side::Buy, 5) ||
             map_book.depth(ob::Side::Sell, 5) != array_book.depth(ob::Side::Sell, 5) ||
             map_book.depth(ob::Side::Sell, 5) != pool_book.depth(ob::Side::Sell, 5))) {
            std::abort();
        }
    }

    if (!same_trades(map_trades, array_trades) ||
        !same_trades(map_trades, pool_trades)) {
        std::abort();
    }
    return 0;
}
