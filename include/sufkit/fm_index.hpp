// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/genome_reference.hpp>
#include <sufkit/types.hpp>

/** @file
 *  @brief SDSL compressed-suffix-array exact search API.
 */

namespace sufkit {

/**
 * @ingroup fm_index
 * @brief Batched FM count orientation and state-chunk configuration.
 */
struct FmBatchOptions {
  /** Orientation(s) counted for every input pattern. */
  StrandMode strands = StrandMode::kForward;
  /** Active states per chunk; zero selects 16, valid explicit range 1–256. */
  std::uint32_t batch_width = 0;
};

/**
 * @ingroup fm_index
 * @brief Move-only fixed SDSL CSA wrapper for compressed exact search.
 *
 * SDSL owns construction, backward search, SA sampling, position recovery,
 * and native payload serialization. Const query methods are concurrent.
 */
class SUFKIT_API FmIndex {
 public:
  /**
   * Build one fixed SDSL CSA type from a normalized reference.
   * @param reference Reference copied into the self-contained index.
   * @param options Fixed backend selector; Huffman is default.
   * @return Queryable immutable index.
   * @throws Error for invalid reference, unavailable backend, or SDSL build
   *         failure.
   */
  static FmIndex Build(const GenomeReference& reference,
                       const FmIndexBuildOptions& options = {});

  /**
   * @param path Self-contained FM index path.
   * @return Validated immutable FM index.
   */
  static FmIndex Load(const std::filesystem::path& path);

  /** @param other Source index whose implementation is transferred. */
  FmIndex(FmIndex&& other) noexcept;
  /**
   * @param other Source index whose implementation is transferred.
   * @return This object after transfer.
   */
  FmIndex& operator=(FmIndex&& other) noexcept;
  /** Destructor. */
  ~FmIndex();

  /** @param other Copy source; copying is intentionally disabled. */
  FmIndex(const FmIndex& other) = delete;
  /**
   * @param other Copy source; copying is intentionally disabled.
   * @return This object; the operation is deleted.
   */
  FmIndex& operator=(const FmIndex& other) = delete;

  /**
   * @param path Target `.sufidx` path.
   * @param options Overwrite policy; false by default.
   */
  void Save(const std::filesystem::path& path,
            const SaveOptions& options = {}) const;

  /** @param pattern A/C/G/T pattern. @return Forward half-open row range. */
  SuffixRange EqualRange(std::string_view pattern) const;

  /**
   * Find forward ranges for patterns in input order. @since 0.2.0
   * @param patterns Non-empty A/C/G/T patterns; one invalid value rejects
   *        the complete batch.
   * @param batch_width Zero for 16, otherwise 1–256.
   * @return Half-open ranges in exactly the input order.
   */
  std::vector<SuffixRange> EqualRangeBatch(
      const std::vector<std::string_view>& patterns,
      std::uint32_t batch_width = 0) const;

  /**
   * @param pattern A/C/G/T pattern.
   * @param strands Orientation(s) searched.
   * @return Complete deduplicated count.
   */
  std::uint64_t Count(std::string_view pattern,
                      StrandMode strands = StrandMode::kForward) const;

  /**
   * Count patterns in input order with fixed-width interleaving.
   * @since 0.2.0
   * @param patterns Non-empty A/C/G/T patterns; one invalid value rejects
   *        the complete batch.
   * @param options Strand mode and batch width.
   * @return Complete counts in input order.
   */
  std::vector<std::uint64_t> CountBatch(
      const std::vector<std::string_view>& patterns,
      const FmBatchOptions& options = {}) const;

  /**
   * @param pattern A/C/G/T pattern.
   * @param options Strand and retained-hit policy.
   * @return Deterministic contig-local locate result.
   */
  QueryResult Locate(std::string_view pattern,
                     const LocateOptions& options = {}) const;

  /** @param id Zero-based contig ID. @return Metadata copy. */
  SequenceInfo GetSequenceInfo(SequenceId id) const;
  /** @return Validated backend, version, size, and reference metadata. */
  IndexInfo GetInfo() const;

 private:
  struct Impl;
  explicit FmIndex(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace sufkit
