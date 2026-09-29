// Tests for the building blocks: object pool, id hash map, price level queue.

#include <gtest/gtest.h>

#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cli.hpp"
#include "lob/object_pool.hpp"
#include "lob/order_id_map.hpp"
#include "lob/price_level.hpp"
#include "order_flow.hpp"

namespace {

using namespace lob;

TEST(ObjectPool, HandsOutDistinctObjectsAndReusesReleasedOnes) {
    ObjectPool<Order> pool(4);
    std::set<Order*> seen;
    std::vector<Order*> taken;
    for (int i = 0; i < 10; ++i) {  // crosses two chunk boundaries
        Order* o = pool.acquire();
        EXPECT_TRUE(seen.insert(o).second) << "same pointer handed out twice";
        taken.push_back(o);
    }
    EXPECT_EQ(pool.capacity(), 12u);
    EXPECT_EQ(pool.in_use(), 10u);

    taken[3]->qty = 42;
    pool.release(taken[3]);
    Order* again = pool.acquire();
    EXPECT_EQ(again, taken[3]);  // LIFO reuse
    EXPECT_EQ(again->qty, 0);    // and value-initialized, no stale data
    EXPECT_EQ(pool.capacity(), 12u);
}

TEST(ObjectPool, ReserveAddsWholeChunksUpFront) {
    ObjectPool<Order> pool(4);
    pool.reserve(9);
    EXPECT_EQ(pool.capacity(), 12u);  // three chunks of 4
    EXPECT_EQ(pool.in_use(), 0u);
    for (int i = 0; i < 12; ++i) {
        pool.acquire();
    }
    EXPECT_EQ(pool.capacity(), 12u);  // no chunk added while acquiring
    pool.reserve(5);                  // already enough: no-op
    EXPECT_EQ(pool.capacity(), 12u);
}

TEST(PriceLevel, FifoQueueWithMiddleErase) {
    PriceLevel level;
    level.price = 50;
    Order a{1, Side::Buy, 50, 3};
    Order b{2, Side::Buy, 50, 4};
    Order c{3, Side::Buy, 50, 5};
    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);
    EXPECT_EQ(level.total_qty, 12);

    level.erase(&b);
    EXPECT_EQ(level.head, &a);
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(level.total_qty, 8);

    level.reduce(&c, 2);
    EXPECT_EQ(c.qty, 3);
    EXPECT_EQ(level.total_qty, 6);

    level.erase(&a);
    level.erase(&c);
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.tail, nullptr);
    EXPECT_EQ(level.total_qty, 0);
}

TEST(OrderIdMap, BasicInsertFindErase) {
    OrderIdMap map(16);
    Order o1, o2;
    EXPECT_TRUE(map.insert(7, &o1));
    EXPECT_FALSE(map.insert(7, &o2));
    EXPECT_EQ(map.find(7), &o1);
    EXPECT_EQ(map.find(8), nullptr);
    EXPECT_TRUE(map.erase(7));
    EXPECT_FALSE(map.erase(7));
    EXPECT_EQ(map.find(7), nullptr);
    EXPECT_EQ(map.size(), 0u);
}

TEST(OrderIdMap, ReserveGrowsOnceForTheRequestedSize) {
    OrderIdMap map(16);
    map.reserve(100);
    const std::size_t capacity = map.capacity();
    EXPECT_GE(capacity, 200u);  // load factor stays <= 1/2
    std::vector<Order> storage(100);
    for (OrderId id = 1; id <= 100; ++id) {
        ASSERT_TRUE(map.insert(id, &storage[id - 1]));
    }
    EXPECT_EQ(map.capacity(), capacity);  // no resize during the inserts
    EXPECT_EQ(map.find(57), &storage[56]);
}

