#include <sufkit/types.hpp>

namespace sufkit {

Error::Error(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

ErrorCode Error::code() const noexcept { return code_; }

const char* to_string(IndexKind value) noexcept {
    switch (value) {
    case IndexKind::suffix_array: return "suffix_array";
    case IndexKind::fm_index: return "fm_index";
    }
    return "unknown";
}

const char* to_string(SaBackend value) noexcept {
    switch (value) {
    case SaBackend::auto_select: return "auto";
    case SaBackend::divsufsort: return "divsufsort";
    case SaBackend::caps: return "caps";
    }
    return "unknown";
}

const char* to_string(SaAcceleration value) noexcept {
    switch (value) {
    case SaAcceleration::none: return "none";
    case SaAcceleration::lcp: return "lcp";
    case SaAcceleration::lcp_child: return "child";
    case SaAcceleration::lcp_suffix_link: return "suffix-link";
    case SaAcceleration::full: return "full";
    }
    return "unknown";
}

const char* to_string(MemSearchAlgorithm value) noexcept {
    switch (value) {
    case MemSearchAlgorithm::auto_select: return "auto";
    case MemSearchAlgorithm::baseline: return "baseline";
    case MemSearchAlgorithm::lcp: return "lcp";
    case MemSearchAlgorithm::child: return "child";
    case MemSearchAlgorithm::suffix_link: return "suffix-link";
    case MemSearchAlgorithm::full: return "full";
    }
    return "unknown";
}

const char* to_string(FmBackend value) noexcept {
    switch (value) {
    case FmBackend::sdsl_csa_wt_huff: return "sdsl-csa-wt-huff";
    case FmBackend::sdsl_csa_wt_balanced: return "sdsl-csa-wt-balanced";
    case FmBackend::sdsl_csa_sada: return "sdsl-csa-sada";
    case FmBackend::sdsl_csa_wt_epr: return "sdsl-csa-wt-epr";
    }
    return "unknown";
}

const char* to_string(Strand value) noexcept {
    switch (value) {
    case Strand::forward: return "+";
    case Strand::reverse_complement: return "-";
    case Strand::both: return "both";
    }
    return "?";
}

const char* to_string(StrandMode value) noexcept {
    switch (value) {
    case StrandMode::forward: return "forward";
    case StrandMode::reverse_complement: return "reverse";
    case StrandMode::both: return "both";
    }
    return "unknown";
}

const char* to_string(ErrorCode value) noexcept {
    switch (value) {
    case ErrorCode::invalid_input: return "invalid_input";
    case ErrorCode::io_error: return "io_error";
    case ErrorCode::unsupported_backend: return "unsupported_backend";
    case ErrorCode::corrupt_index: return "corrupt_index";
    case ErrorCode::version_mismatch: return "version_mismatch";
    case ErrorCode::build_failure: return "build_failure";
    }
    return "unknown";
}

} // namespace sufkit
