// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace sufkit::detail {

// Benchmark-only summary. This private POD is serialized only between the
// right-maximal benchmark worker and its parent process.
struct SuffixLinkScanSummary {
  std::uint64_t attempts = 0;
  std::uint64_t budget_exhaustions = 0;
  std::uint64_t left_rows = 0;
  std::uint64_t right_rows = 0;
  std::uint64_t rows_p50 = 0;
  std::uint64_t rows_p95 = 0;
  std::uint64_t rows_p99 = 0;
  std::uint64_t rows_max = 0;
  std::uint64_t scan_nanoseconds = 0;
  double instrumented_wall_seconds = 0.0;
};

class SuffixLinkScanSink;

#if defined(SUFKIT_ENABLE_SUFFIX_LINK_DIAGNOSTICS)

class SuffixLinkScanSink {
 public:
  // A scanned row is one inspected LCP entry, including the terminating entry
  // whose value is below the target. Boundary-limited directions may scan zero.
  void Record(std::uint64_t left_rows, std::uint64_t right_rows,
              std::uint64_t scan_nanoseconds, bool budget_exhausted) {
    attempts_ = SaturatingAdd(attempts_, 1);
    if (budget_exhausted) {
      budget_exhaustions_ = SaturatingAdd(budget_exhaustions_, 1);
    }
    left_rows_ = SaturatingAdd(left_rows_, left_rows);
    right_rows_ = SaturatingAdd(right_rows_, right_rows);
    scan_nanoseconds_ =
        SaturatingAdd(scan_nanoseconds_, scan_nanoseconds);
    widths_.push_back(SaturatingAdd(left_rows, right_rows));
  }

  SuffixLinkScanSummary Summarize() const {
    SuffixLinkScanSummary result;
    result.attempts = attempts_;
    result.budget_exhaustions = budget_exhaustions_;
    result.left_rows = left_rows_;
    result.right_rows = right_rows_;
    result.scan_nanoseconds = scan_nanoseconds_;
    if (widths_.empty()) {
      return result;
    }
    auto sorted = widths_;
    std::sort(sorted.begin(), sorted.end());
    result.rows_p50 = Percentile(sorted, 50);
    result.rows_p95 = Percentile(sorted, 95);
    result.rows_p99 = Percentile(sorted, 99);
    result.rows_max = sorted.back();
    return result;
  }

 private:
  static std::uint64_t SaturatingAdd(std::uint64_t left,
                                     std::uint64_t right) noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
  }

  static std::uint64_t Percentile(const std::vector<std::uint64_t>& sorted,
                                  std::size_t percent) noexcept {
    // Nearest-rank percentile without overflowing percent * sample_count.
    const auto sample_count = sorted.size();
    const auto quotient = sample_count / 100;
    const auto remainder = sample_count % 100;
    const auto rank = quotient * percent +
                      (remainder * percent + 99) / 100;
    return sorted[std::max<std::size_t>(1, rank) - 1];
  }

  std::uint64_t attempts_ = 0;
  std::uint64_t budget_exhaustions_ = 0;
  std::uint64_t left_rows_ = 0;
  std::uint64_t right_rows_ = 0;
  std::uint64_t scan_nanoseconds_ = 0;
  std::vector<std::uint64_t> widths_;
};

inline thread_local SuffixLinkScanSink* suffix_link_scan_sink = nullptr;

class ScopedSuffixLinkScanSink {
 public:
  explicit ScopedSuffixLinkScanSink(SuffixLinkScanSink& sink) noexcept
      : previous_(suffix_link_scan_sink) {
    suffix_link_scan_sink = &sink;
  }

  ~ScopedSuffixLinkScanSink() { suffix_link_scan_sink = previous_; }

  ScopedSuffixLinkScanSink(const ScopedSuffixLinkScanSink&) = delete;
  ScopedSuffixLinkScanSink& operator=(const ScopedSuffixLinkScanSink&) = delete;

 private:
  SuffixLinkScanSink* previous_ = nullptr;
};

inline SuffixLinkScanSink* CurrentSuffixLinkScanSink() noexcept {
  return suffix_link_scan_sink;
}

#endif

}  // namespace sufkit::detail
