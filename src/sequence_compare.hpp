// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <emmintrin.h>

namespace sufkit::detail {

struct ByteComparison {
  int order = 0;
  std::size_t lcp = 0;
  std::size_t comparisons = 0;
};

// Compare a text suffix against a pattern. Matching the whole pattern returns
// equality even when the text has remaining bytes, which is the ordering
// contract needed by suffix-array equal_range.
inline ByteComparison ComparePatternBytes(const std::uint8_t* text,
                                          std::size_t text_size,
                                          const std::uint8_t* pattern,
                                          std::size_t pattern_size,
                                          std::size_t known_lcp = 0) {
  // LCP-aware callers pass a proven common prefix. Clamp defensively so a
  // future invalid hint cannot underflow comparison accounting or advance a
  // pointer beyond the shorter input.
  const auto initial_lcp = std::min(known_lcp, pattern_size);
  if (initial_lcp > text_size) {
    return {-1, text_size, 0};
  }
  std::size_t index = initial_lcp;
  const auto scalar_count =
      std::min(pattern_size - index, static_cast<std::size_t>(8));
  const auto scalar_end = index + scalar_count;
  while (index < scalar_end) {
    if (index >= text_size) {
      return {-1, index, index - initial_lcp};
    }
    const auto left = text[index];
    const auto right = pattern[index];
    if (left != right) {
      return {left < right ? -1 : 1, index, index - initial_lcp + 1};
    }
    ++index;
  }

  while (pattern_size - index >= 16 && text_size - index >= 16) {
    const auto left = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(text + index));
    const auto right = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(pattern + index));
    const auto equal = _mm_cmpeq_epi8(left, right);
    const auto mask = static_cast<unsigned>(_mm_movemask_epi8(equal));
    if (mask != 0xFFFFU) {
      const auto mismatch = static_cast<std::size_t>(
          __builtin_ctz(static_cast<unsigned>(~mask) & 0xFFFFU));
      const auto position = index + mismatch;
      return {text[position] < pattern[position] ? -1 : 1, position,
              position - initial_lcp + 1};
    }
    index += 16;
  }

  while (index < pattern_size) {
    if (index >= text_size) {
      return {-1, index, index - initial_lcp};
    }
    const auto left = text[index];
    const auto right = pattern[index];
    if (left != right) {
      return {left < right ? -1 : 1, index, index - initial_lcp + 1};
    }
    ++index;
  }
  return {0, index, index - initial_lcp};
}

inline std::size_t LongestCommonPrefixBytesLong(const std::uint8_t* left,
                                                const std::uint8_t* right,
                                                std::size_t length) {
  constexpr std::size_t kVectorBytes = 16;
  constexpr std::size_t kUnrolledBytes = 2 * kVectorBytes;
  constexpr unsigned kAllBytesEqual = 0xFFFFU;

  std::size_t index = 0;

  // Most candidate extensions stop immediately after their proven anchor.
  // Four scalar probes preserve that low-latency case while remaining short
  // enough that long equal runs reach the wider LCE loop quickly.
  const auto scalar_end = std::min(length, static_cast<std::size_t>(4));
  while (index < scalar_end) {
    if (left[index] != right[index]) {
      return index;
    }
    ++index;
  }

  // A shorter scalar prefix than ComparePatternBytes keeps two independent
  // cache-line streams in flight for long equal runs. The remaining-length
  // checks precede every load, so neither the unrolled loop nor the final
  // vector can read beyond [0, length).
  while (length - index >= kUnrolledBytes) {
    const auto left0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(left + index));
    const auto right0 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(right + index));
    const auto left1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(left + index + kVectorBytes));
    const auto right1 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(right + index + kVectorBytes));

    const auto mask0 = static_cast<unsigned>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(left0, right0)));
    const auto mask1 = static_cast<unsigned>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(left1, right1)));
    if (mask0 != kAllBytesEqual) {
      const auto mismatch = static_cast<std::size_t>(
          __builtin_ctz((~mask0) & kAllBytesEqual));
      return index + mismatch;
    }
    if (mask1 != kAllBytesEqual) {
      const auto mismatch = static_cast<std::size_t>(
          __builtin_ctz((~mask1) & kAllBytesEqual));
      return index + kVectorBytes + mismatch;
    }
    index += kUnrolledBytes;
  }

  if (length - index >= kVectorBytes) {
    const auto left_block = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(left + index));
    const auto right_block = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(right + index));
    const auto mask = static_cast<unsigned>(
        _mm_movemask_epi8(_mm_cmpeq_epi8(left_block, right_block)));
    if (mask != kAllBytesEqual) {
      const auto mismatch = static_cast<std::size_t>(
          __builtin_ctz((~mask) & kAllBytesEqual));
      return index + mismatch;
    }
    index += kVectorBytes;
  }

  while (index < length && left[index] == right[index]) {
    ++index;
  }
  return index;
}

inline std::size_t LongestCommonPrefixBytes(const std::uint8_t* left,
                                            const std::uint8_t* right,
                                            std::size_t length) {
  return ComparePatternBytes(left, length, right, length).lcp;
}

}  // namespace sufkit::detail
