#pragma once

#include "fifofill/order.hpp"

namespace fifofill {

enum class CancelReason : std::uint8_t { Explicit, MarketRemainder };
enum class RejectReason : std::uint8_t {
    InvalidId,
    InvalidSide,
    InvalidQuantity,
    InvalidPrice,
    DuplicateId,
    CapacityExceeded,
    UnknownOrder
};

struct Accepted {
    Operation operation;
    OrderId id;
    Side side;
    std::optional<Price> price;
    Qty quantity;
    bool priority_retained;
    bool operator==(const Accepted&) const = default;
};

struct Trade {
    OrderId buyer_id;
    OrderId seller_id;
    Price price;
    Qty quantity;
    bool operator==(const Trade&) const = default;
};

struct Canceled {
    OrderId id;
    Qty quantity;
    CancelReason reason;
    bool operator==(const Canceled&) const = default;
};

struct Rejected {
    Operation operation;
    OrderId id;
    RejectReason reason;
    bool operator==(const Rejected&) const = default;
};

using Event = std::variant<Accepted, Trade, Canceled, Rejected>;

} // namespace fifofill
