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
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
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

TEST_CASE("books reject a level total that overflows") {
    constexpr ob::Quantity max = std::numeric_limits<ob::Quantity>::max();

    ob::Book map_book;
    map_book.add_limit(ob::Side::Buy, 100, max);
    CHECK_THROWS_AS(map_book.add_limit(ob::Side::Buy, 100, 1), std::overflow_error);
    CHECK(map_book.order_count() == 1);
    CHECK(map_book.quantity_at(ob::Side::Buy, 100) == max);

    ob::ArrayBook array_book(100, 200);
    array_book.add_limit(ob::Side::Buy, 100, max);
    CHECK_THROWS_AS(array_book.add_limit(ob::Side::Buy, 100, 1), std::overflow_error);
    CHECK(array_book.order_count() == 1);
    CHECK(array_book.quantity_at(ob::Side::Buy, 100) == max);

    ob::PoolBook pool_book(100, 200, 8);
    pool_book.add_limit(ob::Side::Buy, 100, max);
    CHECK_THROWS_AS(pool_book.add_limit(ob::Side::Buy, 100, 1), std::overflow_error);
    CHECK(pool_book.order_count() == 1);
    CHECK(pool_book.quantity_at(ob::Side::Buy, 100) == max);
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

TEST_CASE("generator ids follow every order that takes one") {
    ob::GeneratorOptions options{};
    options.events = 2'000;
    options.seed = 5;
    const auto trace = ob::generate(options);

    ob::OrderId next = 1;
    std::size_t cancels = 0;
    for (const ob::TraceEvent& e : trace) {
        if (e.op == 'L' || e.op == 'M') {
            CHECK(e.id == next);
            ++next;
        } else if (e.op == 'C') {
            ++cancels;
            CHECK(e.id < next);
        }
    }
    CHECK(cancels > 0);
}

TEST_CASE("generator rejects a zero maximum quantity") {
    ob::GeneratorOptions options{};
    options.events = 10;
    options.max_quantity = 0;
    CHECK_THROWS_AS(ob::generate(options), std::invalid_argument);
}

TEST_CASE("trace csv round-trips the generated events") {
    ob::GeneratorOptions options{};
    options.events = 500;
    options.seed = 3;
    const auto expected = ob::generate(options);

    const std::string path =
        (std::filesystem::temp_directory_path() / "ob_roundtrip.csv").string();
    REQUIRE(ob::write_csv(path, expected));
    const auto actual = ob::read_csv(path);
    std::filesystem::remove(path);

    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(actual[i].ts_ns == expected[i].ts_ns);
        CHECK(actual[i].op == expected[i].op);
        CHECK(actual[i].side == expected[i].side);
        CHECK(actual[i].price == expected[i].price);
        CHECK(actual[i].quantity == expected[i].quantity);
        CHECK(actual[i].id == expected[i].id);
    }
}

TEST_CASE("trace csv rejects malformed input") {
    const auto dir = std::filesystem::temp_directory_path();
    const std::string header = "ts_ns,op,side,price,quantity,id\n";

    const auto check_bad = [&](const std::string& name, const std::string& text) {
        const std::string path = (dir / name).string();
        {
            std::ofstream out(path, std::ios::binary);
            REQUIRE(out.good());
            out << text;
        }
        CHECK_THROWS_AS(ob::read_csv(path), std::runtime_error);
        std::filesystem::remove(path);
    };

    check_bad("ob_bad_header.csv", "wrong,header\n");
    check_bad("ob_bad_fields.csv", header + "0,L,B,100,10\n");
    check_bad("ob_bad_op.csv", header + "0,X,B,100,10,1\n");
    check_bad("ob_bad_side.csv", header + "0,L,X,100,10,1\n");
    check_bad("ob_bad_number.csv", header + "0,L,B,abc,10,1\n");
    check_bad("ob_empty.csv", "");
}

TEST_CASE("trace csv accepts CRLF line endings") {
    const std::string path =
        (std::filesystem::temp_directory_path() / "ob_crlf.csv").string();
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.good());
        out << "ts_ns,op,side,price,quantity,id\r\n"
               "0,L,B,100,10,1\r\n"
               "1,M,S,0,5,2\r\n";
    }
    const auto events = ob::read_csv(path);
    std::filesystem::remove(path);

    REQUIRE(events.size() == 2);
    CHECK(events[0].op == 'L');
    CHECK(events[0].side == ob::Side::Buy);
    CHECK(events[0].id == 1);
    CHECK(events[1].op == 'M');
    CHECK(events[1].side == ob::Side::Sell);
    CHECK(events[1].id == 2);
}

