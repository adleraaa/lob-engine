// One side of the book (all bids or all asks), levels kept in a std::map.
#pragma once

#include <cstddef>
#include <iterator>
#include <limits>
#include <map>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// The map itself works for any positive price. The optional range exists so
// a map book can be configured with exactly the same validation rules as a
// flat book (the differential test and the benchmark do this); by default
// every positive price is accepted. Defined outside MapLevels because a
// nested struct with default member initializers cannot be used as a default
// argument inside its own class.
struct MapLevelsConfig {
    Price min_price = 1;
    Price max_price = std::numeric_limits<Price>::max();
};

// Price levels in a balanced tree keyed by price.
//
// + any price is allowed, memory is proportional to the number of live levels
// - inserting a new level is O(log L) and allocates a tree node; walking the
//   levels chases pointers across the heap
//
// std::map never moves its elements, so a PriceLevel* (stored in every Order)
// stays valid until that level is erased. We erase a level only when its queue
// is empty, i.e. when no Order points at it any more.
class MapLevels {
public:
    using Config = MapLevelsConfig;

    explicit MapLevels(Side side, Config config = {})
        : side_(side), min_price_(config.min_price), max_price_(config.max_price) {}

    bool accepts_price(Price price) const { return price >= min_price_ && price <= max_price_; }
    std::size_t level_count() const { return levels_.size(); }

    // Best level: highest bid or lowest ask. nullptr if this side is empty.
    PriceLevel* best() {
        if (levels_.empty()) {
            return nullptr;
        }
        // The map is sorted ascending, so the best bid is the last element.
        return side_ == Side::Buy ? &std::prev(levels_.end())->second : &levels_.begin()->second;
    }

    // Queues the order at the back of its price level, creating the level if
    // needed. The order's price must already be set.
    void insert(Order* order) {
        auto [it, inserted] = levels_.try_emplace(order->price);
        if (inserted) {
            it->second.price = order->price;
        }
        it->second.push_back(order);
    }

    // Unlinks the order and drops its level if that left the level empty.
    void remove(Order* order) {
        PriceLevel* level = order->level;
        level->erase(order);
        if (level->empty()) {
            levels_.erase(level->price);
        }
    }

    // Calls fn(const PriceLevel&) for each level from best to worst until fn
    // returns false.
    template <typename Fn>
    void for_each_from_best(Fn fn) const {
        if (side_ == Side::Buy) {
            for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
                if (!fn(it->second)) {
                    return;
                }
            }
        } else {
            for (auto it = levels_.begin(); it != levels_.end(); ++it) {
                if (!fn(it->second)) {
                    return;
                }
            }
        }
    }

private:
    Side side_;
    Price min_price_;
    Price max_price_;
    std::map<Price, PriceLevel> levels_;
};

}  // namespace lob
