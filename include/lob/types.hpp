// Basic vocabulary types shared by every part of the engine.
#pragma once

#include <cstdint>
#include <string_view>

namespace lob {

// Order ids are chosen by the client. 0 is reserved: the id hash map uses it
// to mark an empty slot, so the book rejects it.
using OrderId = std::uint64_t;

// Prices are integer ticks, never floating point. Two orders at "the same
// price" must compare exactly equal, and a double cannot promise that.
using Price = std::int64_t;

// Quantities are signed so that an accidental "0 - 1" is a visible negative
// number (and caught by validation) instead of wrapping to 2^64 - 1.
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side side) { return side == Side::Buy ? Side::Sell : Side::Buy; }

enum class OrderType : std::uint8_t {
    Limit,   // trade what crosses, rest the remainder on the book
    Market,  // trade at any price, discard the remainder
    IOC,     // immediate-or-cancel: like Limit, but discard the remainder
    FOK,     // fill-or-kill: trade the full quantity at once or do nothing
};

// Result code for every request. Anything other than Ok means the book was
// not changed and no events were emitted.
enum class Status : std::uint8_t {
    Ok,
    InvalidId,        // id == 0
    InvalidQty,       // qty <= 0
    InvalidPrice,     // price <= 0 on a priced order
    PriceOutOfRange,  // outside the tick range of a bounded (flat) book
    DuplicateId,      // id is already resting on the book
    UnknownId,        // cancel/modify of an id that is not resting
};

// What happened to one add/modify request. For an accepted request
// filled + resting + cancelled == requested quantity.
struct ExecReport {
    Status status = Status::Ok;
    Qty filled = 0;     // traded immediately against resting orders
    Qty resting = 0;    // left on the book (Limit orders only)
    Qty cancelled = 0;  // discarded (Market/IOC remainder, or a killed FOK)

    bool operator==(const ExecReport&) const = default;
};

constexpr std::string_view to_string(Status status) {
    switch (status) {
        case Status::Ok: return "Ok";
        case Status::InvalidId: return "InvalidId";
        case Status::InvalidQty: return "InvalidQty";
        case Status::InvalidPrice: return "InvalidPrice";
        case Status::PriceOutOfRange: return "PriceOutOfRange";
        case Status::DuplicateId: return "DuplicateId";
        case Status::UnknownId: return "UnknownId";
    }
    return "?";
}

constexpr std::string_view to_string(OrderType type) {
    switch (type) {
        case OrderType::Limit: return "Limit";
        case OrderType::Market: return "Market";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
    }
    return "?";
}

}  // namespace lob
