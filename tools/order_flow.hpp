// Synthetic order-flow generator shared by the differential test and the
// benchmark. Deterministic for a given seed on every compiler and platform:
// it uses its own tiny PRNG instead of <random> distributions, whose exact
// output is implementation-defined.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob::flow {

struct Op {
    enum class Kind : std::uint8_t { Add, Cancel, Modify };
    Kind kind = Kind::Add;
    OrderId id = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;  // Add: limit price (ignored for Market); Modify: new price
    Qty qty = 0;      // Add: quantity; Modify: new quantity
};

// Probabilities per generated request. Whatever is not cancel/modify/market/
// ioc/fok is a Limit add. `cross` is the chance that a Limit add is priced to
// trade immediately instead of joining the passive side of the book.
struct Mix {
    double cancel = 0.30;
    double modify = 0.05;
    double market = 0.03;
    double ioc = 0.03;
    double fok = 0.02;
    double cross = 0.15;
    double invalid = 0.0;  // deliberately bad requests (difftest only)
};

struct Config {
    std::uint64_t seed = 1;
    Price mid = 10'000;   // starting mid price, in ticks
    Price min_price = 1;  // generated prices stay inside [min, max]
    Price max_price = 20'000;
    Price passive_depth = 40;  // passive orders land up to this far from mid
    Price cross_depth = 5;     // aggressive orders reach this far through mid
    Qty max_qty = 100;
    // Upper bound on ids the generator believes may be resting. Every Limit
    // order is remembered until the generator cancels it, and once this many
    // are remembered the next request is forced to be a cancel. That keeps
    // the book size bounded (and stationary) however long the stream runs.
    std::size_t max_live_ids = 4096;
    Mix mix;
};

// Small, fast, statistically decent PRNG (splitmix64).
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform integer in [lo, hi]. The modulo bias is negligible for the
    // small ranges used here.
    std::int64_t uniform(std::int64_t lo, std::int64_t hi) {
        const auto span = static_cast<std::uint64_t>(hi - lo) + 1;
        return lo + static_cast<std::int64_t>(next() % span);
    }

    // True with probability p.
    bool chance(double p) { return static_cast<double>(next() >> 11) * 0x1.0p-53 < p; }

private:
    std::uint64_t state_;
};

class Generator {
public:
    explicit Generator(const Config& config) : cfg_(config), rng_(config.seed), mid_(config.mid) {}

    Op next() {
        drift_mid();
        if (tracked_.size() >= cfg_.max_live_ids) {
            return cancel_op();
        }
        const Mix& m = cfg_.mix;
        if (rng_.chance(m.invalid)) {
            return invalid_op();
        }
        double roll = static_cast<double>(rng_.next() >> 11) * 0x1.0p-53;
        if ((roll -= m.cancel) < 0 && !tracked_.empty()) {
            return cancel_op();
        }
        if ((roll -= m.modify) < 0 && !tracked_.empty()) {
            return modify_op();
        }
        if ((roll -= m.market) < 0) {
            return taker_op(OrderType::Market);
        }
        if ((roll -= m.ioc) < 0) {
            return taker_op(OrderType::IOC);
        }
        if ((roll -= m.fok) < 0) {
            return taker_op(OrderType::FOK);
        }
        return limit_op();
    }

    std::vector<Op> generate(std::size_t count) {
        std::vector<Op> ops;
        ops.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            ops.push_back(next());
        }
        return ops;
    }

