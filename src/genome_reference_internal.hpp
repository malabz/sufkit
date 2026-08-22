#pragma once

#include <sufkit/genome_reference.hpp>

#include "reference_data.hpp"

namespace sufkit {

struct GenomeReference::Impl {
    detail::ReferenceData data;
};

} // namespace sufkit

