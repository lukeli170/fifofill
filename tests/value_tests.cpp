#include "fifofill/book.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <type_traits>

using namespace fifofill;

TEST(Config, RangeTickAndCheckedIndices) {
    const BookConfig config{5, 20, 5, 10};
    EXPECT_EQ(config.level_count(), 4U);
    EXPECT_EQ(config.price_index(5), 0U);
    EXPECT_EQ(config.price_index(20), 3U);
    for (const Price price : {std::numeric_limits<Price>::min(), Price{-1}, Price{0},
                              Price{4}, Price{6}, Price{21}, std::numeric_limits<Price>::max()}) {
        EXPECT_FALSE(config.price_index(price)) << price;
    }
    EXPECT_EQ((BookConfig{10, 10, 5, 1}.level_count()), 1U);
}

TEST(Config, InvalidFieldsFailBeforeArithmetic) {
    for (const BookConfig config : {
             BookConfig{0, 100, 1, 1}, {std::numeric_limits<Price>::min(), 100, 1, 1},
             {100, 10, 1, 1}, {1, 100, 0, 1}, {1, 100, -1, 1}, {1, 100, 1, 0},
             {1, 100, 5, 1}, {5, 101, 5, 1}}) {
        EXPECT_THROW(config.level_count(), std::invalid_argument);
        EXPECT_THROW(config.price_index(std::numeric_limits<Price>::min()), std::invalid_argument);
    }
}

TEST(Config, ExtremePricesDoNotOverflowOrAllocateLevels) {
    const auto high = std::numeric_limits<Price>::max();
    EXPECT_EQ((BookConfig{high, high, high, 1}.price_index(high)), 0U);
    const BookConfig widest{1, high, 1, 1};
    if constexpr (sizeof(std::size_t) >= sizeof(Price)) {
        EXPECT_EQ(widest.level_count(), static_cast<std::size_t>(high));
        EXPECT_EQ(widest.price_index(high), static_cast<std::size_t>(high - 1));
    } else {
        EXPECT_THROW(widest.level_count(), std::length_error);
    }
}

TEST(Config, CapacityAndEventSizingCannotWrap) {
    const BookConfig size_overflow{1, 100, 1, std::numeric_limits<std::size_t>::max()};
    EXPECT_THROW(size_overflow.level_count(), std::length_error);
    const BookConfig event_overflow{1, 100, 1, std::vector<Event>{}.max_size()};
    EXPECT_THROW(event_overflow.level_count(), std::length_error);
    if constexpr (sizeof(std::size_t) >= sizeof(std::uint64_t)) {
        const auto limit = std::numeric_limits<std::uint64_t>::max() /
                           std::numeric_limits<Qty>::max();
        const BookConfig aggregate_overflow{1, 100, 1, static_cast<std::size_t>(limit + 1)};
        EXPECT_THROW(aggregate_overflow.level_count(), std::length_error);
    }
}

TEST(Values, EqualityComparesEveryEventFieldAndAlternative) {
    static_assert(std::is_trivially_copyable_v<Event>);
    const Event accepted = Accepted{Operation::Add, 1, Side::Buy, 100, 10, false};
    EXPECT_EQ(accepted, (Event{Accepted{Operation::Add, 1, Side::Buy, 100, 10, false}}));
    for (const Event changed : {
             Event{Accepted{Operation::Modify, 1, Side::Buy, 100, 10, false}},
             Event{Accepted{Operation::Add, 2, Side::Buy, 100, 10, false}},
             Event{Accepted{Operation::Add, 1, Side::Sell, 100, 10, false}},
             Event{Accepted{Operation::Add, 1, Side::Buy, std::nullopt, 10, false}},
             Event{Accepted{Operation::Add, 1, Side::Buy, 100, 11, false}},
             Event{Accepted{Operation::Add, 1, Side::Buy, 100, 10, true}},
             Event{Trade{1, 2, 100, 10}}}) {
        EXPECT_NE(accepted, changed);
    }
    const Event trade = Trade{1, 2, 100, 10};
    EXPECT_EQ(trade, (Event{Trade{1, 2, 100, 10}}));
    EXPECT_NE(trade, (Event{Trade{2, 2, 100, 10}}));
    EXPECT_NE(trade, (Event{Trade{1, 3, 100, 10}}));
    EXPECT_NE(trade, (Event{Trade{1, 2, 101, 10}}));
    EXPECT_NE(trade, (Event{Trade{1, 2, 100, 11}}));
    const Event canceled = Canceled{1, 10, CancelReason::Explicit};
    EXPECT_EQ(canceled, (Event{Canceled{1, 10, CancelReason::Explicit}}));
    EXPECT_NE(canceled, (Event{Canceled{2, 10, CancelReason::Explicit}}));
    EXPECT_NE(canceled, (Event{Canceled{1, 11, CancelReason::Explicit}}));
    EXPECT_NE(canceled, (Event{Canceled{1, 10, CancelReason::MarketRemainder}}));
    const Event rejected = Rejected{Operation::Add, 1, RejectReason::InvalidId};
    EXPECT_EQ(rejected, (Event{Rejected{Operation::Add, 1, RejectReason::InvalidId}}));
    EXPECT_NE(rejected, (Event{Rejected{Operation::Cancel, 1, RejectReason::InvalidId}}));
    EXPECT_NE(rejected, (Event{Rejected{Operation::Add, 2, RejectReason::InvalidId}}));
    EXPECT_NE(rejected, (Event{Rejected{Operation::Add, 1, RejectReason::InvalidSide}}));
}
