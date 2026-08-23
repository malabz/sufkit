#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sufkit/export.hpp>

namespace sufkit {

using SequenceId = std::uint32_t;
using Position = std::uint64_t;

struct SequenceRecord {
    std::string name;
    std::string description;
    std::string sequence;
};

struct SequenceInfo {
    SequenceId id = 0;
    std::string name;
    std::string description;
    Position length = 0;
    Position global_offset = 0;
    Position ambiguous_bases = 0;
};

enum class IndexKind : std::uint8_t {
    suffix_array = 1,
    fm_index = 2
};

enum class SaBackend : std::uint8_t {
    auto_select = 0,
    divsufsort = 1,
    caps = 2
};

enum class CoordinateWidth : std::uint8_t {
    auto_select = 0,
    bits32 = 32,
    bits64 = 64
};

enum class SaAcceleration : std::uint8_t {
    none = 0,
    lcp = 1,
    lcp_child = 2,
    lcp_suffix_link = 3,
    full = 4
};

enum class SaLookupAcceleration : std::uint8_t {
    binary = 0,
    sapling_pwl = 1
};

enum class SaSearchAlgorithm : std::uint8_t {
    auto_select = 0,
    binary = 1,
    lcp_binary = 2,
    sapling_pwl = 3,
    child = 4
};

enum class MemSearchAlgorithm : std::uint8_t {
    auto_select = 0,
    baseline = 1,
    lcp = 2,
    child = 3,
    suffix_link = 4,
    full = 5
};

enum class FmBackend : std::uint8_t {
    sdsl_csa_wt_huff = 1,
    sdsl_csa_wt_balanced = 2,
    sdsl_csa_sada = 3
};

enum class StrandMode : std::uint8_t {
    forward = 0,
    reverse_complement = 1,
    both = 2
};

enum class Strand : std::uint8_t {
    forward = 0,
    reverse_complement = 1,
    both = 2
};

struct LearnedSaOptions {
    bool enabled = false;
    std::uint32_t k = 20;
    std::uint32_t memory_overhead_basis_points = 100;
    std::optional<std::uint32_t> bucket_bits;
};

struct SuffixArrayBuildStatistics {
    double sa_seconds = 0.0;
    double isa_seconds = 0.0;
    double lcp_seconds = 0.0;
    double child_seconds = 0.0;
    double learned_index_seconds = 0.0;
};

struct SuffixArrayBuildOptions {
    SaBackend backend = SaBackend::auto_select;
    CoordinateWidth coordinate_width = CoordinateWidth::auto_select;
    std::uint32_t threads = 1;
    SaAcceleration acceleration = SaAcceleration::lcp_suffix_link;
    LearnedSaOptions learned_index;
    SuffixArrayBuildStatistics* statistics = nullptr;
};

struct SaSearchStatistics {
    std::uint64_t suffix_comparisons = 0;
    std::uint64_t character_comparisons = 0;
    std::uint64_t gallop_probes = 0;
    std::uint64_t local_window_rows = 0;
    std::uint64_t local_window_rows_max = 0;
    std::uint64_t predictions = 0;
    std::uint64_t prediction_absolute_error_sum = 0;
    std::uint64_t prediction_absolute_error_max = 0;
    std::uint64_t full_binary_fallbacks = 0;
};

struct MemSearchStatistics {
    std::uint64_t lookup_calls = 0;
    std::uint64_t binary_lookup_calls = 0;
    std::uint64_t learned_lookup_calls = 0;
    std::uint64_t suffix_link_attempts = 0;
    std::uint64_t suffix_link_successes = 0;
    std::uint64_t suffix_link_fallbacks = 0;
    std::uint64_t previous_empty_lookups = 0;
    SaSearchStatistics lookup;
};

struct MemOptions {
    std::uint64_t min_length = 20;
    StrandMode strands = StrandMode::forward;
    MemSearchAlgorithm algorithm = MemSearchAlgorithm::auto_select;
    SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::auto_select;
    MemSearchStatistics* statistics = nullptr;
};

struct MemMatch {
    SequenceId sequence_id = 0;
    Position reference_position = 0;
    Position query_position = 0;
    std::uint64_t length = 0;
    Strand strand = Strand::forward;
};

struct MemResult {
    std::uint64_t total_matches = 0;
    std::vector<MemMatch> matches;
    bool truncated = false;
};

using MemCallback = std::function<void(const MemMatch&)>;

struct FmIndexBuildOptions {
    FmBackend backend = FmBackend::sdsl_csa_wt_huff;
};

struct LocateOptions {
    StrandMode strands = StrandMode::forward;
    std::optional<std::uint64_t> max_hits;
};

struct SuffixRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;

    std::uint64_t size() const noexcept { return end - begin; }
    bool empty() const noexcept { return begin == end; }
};

struct Match {
    SequenceId sequence_id = 0;
    Position position = 0;
    std::uint64_t length = 0;
    Strand strand = Strand::forward;
};

struct QueryResult {
    std::uint64_t total_hits = 0;
    std::vector<Match> hits;
    bool truncated = false;
};

struct SaveOptions {
    bool overwrite = false;
};

struct BackendDescriptor {
    std::string name;
    bool available = false;
    bool supports_threads = false;
    std::string implementation;
};

struct IndexInfo {
    IndexKind kind = IndexKind::suffix_array;
    std::string format_version = "1.0";
    std::string library_version;
    std::string backend;
    std::string backend_signature;
    std::string sdsl_version;
    std::uint8_t coordinate_width = 0;
    std::uint64_t sequence_count = 0;
    std::uint64_t total_bases = 0;
    std::uint64_t text_symbols = 0;
    std::uint64_t ambiguous_bases = 0;
    std::uint64_t fingerprint = 0;
    std::uint64_t serialized_bytes = 0;
    SaAcceleration sa_acceleration = SaAcceleration::none;
    std::uint64_t auxiliary_bytes = 0;
    SaLookupAcceleration sa_lookup_acceleration = SaLookupAcceleration::binary;
    std::uint64_t learned_index_bytes = 0;
    std::uint32_t learned_k = 0;
    std::uint32_t learned_bucket_bits = 0;
    std::uint32_t learned_memory_overhead_basis_points = 0;
};

enum class ErrorCode : std::uint8_t {
    invalid_input,
    io_error,
    unsupported_backend,
    corrupt_index,
    version_mismatch,
    build_failure
};

class SUFKIT_API Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message);
    ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

SUFKIT_API const char* to_string(IndexKind value) noexcept;
SUFKIT_API const char* to_string(SaBackend value) noexcept;
SUFKIT_API const char* to_string(SaAcceleration value) noexcept;
SUFKIT_API const char* to_string(SaLookupAcceleration value) noexcept;
SUFKIT_API const char* to_string(SaSearchAlgorithm value) noexcept;
SUFKIT_API const char* to_string(MemSearchAlgorithm value) noexcept;
SUFKIT_API const char* to_string(FmBackend value) noexcept;
SUFKIT_API const char* to_string(Strand value) noexcept;
SUFKIT_API const char* to_string(StrandMode value) noexcept;
SUFKIT_API const char* to_string(ErrorCode value) noexcept;

} // namespace sufkit
