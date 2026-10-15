#pragma once

#include "trace.hpp"

#include <random>
#include <string>

namespace fifofill {

inline constexpr std::uint32_t generator_version = 1;
inline constexpr std::string_view scenarios[]{
    "mixed", "clustered", "same-price", "sparse", "cancel-churn", "sweeps",
    "modify-heavy", "tiny", "boundaries"};

// Rejection sampling fixes the mapping from mt19937_64 output to [0, bound).
std::uint64_t bounded(std::mt19937_64& random, std::uint64_t bound);
BookConfig scenario_config(std::string_view scenario);

class Workload {
public:
    Workload(std::uint64_t seed, BookConfig config, std::string_view scenario);
    Command next(const Snapshot& state);
private:
    std::mt19937_64 random_;
    BookConfig config_;
    std::string scenario_;
    std::size_t levels_;
    OrderId next_id_ = 1;
    Price price(Side side);
};

Trace generate_workload(std::uint64_t seed, std::size_t count, std::string_view scenario);

} // namespace fifofill
