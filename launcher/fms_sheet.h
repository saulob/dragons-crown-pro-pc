// Vanillaware FMSB message sheets (msgsheet/*.fms): a header, a pointer table the game fills in when it loads the sheet
// (zeros on disk), the records as NUL-terminated UTF-8 padded to 16 bytes, then the FEOC trailer. A record's position is
// its ID. scripts/localization/fms.py reads and writes the same layout.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dcp {

struct FmsSheet {
    std::vector<uint8_t> header; // up to the pointer table
    std::vector<std::string> records;
    std::vector<uint8_t> trailer;
};

// Nothing when the bytes are not a sheet of that layout.
std::optional<FmsSheet> ParseFms(std::span<const uint8_t> bytes);
// The sheet's bytes; the header's trailer offset and record count follow the records, the rest of it is kept.
std::vector<uint8_t> BuildFms(const FmsSheet& sheet);

} // namespace dcp
