#include "reference_book.hpp"
#include "ledger.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <vector>

using namespace fifofill;

namespace {

void expect_events(auto& book, const Command& command,
                   std::initializer_list<Event> expected) {
    const auto before = book.snapshot();
    const auto events = book.process(command);
    EXPECT_EQ((std::vector<Event>{events.begin(), events.end()}), std::vector<Event>(expected));
    EXPECT_NO_THROW(book.check_invariants());
    if (expected.size() == 1 && std::holds_alternative<Rejected>(*expected.begin())) {
        EXPECT_EQ(book.snapshot(), before);
    }
}

void rest(auto& book, OrderId id, Side side, Price price, Qty quantity) {
    expect_events(book, Add{id, side, price, quantity},
                  {Accepted{Operation::Add, id, side, price, quantity, false}});
}

void expect_book(const auto& book, std::initializer_list<RestingOrder> bids,
                 std::initializer_list<RestingOrder> asks) {
    EXPECT_EQ(book.snapshot(), (Snapshot{bids, asks}));
}

template<class Engine>
class CheckedBook {
public:
    explicit CheckedBook(BookConfig config) : engine_(config) {}
    std::span<const Event> process(const Command& command) {
        const auto events = engine_.process(command);
        ledger_.observe(command, events, engine_.snapshot());
        return events;
    }
    Snapshot snapshot() const { return engine_.snapshot(); }
    void check_invariants() const { engine_.check_invariants(); }
private:
    Engine engine_;
    Ledger ledger_;
};

template<class Book>
class BookTest : public testing::Test {
protected:
    Book book{{1, 1000, 1, 8}};

    void brief_setup() {
        rest(book, 1, Side::Sell, 100, 100);
        rest(book, 2, Side::Sell, 100, 50);
        rest(book, 3, Side::Sell, 101, 200);
        rest(book, 4, Side::Buy, 99, 300);
    }
};

using Books = testing::Types<CheckedBook<ReferenceBook>>;
TYPED_TEST_SUITE(BookTest, Books);

TYPED_TEST(BookTest, BriefSweepUsesMakerPriceAndLeavesPartialMaker) {
    auto& book = this->book;
    this->brief_setup();
    expect_events(book, Add{5, Side::Buy, 101, 220}, {
        Accepted{Operation::Add, 5, Side::Buy, 101, 220, false},
        Trade{5, 1, 100, 100}, Trade{5, 2, 100, 50}, Trade{5, 3, 101, 70}});
    expect_book(book, {{4, Side::Buy, 99, 300}}, {{3, Side::Sell, 101, 130}});
    expect_events(book, Cancel{1}, {Rejected{Operation::Cancel, 1, RejectReason::UnknownOrder}});
    rest(book, 1, Side::Buy, 98, 1);
}

TYPED_TEST(BookTest, BriefLimitRemainderRestsAtSubmittedPrice) {
    auto& book = this->book;
    this->brief_setup();
    expect_events(book, Add{5, Side::Buy, 100, 400}, {
        Accepted{Operation::Add, 5, Side::Buy, 100, 400, false},
        Trade{5, 1, 100, 100}, Trade{5, 2, 100, 50}});
    expect_book(book, {{5, Side::Buy, 100, 250}, {4, Side::Buy, 99, 300}},
                {{3, Side::Sell, 101, 200}});
}

TYPED_TEST(BookTest, SellSweepUsesHighestBidAndFifo) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 101, 100);
    rest(book, 2, Side::Buy, 101, 50);
    rest(book, 3, Side::Buy, 100, 200);
    rest(book, 4, Side::Sell, 102, 300);
    expect_events(book, Add{5, Side::Sell, 99, 220}, {
        Accepted{Operation::Add, 5, Side::Sell, 99, 220, false},
        Trade{1, 5, 101, 100}, Trade{2, 5, 101, 50}, Trade{3, 5, 100, 70}});
    expect_book(book, {{3, Side::Buy, 100, 130}}, {{4, Side::Sell, 102, 300}});
}

