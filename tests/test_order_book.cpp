// Behaviour tests that every book implementation must pass: the two real
// engines and the reference oracle (so the oracle is itself checked against
// hand-worked examples, not just against the engines).

#include <gtest/gtest.h>

#include "lob/order_book.hpp"
#include "reference_book.hpp"

namespace {

using namespace lob;

template <typename Book>
class BookTest : public ::testing::Test {
protected:
    RecordingListener events;
    Book book{events};

    ExecReport limit(OrderId id, Side side, Price price, Qty qty) {
        return book.add(id, side, OrderType::Limit, price, qty);
    }
};

using BookTypes = ::testing::Types<MapOrderBook, FlatOrderBook, ref::ReferenceBook>;
TYPED_TEST_SUITE(BookTest, BookTypes);

constexpr Side kBuy = Side::Buy;
constexpr Side kSell = Side::Sell;

TYPED_TEST(BookTest, RestingLimitOrderEmitsLevelUpdate) {
    EXPECT_EQ(this->limit(1, kBuy, 100, 10), (ExecReport{Status::Ok, 0, 10, 0}));
    EXPECT_EQ(this->limit(2, kBuy, 100, 5), (ExecReport{Status::Ok, 0, 5, 0}));
    EXPECT_TRUE(this->events.trades.empty());
    const std::vector<LevelUpdate> want{{kBuy, 100, 10}, {kBuy, 100, 15}};
    EXPECT_EQ(this->events.updates, want);
    const BookSnapshot snap = this->book.snapshot();
    ASSERT_EQ(snap.bids.size(), 1u);
    EXPECT_EQ(snap.bids[0], (LevelView{100, {{1, 10}, {2, 5}}}));
    EXPECT_TRUE(snap.asks.empty());
}

TYPED_TEST(BookTest, SnapshotOrdersLevelsBestFirst) {
    this->limit(1, kBuy, 99, 1);
    this->limit(2, kBuy, 101, 1);
    this->limit(3, kBuy, 100, 1);
    this->limit(4, kSell, 105, 1);
    this->limit(5, kSell, 103, 1);
    const BookSnapshot snap = this->book.snapshot();
    ASSERT_EQ(snap.bids.size(), 3u);
    EXPECT_EQ(snap.bids[0].price, 101);
    EXPECT_EQ(snap.bids[2].price, 99);
    ASSERT_EQ(snap.asks.size(), 2u);
    EXPECT_EQ(snap.asks[0].price, 103);
    EXPECT_EQ(snap.asks[1].price, 105);
}

TYPED_TEST(BookTest, CrossingOrderTradesAtMakerPriceInTimePriority) {
    this->limit(1, kSell, 101, 5);
    this->limit(2, kSell, 101, 5);  // same price, later: trades second
    this->limit(3, kSell, 100, 5);  // better price: trades first
    this->events.clear();

    EXPECT_EQ(this->limit(10, kBuy, 102, 12), (ExecReport{Status::Ok, 12, 0, 0}));
    const std::vector<Trade> want{{10, 3, kBuy, 100, 5}, {10, 1, kBuy, 101, 5}, {10, 2, kBuy, 101, 2}};
    EXPECT_EQ(this->events.trades, want);
    const std::vector<LevelUpdate> updates{{kSell, 100, 0}, {kSell, 101, 5}, {kSell, 101, 3}};
    EXPECT_EQ(this->events.updates, updates);
    EXPECT_EQ(this->book.snapshot().asks, (std::vector<LevelView>{{101, {{2, 3}}}}));
}

TYPED_TEST(BookTest, PartialFillRestsRemainder) {
    this->limit(1, kSell, 100, 4);
    this->events.clear();
    EXPECT_EQ(this->limit(2, kBuy, 100, 10), (ExecReport{Status::Ok, 4, 6, 0}));
    const std::vector<LevelUpdate> want{{kSell, 100, 0}, {kBuy, 100, 6}};
    EXPECT_EQ(this->events.updates, want);
    const BookSnapshot snap = this->book.snapshot();
    EXPECT_TRUE(snap.asks.empty());
    EXPECT_EQ(snap.bids, (std::vector<LevelView>{{100, {{2, 6}}}}));
}

TYPED_TEST(BookTest, LimitPriceStopsMatching) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 102, 5);
    EXPECT_EQ(this->limit(3, kBuy, 101, 8), (ExecReport{Status::Ok, 5, 3, 0}));
    const BookSnapshot snap = this->book.snapshot();
    EXPECT_EQ(snap.asks, (std::vector<LevelView>{{102, {{2, 5}}}}));
    EXPECT_EQ(snap.bids, (std::vector<LevelView>{{101, {{3, 3}}}}));
}

