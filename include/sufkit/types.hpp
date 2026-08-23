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
    sdsl_csa_sada = 3,
    sdsl_csa_wt_epr = 4
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

struct SuffixArrayBuildOptions {
    SaBackend backend = SaBackend::auto_select;
    CoordinateWidth coordinate_width = CoordinateWidth::auto_select;
    std::uint32_t threads = 1;
    SaAcceleration acceleration = SaAcceleration::full;
};

struct MemOptions {
    std::uint64_t min_length = 20;
    StrandMode strands = StrandMode::forward;
    MemSearchAlgorithm algorithm = MemSearchAlgorithm::auto_select;
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
SUFKIT_API const char* to_string(MemSearchAlgorithm value) noexcept;
SUFKIT_API const char* to_string(FmBackend value) noexcept;
SUFKIT_API const char* to_string(Strand value) noexcept;
SUFKIT_API const char* to_string(StrandMode value) noexcept;
SUFKIT_API const char* to_string(ErrorCode value) noexcept;

} // namespace sufkit
