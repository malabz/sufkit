#include <sufkit/inspect.hpp>

#include "serialization.hpp"

namespace sufkit {

IndexInfo inspect_index(const std::filesystem::path& path) {
    return detail::index_info_from_container(detail::read_container(path));
}

std::vector<BackendDescriptor> available_sa_backends() {
    return {
        {"auto", true, false, "selects divsufsort32 or divsufsort64 by text length"},
        {"divsufsort", true, false, "bundled libdivsufsort 2.0.2"},
        {"caps", false, true, "reserved for sufkit V1.1"}
    };
}

std::vector<BackendDescriptor> available_fm_backends() {
    return {
        {"sdsl-csa-wt-huff", true, false, "sdsl::csa_wt<sdsl::wt_huff<>,32,64>"},
        {"sdsl-csa-wt-balanced", false, false, "reserved for sufkit V1.1"},
        {"sdsl-csa-sada", false, false, "reserved for sufkit V1.1"}
    };
}

} // namespace sufkit