TYPED_TEST(BookTest, SellTakerMatchesHighestBidFirst) {
    this->limit(1, kBuy, 99, 5);
    this->limit(2, kBuy, 100, 5);
    this->events.clear();
    EXPECT_EQ(this->book.add(3, kSell, OrderType::IOC, 99, 7), (ExecReport{Status::Ok, 7, 0, 0}));
    const std::vector<Trade> want{{3, 2, kSell, 100, 5}, {3, 1, kSell, 99, 2}};
    EXPECT_EQ(this->events.trades, want);
}

TYPED_TEST(BookTest, MarketOrderSweepsAndDiscardsRemainder) {
    this->limit(1, kSell, 100, 3);
    this->limit(2, kSell, 250, 3);
    EXPECT_EQ(this->book.add(3, kBuy, OrderType::Market, 0, 10), (ExecReport{Status::Ok, 6, 0, 4}));
    EXPECT_EQ(this->events.trades.back(), (Trade{3, 2, kBuy, 250, 3}));
    EXPECT_TRUE(this->book.snapshot().asks.empty());
    EXPECT_TRUE(this->book.snapshot().bids.empty());
}

TYPED_TEST(BookTest, MarketOrderIntoEmptyBookIsCancelled) {
    EXPECT_EQ(this->book.add(1, kSell, OrderType::Market, 0, 10), (ExecReport{Status::Ok, 0, 0, 10}));
    EXPECT_TRUE(this->events.trades.empty());
    EXPECT_TRUE(this->events.updates.empty());
}

TYPED_TEST(BookTest, IocNeverRests) {
    this->limit(1, kSell, 100, 2);
    EXPECT_EQ(this->book.add(2, kBuy, OrderType::IOC, 100, 5), (ExecReport{Status::Ok, 2, 0, 3}));
    EXPECT_TRUE(this->book.snapshot().bids.empty());
    // The id is free again because the IOC never rested.
    EXPECT_EQ(this->limit(2, kBuy, 90, 1).status, Status::Ok);
}

TYPED_TEST(BookTest, FokKilledWhenNotFullyFillable) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 102, 5);  // beyond the FOK limit, must not count
    this->events.clear();
    EXPECT_EQ(this->book.add(3, kBuy, OrderType::FOK, 101, 6), (ExecReport{Status::Ok, 0, 0, 6}));
    EXPECT_TRUE(this->events.trades.empty());
    EXPECT_TRUE(this->events.updates.empty());
    EXPECT_EQ(this->book.snapshot().asks.size(), 2u);
}

TYPED_TEST(BookTest, FokFillsWhenExactlyEnoughLiquidity) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kSell, 101, 5);
    EXPECT_EQ(this->book.add(3, kBuy, OrderType::FOK, 101, 10), (ExecReport{Status::Ok, 10, 0, 0}));
    EXPECT_EQ(this->events.trades.size(), 2u);
    EXPECT_TRUE(this->book.snapshot().asks.empty());
}

TYPED_TEST(BookTest, CancelRemovesOrderAndEmitsUpdate) {
    this->limit(1, kBuy, 100, 5);
    this->limit(2, kBuy, 100, 7);
    this->events.clear();
    EXPECT_EQ(this->book.cancel(1), Status::Ok);
    EXPECT_EQ(this->book.cancel(2), Status::Ok);
    const std::vector<LevelUpdate> want{{kBuy, 100, 7}, {kBuy, 100, 0}};
    EXPECT_EQ(this->events.updates, want);
    EXPECT_TRUE(this->book.snapshot().bids.empty());
    EXPECT_EQ(this->book.cancel(1), Status::UnknownId);
}

TYPED_TEST(BookTest, CancelFromMiddleOfQueueKeepsOthersInOrder) {
    this->limit(1, kSell, 100, 1);
    this->limit(2, kSell, 100, 2);
    this->limit(3, kSell, 100, 3);
    EXPECT_EQ(this->book.cancel(2), Status::Ok);
    EXPECT_EQ(this->book.snapshot().asks, (std::vector<LevelView>{{100, {{1, 1}, {3, 3}}}}));
}

TYPED_TEST(BookTest, FilledOrderIdCanBeReused) {
    this->limit(1, kSell, 100, 5);
    this->limit(2, kBuy, 100, 5);
    EXPECT_EQ(this->book.cancel(1), Status::UnknownId);
    EXPECT_EQ(this->limit(1, kBuy, 90, 5).status, Status::Ok);
}

