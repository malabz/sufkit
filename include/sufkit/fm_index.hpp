#pragma once

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <sufkit/export.hpp>
#include <sufkit/genome_reference.hpp>
#include <sufkit/types.hpp>

namespace sufkit {

struct FmBatchOptions {
    StrandMode strands = StrandMode::forward;
    std::uint32_t batch_width = 0;
};

class SUFKIT_API FmIndex {
public:
    static FmIndex build(
        const GenomeReference& reference,
        const FmIndexBuildOptions& options = {});
    static FmIndex load(const std::filesystem::path& path);

    FmIndex(FmIndex&&) noexcept;
    FmIndex& operator=(FmIndex&&) noexcept;
    ~FmIndex();

    FmIndex(const FmIndex&) = delete;
    FmIndex& operator=(const FmIndex&) = delete;

    void save(
        const std::filesystem::path& path,
        const SaveOptions& options = {}) const;

    SuffixRange equal_range(std::string_view pattern) const;
    std::vector<SuffixRange> equal_range_batch(
        const std::vector<std::string_view>& patterns,
        std::uint32_t batch_width = 0) const;
    std::uint64_t count(
        std::string_view pattern,
        StrandMode strands = StrandMode::forward) const;
    std::vector<std::uint64_t> count_batch(
        const std::vector<std::string_view>& patterns,
        const FmBatchOptions& options = {}) const;
    QueryResult locate(
        std::string_view pattern,
        const LocateOptions& options = {}) const;

    SequenceInfo sequence_info(SequenceId id) const;
    IndexInfo info() const;

private:
    struct Impl;
    explicit FmIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace sufkit
