// SPDX-License-Identifier: MIT

#include <sufkit/types.hpp>

namespace sufkit {

Error::Error(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

ErrorCode Error::Code() const noexcept { return code_; }

const char* ToString(IndexKind value) noexcept {
  switch (value) {
    case IndexKind::kSuffixArray:
      return "suffix_array";
    case IndexKind::kFmIndex:
      return "fm_index";
  }
  return "unknown";
}

const char* ToString(SaBackend value) noexcept {
  switch (value) {
    case SaBackend::kAutoSelect:
      return "auto";
    case SaBackend::kDivsufsort:
      return "divsufsort";
    case SaBackend::kCaps:
      return "caps";
  }
  return "unknown";
}

const char* ToString(SaAcceleration value) noexcept {
  switch (value) {
    case SaAcceleration::kNone:
      return "none";
    case SaAcceleration::kLcp:
      return "lcp";
    case SaAcceleration::kLcpChild:
      return "child";
    case SaAcceleration::kLcpSuffixLink:
      return "suffix-link";
    case SaAcceleration::kFull:
      return "full";
  }
  return "unknown";
}

const char* ToString(SaLookupAcceleration value) noexcept {
  switch (value) {
    case SaLookupAcceleration::kBinary:
      return "binary";
    case SaLookupAcceleration::kSaplingPwl:
      return "sapling-pwl";
  }
  return "unknown";
}

const char* ToString(SaSearchAlgorithm value) noexcept {
  switch (value) {
    case SaSearchAlgorithm::kAutoSelect:
      return "auto";
    case SaSearchAlgorithm::kBinary:
      return "binary";
    case SaSearchAlgorithm::kLcpBinary:
      return "lcp-binary";
    case SaSearchAlgorithm::kSaplingPwl:
      return "sapling-pwl";
    case SaSearchAlgorithm::kChild:
      return "child";
  }
  return "unknown";
}

const char* ToString(RightMaximalSearchAlgorithm value) noexcept {
  switch (value) {
    case RightMaximalSearchAlgorithm::kAutoSelect:
      return "auto";
    case RightMaximalSearchAlgorithm::kBaseline:
      return "baseline";
    case RightMaximalSearchAlgorithm::kLcp:
      return "lcp";
    case RightMaximalSearchAlgorithm::kChild:
      return "child";
    case RightMaximalSearchAlgorithm::kSuffixLink:
      return "suffix-link";
    case RightMaximalSearchAlgorithm::kFull:
      return "full";
  }
  return "unknown";
}

const char* ToString(MemSearchAlgorithm value) noexcept {
  switch (value) {
    case MemSearchAlgorithm::kAutoSelect:
      return "auto";
    case MemSearchAlgorithm::kBaseline:
      return "baseline";
    case MemSearchAlgorithm::kLcp:
      return "lcp";
    case MemSearchAlgorithm::kChild:
      return "child";
    case MemSearchAlgorithm::kSuffixLink:
      return "suffix-link";
    case MemSearchAlgorithm::kFull:
      return "full";
  }
  return "unknown";
}

const char* ToString(FmBackend value) noexcept {
  switch (value) {
    case FmBackend::kSdslCsaWtHuff:
      return "sdsl-csa-wt-huff";
    case FmBackend::kSdslCsaWtBalanced:
      return "sdsl-csa-wt-balanced";
    case FmBackend::kSdslCsaSada:
      return "sdsl-csa-sada";
    case FmBackend::kSdslCsaWtEpr:
      return "sdsl-csa-wt-epr";
  }
  return "unknown";
}

const char* ToString(Strand value) noexcept {
  switch (value) {
    case Strand::kForward:
      return "+";
    case Strand::kReverseComplement:
      return "-";
    case Strand::kBoth:
      return "both";
  }
  return "?";
}

const char* ToString(StrandMode value) noexcept {
  switch (value) {
    case StrandMode::kForward:
      return "forward";
    case StrandMode::kReverseComplement:
      return "reverse";
    case StrandMode::kBoth:
      return "both";
  }
  return "unknown";
}

const char* ToString(ErrorCode value) noexcept {
  switch (value) {
    case ErrorCode::kInvalidInput:
      return "invalid_input";
    case ErrorCode::kIoError:
      return "io_error";
    case ErrorCode::kUnsupportedBackend:
      return "unsupported_backend";
    case ErrorCode::kCorruptIndex:
      return "corrupt_index";
    case ErrorCode::kVersionMismatch:
      return "version_mismatch";
    case ErrorCode::kBuildFailure:
      return "build_failure";
  }
  return "unknown";
}

}  // namespace sufkit
