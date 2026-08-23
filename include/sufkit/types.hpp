#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <sufkit/export.hpp>

/** @file
 *  @brief Public value types, options, results, enums, and errors.
 */

namespace sufkit {

/** @defgroup core_types Core types
 *  @brief Shared value types and configuration used by all indexes.
 *  @{
 */

/** @defgroup genome_reference Genome reference */
/** @defgroup suffix_array Suffix array */
/** @defgroup fm_index FM-index */
/** @defgroup exact_search Exact search */
/** @defgroup mem_search MEM search */
/** @defgroup persistence Persistence and inspection */
/** @defgroup backends Backend discovery */
/** @defgroup errors Errors */

/** Zero-based reference-record identifier in FASTA input order. */
using SequenceId = std::uint32_t;

/** Public zero-based reference or query position. */
using Position = std::uint64_t;

/** In-memory input record accepted by GenomeReference::from_records(). */
struct SequenceRecord {
    /** Non-empty name, unique within one reference. */
    std::string name;
    /** Optional FASTA header text following the first name token. */
    std::string description;
    /** Non-empty biological sequence; it is normalized to A/C/G/T/N. */
    std::string sequence;
};

/** Immutable metadata describing one normalized reference contig. */
struct SequenceInfo {
    /** Zero-based input-order identifier. */
    SequenceId id = 0;
    /** Unique FASTA name. */
    std::string name;
    /** Preserved FASTA description. */
    std::string description;
    /** Number of normalized biological bases, excluding separators. */
    Position length = 0;
    /** Offset of the contig in sufkit's encoded multi-contig logical text. */
    Position global_offset = 0;
    /** Number of input symbols normalized to N in this contig. */
    Position ambiguous_bases = 0;
};

/** Kind of index stored in a `.sufidx` container. */
enum class IndexKind : std::uint8_t {
    /** Complete standalone suffix array, optionally with ESA/PWL data. */
    suffix_array = 1,
    /** SDSL compressed suffix array used as an FM-index. */
    fm_index = 2
};

/** Standalone suffix-array constructor selection. */
enum class SaBackend : std::uint8_t {
    /**
     * Select CaPS only when compiled in, threads>1, and logical text has at
     * least 2^30 symbols; otherwise select divsufsort.
     */
    auto_select = 0,
    /** Bundled serial libdivsufsort constructor. */
    divsufsort = 1,
    /** Bundled shared-memory parallel CaPS-SA constructor. @since Unreleased */
    caps = 2
};

/** Integer width used for stored SA-style rows. */
enum class CoordinateWidth : std::uint8_t {
    /** Choose the smallest width representable by the effective constructor. */
    auto_select = 0,
    /** Use a 32-bit backend-specific SA integer type. */
    bits32 = 32,
    /** Use a 64-bit SA integer type. */
    bits64 = 64
};

/** Persisted enhanced-suffix-array layout. */
enum class SaAcceleration : std::uint8_t {
    /** Store only the encoded text and complete SA. */
    none = 0,
    /** Store SA+LCP. */
    lcp = 1,
    /** Store SA+LCP+CHILD for explicit ESA navigation. */
    lcp_child = 2,
    /** Store SA+ISA+LCP; this is the default MEM layout. */
    lcp_suffix_link = 3,
    /** Store SA+ISA+LCP+CHILD. */
    full = 4
};

/** Persisted capability used to accelerate exact SA row lookup. */
enum class SaLookupAcceleration : std::uint8_t {
    /** Ordinary complete-SA search. */
    binary = 0,
    /** Optional Sapling-style piecewise-linear predictor. @since Unreleased */
    sapling_pwl = 1
};

/** Algorithm used for one standalone-SA exact range lookup. */
enum class SaSearchAlgorithm : std::uint8_t {
    /** Use PWL only when available and pattern length is at least k; else binary. */
    auto_select = 0,
    /** Two ordinary suffix binary searches. */
    binary = 1,
    /** Binary search that reuses boundary common-prefix lengths. */
    lcp_binary = 2,
    /** Explicit correctness-preserving PWL prediction and local search. */
    sapling_pwl = 3,
    /** Explicit LCP+CHILD top-down traversal. */
    child = 4
};

/** MEM interval-discovery/reuse algorithm. */
enum class MemSearchAlgorithm : std::uint8_t {
    /** Choose suffix-link, then LCP, then baseline; never auto-select CHILD. */
    auto_select = 0,
    /** Start every canonical query position with an SA root lookup. */
    baseline = 1,
    /** Use LCP-assisted interval/candidate work. */
    lcp = 2,
    /** Use explicit LCP+CHILD traversal for each query position. */
    child = 3,
    /** Reuse intervals through ISA+LCP suffix links. */
    suffix_link = 4,
    /** Combine suffix-link reuse and explicit CHILD navigation. */
    full = 5
};

/** Fixed SDSL compressed-suffix-array backend. */
enum class FmBackend : std::uint8_t {
    /** `csa_wt<wt_huff<>,32,64>`; compressed default. */
    sdsl_csa_wt_huff = 1,
    /** `csa_wt<wt_blcd<>,32,64>`; explicit balanced comparison. */
    sdsl_csa_wt_balanced = 2,
    /** Reserved unavailable `csa_sada` identity. */
    sdsl_csa_sada = 3,
    /** `csa_wt<wt_epr<8>,32,64>`; speed-oriented DNA backend. */
    sdsl_csa_wt_epr = 4
};

/** Orientations searched by exact or MEM operations. */
enum class StrandMode : std::uint8_t {
    /** Search the supplied sequence only. */
    forward = 0,
    /** Search only its reverse complement. */
    reverse_complement = 1,
    /** Search both orientations. */
    both = 2
};

/** Orientation attached to one public result. */
enum class Strand : std::uint8_t {
    /** Forward orientation. */
    forward = 0,
    /** Reverse-complement orientation. */
    reverse_complement = 1,
    /** Exact-search coordinate shared by both orientations. */
    both = 2
};

/** Optional Sapling-style PWL model construction options. @since Unreleased */
struct LearnedSaOptions {
    /** Whether to construct and persist a learned lookup model. */
    bool enabled = false;
    /** Number of leading canonical bases encoded into the model key (1–31). */
    std::uint32_t k = 20;
    /** Serialized model budget in basis points of the raw SA payload. */
    std::uint32_t memory_overhead_basis_points = 100;
    /** Explicit bucket exponent; absent means choose from the memory budget. */
    std::optional<std::uint32_t> bucket_bits;
};

/** Optional phase timings written by SuffixArray::build(). */
struct SuffixArrayBuildStatistics {
    /** Complete-SA constructor wall time in seconds. */
    double sa_seconds = 0.0;
    /** ISA construction wall time in seconds. */
    double isa_seconds = 0.0;
    /** Kasai LCP construction wall time in seconds. */
    double lcp_seconds = 0.0;
    /** CHILD construction wall time in seconds. */
    double child_seconds = 0.0;
    /** Learned-model construction wall time in seconds. */
    double learned_index_seconds = 0.0;
};

/** Standalone suffix-array construction configuration. */
struct SuffixArrayBuildOptions {
    /** Requested complete-SA constructor. */
    SaBackend backend = SaBackend::auto_select;
    /** Requested stored row width. */
    CoordinateWidth coordinate_width = CoordinateWidth::auto_select;
    /** Positive build thread count; divsufsort itself remains serial. */
    std::uint32_t threads = 1;
    /** Persisted ESA layout; defaults to SA+ISA+LCP. */
    SaAcceleration acceleration = SaAcceleration::lcp_suffix_link;
    /** Independent optional learned exact-lookup configuration. */
    LearnedSaOptions learned_index;
    /** Optional caller-owned mutable phase output, reset by build. */
    SuffixArrayBuildStatistics* statistics = nullptr;
};

/** Optional work counters for one standalone-SA exact operation. */
struct SaSearchStatistics {
    /** Number of suffix/pattern comparisons. */
    std::uint64_t suffix_comparisons = 0;
    /** Number of encoded character comparisons. */
    std::uint64_t character_comparisons = 0;
    /** Number of exponential-bracketing probes. */
    std::uint64_t gallop_probes = 0;
    /** Total rows in learned local search windows. */
    std::uint64_t local_window_rows = 0;
    /** Largest learned local search window. */
    std::uint64_t local_window_rows_max = 0;
    /** Number of learned predictions. */
    std::uint64_t predictions = 0;
    /** Sum of absolute prediction errors in SA rows. */
    std::uint64_t prediction_absolute_error_sum = 0;
    /** Maximum absolute prediction error in SA rows. */
    std::uint64_t prediction_absolute_error_max = 0;
    /** Number of times a complete binary search fallback was used. */
    std::uint64_t full_binary_fallbacks = 0;
};

/** Optional work counters for one MEM operation. */
struct MemSearchStatistics {
    /** Total root/fallback exact lookup calls. */
    std::uint64_t lookup_calls = 0;
    /** Lookup calls executed by binary/LCP binary search. */
    std::uint64_t binary_lookup_calls = 0;
    /** Lookup calls executed through PWL prediction. */
    std::uint64_t learned_lookup_calls = 0;
    /** Suffix-link derivations attempted. */
    std::uint64_t suffix_link_attempts = 0;
    /** Suffix-link derivations reused successfully. */
    std::uint64_t suffix_link_successes = 0;
    /** Suffix-link derivations that required a root fallback. */
    std::uint64_t suffix_link_fallbacks = 0;
    /** Query positions reached after an empty previous interval. */
    std::uint64_t previous_empty_lookups = 0;
    /** Aggregated exact-lookup work counters. */
    SaSearchStatistics lookup;
};

/** MEM search configuration. */
struct MemOptions {
    /** Positive minimum reported match length. */
    std::uint64_t min_length = 20;
    /** Query orientations to enumerate. */
    StrandMode strands = StrandMode::forward;
    /** Interval-discovery/reuse path. */
    MemSearchAlgorithm algorithm = MemSearchAlgorithm::auto_select;
    /** Exact lookup used for initialization and suffix-link fallback. */
    SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::auto_select;
    /** Optional caller-owned mutable work counters, reset by the call. */
    MemSearchStatistics* statistics = nullptr;
};

/** One reference/query maximal exact match. */
struct MemMatch {
    /** Matched reference contig. */
    SequenceId sequence_id = 0;
    /** Zero-based contig-local reference start. */
    Position reference_position = 0;
    /** Zero-based start in the original forward query. */
    Position query_position = 0;
    /** Match length in canonical bases. */
    std::uint64_t length = 0;
    /** Directional query orientation; MEM results do not use Strand::both. */
    Strand strand = Strand::forward;
};

/** Deterministic vector MEM result. */
struct MemResult {
    /** Complete number of matches before output retention. */
    std::uint64_t total_matches = 0;
    /** Retained query-first sorted matches. */
    std::vector<MemMatch> matches;
    /** True when retained matches are fewer than total_matches. */
    bool truncated = false;
};

/** Synchronous callback invoked by SuffixArray::for_each_mem(). */
using MemCallback = std::function<void(const MemMatch&)>;

/** Fixed FM-index construction configuration. */
struct FmIndexBuildOptions {
    /** SDSL CSA type to construct; Huffman is the stable default. */
    FmBackend backend = FmBackend::sdsl_csa_wt_huff;
};

/** Exact locate orientation and retained-output configuration. */
struct LocateOptions {
    /** Query orientations to locate. */
    StrandMode strands = StrandMode::forward;
    /** Optional number of sorted hits to retain; absent means all. */
    std::optional<std::uint64_t> max_hits;
};

/** Half-open matching row range `[begin,end)`. */
struct SuffixRange {
    /** First matching row. */
    std::uint64_t begin = 0;
    /** One-past-last matching row. */
    std::uint64_t end = 0;

