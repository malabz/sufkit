#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/types.hpp>

namespace sufkit {

class SuffixArray;
class FmIndex;

class SUFKIT_API GenomeReference {
public:
    static GenomeReference from_fasta(const std::filesystem::path& path);
    static GenomeReference from_records(std::vector<SequenceRecord> records);

    GenomeReference(GenomeReference&&) noexcept;
    GenomeReference& operator=(GenomeReference&&) noexcept;
    ~GenomeReference();

    GenomeReference(const GenomeReference&) = delete;
    GenomeReference& operator=(const GenomeReference&) = delete;

    std::uint64_t sequence_count() const noexcept;
    std::uint64_t total_bases() const noexcept;
    std::uint64_t ambiguous_bases() const noexcept;
    std::uint64_t fingerprint() const noexcept;
    SequenceInfo sequence_info(SequenceId id) const;

private:
    struct Impl;
    explicit GenomeReference(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;

    friend class SuffixArray;
    friend class FmIndex;
};

} // namespace sufkit

