// Fixed-size object pool: hands out T* without a heap allocation per object.
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

namespace lob {

// Memory is allocated in chunks of chunk_size objects and never returned to
// the OS until the pool is destroyed. Released objects go on a free list and
// are handed out again (LIFO, so recently used, cache-warm memory first).
//
// Ownership: the pool owns every T it ever created. A pointer from acquire()
// stays valid until the pool is destroyed; after release() the caller must not
// use it any more. Chunks never move, so pointers are stable.
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t chunk_size = 4096) : chunk_size_(chunk_size) {}

    ObjectPool(const ObjectPool&) = delete;  // copying would duplicate ownership
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns a value-initialized T.
    T* acquire() {
        if (free_.empty()) {
            add_chunk();
        }
        T* object = free_.back();
        free_.pop_back();
        *object = T{};
        return object;
    }

    void release(T* object) { free_.push_back(object); }

    std::size_t capacity() const { return chunks_.size() * chunk_size_; }
    std::size_t in_use() const { return capacity() - free_.size(); }

private:
    void add_chunk() {
        chunks_.push_back(std::make_unique<T[]>(chunk_size_));
        T* chunk = chunks_.back().get();
        free_.reserve(capacity());
        // Push in reverse so the first acquire() returns chunk[0], then
        // chunk[1], ...: consecutive orders end up next to each other.
        for (std::size_t i = chunk_size_; i-- > 0;) {
            free_.push_back(&chunk[i]);
        }
    }

    std::size_t chunk_size_;
    std::vector<std::unique_ptr<T[]>> chunks_;
    std::vector<T*> free_;
};

}  // namespace lob
