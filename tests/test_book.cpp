#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "orderbook/array_book.hpp"
#include "orderbook/book.hpp"
#include "orderbook/trace.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

struct Tape {
    std::vector<ob::Trade> trades;
    ob::Book book;

    Tape() : book([this](const ob::Trade& t) { trades.push_back(t); }) {}
};

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

TEST_CASE("a limit order that does not cross rests in the book") {
    Tape tape;
    const ob::OrderId id = tape.book.add_limit(ob::Side::Buy, 100, 10);

    CHECK(tape.trades.empty());
    CHECK(tape.book.order_count() == 1);
    CHECK(tape.book.best_bid() == 100);
    CHECK(tape.book.quantity_at(ob::Side::Buy, 100) == 10);
    CHECK(id == 1);
}

TEST_CASE("a crossing order executes at the maker price") {
    Tape tape;
    tape.book.add_limit(ob::Side::Sell, 101, 10);
    tape.book.add_limit(ob::Side::Buy, 103, 4);

    REQUIRE(tape.trades.size() == 1);
    CHECK(tape.trades[0].price == 101);
    CHECK(tape.trades[0].quantity == 4);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 101) == 6);
    CHECK(tape.book.best_bid() == std::nullopt);
}

TEST_CASE("the remainder of a crossing order rests in the book") {
    Tape tape;
    tape.book.add_limit(ob::Side::Sell, 101, 3);
    tape.book.add_limit(ob::Side::Buy, 101, 8);

    CHECK(tape.book.best_bid() == 101);
    CHECK(tape.book.quantity_at(ob::Side::Buy, 101) == 5);
    CHECK(tape.book.best_ask() == std::nullopt);
}

TEST_CASE("price has priority over time") {
    Tape tape;
    tape.book.add_limit(ob::Side::Sell, 102, 5);
    tape.book.add_limit(ob::Side::Sell, 101, 5);
    tape.book.add_market(ob::Side::Buy, 5);

    REQUIRE(tape.trades.size() == 1);
    CHECK(tape.trades[0].price == 101);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 102) == 5);
}

TEST_CASE("time has priority within the same price") {
    Tape tape;
    const ob::OrderId first = tape.book.add_limit(ob::Side::Sell, 101, 5);
    const ob::OrderId second = tape.book.add_limit(ob::Side::Sell, 101, 5);
    tape.book.add_market(ob::Side::Buy, 7);

    REQUIRE(tape.trades.size() == 2);
    CHECK(tape.trades[0].maker == first);
    CHECK(tape.trades[1].maker == second);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 101) == 3);
}

TEST_CASE("a market order walks several levels") {
    Tape tape;
    tape.book.add_limit(ob::Side::Sell, 101, 2);
    tape.book.add_limit(ob::Side::Sell, 102, 3);
    tape.book.add_limit(ob::Side::Sell, 103, 4);

    const ob::Quantity filled = tape.book.add_market(ob::Side::Buy, 8);

    CHECK(filled == 8);
    CHECK(tape.book.order_count() == 1);
    CHECK(tape.book.best_ask() == 103);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 103) == 1);
}

TEST_CASE("a market order stops when liquidity runs out") {
    Tape tape;
    tape.book.add_limit(ob::Side::Sell, 101, 2);

    CHECK(tape.book.add_market(ob::Side::Buy, 10) == 2);
    CHECK(tape.book.best_ask() == std::nullopt);
}

TEST_CASE("cancel removes the order and the empty level") {
    Tape tape;
    const ob::OrderId id = tape.book.add_limit(ob::Side::Buy, 100, 5);

    CHECK(tape.book.cancel(id));
    CHECK(tape.book.order_count() == 0);
    CHECK(tape.book.level_count(ob::Side::Buy) == 0);
    CHECK_FALSE(tape.book.cancel(id));
}

TEST_CASE("cancelling one order keeps the rest of the level") {
    Tape tape;
    const ob::OrderId a = tape.book.add_limit(ob::Side::Buy, 100, 5);
    tape.book.add_limit(ob::Side::Buy, 100, 7);

    CHECK(tape.book.cancel(a));
    CHECK(tape.book.quantity_at(ob::Side::Buy, 100) == 7);
    CHECK(tape.book.order_count() == 1);
}

TEST_CASE("reducing quantity keeps time priority") {
    Tape tape;
    const ob::OrderId first = tape.book.add_limit(ob::Side::Sell, 101, 5);
    const ob::OrderId second = tape.book.add_limit(ob::Side::Sell, 101, 5);

    CHECK(tape.book.modify(first, 101, 3));
    tape.book.add_market(ob::Side::Buy, 3);

    REQUIRE(tape.trades.size() == 1);
    CHECK(tape.trades[0].maker == first);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 101) == 5);

    tape.book.add_market(ob::Side::Buy, 1);
    REQUIRE(tape.trades.size() == 2);
    CHECK(tape.trades[1].maker == second);
}

