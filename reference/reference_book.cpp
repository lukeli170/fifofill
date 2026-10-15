#include "reference_book.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace fifofill {

ReferenceBook::ReferenceBook(BookConfig config) : config_(config) {
    static_cast<void>(config_.level_count());
    events_.reserve(config_.capacity + 2);
}

std::span<const Event> ReferenceBook::process(const Command& command) {
    events_.clear();
    std::visit([this](const auto& value) { execute(value); }, command);
#ifndef NDEBUG
    check_invariants();
#endif
    return events_;
}

void ReferenceBook::reject(Operation operation, OrderId id, RejectReason reason) {
    events_.push_back(Rejected{operation, id, reason});
}

void ReferenceBook::execute(const Add& command) {
    if (command.id == 0) {
        return reject(Operation::Add, command.id, RejectReason::InvalidId);
    }
    if (command.side != Side::Buy && command.side != Side::Sell) {
        return reject(Operation::Add, command.id, RejectReason::InvalidSide);
    }
    if (command.quantity == 0) {
        return reject(Operation::Add, command.id, RejectReason::InvalidQuantity);
    }
    if (command.price && !config_.price_index(*command.price)) {
        return reject(Operation::Add, command.id, RejectReason::InvalidPrice);
    }
    if (ids_.contains(command.id)) {
        return reject(Operation::Add, command.id, RejectReason::DuplicateId);
    }
    if (command.price && ids_.size() == config_.capacity) {
        return reject(Operation::Add, command.id, RejectReason::CapacityExceeded);
    }
    events_.push_back(Accepted{Operation::Add, command.id, command.side,
                               command.price, command.quantity, false});
    match(command);
}

void ReferenceBook::match(Add incoming) {
    auto& opposite = incoming.side == Side::Buy ? asks_ : bids_;
    while (incoming.quantity != 0 && !opposite.empty()) {
        const auto level = incoming.side == Side::Buy ? opposite.begin() : std::prev(opposite.end());
        if (incoming.price && (incoming.side == Side::Buy ? level->first > *incoming.price
                                                        : level->first < *incoming.price)) {
            break;
        }
        auto& maker = level->second.front();
        const Qty quantity = std::min(incoming.quantity, maker.remaining);
        const OrderId buyer = incoming.side == Side::Buy ? incoming.id : maker.id;
        const OrderId seller = incoming.side == Side::Sell ? incoming.id : maker.id;
        events_.push_back(Trade{buyer, seller, maker.price, quantity});
        incoming.quantity -= quantity;
        maker.remaining -= quantity;
        if (maker.remaining == 0) {
            erase({level, level->second.begin()});
        }
    }
    if (incoming.quantity == 0) {
        return;
    }
    if (!incoming.price) {
        events_.push_back(Canceled{incoming.id, incoming.quantity, CancelReason::MarketRemainder});
        return;
    }
    auto& own = incoming.side == Side::Buy ? bids_ : asks_;
    const auto level = own.try_emplace(*incoming.price).first;
    level->second.push_back({incoming.id, incoming.side, *incoming.price, incoming.quantity});
    ids_.emplace(incoming.id, Handle{level, std::prev(level->second.end())});
}

void ReferenceBook::erase(Handle handle) {
    auto& levels = handle.order->side == Side::Buy ? bids_ : asks_;
    ids_.erase(handle.order->id);
    handle.level->second.erase(handle.order);
    if (handle.level->second.empty()) {
        levels.erase(handle.level);
    }
}

void ReferenceBook::execute(const Cancel& command) {
    if (command.id == 0) {
        return reject(Operation::Cancel, command.id, RejectReason::InvalidId);
    }
    const auto found = ids_.find(command.id);
    if (found == ids_.end()) {
        return reject(Operation::Cancel, command.id, RejectReason::UnknownOrder);
    }
    events_.push_back(Canceled{command.id, found->second.order->remaining, CancelReason::Explicit});
    erase(found->second);
}

void ReferenceBook::execute(const Modify& command) {
    if (command.id == 0) {
        return reject(Operation::Modify, command.id, RejectReason::InvalidId);
    }
    const auto found = ids_.find(command.id);
    if (found == ids_.end()) {
        return reject(Operation::Modify, command.id, RejectReason::UnknownOrder);
    }
    if (command.quantity == 0) {
        return reject(Operation::Modify, command.id, RejectReason::InvalidQuantity);
    }
    if (!config_.price_index(command.price)) {
        return reject(Operation::Modify, command.id, RejectReason::InvalidPrice);
    }
    const auto old = *found->second.order;
    const bool retained = command.price == old.price && command.quantity <= old.remaining;
    events_.push_back(Accepted{Operation::Modify, command.id, old.side,
                               command.price, command.quantity, retained});
    if (retained) {
        found->second.order->remaining = command.quantity;
    } else {
        erase(found->second);
        match({command.id, old.side, command.price, command.quantity});
    }
}

Snapshot ReferenceBook::snapshot() const {
    Snapshot result;
    for (auto level = bids_.rbegin(); level != bids_.rend(); ++level) {
        result.bids.insert(result.bids.end(), level->second.begin(), level->second.end());
    }
    for (const auto& [price, queue] : asks_) {
        result.asks.insert(result.asks.end(), queue.begin(), queue.end());
    }
    return result;
}

void ReferenceBook::check_invariants() const {
    std::size_t count = 0;
    std::uint64_t quantity = 0;
    const auto inspect = [&](const Levels& levels, Side side) {
        for (auto level = levels.begin(); level != levels.end(); ++level) {
            if (level->second.empty() || !config_.price_index(level->first)) {
                throw std::logic_error("empty or invalid price level");
            }
            for (const auto& order : level->second) {
                if (order.id == 0 || order.side != side || order.price != level->first ||
                    order.remaining == 0) {
                    throw std::logic_error("invalid resting order");
                }
                const auto found = ids_.find(order.id);
                if (found == ids_.end() || found->second.level != level ||
                    &*found->second.order != &order) {
                    throw std::logic_error("ID lookup disagrees with queue");
                }
                if (count == config_.capacity ||
                    quantity > std::numeric_limits<std::uint64_t>::max() - order.remaining) {
                    throw std::logic_error("book count or quantity overflow");
                }
                ++count;
                quantity += order.remaining;
            }
        }
    };
    inspect(bids_, Side::Buy);
    inspect(asks_, Side::Sell);
    if (count != ids_.size()) {
        throw std::logic_error("ID lookup size disagrees with queues");
    }
    if (!bids_.empty() && !asks_.empty() && bids_.rbegin()->first >= asks_.begin()->first) {
        throw std::logic_error("crossed book");
    }
}

} // namespace fifofill
