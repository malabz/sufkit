// SPDX-License-Identifier: MIT

#include "caps_backend.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

#if SUFKIT_HAS_CAPS
#include "Suffix_Array.hpp"
#include "parlay/parallel.h"
#endif

namespace sufkit::detail {
namespace {

template <class Index>
CapsBuildResult<Index> BuildCaps(const std::vector<std::uint8_t>& text,
                                 std::uint32_t threads, bool retain_lcp) {
#if SUFKIT_HAS_CAPS
  if (text.size() < 16) {
    throw Error(
        ErrorCode::kInvalidInput,
        "CaPS-SA requires a logical text containing at least 16 symbols");
  }
  if (text.size() >
      static_cast<std::uint64_t>(std::numeric_limits<Index>::max())) {
    throw Error(ErrorCode::kInvalidInput,
                sizeof(Index) == 4
                    ? "reference is too large for CaPS-SA uint32_t"
                    : "reference is too large for CaPS-SA uint64_t");
  }

  const auto subproblems = CapsSubproblemCount(text.size(), threads);
  try {
    CaPS_SA::Suffix_Array<Index> suffix_array(
        reinterpret_cast<const char*>(text.data()),
        static_cast<Index>(text.size()), static_cast<Index>(subproblems), 0);
    parlay::execute_with_scheduler(threads, [&] { suffix_array.construct(); });
    CapsBuildResult<Index> result;
    result.suffix_array.assign(suffix_array.SA(),
                               suffix_array.SA() + text.size());
    if (retain_lcp) {
      // Reuse CaPS's native-width LCP when CHILD construction needs a raw
      // row array. Other layouts rebuild byte-coded LCP after this object is
      // destroyed, avoiding an extra full LCP plane at the CaPS peak.
      result.lcp.assign(suffix_array.LCP(),
                        suffix_array.LCP() + text.size());
    }
    return result;
  } catch (const std::bad_alloc&) {
    throw Error(ErrorCode::kBuildFailure, "CaPS-SA allocation failed");
  } catch (const std::exception& error) {
    throw Error(ErrorCode::kBuildFailure,
                std::string("CaPS-SA construction failed: ") + error.what());
  } catch (...) {
    throw Error(ErrorCode::kBuildFailure, "CaPS-SA construction failed");
  }
#else
  (void)text;
  (void)threads;
  (void)retain_lcp;
  throw Error(ErrorCode::kUnsupportedBackend,
              "CaPS-SA support was disabled when sufkit was built");
#endif
}

}  // namespace

bool CapsBuildAvailable() noexcept {
#if SUFKIT_HAS_CAPS
  return true;
#else
  return false;
#endif
}

CapsBuildResult<std::uint32_t> BuildCaps32(
    const std::vector<std::uint8_t>& text, std::uint32_t threads,
    bool retain_lcp) {
  return BuildCaps<std::uint32_t>(text, threads, retain_lcp);
}

CapsBuildResult<std::uint64_t> BuildCaps64(
    const std::vector<std::uint8_t>& text, std::uint32_t threads,
    bool retain_lcp) {
  return BuildCaps<std::uint64_t>(text, threads, retain_lcp);
}

}  // namespace sufkit::detail
