#pragma once

#include "fifofill/book.hpp"

#include <list>
#include <map>
#include <span>
#include <unordered_map>

namespace fifofill {

class ReferenceBook {
public:
    explicit ReferenceBook(BookConfig config);
    ReferenceBook(const ReferenceBook&) = delete;
    ReferenceBook& operator=(const ReferenceBook&) = delete;
    ReferenceBook(ReferenceBook&&) = delete;
    ReferenceBook& operator=(ReferenceBook&&) = delete;

    // The returned view expires on the next process call or destruction.
    std::span<const Event> process(const Command& command);
    Snapshot snapshot() const;
    void check_invariants() const;

private:
    using Queue = std::list<RestingOrder>;
    using Levels = std::map<Price, Queue>;
    struct Handle {
        Levels::iterator level;
        Queue::iterator order;
    };

    void execute(const Add& command);
    void execute(const Cancel& command);
    void execute(const Modify& command);
    void match(Add incoming);
    void erase(Handle handle);
    void reject(Operation operation, OrderId id, RejectReason reason);

    const BookConfig config_;
    Levels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Handle> ids_;
    std::vector<Event> events_;
};

} // namespace fifofill
