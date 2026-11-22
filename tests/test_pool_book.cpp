#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "orderbook/array_book.hpp"
#include "orderbook/book.hpp"
#include "orderbook/pool_book.hpp"
#include "orderbook/trace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {

std::size_t g_allocations = 0;

template <class BookT>
void apply(BookT& book, const ob::TraceEvent& e) {
    switch (e.op) {
        case 'L':
            book.add_limit(e.side, e.price, e.quantity);
            break;
        case 'M':
            book.add_market(e.side, e.quantity);
            break;
        case 'C':
            book.cancel(e.id);
            break;
        default:
            break;
    }
}

}  // namespace

void* operator new(std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete(void* p) noexcept {
    std::free(p);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void* operator new[](std::size_t size) {
    ++g_allocations;
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete[](void* p) noexcept {
    std::free(p);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

TEST_CASE("pool book rests a limit order") {
    ob::PoolBook book(100, 200, 8);
    const ob::OrderId id = book.add_limit(ob::Side::Buy, 150, 10);

    CHECK(id == 1);
    CHECK(book.order_count() == 1);
    CHECK(book.level_count(ob::Side::Buy) == 1);
    CHECK(book.best_bid() == 150);
    CHECK(book.quantity_at(ob::Side::Buy, 150) == 10);
}

TEST_CASE("pool book executes at the maker price") {
    std::vector<ob::Trade> trades;
    ob::PoolBook book(100, 200, 8, [&](const ob::Trade& t) { trades.push_back(t); });

    book.add_limit(ob::Side::Sell, 151, 10);
    book.add_limit(ob::Side::Buy, 153, 4);

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].price == 151);
    CHECK(trades[0].quantity == 4);
    CHECK(book.quantity_at(ob::Side::Sell, 151) == 6);
    CHECK(book.best_bid() == std::nullopt);
}

TEST_CASE("pool book keeps time priority across a middle cancel") {
    std::vector<ob::Trade> trades;
    ob::PoolBook book(100, 200, 8, [&](const ob::Trade& t) { trades.push_back(t); });

    const ob::OrderId first = book.add_limit(ob::Side::Sell, 150, 5);
    const ob::OrderId middle = book.add_limit(ob::Side::Sell, 150, 5);
    const ob::OrderId last = book.add_limit(ob::Side::Sell, 150, 5);

    CHECK(book.cancel(middle));
    CHECK(book.add_market(ob::Side::Buy, 7) == 7);

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker == first);
    CHECK(trades[1].maker == last);
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 3);
}

TEST_CASE("pool book reuses released slots") {
    std::vector<ob::Trade> trades;
    ob::PoolBook book(100, 200, 2, [&](const ob::Trade& t) { trades.push_back(t); });

    const ob::OrderId a = book.add_limit(ob::Side::Sell, 150, 5);
    const ob::OrderId b = book.add_limit(ob::Side::Sell, 150, 5);
    CHECK(book.cancel(a));
    CHECK(book.cancel(b));

    const ob::OrderId c = book.add_limit(ob::Side::Sell, 150, 4);
    const ob::OrderId d = book.add_limit(ob::Side::Sell, 150, 6);
    CHECK(book.capacity() == 2);
    CHECK(book.order_count() == 2);
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 10);

    CHECK(book.add_market(ob::Side::Buy, 10) == 10);
    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker == c);
    CHECK(trades[1].maker == d);
    CHECK(book.order_count() == 0);
}

TEST_CASE("pool book reports a full pool without corrupting the book") {
    ob::PoolBook book(100, 200, 1);
    const ob::OrderId live = book.add_limit(ob::Side::Sell, 150, 5);

    CHECK_THROWS_AS(book.add_limit(ob::Side::Sell, 151, 5), std::overflow_error);
    CHECK(book.order_count() == 1);
    CHECK(book.best_ask() == 150);
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 5);

    CHECK(book.cancel(live));
    CHECK(book.add_limit(ob::Side::Sell, 151, 5) == 3);
    CHECK(book.best_ask() == 151);
}

TEST_CASE("pool book validates its ladder and its capacity") {
    CHECK_THROWS_AS(ob::PoolBook(200, 100, 4), std::invalid_argument);
    CHECK_THROWS_AS(ob::PoolBook(100, 200, 0), std::invalid_argument);

    ob::PoolBook book(100, 200, 4);
    CHECK_THROWS_AS(book.add_limit(ob::Side::Buy, 99, 1), std::out_of_range);
    CHECK_THROWS_AS(book.add_limit(ob::Side::Sell, 201, 1), std::out_of_range);
    CHECK(book.order_count() == 0);
}