    /** @return Number of rows in the half-open range. */
    std::uint64_t size() const noexcept { return end - begin; }
    /** @return True when begin equals end. */
    bool empty() const noexcept { return begin == end; }
};

/** One exact reference-coordinate occurrence. */
struct Match {
    /** Matched reference contig. */
    SequenceId sequence_id = 0;
    /** Zero-based contig-local start. */
    Position position = 0;
    /** Exact pattern length. */
    std::uint64_t length = 0;
    /** Orientation, including `both` for merged exact palindrome hits. */
    Strand strand = Strand::forward;
};

/** Deterministic exact locate result. */
struct QueryResult {
    /** Complete deduplicated hit count before retention. */
    std::uint64_t total_hits = 0;
    /** Retained sequence/position/length/strand-sorted hits. */
    std::vector<Match> hits;
    /** True when retained hits are fewer than total_hits. */
    bool truncated = false;
};

/** Index-save publication configuration. */
struct SaveOptions {
    /** Whether an existing target may be atomically replaced. */
    bool overwrite = false;
};

/** One discoverable constructor/backend capability. */
struct BackendDescriptor {
    /** Stable public selector name. */
    std::string name;
    /** Whether construction/query support is compiled and usable. */
    bool available = false;
    /** Whether this capability consumes the build thread option. */
    bool supports_threads = false;
    /** Human-readable fixed implementation signature or unavailable reason. */
    std::string implementation;
};

/** Validated metadata reported for a built, loaded, or inspected index. */
struct IndexInfo {
    /** Stored index kind. */
    IndexKind kind = IndexKind::suffix_array;
    /** Outer `.sufidx` major.minor version. */
    std::string format_version = "1.0";
    /** sufkit writer version recorded in the file or current build. */
    std::string library_version;
    /** Stable stored backend name. */
    std::string backend;
    /** Exact fixed implementation/payload signature. */
    std::string backend_signature;
    /** Recorded SDSL version for FM payloads; empty for SA. */
    std::string sdsl_version;
    /** Stored row width in bits. */
    std::uint8_t coordinate_width = 0;
    /** Number of reference contigs. */
    std::uint64_t sequence_count = 0;
    /** Number of biological bases before separators/sentinel. */
    std::uint64_t total_bases = 0;
    /** Number of logical indexed symbols including the sentinel. */
    std::uint64_t text_symbols = 0;
    /** Number of normalized N bases. */
    std::uint64_t ambiguous_bases = 0;
    /** FNV-1a-64 normalized-content fingerprint. */
    std::uint64_t fingerprint = 0;
    /** Complete persisted file size; zero for an unsaved built index. */
    std::uint64_t serialized_bytes = 0;
    /** Persisted ESA layout for a standalone SA. */
    SaAcceleration sa_acceleration = SaAcceleration::none;
    /** Persisted ISA/LCP/CHILD section bytes. */
    std::uint64_t auxiliary_bytes = 0;
    /** Learned lookup capability, independent of ESA layout. */
    SaLookupAcceleration sa_lookup_acceleration = SaLookupAcceleration::binary;
    /** Persisted learned section bytes. */
    std::uint64_t learned_index_bytes = 0;
    /** Learned k, or zero when absent. */
    std::uint32_t learned_k = 0;
    /** Learned bucket exponent, or zero when absent. */
    std::uint32_t learned_bucket_bits = 0;
    /** Requested learned budget in basis points, or zero when absent. */
    std::uint32_t learned_memory_overhead_basis_points = 0;
};

/**
 * @ingroup errors
 * @brief Stable public error categories.
 */
enum class ErrorCode : std::uint8_t {
    /** Invalid reference, pattern, option, width, size, or coordinate. */
    invalid_input,
    /** Filesystem or stream operation failed. */
    io_error,
    /** Recognized capability is unavailable or not implemented. */
    unsupported_backend,
    /** Persisted data violates structural or semantic invariants. */
    corrupt_index,
    /** Format or native dependency serialization version is incompatible. */
    version_mismatch,
    /** Backend construction or correctness-gated benchmark failed. */
    build_failure
};

/**
 * @ingroup errors
 * @brief Exception thrown by public sufkit operations.
 */
class SUFKIT_API Error : public std::runtime_error {
public:
    /**
     * @param code Stable programmatic category.
     * @param message Human-readable diagnostic; callers should not parse it.
     */
    Error(ErrorCode code, const std::string& message);

