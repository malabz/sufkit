// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <sufkit/types.hpp>

namespace sufkit::app {

std::vector<SequenceRecord> ReadFastaRecords(const std::filesystem::path& path);
std::uint64_t ParseUnsigned(const std::string& text,
                            const std::string& option_name);

}  // namespace sufkit::app