private:
    struct Tracked {
        OrderId id;
        Side side;
        Price price;
    };

    Side random_side() { return rng_.chance(0.5) ? Side::Buy : Side::Sell; }
    Qty random_qty() { return rng_.uniform(1, cfg_.max_qty); }

    Price clamp(Price p) const {
        return p < cfg_.min_price ? cfg_.min_price : (p > cfg_.max_price ? cfg_.max_price : p);
    }

    // Random walk of the mid price, kept far enough from the edges that
    // generated prices never need clamping in practice.
    void drift_mid() {
        if (rng_.chance(0.01)) {
            mid_ += rng_.chance(0.5) ? 1 : -1;
            const Price margin = cfg_.passive_depth + cfg_.cross_depth + 1;
            if (mid_ < cfg_.min_price + margin) mid_ = cfg_.min_price + margin;
            if (mid_ > cfg_.max_price - margin) mid_ = cfg_.max_price - margin;
        }
    }

    // A price that rests without trading (usually): below mid for buys,
    // above mid for sells. Distances skew toward the top of the book.
    Price passive_price(Side side) {
        const Price a = rng_.uniform(1, cfg_.passive_depth);
        const Price b = rng_.uniform(1, cfg_.passive_depth);
        const Price offset = a < b ? a : b;  // min of two uniforms: more mass near 1
        return clamp(side == Side::Buy ? mid_ - offset : mid_ + offset);
    }

    // A price that reaches through the mid and will usually trade.
    Price aggressive_price(Side side) {
        const Price offset = rng_.uniform(0, cfg_.cross_depth);
        return clamp(side == Side::Buy ? mid_ + offset : mid_ - offset);
    }

    Op limit_op() {
        const Side side = random_side();
        const Price price = rng_.chance(cfg_.mix.cross) ? aggressive_price(side) : passive_price(side);
        const OrderId id = next_id_++;
        track({id, side, price});
        return Op{Op::Kind::Add, id, side, OrderType::Limit, price, random_qty()};
    }

    Op taker_op(OrderType type) {
        const Side side = random_side();
        const Price price = type == OrderType::Market ? 0 : aggressive_price(side);
        return Op{Op::Kind::Add, next_id_++, side, type, price, random_qty()};
    }

    // Cancels a remembered id. It may already be filled, in which case the
    // book answers UnknownId; that path gets exercised too.
    Op cancel_op() {
        const std::size_t i = static_cast<std::size_t>(rng_.uniform(0, static_cast<std::int64_t>(tracked_.size()) - 1));
        const Tracked t = tracked_[i];
        tracked_[i] = tracked_.back();
        tracked_.pop_back();
        return Op{Op::Kind::Cancel, t.id, t.side, OrderType::Limit, 0, 0};
    }

    // Half the time keep the price (a quantity change, which may or may not
    // keep priority depending on the order's current size), half the time
    // move the order to a new passive or aggressive price.
    Op modify_op() {
        const std::size_t i = static_cast<std::size_t>(rng_.uniform(0, static_cast<std::int64_t>(tracked_.size()) - 1));
        Tracked& t = tracked_[i];
        if (!rng_.chance(0.5)) {
            t.price = rng_.chance(cfg_.mix.cross) ? aggressive_price(t.side) : passive_price(t.side);
        }
        return Op{Op::Kind::Modify, t.id, t.side, OrderType::Limit, t.price, random_qty()};
    }

    Op invalid_op() {
        const Side side = random_side();
        switch (rng_.uniform(0, 5)) {
            case 0: return Op{Op::Kind::Add, next_id_++, side, OrderType::Limit, passive_price(side), 0};
            case 1: return Op{Op::Kind::Add, next_id_++, side, OrderType::Limit, 0, random_qty()};
            case 2: return Op{Op::Kind::Add, 0, side, OrderType::Limit, passive_price(side), random_qty()};
            case 3:  // re-use an id that may still be resting -> DuplicateId
                // (IOC, so that if the old order is already gone this valid
                // request cannot leave an order the generator does not track)
                if (!tracked_.empty()) {
                    const Tracked& t = tracked_[static_cast<std::size_t>(
                        rng_.uniform(0, static_cast<std::int64_t>(tracked_.size()) - 1))];
                    return Op{Op::Kind::Add, t.id, side, OrderType::IOC, passive_price(side), random_qty()};
                }
                return Op{Op::Kind::Cancel, next_id_ + 1'000'000, side, OrderType::Limit, 0, 0};
            case 4: return Op{Op::Kind::Cancel, next_id_ + 1'000'000, side, OrderType::Limit, 0, 0};
            default: return Op{Op::Kind::Modify, next_id_ + 1'000'000, side, OrderType::Limit, mid_, random_qty()};
        }
    }

    void track(const Tracked& t) { tracked_.push_back(t); }

    Config cfg_;
    Rng rng_;
    Price mid_;
    OrderId next_id_ = 1;
    std::vector<Tracked> tracked_;
};

}  // namespace lob::flow
