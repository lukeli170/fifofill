#include "ledger.hpp"
#include "workload.hpp"

#include "reference_book.hpp"

#include <fstream>
#include <iostream>

using namespace fifofill;

int main(int argc, char** argv) {
    try {
        std::string engine = "reference", input, output, scenario = "mixed";
        std::uint64_t seed = 42;
        std::size_t count = 10000;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("options require values");
            const std::string_view key = argv[i], value = argv[i + 1];
            if (key == "--engine") engine = value;
            else if (key == "--input") input = value;
            else if (key == "--output") output = value;
            else if (key == "--scenario") scenario = value;
            else if (key == "--seed") seed = integer<std::uint64_t>(value);
            else if (key == "--commands") count = integer<std::size_t>(value);
            else throw std::invalid_argument("unknown option: " + std::string(key));
        }
        if (engine != "reference") throw std::invalid_argument("supported engine: reference");
        if (!output.empty()) {
            if (!input.empty()) throw std::invalid_argument("export accepts no --input");
            const auto trace = generate_workload(seed, count, scenario);
            std::ofstream file(output);
            write_trace(file, trace);
            file.close();
            if (!file) throw std::runtime_error("cannot finish trace output");
            std::cout << "exported " << trace.commands.size() << " commands, seed " << seed
                      << ", generator " << generator_version << '\n';
        } else {
            if (input.empty())
                throw std::invalid_argument("replay requires --engine reference --input PATH");
            std::ifstream file(input);
            if (!file) throw std::runtime_error("cannot open input");
            const auto trace = read_trace(file); // Parse the whole file before executing anything.
            ReferenceBook book(trace.config);
            Ledger ledger;
            std::size_t event_count = 0;
            for (const auto& command : trace.commands) {
                const auto events = book.process(command);
                ledger.observe(command, events, book.snapshot());
                event_count += events.size();
            }
            std::cout << "replayed " << trace.commands.size() << " commands, " << event_count << " events\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
