#pragma once

#include <filesystem>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/types.hpp>

/** @file
 *  @brief Container inspection and compiled-backend discovery.
 */

namespace sufkit {

/**
 * @ingroup persistence
 * Validate outer container and return metadata without constructing a query
 * object.
 * @param path `.sufidx` path.
 * @return Validated index information.
 * @throws Error for I/O, version, CRC, section, metadata, or backend failure.
 */
SUFKIT_API IndexInfo inspect_index(const std::filesystem::path& path);

/**
 * @ingroup backends
 * @return SA constructor descriptors including auto, divsufsort, and compiled
 *         CaPS availability.
 */
SUFKIT_API std::vector<BackendDescriptor> available_sa_backends();

/**
 * @ingroup backends
 * @return Fixed available and reserved SDSL FM backend descriptors.
 */
SUFKIT_API std::vector<BackendDescriptor> available_fm_backends();

} // namespace sufkit
