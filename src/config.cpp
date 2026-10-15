#include "fifofill/book.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace fifofill {

std::size_t BookConfig::level_count() const {
    if (min_price <= 0 || max_price < min_price || tick_size <= 0 || capacity == 0) {
        throw std::invalid_argument("invalid book range, tick size, or capacity");
    }
    if (min_price % tick_size != 0 || max_price % tick_size != 0) {
        throw std::invalid_argument("book endpoints must be tick aligned");
    }
    const auto count = static_cast<std::uint64_t>((max_price - min_price) / tick_size) + 1;
    if (!std::in_range<std::size_t>(count) ||
        capacity > std::numeric_limits<std::uint64_t>::max() / std::numeric_limits<Qty>::max() ||
        capacity > std::numeric_limits<std::size_t>::max() - 2 ||
        capacity + 2 > std::vector<Event>{}.max_size()) {
        throw std::length_error("book capacity or level count is unrepresentable");
    }
    return static_cast<std::size_t>(count);
}

std::optional<std::size_t> BookConfig::price_index(Price price) const {
    static_cast<void>(level_count());
    if (price < min_price || price > max_price || price % tick_size != 0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>((price - min_price) / tick_size);
}

} // namespace fifofill
