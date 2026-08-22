#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

#include <sufkit/export.hpp>
#include <sufkit/genome_reference.hpp>
#include <sufkit/types.hpp>

namespace sufkit {

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
    std::uint64_t count(
        std::string_view pattern,
        StrandMode strands = StrandMode::forward) const;
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
