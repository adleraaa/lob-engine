// Open-addressing hash map from OrderId to the resting Order.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "lob/price_level.hpp"
#include "lob/types.hpp"

namespace lob {

// Why not std::unordered_map? It allocates one heap node per insert, which
// is exactly the per-order allocation the object pool exists to avoid. This
// map keeps all slots in one flat array (linear probing), so a lookup is
// usually one cache miss and inserts do not allocate unless the table grows.
//
// Key 0 marks an empty slot, which is why OrderId 0 is invalid.
// Deletion uses "backward shift" instead of tombstones, so the table never
// fills up with dead slots under heavy add/cancel churn.
class OrderIdMap {
public:
    explicit OrderIdMap(std::size_t min_capacity = 1024) {
        std::size_t capacity = 16;
        while (capacity < min_capacity) {
            capacity *= 2;
        }
        slots_.resize(capacity);
        mask_ = capacity - 1;
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return slots_.size(); }

    // Grows the table now so that `count` ids fit without a later resize.
    void reserve(std::size_t count) {
        while (count * 2 > slots_.size()) {
            grow();
        }
    }

    // The slot where a lookup for `id` starts. Public only so the tests can
    // pick ids that collide on purpose.
    std::size_t home_slot(OrderId id) const { return home(id); }

    // nullptr if the id is not present.
    Order* find(OrderId id) const {
        assert(id != 0);
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            if (slots_[i].key == id) {
                return slots_[i].value;
            }
            if (slots_[i].key == 0) {
                return nullptr;
            }
        }
    }

    // Returns false (and changes nothing) if the id is already present.
    bool insert(OrderId id, Order* value) {
        assert(id != 0 && value != nullptr);
        // Keep the load factor <= 1/2 so probe sequences stay short.
        if ((size_ + 1) * 2 > slots_.size()) {
            grow();
        }
        std::size_t i = home(id);
        while (slots_[i].key != 0) {
            if (slots_[i].key == id) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{id, value};
        ++size_;
        return true;
    }

    // Returns false if the id was not present.
    bool erase(OrderId id) {
        assert(id != 0);
        std::size_t hole = home(id);
        while (slots_[hole].key != id) {
            if (slots_[hole].key == 0) {
                return false;
            }
            hole = (hole + 1) & mask_;
        }
        // Backward shift. Invariant of linear probing: every entry sits in its
        // home slot or after it, with no empty slot in between (a lookup stops
        // at the first empty slot). Emptying `hole` could break that for the
        // entries that follow it in the same cluster, so we walk the cluster
        // and move an entry back into the hole whenever that is legal.
        std::size_t next = hole;
        while (true) {
            next = (next + 1) & mask_;
            if (slots_[next].key == 0) {
                break;  // end of the cluster: nothing after this can be cut off
            }
            const std::size_t want = home(slots_[next].key);
            // Distances are measured forward from the entry's home, modulo the
            // table size (the "& mask_" makes wrap-around at the end of the
            // array work). The entry at `next` may move into the hole only if
            // the hole lies on its probe path, i.e. between its home and its
            // current slot: then a lookup starting at `want` reaches the hole
            // before `next`. If the hole is before its home (dist_to_hole >
            // dist_to_next after wrapping), moving it would hide it.
            const std::size_t dist_to_hole = (hole - want) & mask_;
            const std::size_t dist_to_next = (next - want) & mask_;
            if (dist_to_hole < dist_to_next) {
                slots_[hole] = slots_[next];
                hole = next;
            }
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

private:
    struct Slot {
        OrderId key = 0;  // 0 = empty
        Order* value = nullptr;
    };

    // Client ids are often sequential; the splitmix64 finalizer spreads them
    // over the whole table instead of filling one contiguous run of slots.
    std::size_t home(OrderId id) const {
        std::uint64_t x = id;
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }

    void grow() {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.size() * 2, Slot{});
        mask_ = slots_.size() - 1;
        size_ = 0;
        for (const Slot& slot : old) {
            if (slot.key != 0) {
                insert(slot.key, slot.value);
            }
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    std::size_t size_ = 0;
};

}  // namespace lob
