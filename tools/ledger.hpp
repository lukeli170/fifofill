#pragma once

#include "fifofill/book.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>

namespace fifofill {

// Event-driven accounting model; it never executes or generates matches.
class Ledger {
public:
    void observe(const Command& command, std::span<const Event> events, const Snapshot& state) {
        require(!events.empty(), "missing outcome");
        if (const auto* rejected = std::get_if<Rejected>(&events.front())) {
            const auto operation = std::holds_alternative<Add>(command) ? Operation::Add
                : std::holds_alternative<Modify>(command) ? Operation::Modify : Operation::Cancel;
            require(events.size() == 1 && rejected->operation == operation &&
                    rejected->id == std::visit([](auto c) { return c.id; }, command),
                    "invalid rejection envelope");
            verify(state);
            return;
        }
        OrderId incoming = 0;
        std::optional<Price> limit;
        if (const auto* accepted = std::get_if<Accepted>(&events.front())) {
            incoming = accepted->id;
            limit = accepted->price;
            require(accepted->quantity != 0, "zero accepted quantity");
            if (const auto* add = std::get_if<Add>(&command)) {
                require(*accepted == Accepted{Operation::Add, add->id, add->side, add->price,
                                              add->quantity, false}, "incorrect add acknowledgement");
                require(!live_.contains(incoming), "accepted duplicate lifetime");
                increment(admitted_[side(add->side)], add->quantity);
                live_.emplace(incoming, Entry{{incoming, add->side, add->price.value_or(0),
                                               add->quantity}, next_priority()});
            } else {
                const auto* modify = std::get_if<Modify>(&command);
                require(modify && live_.contains(incoming), "modify missing lifetime");
                auto& entry = live_.at(incoming);
                auto& order = entry.order;
                const bool retained = order.price == modify->price && modify->quantity <= order.remaining;
                require(*accepted == Accepted{Operation::Modify, modify->id, order.side,
                                              modify->price, modify->quantity, retained},
                        "incorrect modify acknowledgement");
                if (modify->quantity > order.remaining) {
                    increment(increased_[side(order.side)], modify->quantity - order.remaining);
                } else {
                    increment(decreased_[side(order.side)], order.remaining - modify->quantity);
                }
                if (!retained) entry.priority = next_priority();
                order.price = modify->price;
                order.remaining = modify->quantity;
            }
        } else {
            require(std::holds_alternative<Cancel>(command) && events.size() == 1,
                    "missing acceptance");
        }

        for (std::size_t i = incoming ? 1 : 0; i < events.size(); ++i) {
            if (const auto* trade = std::get_if<Trade>(&events[i])) {
                require(incoming != 0 && live_.contains(incoming), "trade without incoming order");
                const auto& taker = live_.at(incoming).order;
                const auto maker_id = taker.side == Side::Buy ? trade->seller_id : trade->buyer_id;
                require((taker.side == Side::Buy ? trade->buyer_id : trade->seller_id) == incoming &&
                        maker_id != incoming && live_.contains(maker_id), "invalid trade participants");
                const auto& maker = live_.at(maker_id).order;
                require(maker.side != taker.side && trade->price == maker.price && trade->quantity > 0 &&
                        trade->quantity == std::min(taker.remaining, maker.remaining),
                        "invalid maker price or trade quantity");
                require(!limit || (taker.side == Side::Buy ? maker.price <= *limit : maker.price >= *limit),
                        "trade outside incoming limit");
                for (const auto& [id, candidate] : live_) {
                    if (candidate.order.side != maker.side) continue;
                    const bool better = maker.side == Side::Buy ? candidate.order.price > maker.price
                                                               : candidate.order.price < maker.price;
                    require(!better && (candidate.order.price != maker.price ||
                            candidate.priority >= live_.at(maker_id).priority), "trade broke price/FIFO priority");
                }
                increment(volume_, trade->quantity);
                consume(incoming, trade->quantity);
                consume(maker_id, trade->quantity);
            } else if (const auto* canceled = std::get_if<Canceled>(&events[i])) {
                require(i + 1 == events.size() && live_.contains(canceled->id), "invalid cancel lifetime");
                const auto order = live_.at(canceled->id).order;
                require(canceled->quantity == order.remaining, "incorrect canceled quantity");
                if (incoming) {
                    require(!limit && canceled->id == incoming && canceled->reason == CancelReason::MarketRemainder,
                            "invalid market remainder");
                    for (const auto& [id, entry] : live_)
                        require(entry.order.side == order.side, "market canceled before consuming liquidity");
                } else {
                    require(canceled->id == std::get<Cancel>(command).id &&
                            canceled->reason == CancelReason::Explicit, "invalid explicit cancel");
                }
                increment(canceled_[side(order.side)], canceled->quantity);
                live_.erase(canceled->id);
            } else {
                require(false, "unexpected event order");
            }
        }
        if (incoming && !limit) require(!live_.contains(incoming), "market order rested");
        verify(state);
    }

    std::uint64_t admitted(Side value) const { return admitted_[side(value)]; }

private:
    struct Entry { RestingOrder order; std::uint64_t priority; };
    std::map<OrderId, Entry> live_;
    std::array<std::uint64_t, 2> admitted_{}, increased_{}, decreased_{}, canceled_{};
    std::uint64_t volume_ = 0, priority_ = 0;

    static void require(bool condition, const char* message) {
        if (!condition) throw std::logic_error(message);
    }
    static std::size_t side(Side value) { return value == Side::Buy ? 0 : 1; }
    static void increment(std::uint64_t& total, std::uint64_t value) {
        if (value > std::numeric_limits<std::uint64_t>::max() - total)
            throw std::overflow_error("quantity ledger overflow");
        total += value;
    }
    std::uint64_t next_priority() { increment(priority_, 1); return priority_; }
    void consume(OrderId id, Qty quantity) {
        auto& order = live_.at(id).order;
        order.remaining -= quantity;
        if (order.remaining == 0) live_.erase(id);
    }
    void verify(const Snapshot& state) const {
        Snapshot expected;
        std::array<std::uint64_t, 2> remaining{};
        for (const auto& [id, entry] : live_) {
            const auto& order = entry.order;
            require(order.id != 0 && order.remaining != 0 && order.price > 0, "invalid resting lifetime");
            (order.side == Side::Buy ? expected.bids : expected.asks).push_back(order);
            increment(remaining[side(order.side)], order.remaining);
        }
        for (auto* orders : {&expected.bids, &expected.asks}) {
            std::sort(orders->begin(), orders->end(), [&](const auto& a, const auto& b) {
                if (a.price != b.price) return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
                return live_.at(a.id).priority < live_.at(b.id).priority;
            });
        }
        require(expected == state, "snapshot differs from quantity/FIFO ledger");
        require(state.bids.empty() || state.asks.empty() || state.bids.front().price < state.asks.front().price,
                "crossed snapshot");
        // Per-side A + I = L + D + X + V avoids multiplying a potentially large V.
        for (std::size_t s = 0; s < 2; ++s) {
            auto left = admitted_[s];
            increment(left, increased_[s]);
            auto right = remaining[s];
            increment(right, decreased_[s]);
            increment(right, canceled_[s]);
            increment(right, volume_);
            require(left == right, "quantity conservation failed");
        }
    }
};

} // namespace fifofill
