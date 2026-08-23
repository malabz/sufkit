#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/types.hpp>

/** @file
 *  @brief Normalized multi-contig genome-reference input.
 */

namespace sufkit {

class SuffixArray;
class FmIndex;

/**
 * @ingroup genome_reference
 * @brief Validated, normalized, move-only reference used to build indexes.
 *
 * A/C/G/T is upper-cased and every other reference symbol becomes N. FASTA
 * record order defines SequenceId. The class owns no open input stream after
 * construction and is safe to read concurrently through const methods.
 */
class SUFKIT_API GenomeReference {
public:
    /**
     * Read plain or gzip FASTA through kseq/zlib.
     * @param path Input FASTA path.
     * @return Normalized reference.
     * @throws Error with invalid_input for invalid records or io_error for
     *         stream failures.
     */
    static GenomeReference from_fasta(const std::filesystem::path& path);

    /**
     * Validate and normalize in-memory records.
     * @param records Records with unique non-empty names and non-empty sequence.
     * @return Normalized reference.
     * @throws Error with invalid_input when the record contract is violated.
     */
    static GenomeReference from_records(std::vector<SequenceRecord> records);

    /** @param other Source reference whose implementation is transferred. */
    GenomeReference(GenomeReference&& other) noexcept;
    /**
     * @param other Source reference whose implementation is transferred.
     * @return This object after transfer.
     */
    GenomeReference& operator=(GenomeReference&& other) noexcept;
    /** Destructor. */
    ~GenomeReference();

    /** @param other Copy source; copying is intentionally disabled. */
    GenomeReference(const GenomeReference& other) = delete;
    /**
     * @param other Copy source; copying is intentionally disabled.
     * @return This object; the operation is deleted.
     */
    GenomeReference& operator=(const GenomeReference& other) = delete;

    /** @return Number of input-order contigs. */
    std::uint64_t sequence_count() const noexcept;
    /** @return Total biological bases, excluding separators/sentinel. */
    std::uint64_t total_bases() const noexcept;
    /** @return Number of symbols normalized to N. */
    std::uint64_t ambiguous_bases() const noexcept;
    /** @return Deterministic FNV-1a-64 normalized-content fingerprint. */
    std::uint64_t fingerprint() const noexcept;
    /**
     * @param id Zero-based input-order contig ID.
     * @return Immutable metadata copy.
     * @throws Error with invalid_input when id is out of range.
     */
    SequenceInfo sequence_info(SequenceId id) const;

private:
    struct Impl;
    explicit GenomeReference(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SuffixArray;
    friend class FmIndex;
};

} // namespace sufkit
