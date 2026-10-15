#include "workload.hpp"
#include "reference_book.hpp"

#include <algorithm>
#include <limits>

namespace fifofill {

std::uint64_t bounded(std::mt19937_64& random, std::uint64_t bound) {
    if (bound == 0) throw std::invalid_argument("zero sampling bound");
    const auto threshold = (std::uint64_t{0} - bound) % bound;
    for (;;) {
        const auto value = random();
        if (value >= threshold) return value % bound;
    }
}

BookConfig scenario_config(std::string_view scenario) {
    if (std::find(std::begin(scenarios), std::end(scenarios), scenario) == std::end(scenarios))
        throw std::invalid_argument("unknown scenario: " + std::string(scenario));
    return {1, scenario == "sparse" ? 65536 : 4096, 1, scenario == "tiny" ? 3U : 128U};
}

Workload::Workload(std::uint64_t seed, BookConfig config, std::string_view scenario)
    : random_(seed), config_(config), scenario_(scenario), levels_(config.level_count()) {
    static_cast<void>(scenario_config(scenario));
}

Price Workload::price(Side side) {
    std::size_t index;
    const auto center = levels_ / 2;
    if (scenario_ == "sparse" || scenario_ == "boundaries") {
        index = static_cast<std::size_t>(bounded(random_, levels_));
    } else if (scenario_ == "same-price") {
        index = center;
    } else {
        const auto width = std::min<std::size_t>(center, 16);
        const auto offset = static_cast<std::size_t>(bounded(random_, width + 1));
        index = side == Side::Buy ? center - offset : std::min(levels_ - 1, center + offset + 1);
    }
    return config_.min_price + static_cast<Price>(index) * config_.tick_size;
}

Command Workload::next(const Snapshot& state) {
    const auto count = state.bids.size() + state.asks.size();
    const auto pick = [&]() -> const RestingOrder& {
        const auto index = static_cast<std::size_t>(bounded(random_, count));
        return index < state.bids.size() ? state.bids[index] : state.asks[index - state.bids.size()];
    };
    const auto draw = bounded(random_, 100);
    const Side side = bounded(random_, 2) == 0 ? Side::Buy : Side::Sell;
    if (draw < 10) {
        switch (bounded(random_, 6)) {
        case 0: return Add{0, side, price(side), 1};
        case 1: return Add{next_id_, side, price(side), 0};
        case 2: return Add{next_id_, static_cast<Side>(255), price(side), 1};
        case 3: return Modify{std::numeric_limits<OrderId>::max(), 0, 0};
        case 4: return Add{next_id_, side, config_.min_price - 1, 1};
        default: return count ? Command{Add{pick().id, side, std::nullopt, 1}} : Command{Cancel{0}};
        }
    }
    if (count && (count >= config_.capacity - config_.capacity / 4 ||
                  draw < (scenario_ == "cancel-churn" ? 65U : 30U))) {
        return Cancel{pick().id};
    }
    const auto modify_threshold = scenario_ == "modify-heavy" ? 85U : scenario_ == "cancel-churn" ? 75U : 50U;
    if (count && draw < modify_threshold) {
        const auto order = pick();
        const auto mode = bounded(random_, 4);
        const Qty quantity = mode == 0 ? std::max(Qty{1}, order.remaining / 2)
            : mode == 1 ? order.remaining : static_cast<Qty>(bounded(random_, 64) + 1);
        return Modify{order.id, mode == 3 ? price(order.side) : order.price, quantity};
    }
    if (next_id_ == std::numeric_limits<OrderId>::max()) throw std::overflow_error("generator IDs exhausted");
    const auto id = next_id_++;
    const Qty quantity = scenario_ == "boundaries" && draw % 3 == 0 ? std::numeric_limits<Qty>::max()
        : static_cast<Qty>(bounded(random_, scenario_ == "sweeps" ? 512 : 64) + 1);
    if (draw >= (scenario_ == "sweeps" ? 70U : 90U)) return Add{id, side, std::nullopt, quantity};
    return Add{id, side, price(side), quantity};
}

Trace generate_workload(std::uint64_t seed, std::size_t count, std::string_view scenario) {
    Trace trace{scenario_config(scenario), seed, generator_version, {}};
    Workload workload(seed, trace.config, scenario);
    ReferenceBook book(trace.config);
    trace.commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        trace.commands.push_back(workload.next(book.snapshot()));
        book.process(trace.commands.back());
    }
    return trace;
}

} // namespace fifofill
