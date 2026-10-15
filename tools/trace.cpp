#include "trace.hpp"

#include <sstream>
#include <string>

namespace fifofill {

Trace read_trace(std::istream& input) {
    Trace trace{};
    std::size_t line_number = 0, records = 0, expected_count = 0;
    std::string line;
    while (std::getline(input, line)) {
        ++line_number;
        std::istringstream words(line);
        std::vector<std::string> fields;
        for (std::string word; words >> word;) fields.push_back(word);
        if (fields.empty() || fields.front().starts_with('#')) continue;
        try {
            const auto count = [&](std::size_t n) {
                if (fields.size() != n) throw std::invalid_argument("wrong field count or trailing garbage");
            };
            if (records == 0) {
                count(2);
                if (fields[0] != "FIFOFILL" || fields[1] != "1")
                    throw std::invalid_argument("expected FIFOFILL 1");
            } else if (records == 1) {
                count(5);
                if (fields[0] != "CONFIG") throw std::invalid_argument("expected CONFIG");
                trace.config = {integer<Price>(fields[1]), integer<Price>(fields[2]),
                                integer<Price>(fields[3]), integer<std::size_t>(fields[4])};
                static_cast<void>(trace.config.level_count());
            } else if (records == 2) {
                count(4);
                if (fields[0] != "META") throw std::invalid_argument("expected META seed generator_version count");
                trace.seed = integer<std::uint64_t>(fields[1]);
                trace.generator_version = integer<std::uint32_t>(fields[2]);
                expected_count = integer<std::size_t>(fields[3]);
            } else if (fields[0] == "ADD") {
                count(5);
                const auto side = fields[2] == "B" ? Side::Buy : fields[2] == "S" ? Side::Sell
                    : static_cast<Side>(integer<std::uint8_t>(fields[2]));
                const auto price = fields[3] == "M" ? std::optional<Price>{}
                                                    : integer<Price>(fields[3]);
                trace.commands.push_back(Add{integer<OrderId>(fields[1]), side, price, integer<Qty>(fields[4])});
            } else if (fields[0] == "CANCEL") {
                count(2);
                trace.commands.push_back(Cancel{integer<OrderId>(fields[1])});
            } else if (fields[0] == "MODIFY") {
                count(4);
                trace.commands.push_back(Modify{integer<OrderId>(fields[1]), integer<Price>(fields[2]),
                                                integer<Qty>(fields[3])});
            } else {
                throw std::invalid_argument("unknown record");
            }
            ++records;
        } catch (const std::exception& error) {
            throw std::invalid_argument("line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    if (!input.eof() || input.bad()) throw std::runtime_error("trace read failed");
    if (records < 3 || trace.commands.size() != expected_count)
        throw std::invalid_argument("line " + std::to_string(line_number + 1) + ": incomplete header or command count mismatch");
    return trace;
}

void write_trace(std::ostream& output, const Trace& trace) {
    output << "FIFOFILL 1\nCONFIG " << trace.config.min_price << ' ' << trace.config.max_price
           << ' ' << trace.config.tick_size << ' ' << trace.config.capacity << "\nMETA "
           << trace.seed << ' ' << trace.generator_version << ' ' << trace.commands.size() << '\n';
    for (const auto& command : trace.commands) {
        if (const auto* add = std::get_if<Add>(&command)) {
            output << "ADD " << add->id << ' ';
            if (add->side == Side::Buy) output << 'B';
            else if (add->side == Side::Sell) output << 'S';
            else output << static_cast<unsigned>(add->side);
            output << ' ';
            if (add->price) output << *add->price;
            else output << 'M';
            output << ' ' << add->quantity << '\n';
        } else if (const auto* cancel = std::get_if<Cancel>(&command)) {
            output << "CANCEL " << cancel->id << '\n';
        } else {
            const auto& modify = std::get<Modify>(command);
            output << "MODIFY " << modify.id << ' ' << modify.price << ' ' << modify.quantity << '\n';
        }
    }
    if (!output) throw std::runtime_error("trace write failed");
}

} // namespace fifofill