    /** @return Stable category supplied at construction. */
    ErrorCode code() const noexcept;

private:
    ErrorCode code_;
};

/** @param value Index kind. @return Stable lowercase name. */
SUFKIT_API const char* to_string(IndexKind value) noexcept;
/** @param value SA constructor. @return Stable selector name. */
SUFKIT_API const char* to_string(SaBackend value) noexcept;
/** @param value SA auxiliary layout. @return Stable name. */
SUFKIT_API const char* to_string(SaAcceleration value) noexcept;
/** @param value SA lookup capability. @return Stable name. */
SUFKIT_API const char* to_string(SaLookupAcceleration value) noexcept;
/** @param value Exact SA search algorithm. @return Stable name. */
SUFKIT_API const char* to_string(SaSearchAlgorithm value) noexcept;
/** @param value MEM search algorithm. @return Stable name. */
SUFKIT_API const char* to_string(MemSearchAlgorithm value) noexcept;
/** @param value FM backend. @return Stable selector name. */
SUFKIT_API const char* to_string(FmBackend value) noexcept;
/** @param value Result orientation. @return `+`, `-`, or `both`. */
SUFKIT_API const char* to_string(Strand value) noexcept;
/** @param value Strand mode. @return Stable selector name. */
SUFKIT_API const char* to_string(StrandMode value) noexcept;
/** @param value Error category. @return Stable lowercase name. */
SUFKIT_API const char* to_string(ErrorCode value) noexcept;

/** @} */

} // namespace sufkit
