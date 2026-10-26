// Just enough of the ZIP format to look into game archives and extract
// single files from them (stored or deflated, no ZIP64, no encryption).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rd::zip {

struct Entry {
    std::string name;
    std::uint32_t compressedSize = 0, size = 0;
    std::uint16_t method = 0;
    std::uint32_t localHeader = 0;
};

// The archive's files (not its folders); empty if it is not a ZIP
std::vector<Entry> list(const std::filesystem::path &archive);
bool extract(const std::filesystem::path &archive, const Entry &entry, const std::filesystem::path &target);

} // namespace rd::zip