TEST_CASE("books reject ladders whose span overflows a Price") {
    constexpr ob::Price lo = std::numeric_limits<ob::Price>::min();
    constexpr ob::Price hi = std::numeric_limits<ob::Price>::max();

    CHECK_THROWS_AS(ob::ArrayBook(lo, hi), std::invalid_argument);
    CHECK_THROWS_AS(ob::PoolBook(lo, hi, 4), std::invalid_argument);

    ob::ArrayBook array_top(hi - 1, hi);
    array_top.add_limit(ob::Side::Buy, hi, 1);
    CHECK(array_top.best_bid() == hi);

    ob::PoolBook pool_top(hi - 1, hi, 4);
    pool_top.add_limit(ob::Side::Buy, hi, 1);
    CHECK(pool_top.best_bid() == hi);
}

TEST_CASE("pool book keeps the best price after a cancel") {
    ob::PoolBook book(100, 200, 8);
    const ob::OrderId top = book.add_limit(ob::Side::Buy, 110, 5);
    book.add_limit(ob::Side::Buy, 108, 5);

    CHECK(book.cancel(top));
    CHECK(book.best_bid() == 108);
    CHECK(book.level_count(ob::Side::Buy) == 1);
}

TEST_CASE("pool book advances past emptied levels") {
    ob::PoolBook book(100, 200, 8);
    book.add_limit(ob::Side::Buy, 105, 5);
    book.add_limit(ob::Side::Buy, 103, 5);

    CHECK(book.best_bid() == 105);
    CHECK(book.add_market(ob::Side::Sell, 5) == 5);
    CHECK(book.best_bid() == 103);
    CHECK(book.add_market(ob::Side::Sell, 5) == 5);
    CHECK(book.best_bid() == std::nullopt);
}

TEST_CASE("pool book modify follows the exchange priority rules") {
    std::vector<ob::Trade> trades;
    ob::PoolBook book(100, 200, 8, [&](const ob::Trade& t) { trades.push_back(t); });

    const ob::OrderId first = book.add_limit(ob::Side::Sell, 150, 5);
    const ob::OrderId second = book.add_limit(ob::Side::Sell, 150, 5);
    const ob::OrderId third = book.add_limit(ob::Side::Sell, 150, 5);

    CHECK(book.modify(second, 150, 8));
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 18);

    CHECK(book.add_market(ob::Side::Buy, 10) == 10);
    REQUIRE(trades.size() == 2);
    CHECK(trades[0].maker == first);
    CHECK(trades[1].maker == third);
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 8);

    CHECK(book.modify(second, 152, 8));
    CHECK(book.quantity_at(ob::Side::Sell, 150) == 0);
    CHECK(book.quantity_at(ob::Side::Sell, 152) == 8);
    CHECK(book.best_ask() == 152);

    CHECK(book.modify(second, 152, 0));
    CHECK(book.order_count() == 0);
}

TEST_CASE("pool book modify to an invalid price keeps the order") {
    ob::PoolBook book(100, 200, 8);
    const ob::OrderId id = book.add_limit(ob::Side::Buy, 150, 5);

    CHECK_THROWS_AS(book.modify(id, 50, 5), std::out_of_range);
    CHECK(book.order_count() == 1);
    CHECK(book.quantity_at(ob::Side::Buy, 150) == 5);
}

TEST_CASE("pool book exposes depth and rejects out-of-range quantities") {
    ob::PoolBook book(100, 200, 8);
    book.add_limit(ob::Side::Buy, 100, 3);
    book.add_limit(ob::Side::Buy, 101, 4);
    book.add_limit(ob::Side::Buy, 102, 5);

    const auto depth = book.depth(ob::Side::Buy, 2);
    REQUIRE(depth.size() == 2);
    CHECK(depth[0].first == 102);
    CHECK(depth[0].second == 5);
    CHECK(depth[1].first == 101);
    CHECK(depth[1].second == 4);
    CHECK(book.quantity_at(ob::Side::Buy, 999) == 0);
}

TEST_CASE("pool book does not allocate while it runs") {
    ob::PoolBook book(1000, 2000, 256, [](const ob::Trade&) {});

    const auto exercise = [&book]() {
        for (int i = 0; i < 50; ++i) {
            const ob::OrderId id = book.add_limit(ob::Side::Buy, 1100 + i, 5);
            book.cancel(id);
        }
        for (int i = 0; i < 50; ++i) {
            book.add_limit(ob::Side::Sell, 1900 + i, 5);
        }
        book.add_market(ob::Side::Buy, 250);
        for (int i = 0; i < 20; ++i) {
            const ob::OrderId id = book.add_limit(ob::Side::Buy, 1200 + i, 10);
            book.modify(id, 1200 + i, 5);
            book.modify(id, 1300 + i, 5);
            book.cancel(id);
        }
    };

    exercise();
    CHECK(book.order_count() == 0);

    const std::size_t before = g_allocations;
    for (int round = 0; round < 50; ++round) {
        exercise();
    }
    const std::size_t measured = g_allocations;

    CHECK(book.order_count() == 0);
    CHECK(measured == before);

    // Positive control: the counter must see a deliberate allocation.
    std::size_t after_probe = 0;
    {
        std::vector<int> probe(4, 7);
        after_probe = g_allocations;
        CHECK(probe[0] == 7);
    }
    CHECK(after_probe > measured);
}

