#pragma once

#include <cstdint>
#include <optional>
#include <variant>

namespace fifofill {

using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class Operation : std::uint8_t { Add, Cancel, Modify };

struct Add {
    OrderId id;
    Side side;
    std::optional<Price> price;
    Qty quantity;
    bool operator==(const Add&) const = default;
};

struct Cancel {
    OrderId id;
    bool operator==(const Cancel&) const = default;
};

struct Modify {
    OrderId id;
    Price price;
    Qty quantity;
    bool operator==(const Modify&) const = default;
};

using Command = std::variant<Add, Cancel, Modify>;

} // namespace fifofill
