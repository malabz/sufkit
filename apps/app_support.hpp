#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::app {

std::vector<SequenceRecord> read_fasta_records(const std::filesystem::path& path);
std::uint64_t parse_unsigned(const std::string& text, const std::string& option_name);

} // namespace sufkit::app

