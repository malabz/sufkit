#pragma once

#include <filesystem>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/types.hpp>

namespace sufkit {

SUFKIT_API IndexInfo inspect_index(const std::filesystem::path& path);
SUFKIT_API std::vector<BackendDescriptor> available_sa_backends();
SUFKIT_API std::vector<BackendDescriptor> available_fm_backends();

} // namespace sufkit

