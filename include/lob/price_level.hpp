// A resting order and the FIFO queue of orders at one price.
#pragma once

#include <cassert>
#include <cstddef>

#include "lob/types.hpp"

namespace lob {

struct PriceLevel;

// A resting order. The memory is owned by the book's ObjectPool; the order
// lives from the moment it rests until it is fully filled or cancelled.
//
// prev/next are the links of the intrusive FIFO list inside its PriceLevel.
// "Intrusive" means the links live inside the order itself, so putting an
// order in a queue needs no extra allocation, and removing it from the middle
// (a cancel) is O(1) once we have the pointer (from the id hash map).
struct Order {
    OrderId id = 0;
    Side side = Side::Buy;
    Price price = 0;
    Qty qty = 0;  // remaining (unfilled) quantity, always > 0 while resting

    Order* prev = nullptr;        // older order at the same price (nullptr = head)
    Order* next = nullptr;        // newer order at the same price (nullptr = tail)
    PriceLevel* level = nullptr;  // the level this order is queued in
};

// All resting orders at one price, oldest first. Does not own the orders.
//
// Invariants:
//   - head == nullptr  <=>  tail == nullptr  <=>  count == 0
//   - total_qty == sum of qty over the queue
struct PriceLevel {
    Price price = 0;
    Qty total_qty = 0;
    std::size_t count = 0;
    Order* head = nullptr;  // oldest: next to trade
    Order* tail = nullptr;  // newest

    bool empty() const { return head == nullptr; }

    // Appends at the back: a new order has the lowest time priority.
    void push_back(Order* order) {
        assert(order->price == price);
        order->prev = tail;
        order->next = nullptr;
        order->level = this;
        if (tail != nullptr) {
            tail->next = order;
        } else {
            head = order;
        }
        tail = order;
        total_qty += order->qty;
        ++count;
    }

    // Unlinks an order from anywhere in the queue in O(1).
    void erase(Order* order) {
        assert(order->level == this);
        if (order->prev != nullptr) {
            order->prev->next = order->next;
        } else {
            head = order->next;
        }
        if (order->next != nullptr) {
            order->next->prev = order->prev;
        } else {
            tail = order->prev;
        }
        total_qty -= order->qty;
        --count;
        order->prev = order->next = nullptr;
        order->level = nullptr;
    }

    // Lowers an order's quantity without touching its queue position. Used by
    // partial fills and by modify-down (which keeps time priority).
    void reduce(Order* order, Qty by) {
        assert(order->level == this && by > 0 && by < order->qty);
        order->qty -= by;
        total_qty -= by;
    }
};

}  // namespace lob
