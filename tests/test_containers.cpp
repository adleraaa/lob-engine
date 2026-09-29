// Tests for the building blocks: object pool, id hash map, price level queue.

#include <gtest/gtest.h>

#include <set>
#include <unordered_map>
#include <vector>

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
    EXPECT_EQ(level.count, 3u);

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
    EXPECT_EQ(level.count, 0u);
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

// Random inserts/erases checked against std::unordered_map. A tiny starting
// capacity forces many collisions, long probe runs, wrap-around at the end of
// the array and several resizes - the cases where backward-shift deletion
// could go wrong.
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