TEST_CASE("the three books agree on random modifies") {
    constexpr ob::Price lo = 1000;
    constexpr ob::Price hi = 2000;
    constexpr std::size_t capacity = 8192;

    std::vector<ob::Trade> map_trades;
    std::vector<ob::Trade> array_trades;
    std::vector<ob::Trade> pool_trades;

    ob::Book map_book([&](const ob::Trade& t) { map_trades.push_back(t); });
    ob::ArrayBook array_book(lo, hi,
                             [&](const ob::Trade& t) { array_trades.push_back(t); });
    ob::PoolBook pool_book(lo, hi, capacity,
                           [&](const ob::Trade& t) { pool_trades.push_back(t); });

    std::mt19937_64 rng(99);
    std::uniform_int_distribution<int> side_pick(0, 1);
    std::uniform_int_distribution<ob::Price> price_pick(lo, hi);
    std::uniform_int_distribution<ob::Quantity> size_pick(1, 20);
    std::uniform_int_distribution<ob::Quantity> modify_size_pick(0, 20);
    std::uniform_int_distribution<int> op_pick(0, 9);

    std::vector<ob::OrderId> ids;

    for (int step = 0; step < 5'000; ++step) {
        const int op = op_pick(rng);

        if (op < 5 || ids.empty()) {
            const ob::Side side = side_pick(rng) == 0 ? ob::Side::Buy : ob::Side::Sell;
            const ob::Price price = price_pick(rng);
            const ob::Quantity quantity = size_pick(rng);

            ids.push_back(map_book.add_limit(side, price, quantity));
            array_book.add_limit(side, price, quantity);
            pool_book.add_limit(side, price, quantity);
        } else if (op < 7) {
            const ob::Side side = side_pick(rng) == 0 ? ob::Side::Buy : ob::Side::Sell;
            const ob::Quantity quantity = size_pick(rng);

            const ob::Quantity map_filled = map_book.add_market(side, quantity);
            const ob::Quantity array_filled = array_book.add_market(side, quantity);
            const ob::Quantity pool_filled = pool_book.add_market(side, quantity);
            CHECK(map_filled == array_filled);
            CHECK(map_filled == pool_filled);
        } else if (op < 9) {
            std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
            const ob::OrderId id = ids[pick(rng)];

            const bool map_cancelled = map_book.cancel(id);
            const bool array_cancelled = array_book.cancel(id);
            const bool pool_cancelled = pool_book.cancel(id);
            CHECK(map_cancelled == array_cancelled);
            CHECK(map_cancelled == pool_cancelled);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
            const ob::OrderId id = ids[pick(rng)];
            const ob::Price price = price_pick(rng);
            const ob::Quantity quantity = modify_size_pick(rng);

            const bool map_modified = map_book.modify(id, price, quantity);
            const bool array_modified = array_book.modify(id, price, quantity);
            const bool pool_modified = pool_book.modify(id, price, quantity);
            CHECK(map_modified == array_modified);
            CHECK(map_modified == pool_modified);
        }

        if (step % 100 == 0) {
            CHECK(map_book.order_count() == array_book.order_count());
            CHECK(map_book.order_count() == pool_book.order_count());
            CHECK(map_book.best_bid() == array_book.best_bid());
            CHECK(map_book.best_bid() == pool_book.best_bid());
            CHECK(map_book.best_ask() == array_book.best_ask());
            CHECK(map_book.best_ask() == pool_book.best_ask());
            CHECK(map_book.depth(ob::Side::Buy, 8) == array_book.depth(ob::Side::Buy, 8));
            CHECK(map_book.depth(ob::Side::Buy, 8) == pool_book.depth(ob::Side::Buy, 8));
            CHECK(map_book.depth(ob::Side::Sell, 8) ==
                  array_book.depth(ob::Side::Sell, 8));
            CHECK(map_book.depth(ob::Side::Sell, 8) ==
                  pool_book.depth(ob::Side::Sell, 8));
        }
    }

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
    CHECK(map_book.order_count() == pool_book.order_count());
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
