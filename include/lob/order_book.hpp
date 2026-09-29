// The matching engine: a limit order book with price-time priority.
#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

#include "lob/events.hpp"
#include "lob/flat_levels.hpp"
#include "lob/map_levels.hpp"
#include "lob/object_pool.hpp"
#include "lob/order_id_map.hpp"
#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Levels is the per-side price level container: MapLevels or FlatLevels.
// Both expose the same small set of member functions (insert, remove, best,
// for_each_from_best, accepts_price, level_count), so the matching logic below
// is written once and the benchmark can compare the two containers directly.
//
// Matching rules:
//   - An incoming order trades against the best opposite price first; within
//     a price, against the oldest resting order first (price-time priority).
//   - Every trade happens at the resting (maker) order's price.
//   - Self-trades are not prevented (no self-trade protection).
//
// Not thread-safe: one thread owns a book.
template <typename Levels>
class OrderBook {
public:
    using Config = typename Levels::Config;

    // The listener must outlive the book.
    explicit OrderBook(EventListener& listener, Config config = {})
        : listener_(listener), bids_(Side::Buy, config), asks_(Side::Sell, config) {}

    OrderBook(const OrderBook&) = delete;  // orders point into this book's pool
    OrderBook& operator=(const OrderBook&) = delete;

    // Submits a new order. price is ignored for Market orders.
    ExecReport add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        const bool is_market = type == OrderType::Market;
        if (Status s = validate_new(id, qty, is_market, price); s != Status::Ok) {
            return ExecReport{s, 0, 0, 0};
        }

        ExecReport report;
        // FOK must not trade at all unless it can trade everything, so check
        // the available liquidity before touching the book.
        if (type == OrderType::FOK && liquidity_up_to(side, price, qty) < qty) {
            report.cancelled = qty;
            return report;
        }

        report.filled = match(id, side, is_market, price, qty);
        const Qty remaining = qty - report.filled;
        if (remaining > 0) {
            if (type == OrderType::Limit) {
                rest(id, side, price, remaining);
                report.resting = remaining;
            } else {
                report.cancelled = remaining;
            }
        }
        return report;
    }

    Status cancel(OrderId id) {
        if (id == 0) {
            return Status::InvalidId;
        }
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return Status::UnknownId;
        }
        remove_resting(order);
        return Status::Ok;
    }

    // Changes a resting order's price and/or remaining quantity.
    //   - same price and new_qty <= current qty: reduced in place, keeps its
    //     place in the queue (the market lost nothing by it shrinking)
    //   - price change or quantity increase: treated as cancel + new Limit
    //     order with the same id, so it goes to the back of the queue and may
    //     trade immediately if the new price crosses.
    ExecReport modify(OrderId id, Price new_price, Qty new_qty) {
        if (id == 0) {
            return ExecReport{Status::InvalidId, 0, 0, 0};
        }
        Order* order = ids_.find(id);
        if (order == nullptr) {
            return ExecReport{Status::UnknownId, 0, 0, 0};
        }
        if (new_qty <= 0) {
            return ExecReport{Status::InvalidQty, 0, 0, 0};
        }
        if (Status s = validate_price(new_price); s != Status::Ok) {
            return ExecReport{s, 0, 0, 0};
        }

        if (new_price == order->price && new_qty <= order->qty) {
            if (new_qty < order->qty) {
                PriceLevel* level = order->level;
                level->reduce(order, order->qty - new_qty);
                listener_.on_level_update({order->side, level->price, level->total_qty});
            }
            return ExecReport{Status::Ok, 0, new_qty, 0};
        }

        const Side side = order->side;
        remove_resting(order);  // `order` is dangling from here on
        return add(id, side, OrderType::Limit, new_price, new_qty);
    }

    // ---- queries ------------------------------------------------------------

    std::optional<Price> best_bid() const { return best_price(bids_); }
    std::optional<Price> best_ask() const { return best_price(asks_); }

    bool contains(OrderId id) const { return id != 0 && ids_.find(id) != nullptr; }
    std::size_t order_count() const { return ids_.size(); }
    std::size_t level_count(Side side) const { return levels(side).level_count(); }

    // Copies out the whole book. O(orders); meant for tests and debugging.
    BookSnapshot snapshot() const {
        BookSnapshot snap;
        copy_side(bids_, snap.bids);
        copy_side(asks_, snap.asks);
        return snap;
    }