// Forces collisions instead of hoping for them: picks ids whose home slots
// are 14, 15 and 0 of a 16-slot table, so they form one probe cluster that
// wraps around the end of the array. Inserting them in a random order and
// erasing them in another random order exercises backward shift across the
// wrap, where the modular distance arithmetic in erase() matters.
TEST(OrderIdMap, BackwardShiftAcrossTableEndWithForcedCollisions) {
    const OrderIdMap probe(16);  // only used to compute home slots
    int wanted[16] = {};
    wanted[14] = 2;
    wanted[15] = 3;
    wanted[0] = 2;
    std::vector<OrderId> ids;
    for (OrderId id = 1; ids.size() < 7; ++id) {
        const std::size_t slot = probe.home_slot(id);
        if (wanted[slot] > 0) {
            --wanted[slot];
            ids.push_back(id);
        }
    }
    std::vector<Order> storage(ids.size());
    auto value_of = [&](OrderId id) -> Order* {
        for (std::size_t k = 0; k < ids.size(); ++k) {
            if (ids[k] == id) return &storage[k];
        }
        return nullptr;
    };
    auto shuffle = [](std::vector<OrderId>& v, flow::Rng& rng) {
        for (std::size_t k = v.size(); k > 1; --k) {
            std::swap(v[k - 1], v[static_cast<std::size_t>(rng.uniform(0, static_cast<std::int64_t>(k) - 1))]);
        }
    };

    flow::Rng rng(7);
    for (int trial = 0; trial < 2000; ++trial) {
        OrderIdMap map(16);
        std::vector<OrderId> order = ids;
        shuffle(order, rng);
        for (OrderId id : order) {
            ASSERT_TRUE(map.insert(id, value_of(id)));
        }
        ASSERT_EQ(map.capacity(), 16u) << "table grew, so the collisions are no longer forced";

        shuffle(order, rng);
        for (std::size_t erased = 0; erased < order.size(); ++erased) {
            ASSERT_TRUE(map.erase(order[erased]));
            ASSERT_EQ(map.find(order[erased]), nullptr);
            for (std::size_t k = erased + 1; k < order.size(); ++k) {
                ASSERT_EQ(map.find(order[k]), value_of(order[k])) << "trial " << trial << ": lost id " << order[k];
            }
        }
        ASSERT_EQ(map.size(), 0u);
    }
}

// Random inserts/erases checked against std::unordered_map. A small starting
// table and ids drawn from a small range give heavy churn, several resizes
// and naturally occurring collisions (the forced-collision test above covers
// the wrap-around case deliberately).
TEST(OrderIdMap, MatchesStdUnorderedMapUnderRandomChurn) {
    OrderIdMap map(16);
    std::unordered_map<OrderId, Order*> expected;
    std::vector<Order> storage(512);
    flow::Rng rng(12345);
    for (int step = 0; step < 200'000; ++step) {
        const auto id = static_cast<OrderId>(rng.uniform(1, 600));
        Order* value = &storage[static_cast<std::size_t>(rng.uniform(0, 511))];
        switch (rng.uniform(0, 2)) {
            case 0: {
                const bool inserted = map.insert(id, value);
                EXPECT_EQ(inserted, expected.emplace(id, value).second);
                break;
            }
            case 1: EXPECT_EQ(map.erase(id), expected.erase(id) == 1); break;
            default: {
                auto it = expected.find(id);
                EXPECT_EQ(map.find(id), it == expected.end() ? nullptr : it->second);
            }
        }
        ASSERT_EQ(map.size(), expected.size());
    }
    for (const auto& [id, value] : expected) {
        EXPECT_EQ(map.find(id), value);
    }
}

TEST(Cli, ParseU64AcceptsOnlyWholeDecimalNumbers) {
    std::uint64_t v = 7;
    EXPECT_TRUE(cli::parse_u64("0", v));
    EXPECT_EQ(v, 0u);
    EXPECT_TRUE(cli::parse_u64("2000000", v));
    EXPECT_EQ(v, 2'000'000u);
    v = 7;
    for (const char* bad : {"", "abc", "12abc", "-1", "+5", " 5", "99999999999999999999999"}) {
        EXPECT_FALSE(cli::parse_u64(bad, v)) << "accepted '" << bad << "'";
        EXPECT_EQ(v, 7u) << "changed the output on '" << bad << "'";
    }
    EXPECT_FALSE(cli::parse_u64(nullptr, v));
}

TEST(OrderFlowGenerator, IsDeterministicForASeed) {
    flow::Config cfg;
    cfg.seed = 99;
    flow::Generator a(cfg);
    flow::Generator b(cfg);
    for (int i = 0; i < 1000; ++i) {
        const flow::Op x = a.next();
        const flow::Op y = b.next();
        ASSERT_EQ(x.id, y.id);
        ASSERT_EQ(x.price, y.price);
        ASSERT_EQ(x.qty, y.qty);
        ASSERT_EQ(x.kind, y.kind);
        ASSERT_TRUE(x.kind != flow::Op::Kind::Add || x.type == OrderType::Market ||
                    (x.price >= cfg.min_price && x.price <= cfg.max_price));
    }
}

}  // namespace
