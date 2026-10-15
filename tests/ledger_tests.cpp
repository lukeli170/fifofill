#include "ledger.hpp"
#include "reference_book.hpp"

#include <gtest/gtest.h>
#include <limits>

using namespace fifofill;

TEST(Ledger, DetectsPerturbedMakerPriceAndFifoSnapshot) {
    for (const bool price_error : {false, true}) {
        ReferenceBook book{{1, 200, 1, 8}};
        Ledger ledger;
        for (OrderId id = 1; id <= 2; ++id) {
            const Command command = Add{id, Side::Sell, 100, 10};
            const auto events = book.process(command);
            ledger.observe(command, events, book.snapshot());
        }
        if (price_error) {
            const Command command = Add{3, Side::Buy, 101, 1};
            const auto view = book.process(command);
            std::vector<Event> events(view.begin(), view.end());
            std::get<Trade>(events[1]).price = 101;
            EXPECT_THROW(ledger.observe(command, events, book.snapshot()), std::logic_error);
        } else {
            const Command command = Modify{1, 100, 5};
            const auto events = book.process(command);
            auto state = book.snapshot();
            std::swap(state.asks[0], state.asks[1]);
            EXPECT_THROW(ledger.observe(command, events, state), std::logic_error);
        }
    }
}

TEST(Ledger, MultipleMaximumQuantitiesAndReusedLifetimes) {
    ReferenceBook book{{1, 100, 1, 4}};
    Ledger ledger;
    const Qty max = std::numeric_limits<Qty>::max();
    const std::vector<Command> commands{
        Add{1, Side::Buy, 50, max}, Add{2, Side::Buy, 50, max},
        Modify{1, 50, max - 1}, Modify{1, 50, max},
        Add{3, Side::Sell, std::nullopt, max}, Cancel{1},
        Add{1, Side::Buy, 50, max}, Cancel{1}, Cancel{1}};
    for (const auto& command : commands) {
        const auto events = book.process(command);
        EXPECT_NO_THROW(ledger.observe(command, events, book.snapshot()));
    }
    EXPECT_EQ(ledger.admitted(Side::Buy), std::uint64_t{max} * 3);
    EXPECT_EQ(book.snapshot(), Snapshot{});
}

TEST(Ledger, DetectsMarketCancellationWithUnconsumedLiquidity) {
    Ledger ledger;
    const Command maker = Add{1, Side::Sell, 100, 10};
    const std::vector<Event> accepted{Accepted{Operation::Add, 1, Side::Sell, 100, 10, false}};
    const Snapshot state{{}, {{1, Side::Sell, 100, 10}}};
    ledger.observe(maker, accepted, state);
    const Command market = Add{2, Side::Buy, std::nullopt, 10};
    const std::vector<Event> premature{Accepted{Operation::Add, 2, Side::Buy, std::nullopt, 10, false},
                                      Canceled{2, 10, CancelReason::MarketRemainder}};
    EXPECT_THROW(ledger.observe(market, premature, state), std::logic_error);
}