TYPED_TEST(BookTest, SellLimitStopsAndRestsAtTail) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 99, 10);
    rest(book, 2, Side::Buy, 100, 10);
    expect_events(book, Add{3, Side::Sell, 100, 15}, {
        Accepted{Operation::Add, 3, Side::Sell, 100, 15, false}, Trade{2, 3, 100, 10}});
    rest(book, 4, Side::Sell, 100, 5);
    expect_book(book, {{1, Side::Buy, 99, 10}},
                {{3, Side::Sell, 100, 5}, {4, Side::Sell, 100, 5}});
}

TYPED_TEST(BookTest, Matching_PartialMakersKeepPriorityOnBothSides) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        TypeParam book{{1, 1000, 1, 8}};
        const auto aggressor = side == Side::Buy ? Side::Sell : Side::Buy;
        rest(book, 1, side, 100, 10);
        rest(book, 2, side, 100, 10);
        const auto trade = [&](OrderId maker, OrderId taker, Qty quantity) -> Event {
            return side == Side::Buy ? Trade{maker, taker, 100, quantity}
                                     : Trade{taker, maker, 100, quantity};
        };
        expect_events(book, Add{3, aggressor, 100, 4}, {
            Accepted{Operation::Add, 3, aggressor, 100, 4, false}, trade(1, 3, 4)});
        expect_events(book, Add{4, aggressor, 100, 8}, {
            Accepted{Operation::Add, 4, aggressor, 100, 8, false},
            trade(1, 4, 6), trade(2, 4, 2)});
        const Snapshot expected = side == Side::Buy
            ? Snapshot{{{2, side, 100, 8}}, {}} : Snapshot{{}, {{2, side, 100, 8}}};
        EXPECT_EQ(book.snapshot(), expected);
    }
}

TYPED_TEST(BookTest, Matching_EmptyMarketsAcceptAndCancelOnBothSides) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        TypeParam book{{1, 1000, 1, 8}};
        expect_events(book, Add{1, side, std::nullopt, 10}, {
            Accepted{Operation::Add, 1, side, std::nullopt, 10, false},
            Canceled{1, 10, CancelReason::MarketRemainder}});
        expect_book(book, {}, {});
        rest(book, 1, side, 100, 1);
    }
}

TYPED_TEST(BookTest, Matching_MarketsSweepBestFirstAndCancelOnlyRemainder) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        TypeParam book{{1, 1000, 1, 8}};
        const auto maker_side = side == Side::Buy ? Side::Sell : Side::Buy;
        const Price best = side == Side::Buy ? 99 : 101;
        rest(book, 1, maker_side, best, 10);
        rest(book, 2, maker_side, 100, 5);
        const Event first = side == Side::Buy ? Trade{3, 1, best, 10} : Trade{1, 3, best, 10};
        const Event second = side == Side::Buy ? Trade{3, 2, 100, 5} : Trade{2, 3, 100, 5};
        expect_events(book, Add{3, side, std::nullopt, 20}, {
            Accepted{Operation::Add, 3, side, std::nullopt, 20, false},
            first, second, Canceled{3, 5, CancelReason::MarketRemainder}});
        expect_book(book, {}, {});
        rest(book, 1, maker_side, 100, 5);
        const Event full = side == Side::Buy ? Trade{3, 1, 100, 5} : Trade{1, 3, 100, 5};
        expect_events(book, Add{3, side, std::nullopt, 5}, {
            Accepted{Operation::Add, 3, side, std::nullopt, 5, false}, full});
        expect_book(book, {}, {});
    }
}

TYPED_TEST(BookTest, AddRejectionPrecedenceAndAtomicity) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 100, 10);
    const auto invalid = static_cast<Side>(255);
    expect_events(book, Add{0, invalid, 0, 0}, {Rejected{Operation::Add, 0, RejectReason::InvalidId}});
    expect_events(book, Add{1, invalid, 0, 0}, {Rejected{Operation::Add, 1, RejectReason::InvalidSide}});
    expect_events(book, Add{1, Side::Sell, 0, 0}, {Rejected{Operation::Add, 1, RejectReason::InvalidQuantity}});
    for (const Price price : {std::numeric_limits<Price>::min(), Price{-1}, Price{0},
                              Price{1001}, std::numeric_limits<Price>::max()}) {
        expect_events(book, Add{1, Side::Sell, price, 1},
                      {Rejected{Operation::Add, 1, RejectReason::InvalidPrice}});
    }
    expect_events(book, Add{1, Side::Sell, 100, 1},
                  {Rejected{Operation::Add, 1, RejectReason::DuplicateId}});
    expect_events(book, Add{1, Side::Sell, std::nullopt, 1},
                  {Rejected{Operation::Add, 1, RejectReason::DuplicateId}});
}

