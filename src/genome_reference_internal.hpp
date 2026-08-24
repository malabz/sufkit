// SPDX-License-Identifier: MIT

#pragma once

#include "reference_data.hpp"
#include <sufkit/genome_reference.hpp>

namespace sufkit {

struct GenomeReference::Impl {
  detail::ReferenceData data;
};

}  // namespace sufkit
