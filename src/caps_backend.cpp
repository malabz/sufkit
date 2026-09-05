// SPDX-License-Identifier: MIT

#include "caps_backend.hpp"

#include <chrono>
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
                                 std::uint32_t threads, bool retain_lcp,
                                 const SuffixArrayBuildOptions* options) {
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
    using Clock = std::chrono::steady_clock;
    auto elapsed = [](Clock::time_point t) {
      return std::chrono::duration<double>(Clock::now() - t).count();
    };
    const auto allocate_begin = Clock::now();
    CapsBuildResult<Index> result;
    result.suffix_array.resize(text.size());
    result.lcp.resize(text.size());
    if (options && options->statistics) {
      options->statistics->caps_output_allocation_seconds = elapsed(allocate_begin);
    }
    const auto construct_begin = Clock::now();
    CaPS_SA::Suffix_Array<Index> suffix_array(
        reinterpret_cast<const char*>(text.data()),
        static_cast<Index>(text.size()), static_cast<Index>(subproblems), 0,
        result.suffix_array.data(), result.lcp.data());
    if (options) {
      suffix_array.set_stage_callback(options->stage_callback,
                                      options->stage_context);
    }
    parlay::execute_with_scheduler(threads, [&] { suffix_array.construct(); });
    if (options && options->statistics) {
      options->statistics->caps_construct_seconds = elapsed(construct_begin);
    }
    if (!retain_lcp) {
      std::vector<Index>().swap(result.lcp);
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
  (void)options;
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
    bool retain_lcp, const SuffixArrayBuildOptions* options) {
  return BuildCaps<std::uint32_t>(text, threads, retain_lcp, options);
}

CapsBuildResult<std::uint64_t> BuildCaps64(
    const std::vector<std::uint8_t>& text, std::uint32_t threads,
    bool retain_lcp, const SuffixArrayBuildOptions* options) {
  return BuildCaps<std::uint64_t>(text, threads, retain_lcp, options);
}

}  // namespace sufkit::detail
