// SPDX-License-Identifier: MIT

#include "divsufsort_backend.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <type_traits>

extern "C" {
#include "divsufsort.h"
#include "divsufsort64.h"
}

#include <sufkit/types.hpp>

#include "sequence_compare.hpp"

namespace sufkit::detail {
namespace {

template <class Index>
DivSufsortBuildResult<Index> BuildDivsufsort(
    const std::vector<std::uint8_t>& text, std::uint32_t sampling_rate,
    bool retain_lcp, bool retain_isa, std::uint32_t requested_threads) {
  DivSufsortBuildResult<Index> result;
  const auto sa_begin = std::chrono::steady_clock::now();
  result.suffix_array.resize(text.size());
  int status = 0;
  if constexpr (std::is_same_v<Index, std::int32_t>) {
    if (text.size() >
        static_cast<std::uint64_t>(std::numeric_limits<saidx_t>::max())) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference is too large for divsufsort32");
    }
    status = divsufsort(reinterpret_cast<const sauchar_t*>(text.data()),
                        reinterpret_cast<saidx_t*>(result.suffix_array.data()),
                        static_cast<saidx_t>(text.size()));
  } else {
    if (text.size() >
        static_cast<std::uint64_t>(std::numeric_limits<saidx64_t>::max())) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference is too large for divsufsort64");
    }
    status =
        divsufsort64(reinterpret_cast<const sauchar_t*>(text.data()),
                     reinterpret_cast<saidx64_t*>(result.suffix_array.data()),
                     static_cast<saidx64_t>(text.size()));
  }
  if (status != 0) {
    throw Error(ErrorCode::kBuildFailure, "libdivsufsort construction failed");
  }

  if (sampling_rate > 1) {
    // Persist residue-zero text positions only. Dividing a retained position
    // by the sampling rate gives its dense ISA slot.
    std::size_t output = 0;
    for (const auto suffix : result.suffix_array) {
      if (static_cast<std::uint64_t>(suffix) % sampling_rate == 0) {
        result.suffix_array[output++] = suffix;
      }
    }
    result.suffix_array.resize(output);
    result.suffix_array.shrink_to_fit();
  }
  result.sa_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - sa_begin)
          .count();
  if (!retain_lcp && !retain_isa) {
    return result;
  }

  const auto isa_begin = std::chrono::steady_clock::now();
  const auto count = result.suffix_array.size();
  result.isa.resize(count);
  const auto thread_count = std::min<std::uint64_t>(requested_threads, count);
  if (thread_count <= 1 || count < (1U << 20)) {
    for (std::uint64_t row = 0; row < count; ++row) {
      const auto suffix = static_cast<std::uint64_t>(
          result.suffix_array[static_cast<std::size_t>(row)]);
      result.isa[static_cast<std::size_t>(suffix / sampling_rate)] = row;
    }
  } else {
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(thread_count));
    for (std::uint64_t worker = 0; worker < thread_count; ++worker) {
      const auto begin = count * worker / thread_count;
      const auto end = count * (worker + 1) / thread_count;
      workers.emplace_back([&, begin, end] {
        for (auto row = begin; row < end; ++row) {
          const auto suffix = static_cast<std::uint64_t>(
              result.suffix_array[static_cast<std::size_t>(row)]);
          result.isa[static_cast<std::size_t>(suffix / sampling_rate)] = row;
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
  }
  result.isa_seconds = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - isa_begin)
                           .count();

  if (retain_lcp) {
    const auto lcp_begin = std::chrono::steady_clock::now();
    result.lcp.assign(count, 0);
    std::uint64_t common = 0;
    // This is Kasai's carry invariant generalized to step K: moving from text
    // position p to p+K can reduce the known common prefix by at most K.
    for (std::uint64_t sample = 0; sample < count; ++sample) {
      const auto suffix = sample * sampling_rate;
      const auto row = result.isa[static_cast<std::size_t>(sample)];
      if (row != 0) {
        const auto previous = static_cast<std::uint64_t>(
            result.suffix_array[static_cast<std::size_t>(row - 1)]);
        if (suffix + common < text.size() &&
            previous + common < text.size()) {
          const auto remaining = static_cast<std::size_t>(std::min(
              text.size() - static_cast<std::size_t>(suffix + common),
              text.size() - static_cast<std::size_t>(previous + common)));
          common += LongestCommonPrefixBytes(
              text.data() + static_cast<std::size_t>(suffix + common),
              text.data() + static_cast<std::size_t>(previous + common),
              remaining);
        }
        result.lcp[static_cast<std::size_t>(row)] = common;
      }
      common = common > sampling_rate ? common - sampling_rate : 0;
    }
    result.lcp_seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - lcp_begin)
                             .count();
  }
  if (!retain_isa) {
    result.isa.clear();
  }
  return result;
}

}  // namespace

DivSufsortBuildResult<std::int32_t> BuildDivsufsort32(
    const std::vector<std::uint8_t>& text, std::uint32_t sampling_rate,
    bool retain_lcp, bool retain_isa, std::uint32_t threads) {
  return BuildDivsufsort<std::int32_t>(text, sampling_rate, retain_lcp,
                                       retain_isa, threads);
}

DivSufsortBuildResult<std::int64_t> BuildDivsufsort64(
    const std::vector<std::uint8_t>& text, std::uint32_t sampling_rate,
    bool retain_lcp, bool retain_isa, std::uint32_t threads) {
  return BuildDivsufsort<std::int64_t>(text, sampling_rate, retain_lcp,
                                       retain_isa, threads);
}

}  // namespace sufkit::detail