private:
    Status validate_price(Price price) const {
        if (price <= 0) {
            return Status::InvalidPrice;
        }
        // Both sides share one Config, so asking the bid side is enough.
        if (!bids_.accepts_price(price)) {
            return Status::PriceOutOfRange;
        }
        return Status::Ok;
    }

    Status validate_new(OrderId id, Qty qty, bool is_market, Price price) const {
        if (id == 0) {
            return Status::InvalidId;
        }
        if (qty <= 0) {
            return Status::InvalidQty;
        }
        if (ids_.find(id) != nullptr) {
            return Status::DuplicateId;
        }
        return is_market ? Status::Ok : validate_price(price);
    }

    Levels& levels(Side side) { return side == Side::Buy ? bids_ : asks_; }
    const Levels& levels(Side side) const { return side == Side::Buy ? bids_ : asks_; }

    // Can an incoming order on taker_side with this limit trade at level_price?
    static bool crosses(Side taker_side, Price limit, Price level_price) {
        return taker_side == Side::Buy ? level_price <= limit : level_price >= limit;
    }

    // Total opposite-side quantity a FOK order could trade, counting only up
    // to `needed` (we can stop walking once we know there is enough).
    Qty liquidity_up_to(Side taker_side, Price limit, Qty needed) const {
        Qty available = 0;
        levels(opposite(taker_side)).for_each_from_best([&](const PriceLevel& level) {
            if (!crosses(taker_side, limit, level.price)) {
                return false;
            }
            available += level.total_qty;
            return available < needed;
        });
        return available;
    }

    // Trades the incoming order against the opposite side. Returns the filled
    // quantity. Emits one Trade and one LevelUpdate per fill.
    Qty match(OrderId taker_id, Side taker_side, bool is_market, Price limit, Qty qty) {
        Levels& book = levels(opposite(taker_side));
        Qty filled = 0;
        while (filled < qty) {
            PriceLevel* level = book.best();
            if (level == nullptr || (!is_market && !crosses(taker_side, limit, level->price))) {
                break;
            }
            Order* maker = level->head;
            const Qty fill = std::min(qty - filled, maker->qty);
            const Price price = level->price;
            filled += fill;
            listener_.on_trade({taker_id, maker->id, taker_side, price, fill});

            if (fill == maker->qty) {
                // remove_resting may destroy `level`, so do not touch it after.
                remove_resting(maker);
            } else {
                level->reduce(maker, fill);
                listener_.on_level_update({maker->side, price, level->total_qty});
            }
        }
        return filled;
    }

    void rest(OrderId id, Side side, Price price, Qty qty) {
        Order* order = pool_.acquire();
        order->id = id;
        order->side = side;
        order->price = price;
        order->qty = qty;
        levels(side).insert(order);
        ids_.insert(id, order);
        listener_.on_level_update({side, price, order->level->total_qty});
    }

    // Takes a resting order off the book entirely and returns its memory to
    // the pool. Emits the level update for its price.
    void remove_resting(Order* order) {
        const Side side = order->side;
        const Price price = order->price;
        const Qty level_total_after = order->level->total_qty - order->qty;
        levels(side).remove(order);
        ids_.erase(order->id);
        pool_.release(order);
        listener_.on_level_update({side, price, level_total_after});
    }

    static std::optional<Price> best_price(const Levels& side) {
        std::optional<Price> result;
        side.for_each_from_best([&](const PriceLevel& level) {
            result = level.price;
            return false;
        });
        return result;
    }

    static void copy_side(const Levels& side, std::vector<LevelView>& out) {
        side.for_each_from_best([&](const PriceLevel& level) {
            LevelView view{level.price, {}};
            for (const Order* o = level.head; o != nullptr; o = o->next) {
                view.orders.push_back({o->id, o->qty});
            }
            out.push_back(std::move(view));
            return true;
        });
    }

    EventListener& listener_;
    Levels bids_;
    Levels asks_;
    OrderIdMap ids_;
    ObjectPool<Order> pool_;
};

using MapOrderBook = OrderBook<MapLevels>;
using FlatOrderBook = OrderBook<FlatLevels>;

}  // namespace lob
