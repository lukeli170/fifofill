#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <span>

TEST(Build, Cxx20SpanAndBitOperations) {
    const std::uint64_t words[]{0, 8};
    const std::span<const std::uint64_t> view{words};
    EXPECT_EQ(view.size(), 2U);
    EXPECT_EQ(std::countr_zero(view[1]), 3);
    EXPECT_EQ(std::countl_zero(view[0]), 64);
}
