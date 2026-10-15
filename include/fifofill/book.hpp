#pragma once

#include "fifofill/events.hpp"

#include <cstddef>
#include <vector>

namespace fifofill {

struct BookConfig {
    Price min_price;
    Price max_price;
    Price tick_size;
    std::size_t capacity;

    // Both helpers validate configuration before doing index arithmetic.
    std::size_t level_count() const;
    std::optional<std::size_t> price_index(Price price) const;
};

struct RestingOrder {
    OrderId id;
    Side side;
    Price price;
    Qty remaining;
    bool operator==(const RestingOrder&) const = default;
};

struct Snapshot {
    std::vector<RestingOrder> bids;
    std::vector<RestingOrder> asks;
    bool operator==(const Snapshot&) const = default;
};

} // namespace fifofill