TEST_CASE("increasing quantity loses time priority but keeps the id") {
    Tape tape;
    const ob::OrderId first = tape.book.add_limit(ob::Side::Sell, 101, 5);
    tape.book.add_limit(ob::Side::Sell, 101, 5);

    CHECK(tape.book.modify(first, 101, 8));
    tape.book.add_market(ob::Side::Buy, 5);

    REQUIRE(tape.trades.size() == 1);
    CHECK(tape.trades[0].maker != first);
    CHECK(tape.book.order_count() == 1);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 101) == 8);
}

TEST_CASE("a price change moves the order to the new level") {
    Tape tape;
    const ob::OrderId id = tape.book.add_limit(ob::Side::Sell, 105, 5);

    CHECK(tape.book.modify(id, 103, 5));
    CHECK(tape.book.quantity_at(ob::Side::Sell, 105) == 0);
    CHECK(tape.book.quantity_at(ob::Side::Sell, 103) == 5);
    CHECK(tape.book.best_ask() == 103);
}

TEST_CASE("modify to zero quantity is a cancel") {
    Tape tape;
    const ob::OrderId id = tape.book.add_limit(ob::Side::Buy, 100, 5);

    CHECK(tape.book.modify(id, 100, 0));
    CHECK(tape.book.order_count() == 0);
}

TEST_CASE("the spread stays valid under a random workload") {
    ob::GeneratorOptions options{};
    options.events = 5'000;
    options.seed = 7;
    const auto trace = ob::generate(options);

    Tape tape;
    for (const ob::TraceEvent& e : trace) {
        if (e.op == 'L') {
            tape.book.add_limit(e.side, e.price, e.quantity);
        } else if (e.op == 'M') {
            tape.book.add_market(e.side, e.quantity);
        } else if (e.op == 'C') {
            tape.book.cancel(e.id);
        }

        const auto bid = tape.book.best_bid();
        const auto ask = tape.book.best_ask();
        if (bid && ask) {
            REQUIRE(*bid < *ask);
        }
    }
}

TEST_CASE("the same seed produces the same trace") {
    ob::GeneratorOptions options{};
    options.events = 1'000;
    options.seed = 123;

    const auto a = ob::generate(options);
    const auto b = ob::generate(options);

    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i].op == b[i].op);
        CHECK(a[i].side == b[i].side);
        CHECK(a[i].price == b[i].price);
        CHECK(a[i].quantity == b[i].quantity);
    }
}

TEST_CASE("array book rejects prices outside its ladder") {
    ob::ArrayBook book(100, 200);

    CHECK_THROWS_AS(book.add_limit(ob::Side::Buy, 99, 1), std::out_of_range);
    CHECK_THROWS_AS(book.add_limit(ob::Side::Sell, 201, 1), std::out_of_range);
    CHECK(book.order_count() == 0);
}

TEST_CASE("array book advances past emptied levels") {
    ob::ArrayBook book(100, 200);
    book.add_limit(ob::Side::Buy, 105, 5);
    book.add_limit(ob::Side::Buy, 103, 5);

    CHECK(book.best_bid() == 105);
    CHECK(book.add_market(ob::Side::Sell, 5) == 5);
    CHECK(book.best_bid() == 103);
    CHECK(book.add_market(ob::Side::Sell, 5) == 5);
    CHECK(book.best_bid() == std::nullopt);
}

TEST_CASE("array book keeps the best price after a cancel") {
    ob::ArrayBook book(100, 200);
    const ob::OrderId top = book.add_limit(ob::Side::Buy, 110, 5);
    book.add_limit(ob::Side::Buy, 108, 5);

    CHECK(book.cancel(top));
    CHECK(book.best_bid() == 108);
}

TEST_CASE("array book and map book agree on a random workload") {
    ob::GeneratorOptions options{};
    options.events = 20'000;
    options.seed = 11;
    const auto trace = ob::generate(options);

    ob::Price lo = 0;
    ob::Price hi = 0;
    bool seen = false;
    for (const ob::TraceEvent& e : trace) {
        if (e.op != 'L') {
            continue;
        }
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

    std::uint64_t trades_map = 0;
    std::uint64_t volume_map = 0;
    std::uint64_t trades_array = 0;
    std::uint64_t volume_array = 0;

    ob::Book map_book([&](const ob::Trade& t) {
        ++trades_map;
        volume_map += t.quantity;
    });
    ob::ArrayBook array_book(lo, hi, [&](const ob::Trade& t) {
        ++trades_array;
        volume_array += t.quantity;
    });

    std::size_t checked = 0;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        apply(map_book, trace[i]);
        apply(array_book, trace[i]);

        if (i % 500 == 0) {
            CHECK(map_book.order_count() == array_book.order_count());
            CHECK(map_book.best_bid() == array_book.best_bid());
            CHECK(map_book.best_ask() == array_book.best_ask());
            ++checked;
        }
    }

    CHECK(checked > 0);
    CHECK(trades_map == trades_array);
    CHECK(volume_map == volume_array);
    CHECK(map_book.order_count() == array_book.order_count());
}

