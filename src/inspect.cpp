// SPDX-License-Identifier: MIT

#include "sufkit/inspect.hpp"

#include "caps_backend.hpp"
#include "serialization.hpp"

namespace sufkit {

IndexInfo InspectIndex(const std::filesystem::path& path) {
  return detail::IndexInfoFromContainer(detail::ReadContainer(path));
}

std::vector<BackendDescriptor> AvailableSaBackends() {
  const bool caps = detail::CapsBuildAvailable();
  return {{"auto", true, true,
           "selects CaPS-SA at >=1 GiB with threads>1; otherwise divsufsort"},
          {"divsufsort", true, false, "bundled libdivsufsort 2.0.2"},
          {"caps", caps, true,
           caps ? "bundled CaPS-SA 2597b373 with ParlayLib e1f1dc0"
                : "disabled by SUFKIT_ENABLE_CAPS=OFF"}};
}

std::vector<BackendDescriptor> AvailableFmBackends() {
  return {
      {"sdsl-csa-wt-huff", true, false, "sdsl::csa_wt<sdsl::wt_huff<>,32,64>"},
      {"sdsl-csa-wt-balanced", true, false,
       "sdsl::csa_wt<sdsl::wt_blcd<>,32,64>"},
      {"sdsl-csa-sada", false, false, "reserved; not implemented"},
      {"sdsl-csa-wt-epr", true, false, "sdsl::csa_wt<sdsl::wt_epr<8>,32,64>"}};
}

}  // namespace sufkit
