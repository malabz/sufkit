// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <sufkit/export.hpp>
#include <sufkit/genome_reference.hpp>
#include <sufkit/types.hpp>

/** @file
 *  @brief Standalone suffix-array exact and right-maximal-match search API.
 */

namespace sufkit {

/**
 * @ingroup suffix_array
 * @brief Move-only complete suffix array with optional ESA and learned data.
 *
 * The default build stores SA+ISA+LCP. Built or loaded objects are immutable;
 * const query methods may be called concurrently when caller-owned statistics
 * outputs are not shared unsafely.
 */
class SUFKIT_API SuffixArray {
 public:
  /**
   * Build a standalone SA from a normalized reference.
   * @param reference Reference whose data is copied into the index.
   * @param options Constructor, width, threads, auxiliary, and PWL options.
   * @return Complete initialized index.
   * @throws Error on invalid options, unavailable backend, allocation, or
   *         constructor failure.
   */
  static SuffixArray Build(const GenomeReference& reference,
                           const SuffixArrayBuildOptions& options = {});

  /**
   * Load and fully validate a suffix-array `.sufidx`.
   * @param path Self-contained index path.
   * @return Immutable queryable index.
   * @throws Error for I/O, kind, version, CRC, section, or semantic failure.
   */
  static SuffixArray Load(const std::filesystem::path& path);

  /** @param other Source index whose implementation is transferred. */
  SuffixArray(SuffixArray&& other) noexcept;
  /**
   * @param other Source index whose implementation is transferred.
   * @return This object after transfer.
   */
  SuffixArray& operator=(SuffixArray&& other) noexcept;
  /** Destructor. */
  ~SuffixArray();

  /** @param other Copy source; copying is intentionally disabled. */
  SuffixArray(const SuffixArray& other) = delete;
  /**
   * @param other Copy source; copying is intentionally disabled.
   * @return This object; the operation is deleted.
   */
  SuffixArray& operator=(const SuffixArray& other) = delete;

  /**
   * Atomically persist the self-contained index.
   * @param path Target `.sufidx` path.
   * @param options Overwrite policy; false by default.
   * @throws Error for existing target, I/O, serialization, or self-check
   *         failure.
   */
  void Save(const std::filesystem::path& path,
            const SaveOptions& options = {}) const;

  /**
   * @ingroup exact_search
   * Find the forward pattern row range using automatic binary/PWL selection.
   * @param pattern Non-empty case-insensitive A/C/G/T pattern.
   * @return Half-open matching range, or `[0,0)` when absent.
   * @throws Error with ErrorCode::kUnsupportedBackend for a sampled SA
   *         because complete matches span multiple residue-specific
   *         intervals.
   */
  SuffixRange EqualRange(std::string_view pattern) const;

  /**
   * @ingroup exact_search
   * Find the forward pattern row range with explicit SA algorithm control.
   * @param pattern Non-empty case-insensitive A/C/G/T pattern.
   * @param algorithm Required range-search algorithm.
   * @param statistics Optional caller-owned output reset/populated by call.
   * @return Half-open matching range, or `[0,0)` when absent.
   * @throws Error for invalid pattern, missing explicit capability, or a
   *         sampled SA whose result is not representable by one interval.
   */
  SuffixRange EqualRange(std::string_view pattern, SaSearchAlgorithm algorithm,
                         SaSearchStatistics* statistics = nullptr) const;

  /**
   * @ingroup exact_search
   * @param pattern Non-empty A/C/G/T pattern.
   * @param strands Orientation(s) searched.
   * @return Complete deduplicated hit count using automatic lookup.
   */
  std::uint64_t Count(std::string_view pattern,
                      StrandMode strands = StrandMode::kForward) const;

  /**
   * @ingroup exact_search
   * Count exact occurrences with explicit SA lookup control.
   * @param pattern Non-empty A/C/G/T pattern.
   * @param strands Orientation(s) searched.
   * @param algorithm SA range-search algorithm.
   * @param statistics Optional caller-owned aggregate work counters.
   * @return Complete deduplicated hit count.
   */
  std::uint64_t Count(std::string_view pattern, StrandMode strands,
                      SaSearchAlgorithm algorithm,
                      SaSearchStatistics* statistics = nullptr) const;

  /**
   * @ingroup exact_search
   * @param pattern Non-empty A/C/G/T pattern.
   * @param options Strand and retained-hit policy.
   * @return Deterministically sorted result using automatic lookup.
   */
  QueryResult Locate(std::string_view pattern,
                     const LocateOptions& options = {}) const;

  /**
   * @ingroup exact_search
   * Locate exact occurrences with explicit SA lookup control.
   * @param pattern Non-empty A/C/G/T pattern.
   * @param options Strand and retained-hit policy.
   * @param algorithm SA range-search algorithm.
   * @param statistics Optional caller-owned aggregate work counters.
   * @return Deterministically sorted result with complete total_hits.
   */
  QueryResult Locate(std::string_view pattern, const LocateOptions& options,
                     SaSearchAlgorithm algorithm,
                     SaSearchStatistics* statistics = nullptr) const;

  /**
   * @ingroup right_maximal_search
   * Stream every right-maximal exact match candidate in caller thread.
   * @param query Query whose non-ACGT symbols are hard breaks.
   * @param options Positive minimum length, strands, algorithms, statistics.
   * @param callback Invoked for each directional match; enumeration order is
   *        not stable across algorithms.
   * @throws Error for invalid options/capability. Callback exceptions pass
   *         through unchanged.
   */
  void ForEachRightMaximalMatch(std::string_view query,
                                const RightMaximalOptions& options,
                                const RightMaximalCallback& callback) const;

  /**
   * @ingroup right_maximal_search
   * Return deterministic query-first right-maximal exact matches.
   * @param query Query whose non-ACGT symbols are hard breaks.
   * @param options Search behavior and optional statistics.
   * @param max_matches Number of sorted matches retained; absent means all.
   * @return Complete total_matches plus retained sorted vector.
   */
  RightMaximalResult FindRightMaximalMatches(
      std::string_view query, const RightMaximalOptions& options = {},
      std::optional<std::uint64_t> max_matches = {}) const;

  /** @return Persisted ESA auxiliary layout. */
  SaAcceleration Acceleration() const noexcept;
  /** @return Binary or present PWL lookup capability. */
  SaLookupAcceleration LookupAcceleration() const noexcept;
  /** @return Text-position SA sampling rate; one denotes a complete SA. */
  std::uint32_t SamplingRate() const noexcept;

  /**
   * Read a global logical-text position stored at one SA row.
   * @param row Zero-based stored SA row in `[0,GetInfo().suffix_count)`.
   * @return Global encoded-text position, possibly separator/sentinel.
   * @throws Error with ErrorCode::kInvalidInput when row is out of range.
   */
  Position SuffixAt(std::uint64_t row) const;
  /** @param id Zero-based contig ID. @return Metadata copy. */
  SequenceInfo GetSequenceInfo(SequenceId id) const;
  /** @return Validated backend, format, reference, and auxiliary metadata. */
  IndexInfo GetInfo() const;

 private:
  struct Impl;
  explicit SuffixArray(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sufkit
