#include <orderbook/pool_book.hpp>
#include <orderbook/version.hpp>

#include <iostream>

int main() {
    ob::PoolBook book(1000, 2000, 16);
    book.add_limit(ob::Side::Buy, 1500, 10);
    book.add_limit(ob::Side::Sell, 1600, 4);

    std::cout << ORDERBOOK_VERSION_STRING << ' ' << book.best_bid().value_or(0)
              << ' ' << book.best_ask().value_or(0) << ' ' << book.order_count()
              << '\n';
    return book.best_bid() == 1500 && book.best_ask() == 1600 ? 0 : 1;
}