TYPED_TEST(BookTest, Validation_TickAndRangeBoundariesApplyToAddAndModify) {
    TypeParam book{{5, 20, 5, 8}};
    rest(book, 1, Side::Buy, 5, 1);
    rest(book, 2, Side::Sell, 20, 1);
    for (const Price price : {Price{4}, Price{6}, Price{19}, Price{21}}) {
        expect_events(book, Add{3, Side::Buy, price, 1},
                      {Rejected{Operation::Add, 3, RejectReason::InvalidPrice}});
        expect_events(book, Modify{1, price, 1},
                      {Rejected{Operation::Modify, 1, RejectReason::InvalidPrice}});
    }
    expect_book(book, {{1, Side::Buy, 5, 1}}, {{2, Side::Sell, 20, 1}});
}

TYPED_TEST(BookTest, Validation_MaximumIdsQuantitiesAndPricesAreValid) {
    const auto id = std::numeric_limits<OrderId>::max();
    const auto quantity = std::numeric_limits<Qty>::max();
    const auto price = std::numeric_limits<Price>::max();
    TypeParam book{{price, price, price, 8}};
    rest(book, id, Side::Sell, price, quantity);
    rest(book, id - 1, Side::Sell, price, quantity);
    expect_events(book, Add{1, Side::Buy, price, quantity}, {
        Accepted{Operation::Add, 1, Side::Buy, price, quantity, false}, Trade{1, id, price, quantity}});
    expect_book(book, {}, {{id - 1, Side::Sell, price, quantity}});
    rest(book, id, Side::Sell, price, 1);
}

TYPED_TEST(BookTest, Cancel_HeadMiddleTailAndSoleOrderOnBothSides) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        TypeParam book{{1, 1000, 1, 8}};
        for (OrderId id = 1; id <= 4; ++id) {
            rest(book, id, side, 100, static_cast<Qty>(id));
        }
        for (const OrderId id : {OrderId{2}, OrderId{1}, OrderId{4}, OrderId{3}}) {
            expect_events(book, Cancel{id}, {Canceled{id, static_cast<Qty>(id), CancelReason::Explicit}});
            std::vector<RestingOrder> expected;
            if (id == 2) expected = {{1, side, 100, 1}, {3, side, 100, 3}, {4, side, 100, 4}};
            if (id == 1) expected = {{3, side, 100, 3}, {4, side, 100, 4}};
            if (id == 4) expected = {{3, side, 100, 3}};
            const auto state = book.snapshot();
            EXPECT_EQ(side == Side::Buy ? state.bids : state.asks, expected);
        }
        expect_book(book, {}, {});
        rest(book, 1, side, 100, 10);
    }
}

TYPED_TEST(BookTest, CancelRejectsZeroUnknownFilledAndCanceledIds) {
    auto& book = this->book;
    expect_events(book, Cancel{0}, {Rejected{Operation::Cancel, 0, RejectReason::InvalidId}});
    expect_events(book, Cancel{99}, {Rejected{Operation::Cancel, 99, RejectReason::UnknownOrder}});
    rest(book, 1, Side::Sell, 100, 2);
    expect_events(book, Add{2, Side::Buy, 100, 2}, {
        Accepted{Operation::Add, 2, Side::Buy, 100, 2, false}, Trade{2, 1, 100, 2}});
    for (const OrderId id : {OrderId{1}, OrderId{2}}) {
        expect_events(book, Cancel{id}, {Rejected{Operation::Cancel, id, RejectReason::UnknownOrder}});
    }
    rest(book, 1, Side::Buy, 99, 3);
    expect_events(book, Cancel{1}, {Canceled{1, 3, CancelReason::Explicit}});
    expect_events(book, Cancel{1}, {Rejected{Operation::Cancel, 1, RejectReason::UnknownOrder}});
    rest(book, 1, Side::Buy, 99, 4);
}

