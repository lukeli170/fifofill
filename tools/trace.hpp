#pragma once

#include "fifofill/book.hpp"

#include <charconv>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fifofill {

template<class Integer>
Integer integer(std::string_view text) {
    Integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw std::invalid_argument("invalid or overflowing integer: " + std::string(text));
    return value;
}

struct Trace {
    BookConfig config;
    std::uint64_t seed = 0;
    std::uint32_t generator_version = 0; // Zero denotes handwritten input.
    std::vector<Command> commands;
};

Trace read_trace(std::istream& input);
void write_trace(std::ostream& output, const Trace& trace);

} // namespace fifofill
