#include "ledger.hpp"
#include "reference_book.hpp"
#include "workload.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>

using namespace fifofill;

TEST(Trace, RoundTripPreservesInvalidCommandsAndExtremeIntegers) {
    Trace original{{5, 100, 5, 8}, UINT64_MAX, generator_version, {
        Add{UINT64_MAX, Side::Buy, std::nullopt, UINT32_MAX},
        Add{0, static_cast<Side>(255), INT64_MIN, 0}, Cancel{0}, Modify{1, INT64_MAX, 3}}};
    std::stringstream stream;
    write_trace(stream, original);
    const auto copy = read_trace(stream);
    EXPECT_EQ(copy.commands, original.commands);
    EXPECT_EQ(copy.seed, original.seed);
    EXPECT_EQ(copy.generator_version, original.generator_version);
    EXPECT_EQ(copy.config.min_price, 5);
    EXPECT_EQ(copy.config.max_price, 100);
    EXPECT_EQ(copy.config.tick_size, 5);
    EXPECT_EQ(copy.config.capacity, 8U);
}

TEST(Trace, RejectsMalformedRecordsWithLineNumbers) {
    for (const auto* record : {"ADD 1 B 50 1 garbage", "ADD -1 B 50 1", "ADD 1 B 50 4294967296",
             "ADD 18446744073709551616 B 50 1", "ADD 1 X 50 1", "ADD 1 256 50 1",
             "MODIFY 1 9223372036854775808 1", "CANCEL +1", "CANCEL 1x", "CANCEL", "UNKNOWN 1"}) {
        std::istringstream input(std::string("FIFOFILL 1\nCONFIG 1 100 1 8\nMETA 0 0 1\n") + record);
        try {
            read_trace(input);
            FAIL() << record;
        } catch (const std::invalid_argument& error) {
            EXPECT_TRUE(std::string_view(error.what()).starts_with("line 4:")) << error.what();
        }
    }
    for (const auto* text : {"", "FIFOFILL 2", "FIFOFILL 1\nCONFIG 0 100 1 8",
             "FIFOFILL 1\nCONFIG 1 100 1 8\nMETA 0 0 2\nCANCEL 1",
             "FIFOFILL 1\nCONFIG 1 100 1 8\nMETA 0 0 0\nCANCEL 1"}) {
        std::istringstream input(text);
        EXPECT_THROW(read_trace(input), std::invalid_argument);
    }
}

TEST(Trace, FixturesReplayWithoutRngAndHaveExactFinalStates) {
    const Snapshot expected[]{
        {{{4, Side::Buy, 99, 300}}, {{3, Side::Sell, 101, 130}}},
        {{{1, Side::Buy, 99, 2}}, {}}, {}};
    std::size_t fixture = 0;
    for (const auto* name : {"sweep", "priority", "market"}) {
        std::ifstream file(std::string(FIFOFILL_FIXTURES) + "/" + name + ".ff");
        ASSERT_TRUE(file);
        const auto trace = read_trace(file);
        std::vector<Event> previous;
        for (int repeat = 0; repeat < 2; ++repeat) {
            ReferenceBook book(trace.config);
            Ledger ledger;
            std::vector<Event> all;
            for (const auto& command : trace.commands) {
                const auto events = book.process(command);
                ledger.observe(command, events, book.snapshot());
                all.insert(all.end(), events.begin(), events.end());
            }
            EXPECT_EQ(book.snapshot(), expected[fixture]);
            if (repeat) EXPECT_EQ(all, previous);
            previous = all;
        }
        ++fixture;
    }
}

TEST(Workload, BoundedSamplingGoldenAndZeroBound) {
    std::mt19937_64 random(42);
    const std::uint64_t expected[]{6, 4, 0, 2, 1, 8, 6, 4};
    for (const auto value : expected) EXPECT_EQ(bounded(random, 10), value);
    EXPECT_THROW(bounded(random, 0), std::invalid_argument);
}

TEST(Workload, VersionOneHasPortableGoldenCommands) {
    std::istringstream golden(
        "FIFOFILL 1\nCONFIG 1 4096 1 128\nMETA 42 1 12\n"
        "ADD 1 B 0 1\nADD 1 S 2058 61\nMODIFY 1 2059 7\nADD 2 255 2062 1\n"
        "CANCEL 1\nADD 2 B 2033 26\nADD 3 B 2043 42\nMODIFY 2 2039 11\n"
        "CANCEL 3\nMODIFY 2 2038 49\nCANCEL 2\nADD 4 S 2061 26\n");
    EXPECT_EQ(generate_workload(42, 12, "mixed").commands, read_trace(golden).commands);
    EXPECT_THROW(generate_workload(42, 1, "unknown"), std::invalid_argument);
}

TEST(Workload, EveryScenarioExercisesSuccessfulCancelsModifiesAndRejections) {
    for (const auto scenario : scenarios) {
        SCOPED_TRACE(scenario);
        const auto trace = generate_workload(42, 2000, scenario);
        EXPECT_EQ(trace.commands, generate_workload(42, 2000, scenario).commands);
        ReferenceBook book(trace.config);
        Ledger ledger;
        std::size_t cancels = 0, modifies = 0, rejects = 0;
        for (const auto& command : trace.commands) {
            const auto events = book.process(command);
            const auto* accepted = std::get_if<Accepted>(&events.front());
            modifies += accepted && accepted->operation == Operation::Modify;
            cancels += std::holds_alternative<Cancel>(command) && std::holds_alternative<Canceled>(events.front());
            rejects += std::holds_alternative<Rejected>(events.front());
            ledger.observe(command, events, book.snapshot());
        }
        EXPECT_GT(cancels, 0U);
        EXPECT_GT(modifies, 0U);
        EXPECT_GT(rejects, 0U);
        EXPECT_LT(rejects, trace.commands.size() / 4);
    }
}