TYPED_TEST(BookTest, Modify_SamePriceShrinkNoopAndIncreaseHaveObservablePriority) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const Qty quantity : {Qty{5}, Qty{10}, Qty{15}}) {
            TypeParam book{{1, 1000, 1, 8}};
            rest(book, 1, side, 100, 10);
            rest(book, 2, side, 100, 10);
            const bool retained = quantity <= 10;
            expect_events(book, Modify{1, 100, quantity}, {
                Accepted{Operation::Modify, 1, side, 100, quantity, retained}});
            const RestingOrder first{retained ? 1U : 2U, side, 100, retained ? quantity : 10U};
            const RestingOrder second{retained ? 2U : 1U, side, 100, retained ? 10U : quantity};
            const Snapshot expected = side == Side::Buy
                ? Snapshot{{first, second}, {}} : Snapshot{{}, {first, second}};
            EXPECT_EQ(book.snapshot(), expected);
            const auto other = side == Side::Buy ? Side::Sell : Side::Buy;
            const Event first_trade = side == Side::Buy ? Trade{first.id, 3, 100, first.remaining}
                                                       : Trade{3, first.id, 100, first.remaining};
            const Event second_trade = side == Side::Buy ? Trade{second.id, 3, 100, 1}
                                                        : Trade{3, second.id, 100, 1};
            expect_events(book, Add{3, other, 100, first.remaining + 1}, {
                Accepted{Operation::Add, 3, other, 100, first.remaining + 1, false},
                first_trade, second_trade});
        }
    }
}

TYPED_TEST(BookTest, Modify_EveryRepriceLosesPriorityRegardlessOfSize) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const Price price : {Price{99}, Price{101}}) {
            for (const Qty quantity : {Qty{5}, Qty{10}, Qty{15}}) {
                TypeParam book{{1, 1000, 1, 8}};
                rest(book, 1, side, 100, 10);
                rest(book, 2, side, price, 10);
                expect_events(book, Modify{1, price, quantity}, {
                    Accepted{Operation::Modify, 1, side, price, quantity, false}});
                const std::vector<RestingOrder> queue{{2, side, price, 10}, {1, side, price, quantity}};
                const auto state = book.snapshot();
                EXPECT_EQ(side == Side::Buy ? state.bids : state.asks, queue);
                EXPECT_TRUE(side == Side::Buy ? state.asks.empty() : state.bids.empty());
            }
        }
    }
}

TYPED_TEST(BookTest, ModifyComparesRemainingRatherThanOriginalSize) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 100, 100);
    rest(book, 2, Side::Buy, 100, 100);
    expect_events(book, Add{3, Side::Sell, 100, 40}, {
        Accepted{Operation::Add, 3, Side::Sell, 100, 40, false}, Trade{1, 3, 100, 40}});
    expect_events(book, Modify{1, 100, 80}, {Accepted{Operation::Modify, 1, Side::Buy, 100, 80, false}});
    expect_book(book, {{2, Side::Buy, 100, 100}, {1, Side::Buy, 100, 80}}, {});
}

TYPED_TEST(BookTest, Modify_CrossingReplacementsMatchImmediatelyOnBothSides) {
    for (const auto side : {Side::Buy, Side::Sell}) {
        for (const Qty quantity : {Qty{5}, Qty{10}}) {
            TypeParam book{{1, 1000, 1, 2}};
            const auto other = side == Side::Buy ? Side::Sell : Side::Buy;
            const Price old = side == Side::Buy ? 99 : 103;
            const Price maker = 101;
            const Price replacement = side == Side::Buy ? 102 : 100;
            rest(book, 1, side, old, 10);
            rest(book, 2, other, maker, 5);
            const Event trade = side == Side::Buy ? Trade{1, 2, maker, 5} : Trade{2, 1, maker, 5};
            expect_events(book, Modify{1, replacement, quantity}, {
                Accepted{Operation::Modify, 1, side, replacement, quantity, false}, trade});
            if (quantity == 5) {
                expect_book(book, {}, {});
                expect_events(book, Modify{1, replacement, 1},
                              {Rejected{Operation::Modify, 1, RejectReason::UnknownOrder}});
            } else {
                const RestingOrder remainder{1, side, replacement, 5};
                const Snapshot expected = side == Side::Buy
                    ? Snapshot{{remainder}, {}} : Snapshot{{}, {remainder}};
                EXPECT_EQ(book.snapshot(), expected);
            }
        }
    }
}

