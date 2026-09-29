// One side of the book, levels kept in a flat array indexed by price.
#pragma once

#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Price levels in a std::vector with one slot per tick in [min_price,
// max_price]. The level for price p is levels_[p - min_price].
//
// + finding a level is one subtraction; no allocation ever happens after
//   construction; neighbouring prices are neighbours in memory
// - prices outside the configured range are rejected, and memory is
//   proportional to the range, not to the number of live levels
// - when the best level empties we scan toward worse prices for the next
//   non-empty slot. That scan is short when the book is dense near the top,
//   which is the normal case, but O(range) in the worst case.
//
// The vector is sized once in the constructor and never resized, so the
// PriceLevel* stored in each Order stays valid for the lifetime of the book.
class FlatLevels {
public:
    struct Config {
        Price min_price = 1;
        Price max_price = 100'000;
    };

    FlatLevels(Side side, Config config) : side_(side), min_price_(config.min_price), max_price_(config.max_price) {
        if (min_price_ <= 0 || max_price_ < min_price_) {
            throw std::invalid_argument("FlatLevels: need 0 < min_price <= max_price");
        }
        levels_.resize(static_cast<std::size_t>(max_price_ - min_price_ + 1));
        for (std::size_t i = 0; i < levels_.size(); ++i) {
            levels_[i].price = min_price_ + static_cast<Price>(i);
        }
    }

    bool accepts_price(Price price) const { return price >= min_price_ && price <= max_price_; }
    std::size_t level_count() const { return non_empty_; }

    PriceLevel* best() { return non_empty_ == 0 ? nullptr : &levels_[best_]; }

    void insert(Order* order) {
        assert(accepts_price(order->price));
        std::size_t index = index_of(order->price);
        PriceLevel& level = levels_[index];
        if (level.empty()) {
            if (non_empty_ == 0 || is_better(index, best_)) {
                best_ = index;
            }
            ++non_empty_;
        }
        level.push_back(order);
    }

    void remove(Order* order) {
        PriceLevel* level = order->level;
        level->erase(order);
        if (!level->empty()) {
            return;
        }
        --non_empty_;
        std::size_t index = index_of(level->price);
        if (non_empty_ > 0 && index == best_) {
            // The best level just emptied: step toward worse prices until we
            // find a non-empty one. non_empty_ > 0 guarantees we will find it.
            do {
                best_ = worse(best_);
            } while (levels_[best_].empty());
        }
    }

    template <typename Fn>
    void for_each_from_best(Fn fn) const {
        std::size_t remaining = non_empty_;  // lets us stop without scanning to the end
        std::size_t index = best_;
        while (remaining > 0) {
            const PriceLevel& level = levels_[index];
            if (!level.empty()) {
                --remaining;
                if (!fn(level)) {
                    return;
                }
            }
            if (remaining > 0) {
                index = worse(index);
            }
        }
    }

private:
    std::size_t index_of(Price price) const { return static_cast<std::size_t>(price - min_price_); }

    // For bids a higher price (index) is better; for asks a lower one.
    bool is_better(std::size_t a, std::size_t b) const { return side_ == Side::Buy ? a > b : a < b; }
    std::size_t worse(std::size_t index) const { return side_ == Side::Buy ? index - 1 : index + 1; }

    Side side_;
    Price min_price_;
    Price max_price_;
    std::vector<PriceLevel> levels_;
    std::size_t best_ = 0;       // index of the best level; meaningful only if non_empty_ > 0
    std::size_t non_empty_ = 0;  // number of levels with at least one order
};

}  // namespace lob