TYPED_TEST(BookTest, RejectsInvalidRequestsWithoutSideEffects) {
    this->limit(1, kBuy, 100, 5);
    this->events.clear();
    EXPECT_EQ(this->limit(0, kBuy, 100, 5).status, Status::InvalidId);
    EXPECT_EQ(this->limit(2, kBuy, 100, 0).status, Status::InvalidQty);
    EXPECT_EQ(this->limit(2, kBuy, 100, -3).status, Status::InvalidQty);
    EXPECT_EQ(this->limit(2, kBuy, 0, 5).status, Status::InvalidPrice);
    EXPECT_EQ(this->limit(1, kSell, 100, 5).status, Status::DuplicateId);
    EXPECT_EQ(this->book.cancel(0), Status::InvalidId);
    EXPECT_EQ(this->book.modify(1, 100, 0).status, Status::InvalidQty);
    EXPECT_EQ(this->book.modify(1, -1, 5).status, Status::InvalidPrice);
    EXPECT_EQ(this->book.modify(99, 100, 5).status, Status::UnknownId);
    EXPECT_TRUE(this->events.trades.empty());
    EXPECT_TRUE(this->events.updates.empty());
    EXPECT_EQ(this->book.snapshot().bids, (std::vector<LevelView>{{100, {{1, 5}}}}));
}

TYPED_TEST(BookTest, ModifyDownKeepsTimePriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->events.clear();
    EXPECT_EQ(this->book.modify(1, 100, 4), (ExecReport{Status::Ok, 0, 4, 0}));
    EXPECT_EQ(this->events.updates, (std::vector<LevelUpdate>{{kBuy, 100, 14}}));
    EXPECT_EQ(this->book.snapshot().bids, (std::vector<LevelView>{{100, {{1, 4}, {2, 10}}}}));
}

TYPED_TEST(BookTest, ModifyToSameQuantityIsNoOp) {
    this->limit(1, kBuy, 100, 10);
    this->events.clear();
    EXPECT_EQ(this->book.modify(1, 100, 10), (ExecReport{Status::Ok, 0, 10, 0}));
    EXPECT_TRUE(this->events.updates.empty());
}

TYPED_TEST(BookTest, ModifyUpLosesTimePriority) {
    this->limit(1, kBuy, 100, 10);
    this->limit(2, kBuy, 100, 10);
    this->events.clear();
    EXPECT_EQ(this->book.modify(1, 100, 11), (ExecReport{Status::Ok, 0, 11, 0}));
    // Cancel + re-add: the level drops to 10 (order 1 removed), then rises
    // to 21 (order 1 re-queued at the back). Two updates, not one.
    EXPECT_EQ(this->events.updates, (std::vector<LevelUpdate>{{kBuy, 100, 10}, {kBuy, 100, 21}}));
    EXPECT_EQ(this->book.snapshot().bids, (std::vector<LevelView>{{100, {{2, 10}, {1, 11}}}}));
}

TYPED_TEST(BookTest, ModifyPriceMovesOrderAndCanTrade) {
    this->limit(1, kSell, 105, 3);
    this->limit(2, kBuy, 100, 10);
    this->events.clear();
    // Moving the bid up to 105 crosses the resting ask.
    EXPECT_EQ(this->book.modify(2, 105, 10), (ExecReport{Status::Ok, 3, 7, 0}));
    EXPECT_EQ(this->events.trades, (std::vector<Trade>{{2, 1, kBuy, 105, 3}}));
    const std::vector<LevelUpdate> want{{kBuy, 100, 0}, {kSell, 105, 0}, {kBuy, 105, 7}};
    EXPECT_EQ(this->events.updates, want);
    EXPECT_EQ(this->book.snapshot().bids, (std::vector<LevelView>{{105, {{2, 7}}}}));
}

// ---- bounded tick range: every book type can be configured with one ---------

template <typename Book>
class RangeTest : public ::testing::Test {
protected:
    RecordingListener events;
    Book book{events, {100, 200}};
};
TYPED_TEST_SUITE(RangeTest, BookTypes);

TYPED_TEST(RangeTest, RejectsPricesOutsideConfiguredRangeWithoutSideEffects) {
    auto& book = this->book;
    EXPECT_EQ(book.add(1, kBuy, OrderType::Limit, 99, 1).status, Status::PriceOutOfRange);
    EXPECT_EQ(book.add(1, kBuy, OrderType::Limit, 201, 1).status, Status::PriceOutOfRange);
    EXPECT_EQ(book.add(1, kBuy, OrderType::IOC, 201, 1).status, Status::PriceOutOfRange);
    EXPECT_TRUE(this->events.updates.empty());

    EXPECT_EQ(book.add(1, kBuy, OrderType::Limit, 100, 1).status, Status::Ok);  // both ends are inside
    EXPECT_EQ(book.add(2, kSell, OrderType::Limit, 200, 1).status, Status::Ok);
    this->events.clear();

    // modify() removes the order before re-adding it, so it must validate the
    // new price first: a rejected modify must leave order 2 where it was.
    EXPECT_EQ(book.modify(2, 201, 1).status, Status::PriceOutOfRange);
    EXPECT_EQ(book.modify(2, 201, 5).status, Status::PriceOutOfRange);
    EXPECT_TRUE(this->events.updates.empty());
    EXPECT_EQ(book.snapshot().asks, (std::vector<LevelView>{{200, {{2, 1}}}}));

    // Market orders carry no price, so the range does not apply.
    EXPECT_EQ(book.add(3, kSell, OrderType::Market, 0, 1), (ExecReport{Status::Ok, 1, 0, 0}));
    EXPECT_EQ(this->events.trades, (std::vector<Trade>{{3, 1, kSell, 100, 1}}));
    EXPECT_EQ(this->events.updates, (std::vector<LevelUpdate>{{kBuy, 100, 0}}));
    EXPECT_TRUE(book.snapshot().bids.empty());
}

