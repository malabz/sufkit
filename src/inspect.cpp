#include <sufkit/inspect.hpp>

#include "caps_backend.hpp"
#include "serialization.hpp"

namespace sufkit {

IndexInfo inspect_index(const std::filesystem::path& path) {
    return detail::index_info_from_container(detail::read_container(path));
}

std::vector<BackendDescriptor> available_sa_backends() {
    const bool caps = detail::caps_build_available();
    return {
        {"auto", true, true, "selects CaPS-SA at >=1 GiB with threads>1; otherwise divsufsort"},
        {"divsufsort", true, false, "bundled libdivsufsort 2.0.2"},
        {"caps", caps, true, caps
            ? "bundled CaPS-SA 2597b373 with ParlayLib e1f1dc0"
            : "disabled by SUFKIT_ENABLE_CAPS=OFF"}
    };
}

std::vector<BackendDescriptor> available_fm_backends() {
    return {
        {"sdsl-csa-wt-huff", true, false, "sdsl::csa_wt<sdsl::wt_huff<>,32,64>"},
        {"sdsl-csa-wt-balanced", true, false, "sdsl::csa_wt<sdsl::wt_blcd<>,32,64>"},
        {"sdsl-csa-sada", false, false, "reserved; not implemented"},
        {"sdsl-csa-wt-epr", true, false, "sdsl::csa_wt<sdsl::wt_epr<8>,32,64>"}
    };
}

} // namespace sufkit
