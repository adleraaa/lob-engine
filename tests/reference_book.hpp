// A deliberately simple, slow order book used as the oracle in differential
// tests. It shares no data structures with the real engine: every resting
// order sits in one flat vector and every question is answered by a linear
// scan. The point is that it is short enough to check by reading it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "lob/events.hpp"
#include "lob/types.hpp"

namespace lob::ref {

// Defined outside the class: a nested struct with default member
// initializers cannot be used as a default argument inside its own class.
struct ReferenceConfig {
    Price min_price = 1;
    Price max_price = std::numeric_limits<Price>::max();
};

class ReferenceBook {
public:
    using Config = ReferenceConfig;

    explicit ReferenceBook(EventListener& listener, Config config = {}) : listener_(listener), config_(config) {}

    ExecReport add(OrderId id, Side side, OrderType type, Price price, Qty qty) {
        if (id == 0) return {Status::InvalidId, 0, 0, 0};
        if (qty <= 0) return {Status::InvalidQty, 0, 0, 0};
        if (find(id) != nullptr) return {Status::DuplicateId, 0, 0, 0};
        const bool is_market = type == OrderType::Market;
        if (!is_market) {
            if (price <= 0) return {Status::InvalidPrice, 0, 0, 0};
            if (price < config_.min_price || price > config_.max_price) {
                return {Status::PriceOutOfRange, 0, 0, 0};
            }
        }

        if (type == OrderType::FOK) {
            Qty available = 0;
            for (const Resting& r : orders_) {
                if (r.side != side && can_trade(side, is_market, price, r.price)) available += r.qty;
            }
            if (available < qty) return {Status::Ok, 0, 0, qty};
        }

        ExecReport report;
        Qty left = qty;
        while (left > 0) {
            Resting* maker = best_maker(side, is_market, price);
            if (maker == nullptr) break;
            const Qty fill = std::min(left, maker->qty);
            left -= fill;
            report.filled += fill;
            const Price maker_price = maker->price;
            const OrderId maker_id = maker->id;
            listener_.on_trade({id, maker_id, side, maker_price, fill});
            maker->qty -= fill;
            if (maker->qty == 0) erase(maker_id);
            listener_.on_level_update({opposite(side), maker_price, level_total(opposite(side), maker_price)});
        }

        if (left > 0) {
            if (type == OrderType::Limit) {
                orders_.push_back({id, side, price, left, next_seq_++});
                report.resting = left;
                listener_.on_level_update({side, price, level_total(side, price)});
            } else {
                report.cancelled = left;
            }
        }
        return report;
    }

    Status cancel(OrderId id) {
        if (id == 0) return Status::InvalidId;
        const Resting* r = find(id);
        if (r == nullptr) return Status::UnknownId;
        const Side side = r->side;
        const Price price = r->price;
        erase(id);
        listener_.on_level_update({side, price, level_total(side, price)});
        return Status::Ok;
    }

    ExecReport modify(OrderId id, Price new_price, Qty new_qty) {
        if (id == 0) return {Status::InvalidId, 0, 0, 0};
        Resting* r = find(id);
        if (r == nullptr) return {Status::UnknownId, 0, 0, 0};
        if (new_qty <= 0) return {Status::InvalidQty, 0, 0, 0};
        if (new_price <= 0) return {Status::InvalidPrice, 0, 0, 0};
        if (new_price < config_.min_price || new_price > config_.max_price) {
            return {Status::PriceOutOfRange, 0, 0, 0};
        }
        if (new_price == r->price && new_qty <= r->qty) {
            if (new_qty < r->qty) {
                r->qty = new_qty;  // same seq: keeps time priority
                listener_.on_level_update({r->side, r->price, level_total(r->side, r->price)});
            }
            return {Status::Ok, 0, new_qty, 0};
        }
        const Side side = r->side;
        cancel(id);
        return add(id, side, OrderType::Limit, new_price, new_qty);
    }

    BookSnapshot snapshot() const {
        std::vector<Resting> sorted = orders_;
        // Best price first, then oldest first.
        std::sort(sorted.begin(), sorted.end(), [](const Resting& a, const Resting& b) {
            if (a.side != b.side) return a.side < b.side;
            if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
            return a.seq < b.seq;
        });
        BookSnapshot snap;
        for (const Resting& r : sorted) {
            std::vector<LevelView>& out = r.side == Side::Buy ? snap.bids : snap.asks;
            if (out.empty() || out.back().price != r.price) out.push_back({r.price, {}});
            out.back().orders.push_back({r.id, r.qty});
        }
        return snap;
    }

    std::size_t order_count() const { return orders_.size(); }

private:
    struct Resting {
        OrderId id;
        Side side;
        Price price;
        Qty qty;
        std::uint64_t seq;  // arrival order: smaller = older = higher priority
    };

    static bool can_trade(Side taker_side, bool is_market, Price limit, Price maker_price) {
        if (is_market) return true;
        return taker_side == Side::Buy ? maker_price <= limit : maker_price >= limit;
    }

    // Best opposite order the taker can trade with: best price, then oldest.
    Resting* best_maker(Side taker_side, bool is_market, Price limit) {
        Resting* best = nullptr;
        for (Resting& r : orders_) {
            if (r.side == taker_side || !can_trade(taker_side, is_market, limit, r.price)) continue;
            if (best == nullptr) {
                best = &r;
                continue;
            }
            const bool better_price = taker_side == Side::Buy ? r.price < best->price : r.price > best->price;
            if (better_price || (r.price == best->price && r.seq < best->seq)) best = &r;
        }
        return best;
    }

    Qty level_total(Side side, Price price) const {
        Qty total = 0;
        for (const Resting& r : orders_) {
            if (r.side == side && r.price == price) total += r.qty;
        }
        return total;
    }

    Resting* find(OrderId id) {
        for (Resting& r : orders_) {
            if (r.id == id) return &r;
        }
        return nullptr;
    }

    void erase(OrderId id) {
        std::erase_if(orders_, [id](const Resting& r) { return r.id == id; });
    }

    EventListener& listener_;
    Config config_;
    std::vector<Resting> orders_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace lob::ref