TYPED_TEST(BookTest, InvalidModifiesPreserveQuantityPriceAndQueuePosition) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 100, 10);
    rest(book, 2, Side::Buy, 100, 20);
    rest(book, 3, Side::Sell, 101, 30);
    expect_events(book, Modify{0, 0, 0}, {Rejected{Operation::Modify, 0, RejectReason::InvalidId}});
    expect_events(book, Modify{99, 0, 0}, {Rejected{Operation::Modify, 99, RejectReason::UnknownOrder}});
    expect_events(book, Modify{1, 0, 0}, {Rejected{Operation::Modify, 1, RejectReason::InvalidQuantity}});
    for (const Price price : {std::numeric_limits<Price>::min(), Price{0}, Price{1001}}) {
        expect_events(book, Modify{1, price, 10}, {Rejected{Operation::Modify, 1, RejectReason::InvalidPrice}});
    }
    expect_events(book, Cancel{3}, {Canceled{3, 30, CancelReason::Explicit}});
    expect_events(book, Modify{3, 0, 0}, {Rejected{Operation::Modify, 3, RejectReason::UnknownOrder}});
}

TYPED_TEST(BookTest, Capacity_LimitsRejectBeforeMatchingMarketsExecuteAndCancelFreesSlot) {
    TypeParam book{{1, 1000, 1, 2}};
    rest(book, 1, Side::Buy, 99, 10);
    rest(book, 2, Side::Sell, 101, 10);
    expect_events(book, Add{3, Side::Buy, 102, 10},
                  {Rejected{Operation::Add, 3, RejectReason::CapacityExceeded}});
    expect_events(book, Add{1, Side::Buy, 102, 10},
                  {Rejected{Operation::Add, 1, RejectReason::DuplicateId}});
    expect_events(book, Add{3, Side::Buy, std::nullopt, 5}, {
        Accepted{Operation::Add, 3, Side::Buy, std::nullopt, 5, false}, Trade{3, 2, 101, 5}});
    expect_events(book, Modify{1, 99, 20}, {Accepted{Operation::Modify, 1, Side::Buy, 99, 20, false}});
    expect_events(book, Modify{1, 99, 10}, {Accepted{Operation::Modify, 1, Side::Buy, 99, 10, true}});
    expect_events(book, Cancel{1}, {Canceled{1, 10, CancelReason::Explicit}});
    rest(book, 3, Side::Buy, 99, 2);
    expect_book(book, {{3, Side::Buy, 99, 2}}, {{2, Side::Sell, 101, 5}});
}

TYPED_TEST(BookTest, Capacity_EventBufferHandlesCapacityPlusTwoEvents) {
    TypeParam book{{1, 1000, 1, 3}};
    rest(book, 1, Side::Sell, 100, 1);
    rest(book, 2, Side::Sell, 100, 1);
    rest(book, 3, Side::Sell, 101, 1);
    expect_events(book, Add{4, Side::Buy, std::nullopt, 4}, {
        Accepted{Operation::Add, 4, Side::Buy, std::nullopt, 4, false},
        Trade{4, 1, 100, 1}, Trade{4, 2, 100, 1}, Trade{4, 3, 101, 1},
        Canceled{4, 1, CancelReason::MarketRemainder}});
    expect_book(book, {}, {});
}

TYPED_TEST(BookTest, SnapshotsOwnValuesAcrossLaterProcessing) {
    auto& book = this->book;
    rest(book, 1, Side::Buy, 100, 10);
    const auto saved = book.snapshot();
    const auto events = book.process(Modify{1, 100, 5});
    const std::vector<Event> copied(events.begin(), events.end());
    expect_events(book, Cancel{1}, {Canceled{1, 5, CancelReason::Explicit}});
    EXPECT_EQ(saved, (Snapshot{{{1, Side::Buy, 100, 10}}, {}}));
    EXPECT_EQ(copied, (std::vector<Event>{Accepted{Operation::Modify, 1, Side::Buy, 100, 5, true}}));
    expect_book(book, {}, {});
}

} // namespace
