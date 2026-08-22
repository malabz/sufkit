#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <sufkit/export.hpp>
#include <sufkit/genome_reference.hpp>
#include <sufkit/types.hpp>

namespace sufkit {

class SUFKIT_API SuffixArray {
public:
    static SuffixArray build(
        const GenomeReference& reference,
        const SuffixArrayBuildOptions& options = {});
    static SuffixArray load(const std::filesystem::path& path);

    SuffixArray(SuffixArray&&) noexcept;
    SuffixArray& operator=(SuffixArray&&) noexcept;
    ~SuffixArray();

    SuffixArray(const SuffixArray&) = delete;
    SuffixArray& operator=(const SuffixArray&) = delete;

    void save(
        const std::filesystem::path& path,
        const SaveOptions& options = {}) const;

    SuffixRange equal_range(std::string_view pattern) const;
    std::uint64_t count(
        std::string_view pattern,
        StrandMode strands = StrandMode::forward) const;
    QueryResult locate(
        std::string_view pattern,
        const LocateOptions& options = {}) const;

    Position suffix_at(std::uint64_t row) const;
    SequenceInfo sequence_info(SequenceId id) const;
    IndexInfo info() const;

private:
    struct Impl;
    explicit SuffixArray(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace sufkit
