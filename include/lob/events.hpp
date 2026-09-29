// Events the book emits while it processes a request, plus the snapshot type
// used to compare two books for equality.
#pragma once

#include <vector>

#include "lob/types.hpp"

namespace lob {

// One execution between an incoming (taker) order and a resting (maker)
// order. The price is always the maker's price.
struct Trade {
    OrderId taker_id = 0;
    OrderId maker_id = 0;
    Side taker_side = Side::Buy;
    Price price = 0;
    Qty qty = 0;

    bool operator==(const Trade&) const = default;
};

// The aggregate resting quantity at one price changed. total_qty == 0 means
// the level is now gone. One update is emitted per change (per fill, per
// cancel, per new resting order), right after the change, in order.
struct LevelUpdate {
    Side side = Side::Buy;
    Price price = 0;
    Qty total_qty = 0;

    bool operator==(const LevelUpdate&) const = default;
};

// Receiver of book events. The book holds a reference to it and calls it
// synchronously from inside add/cancel/modify.
class EventListener {
public:
    virtual ~EventListener() = default;
    virtual void on_trade(const Trade& trade) = 0;
    virtual void on_level_update(const LevelUpdate& update) = 0;
};

// Drops everything. Useful when only the book state matters.
class NullListener final : public EventListener {
public:
    void on_trade(const Trade&) override {}
    void on_level_update(const LevelUpdate&) override {}
};

// Keeps every event, in order. Used by tests and the differential checker.
class RecordingListener final : public EventListener {
public:
    void on_trade(const Trade& trade) override { trades.push_back(trade); }
    void on_level_update(const LevelUpdate& update) override { updates.push_back(update); }

    void clear() {
        trades.clear();
        updates.clear();
    }

    std::vector<Trade> trades;
    std::vector<LevelUpdate> updates;
};

// Full visible state of a book: every resting order, grouped by level, levels
// ordered best-first, orders inside a level in time priority (FIFO) order.
struct OrderView {
    OrderId id = 0;
    Qty qty = 0;
    bool operator==(const OrderView&) const = default;
};

struct LevelView {
    Price price = 0;
    std::vector<OrderView> orders;
    bool operator==(const LevelView&) const = default;
};

struct BookSnapshot {
    std::vector<LevelView> bids;  // highest price first
    std::vector<LevelView> asks;  // lowest price first
    bool operator==(const BookSnapshot&) const = default;
};

}  // namespace lob