TEST_CASE("the three books agree on a random workload") {
    ob::GeneratorOptions options{};
    options.events = 20'000;
    options.seed = 11;
    const auto trace = ob::generate(options);

    ob::Price lo = 0;
    ob::Price hi = 0;
    bool seen = false;
    std::size_t limit_events = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op != 'L') {
            continue;
        }
        ++limit_events;
        if (!seen) {
            lo = hi = e.price;
            seen = true;
        } else {
            lo = std::min(lo, e.price);
            hi = std::max(hi, e.price);
        }
    }
    REQUIRE(seen);
    lo -= 50;
    hi += 50;

    std::vector<ob::Trade> map_trades;
    std::vector<ob::Trade> array_trades;
    std::vector<ob::Trade> pool_trades;
    std::uint64_t map_volume = 0;
    std::uint64_t array_volume = 0;
    std::uint64_t pool_volume = 0;

    ob::Book map_book([&](const ob::Trade& t) {
        map_trades.push_back(t);
        map_volume += t.quantity;
    });
    ob::ArrayBook array_book(lo, hi, [&](const ob::Trade& t) {
        array_trades.push_back(t);
        array_volume += t.quantity;
    });
    ob::PoolBook pool_book(lo, hi, limit_events, [&](const ob::Trade& t) {
        pool_trades.push_back(t);
        pool_volume += t.quantity;
    });

    std::size_t checked = 0;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        apply(map_book, trace[i]);
        apply(array_book, trace[i]);
        apply(pool_book, trace[i]);

        if (i % 500 == 0) {
            CHECK(map_book.order_count() == array_book.order_count());
            CHECK(map_book.order_count() == pool_book.order_count());
            CHECK(map_book.best_bid() == array_book.best_bid());
            CHECK(map_book.best_bid() == pool_book.best_bid());
            CHECK(map_book.best_ask() == array_book.best_ask());
            CHECK(map_book.best_ask() == pool_book.best_ask());
            if (const auto bid = map_book.best_bid()) {
                CHECK(map_book.quantity_at(ob::Side::Buy, *bid) ==
                      array_book.quantity_at(ob::Side::Buy, *bid));
                CHECK(map_book.quantity_at(ob::Side::Buy, *bid) ==
                      pool_book.quantity_at(ob::Side::Buy, *bid));
            }
            if (const auto ask = map_book.best_ask()) {
                CHECK(map_book.quantity_at(ob::Side::Sell, *ask) ==
                      array_book.quantity_at(ob::Side::Sell, *ask));
                CHECK(map_book.quantity_at(ob::Side::Sell, *ask) ==
                      pool_book.quantity_at(ob::Side::Sell, *ask));
            }
            CHECK(map_book.level_count(ob::Side::Buy) ==
                  array_book.level_count(ob::Side::Buy));
            CHECK(map_book.level_count(ob::Side::Buy) ==
                  pool_book.level_count(ob::Side::Buy));
            CHECK(map_book.level_count(ob::Side::Sell) ==
                  array_book.level_count(ob::Side::Sell));
            CHECK(map_book.level_count(ob::Side::Sell) ==
                  pool_book.level_count(ob::Side::Sell));
            ++checked;
        }
    }

    CHECK(checked > 0);
    REQUIRE(map_trades.size() == array_trades.size());
    REQUIRE(map_trades.size() == pool_trades.size());
    for (std::size_t i = 0; i < map_trades.size(); ++i) {
        CHECK(map_trades[i].taker == array_trades[i].taker);
        CHECK(map_trades[i].maker == array_trades[i].maker);
        CHECK(map_trades[i].price == array_trades[i].price);
        CHECK(map_trades[i].quantity == array_trades[i].quantity);
        CHECK(map_trades[i].taker == pool_trades[i].taker);
        CHECK(map_trades[i].maker == pool_trades[i].maker);
        CHECK(map_trades[i].price == pool_trades[i].price);
        CHECK(map_trades[i].quantity == pool_trades[i].quantity);
    }
    CHECK(map_volume == array_volume);
    CHECK(map_volume == pool_volume);
    CHECK(map_book.order_count() == pool_book.order_count());
    CHECK(map_book.level_count(ob::Side::Buy) == pool_book.level_count(ob::Side::Buy));
    CHECK(map_book.level_count(ob::Side::Sell) == pool_book.level_count(ob::Side::Sell));
}