// ---- engine-only tests (the reference book has no such queries) -------------

template <typename Book>
class EngineTest : public BookTest<Book> {};
using EngineTypes = ::testing::Types<MapOrderBook, FlatOrderBook>;
TYPED_TEST_SUITE(EngineTest, EngineTypes);

TYPED_TEST(EngineTest, BestPricesTrackAddsAndRemovals) {
    EXPECT_FALSE(this->book.best_bid().has_value());
    this->limit(1, kBuy, 100, 1);
    this->limit(2, kBuy, 102, 1);
    this->limit(3, kSell, 110, 1);
    this->limit(4, kSell, 108, 1);
    EXPECT_EQ(this->book.best_bid(), 102);
    EXPECT_EQ(this->book.best_ask(), 108);
    this->book.cancel(2);
    this->book.cancel(4);
    EXPECT_EQ(this->book.best_bid(), 100);
    EXPECT_EQ(this->book.best_ask(), 110);
    EXPECT_EQ(this->book.level_count(kBuy), 1u);
    this->book.cancel(1);
    EXPECT_FALSE(this->book.best_bid().has_value());
    EXPECT_EQ(this->book.level_count(kBuy), 0u);
}

TYPED_TEST(EngineTest, OrderCountAndContains) {
    this->limit(1, kBuy, 100, 1);
    this->limit(2, kSell, 101, 1);
    EXPECT_EQ(this->book.order_count(), 2u);
    EXPECT_TRUE(this->book.contains(1));
    this->book.add(3, kBuy, OrderType::Market, 0, 1);
    EXPECT_FALSE(this->book.contains(2));
    EXPECT_FALSE(this->book.contains(3));
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(EngineTest, ReserveDoesNotChangeBehaviour) {
    this->book.reserve(10'000);
    this->limit(1, kSell, 100, 5);
    EXPECT_EQ(this->limit(2, kBuy, 100, 3), (ExecReport{Status::Ok, 3, 0, 0}));
    EXPECT_EQ(this->book.snapshot().asks, (std::vector<LevelView>{{100, {{1, 2}}}}));
    EXPECT_EQ(this->book.order_count(), 1u);
}

// Many orders force the id map to grow and the pool to add chunks; every
// order must still be found and cancellable afterwards.
TYPED_TEST(EngineTest, SurvivesGrowthOfPoolAndIdMap) {
    constexpr OrderId kCount = 20'000;
    for (OrderId id = 1; id <= kCount; ++id) {
        ASSERT_EQ(this->limit(id, kBuy, 100 + static_cast<Price>(id % 50), 1).status, Status::Ok);
    }
    EXPECT_EQ(this->book.order_count(), kCount);
    for (OrderId id = 1; id <= kCount; id += 2) {
        ASSERT_EQ(this->book.cancel(id), Status::Ok);
    }
    EXPECT_EQ(this->book.order_count(), kCount / 2);
    EXPECT_EQ(this->book.best_bid(), 148);  // odd ids (odd price offsets) are gone
}

TEST(FlatOrderBookTest, BestScansPastEmptyLevelsAtTheEdgesOfTheRange) {
    NullListener events;
    FlatOrderBook book(events, {1, 1000});
    book.add(1, kBuy, OrderType::Limit, 1, 1);  // lowest possible bid
    book.add(2, kBuy, OrderType::Limit, 1000, 1);
    book.add(3, kSell, OrderType::Limit, 1000, 1);  // crosses, fills bid 2
    EXPECT_EQ(book.best_bid(), 1);
    EXPECT_FALSE(book.best_ask().has_value());
    book.add(4, kSell, OrderType::Limit, 1, 1);  // fills bid 1
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(FlatOrderBookTest, RejectsBadRangeConfig) {
    NullListener events;
    EXPECT_THROW(FlatOrderBook(events, {0, 10}), std::invalid_argument);
    EXPECT_THROW(FlatOrderBook(events, {10, 5}), std::invalid_argument);
}

}  // namespace
