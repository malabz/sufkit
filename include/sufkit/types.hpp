// SPDX-License-Identifier: MIT

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
/** @defgroup right_maximal_search Right-maximal exact match search */
/** @defgroup persistence Persistence and inspection */
/** @defgroup backends Backend discovery */
/** @defgroup errors Errors */

/** Zero-based reference-record identifier in FASTA input order. */
using SequenceId = std::uint32_t;

/** Public zero-based reference or query position. */
using Position = std::uint64_t;

/** In-memory input record accepted by GenomeReference::FromRecords(). */
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
  kSuffixArray = 1,
  /** SDSL compressed suffix array used as an FM-index. */
  kFmIndex = 2
};

/** Standalone suffix-array constructor selection. */
enum class SaBackend : std::uint8_t {
  /**
   * Select CaPS only when compiled in, threads>1, and logical text has at
   * least 2^30 symbols; otherwise select divsufsort.
   */
  kAutoSelect = 0,
  /** Bundled serial libdivsufsort constructor. */
  kDivsufsort = 1,
  /** Bundled shared-memory parallel CaPS-SA constructor. @since 0.2.0 */
  kCaps = 2
};

/** Integer width used by a standalone-SA constructor. */
enum class CoordinateWidth : std::uint8_t {
  /** Choose the smallest width accepted by the effective constructor. */
  kAutoSelect = 0,
  /** Use a 32-bit backend-specific construction integer type. */
  kBits32 = 32,
  /** Use a 64-bit construction integer type. */
  kBits64 = 64
};

/** Physical width used by persisted and resident SA-style coordinates.
 *  @since 0.3.0
 */
enum class CoordinateStorageWidth : std::uint8_t {
  /** Select the narrowest profile-compatible representation. */
  kAutoSelect = 0,
  /** Native unsigned 32-bit coordinates. */
  kBits32 = 32,
  /** Split low-32/high-8 coordinates. */
  kBits40 = 40,
  /** Split low-32/high-16 coordinates. */
  kBits48 = 48,
  /** Native unsigned 64-bit coordinates. */
  kBits64 = 64
};

/** Resource policy applied to a standalone suffix-array index. @since 0.3.0 */
enum class SaResourceProfile : std::uint8_t {
  /** Preserve query acceleration and raw LCP; auto uses native coordinates. */
  kFast = 0,
  /** Keep complete SA+byte-coded LCP without persistent ISA/CHILD/PWL. */
  kLowMemory = 1
};

/** Physical representation of the persisted LCP array. @since 0.3.0 */
enum class SaLcpEncoding : std::uint8_t {
  /** No LCP is present. */
  kNone = 0,
  /** One native 32- or 64-bit integer per stored SA row. */
  kRaw = 1,
  /** One primary byte per row plus compact PLCP-range anchors. */
  kByteCoded = 2
};

/** Persisted enhanced-suffix-array layout. */
enum class SaAcceleration : std::uint8_t {
  /** Store only the encoded text and complete SA. */
  kNone = 0,
  /** Store SA+LCP. */
  kLcp = 1,
  /** Store SA+LCP+CHILD for explicit ESA navigation. */
  kLcpChild = 2,
  /** Store SA+ISA+LCP; default right-maximal-match query layout. */
  kLcpSuffixLink = 3,
  /** Store SA+ISA+LCP+CHILD. */
  kFull = 4
};

/** Persisted capability used to accelerate exact SA row lookup. */
enum class SaLookupAcceleration : std::uint8_t {
  /** Ordinary complete-SA search. */
  kBinary = 0,
  /** Optional Sapling-style piecewise-linear predictor. @since 0.2.0 */
  kSaplingPwl = 1
};

/** Algorithm used for one standalone-SA exact range lookup. */
enum class SaSearchAlgorithm : std::uint8_t {
  /** Use eligible PWL/Fast-prefix lookup, then LCP-aware binary search. */
  kAutoSelect = 0,
  /** Two ordinary suffix binary searches. */
  kBinary = 1,
  /** Binary search that reuses boundary common-prefix lengths. */
  kLcpBinary = 2,
  /** Explicit correctness-preserving PWL prediction and local search. */
  kSaplingPwl = 3,
  /** Explicit LCP+CHILD top-down traversal. */
  kChild = 4
};

/** Right-maximal exact match interval-discovery/reuse algorithm. */
enum class RightMaximalSearchAlgorithm : std::uint8_t {
  /** Choose suffix-link, then LCP, then baseline; never auto-select CHILD. */
  kAutoSelect = 0,
  /** Start every canonical query position with an SA root lookup. */
  kBaseline = 1,
  /** Use LCP-assisted interval/candidate work. */
  kLcp = 2,
  /** Use explicit LCP+CHILD traversal for each query position. */
  kChild = 3,
  /** Reuse intervals through ISA+LCP suffix links. */
  kSuffixLink = 4,
  /** Combine suffix-link reuse and explicit CHILD navigation. */
  kFull = 5
};

/** Maximal-match interval-discovery/reuse algorithm. @since 0.3.0 */
enum class MemSearchAlgorithm : std::uint8_t {
  /** Select the workload-specific best path; never auto-select CHILD. */
  kAutoSelect = 0,
  /** Start every canonical query anchor with an SA root lookup. */
  kBaseline = 1,
  /** Use LCP-assisted interval and candidate work. */
  kLcp = 2,
  /** Use explicit LCP+CHILD traversal for each query anchor. */
  kChild = 3,
  /** Reuse intervals through ISA+LCP suffix links. */
  kSuffixLink = 4,
  /** Combine suffix-link reuse and explicit CHILD navigation. */
  kFull = 5
};

/** Fixed SDSL compressed-suffix-array backend. */
enum class FmBackend : std::uint8_t {
  /** `csa_wt<wt_huff<>,32,64>`; compressed default. */
  kSdslCsaWtHuff = 1,
  /** `csa_wt<wt_blcd<>,32,64>`; explicit balanced comparison. */
  kSdslCsaWtBalanced = 2,
  /** Reserved unavailable `csa_sada` identity. */
  kSdslCsaSada = 3,
  /** `csa_wt<wt_epr<8>,32,64>`; speed-oriented DNA backend. */
  kSdslCsaWtEpr = 4
};

/** Orientations searched by exact or maximal-match operations. */
enum class StrandMode : std::uint8_t {
  /** Search the supplied sequence only. */
  kForward = 0,
  /** Search only its reverse complement. */
  kReverseComplement = 1,
  /** Search both orientations. */
  kBoth = 2
};

/** Orientation attached to one public result. */
enum class Strand : std::uint8_t {
  /** Forward orientation. */
  kForward = 0,
  /** Reverse-complement orientation. */
  kReverseComplement = 1,
  /** Exact-search coordinate shared by both orientations. */
  kBoth = 2
};

/** Optional Sapling-style PWL model construction options. @since 0.2.0 */
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

/** Optional phase timings written by SuffixArray::Build(). */
struct SuffixArrayBuildStatistics {
  /** Backend complete-SA construction plus optional in-adapter compaction. */
  double sa_seconds = 0.0;
  /** Validation and conversion from constructor output to resident storage. */
  double storage_compaction_seconds = 0.0;
  /** ISA construction wall time in seconds. */
  double isa_seconds = 0.0;
  /** LCP construction/retention wall time; algorithm is backend-dependent. */
  double lcp_seconds = 0.0;
  /** CHILD construction wall time in seconds. */
  double child_seconds = 0.0;
  /** Learned-model construction wall time in seconds. */
  double learned_index_seconds = 0.0;
  /** Nested CaPS timings; sa_seconds retains its historical aggregate scope. */
  double caps_construct_seconds = 0.0;
  double caps_output_allocation_seconds = 0.0;
  double text_prepare_seconds = 0.0;
  double lcp_finalize_seconds = 0.0;
  double prefix_directory_seconds = 0.0;
  double total_seconds = 0.0;
};

/** Standalone suffix-array construction configuration. */
struct SuffixArrayBuildOptions {
  /** Requested complete-SA constructor. */
  SaBackend backend = SaBackend::kAutoSelect;
  /** Requested construction-backend coordinate width. */
  CoordinateWidth coordinate_width = CoordinateWidth::kAutoSelect;
  /** Positive build thread count; divsufsort itself remains serial. */
  std::uint32_t threads = 1;
  /**
   * Keep suffixes whose text position is divisible by this positive value.
   * One stores the complete suffix array. Sampling reduces resident and
   * serialized SA memory, but not full-SA constructor peak memory.
   */
  std::uint32_t sampling_rate = 1;
  /** Persisted ESA layout; defaults to SA+ISA+LCP. */
  SaAcceleration acceleration = SaAcceleration::kLcpSuffixLink;
  /** Independent optional learned exact-lookup configuration. */
  LearnedSaOptions learned_index;
  /** Optional caller-owned mutable phase output, reset by build. */
  SuffixArrayBuildStatistics* statistics = nullptr;
  /** Requested resident/persisted coordinate width, independent of builder. */
  CoordinateStorageWidth storage_width =
      CoordinateStorageWidth::kAutoSelect;
  /**
   * Fast or complete-SA low-memory resource policy. Fast retains raw LCP;
   * Low-memory requires K=1, retains byte-coded LCP, and overrides
   * acceleration/learned settings so only SA+LCP remains.
   */
  SaResourceProfile resource_profile = SaResourceProfile::kFast;
  /** Optional synchronous calling-thread phase notification; null is silent.
   * Callbacks must not throw. They do not cancel an active constructor.
   */
  void (*stage_callback)(const char* stage, void* context) = nullptr;
  void* stage_context = nullptr;
};

/** @return The canonical complete-SA suffix-link/raw-LCP performance preset.
 *  @since 0.3.0
 */
SUFKIT_API SuffixArrayBuildOptions FastSuffixArrayBuildOptions();

/** @return The canonical complete-SA byte-coded-LCP low-memory preset.
 *  @since 0.3.0
 */
SUFKIT_API SuffixArrayBuildOptions LowMemorySuffixArrayBuildOptions();

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

/** Optional work counters for one right-maximal exact match operation. */
struct RightMaximalSearchStatistics {
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

/** Right-maximal exact match search configuration. */
struct RightMaximalOptions {
  /** Positive minimum reported match length. */
  std::uint64_t min_length = 20;
  /** Query orientations to enumerate. */
  StrandMode strands = StrandMode::kForward;
  /** Interval-discovery/reuse path. */
  RightMaximalSearchAlgorithm algorithm =
      RightMaximalSearchAlgorithm::kAutoSelect;
  /** Exact lookup used for initialization and suffix-link fallback. */
  SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::kAutoSelect;
  /** Optional caller-owned mutable work counters, reset by the call. */
  RightMaximalSearchStatistics* statistics = nullptr;
};

/**
 * One right-maximal exact match candidate.
 *
 * The current implementation guarantees exactness and that the match cannot
 * be extended to the right. It does not yet promise left maximality and is
 * therefore not a MEM.
 */
struct RightMaximalMatch {
  /** Matched reference contig. */
  SequenceId sequence_id = 0;
  /** Zero-based contig-local reference start. */
  Position reference_position = 0;
  /** Zero-based start in the original forward query. */
  Position query_position = 0;
  /** Match length in canonical bases. */
  std::uint64_t length = 0;
  /** Directional query orientation; results do not use Strand::kBoth. */
  Strand strand = Strand::kForward;
};

/** Deterministic vector result for right-maximal exact matches. */
struct RightMaximalResult {
  /** Complete number of matches before output retention. */
  std::uint64_t total_matches = 0;
  /** Retained query-first sorted matches. */
  std::vector<RightMaximalMatch> matches;
  /** True when retained matches are fewer than total_matches. */
  bool truncated = false;
};

/** Synchronous callback invoked by SuffixArray::ForEachRightMaximalMatch(). */
using RightMaximalCallback = std::function<void(const RightMaximalMatch&)>;

/** Two-sided maximal exact match search configuration. @since 0.3.0 */
struct MemOptions {
  /** Positive minimum reported match length. */
  std::uint64_t min_length = 20;
  /** Query orientations to enumerate. */
  StrandMode strands = StrandMode::kForward;
  /** Interval-discovery/reuse path. */
  MemSearchAlgorithm algorithm = MemSearchAlgorithm::kAutoSelect;
  /** Exact lookup used for initialization and suffix-link fallback. */
  SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::kAutoSelect;
  /** Optional MUMmer-style query skip multiplier; absent selects auto. */
  std::optional<std::uint32_t> skip_multiplier;
};

/** Reference-unique maximal match search configuration. @since 0.3.0 */
struct MamOptions {
  /** Positive minimum reported match length. */
  std::uint64_t min_length = 20;
  /** Query orientations to enumerate. */
  StrandMode strands = StrandMode::kForward;
  /** Interval-discovery/reuse path. */
  MemSearchAlgorithm algorithm = MemSearchAlgorithm::kAutoSelect;
  /** Exact lookup used for initialization and suffix-link fallback. */
  SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::kAutoSelect;
};

/** Generalized `(length,occurrences)` SMEM configuration. @since 0.3.0 */
struct SmemOptions {
  /** Positive minimum query-interval length. */
  std::uint64_t min_length = 20;
  /** Positive minimum number of occurrences in the joined reference. */
  std::uint64_t min_occurrences = 1;
  /** Query orientations to enumerate independently. */
  StrandMode strands = StrandMode::kForward;
  /** Interval-discovery/reuse path. */
  MemSearchAlgorithm algorithm = MemSearchAlgorithm::kAutoSelect;
  /** Exact lookup used for initialization and suffix-link fallback. */
  SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::kAutoSelect;
};

/** Strict reference- and query-unique MUM configuration. @since 0.3.0 */
struct MumOptions {
  /** Positive minimum reported match length. */
  std::uint64_t min_length = 20;
  /** Query orientations to enumerate independently. */
  StrandMode strands = StrandMode::kForward;
  /** Interval-discovery/reuse path. */
  MemSearchAlgorithm algorithm = MemSearchAlgorithm::kAutoSelect;
  /** Exact lookup used for initialization and suffix-link fallback. */
  SaSearchAlgorithm lookup_algorithm = SaSearchAlgorithm::kAutoSelect;
};

/** One directional two-sided maximal exact match. @since 0.3.0 */
struct MemMatch {
  /** Matched reference contig. */
  SequenceId sequence_id = 0;
  /** Zero-based contig-local reference start. */
  Position reference_position = 0;
  /** Zero-based start in the original forward query. */
  Position query_position = 0;
  /** Match length in canonical bases. */
  std::uint64_t length = 0;
  /** Directional orientation; never Strand::kBoth. */
  Strand strand = Strand::kForward;
};

/** One MEM whose matched string is unique in the reference. @since 0.3.0 */
struct MamMatch {
  /** Matched reference contig. */
  SequenceId sequence_id = 0;
  /** Zero-based contig-local reference start. */
  Position reference_position = 0;
  /** Zero-based start in the original forward query. */
  Position query_position = 0;
  /** Match length in canonical bases. */
  std::uint64_t length = 0;
  /** Directional orientation; never Strand::kBoth. */
  Strand strand = Strand::kForward;
};

/** One located occurrence of a generalized SMEM. @since 0.3.0 */
struct SmemMatch {
  /** Matched reference contig. */
  SequenceId sequence_id = 0;
  /** Zero-based contig-local reference start. */
  Position reference_position = 0;
  /** Zero-based start in the original forward query. */
  Position query_position = 0;
  /** Match length in canonical bases. */
  std::uint64_t length = 0;
  /** Number of occurrences of this SMEM in the joined reference. */
  std::uint64_t reference_occurrences = 0;
  /** Directional orientation; never Strand::kBoth. */
  Strand strand = Strand::kForward;
};

/** One strict reference- and query-unique maximal match. @since 0.3.0 */
struct MumMatch {
  /** Matched reference contig. */
  SequenceId sequence_id = 0;
  /** Zero-based contig-local reference start. */
  Position reference_position = 0;
  /** Zero-based start in the original forward query. */
  Position query_position = 0;
  /** Match length in canonical bases. */
  std::uint64_t length = 0;
  /** Directional orientation; never Strand::kBoth. */
  Strand strand = Strand::kForward;
};

/** Deterministic vector result for MEM search. @since 0.3.0 */
struct MemResult {
  /** Complete number of unique directional MEM tuples. */
  std::uint64_t total_matches = 0;
  /** Retained query-first sorted MEMs. */
  std::vector<MemMatch> matches;
  /** True when retained matches are fewer than total_matches. */
  bool truncated = false;
};

/** Deterministic vector result for reference-MAM search. @since 0.3.0 */
struct MamResult {
  /** Complete number of unique directional reference-MAM tuples. */
  std::uint64_t total_matches = 0;
  /** Retained query-first sorted reference-MAMs. */
  std::vector<MamMatch> matches;
  /** True when retained matches are fewer than total_matches. */
  bool truncated = false;
};

/** Deterministic coordinate-level generalized-SMEM result. @since 0.3.0 */
struct SmemResult {
  /** Complete number of unique directional query SMEM intervals. */
  std::uint64_t total_smems = 0;
  /** Complete number of located reference-coordinate tuples. */
  std::uint64_t total_matches = 0;
  /** Retained query-first sorted coordinate matches. */
  std::vector<SmemMatch> matches;
  /** True when retained matches are fewer than total_matches. */
  bool truncated = false;
};

/** Deterministic vector result for strict MUM search. @since 0.3.0 */
struct MumResult {
  /** Complete number of unique directional MUM tuples. */
  std::uint64_t total_matches = 0;
  /** Retained query-first sorted MUMs. */
  std::vector<MumMatch> matches;
  /** True when retained matches are fewer than total_matches. */
  bool truncated = false;
};

/** Synchronous callback invoked by SuffixArray::ForEachMem(). */
using MemCallback = std::function<void(const MemMatch&)>;
/** Synchronous callback invoked by SuffixArray::ForEachMam(). */
using MamCallback = std::function<void(const MamMatch&)>;
/** Synchronous callback invoked by SuffixArray::ForEachSmem(). */
using SmemCallback = std::function<void(const SmemMatch&)>;
/** Synchronous callback invoked by SuffixArray::ForEachMum(). */
using MumCallback = std::function<void(const MumMatch&)>;

/** Fixed FM-index construction configuration. */
struct FmIndexBuildOptions {
  /** SDSL CSA type to construct; Huffman is the stable default. */
  FmBackend backend = FmBackend::kSdslCsaWtHuff;
};

/** Exact locate orientation and retained-output configuration. */
struct LocateOptions {
  /** Query orientations to locate. */
  StrandMode strands = StrandMode::kForward;
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
  std::uint64_t Size() const noexcept { return end - begin; }
  /** @return True when begin equals end. */
  bool Empty() const noexcept { return begin == end; }
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
  Strand strand = Strand::kForward;
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
  IndexKind kind = IndexKind::kSuffixArray;
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
  /** Construction backend row width in bits. */
  std::uint8_t coordinate_width = 0;
  /** Physical resident/persisted width of the primary SA in bits. */
  std::uint8_t stored_coordinate_width = 0;
  /** Number of reference contigs. */
  std::uint64_t sequence_count = 0;
  /** Number of biological bases before separators/sentinel. */
  std::uint64_t total_bases = 0;
  /** Number of logical indexed symbols including the sentinel. */
  std::uint64_t text_symbols = 0;
  /** Number of stored suffix rows after optional text-position sampling. */
  std::uint64_t suffix_count = 0;
  /** Text-position sampling rate; one denotes a complete suffix array. */
  std::uint32_t sa_sampling_rate = 1;
  /** Number of normalized N bases. */
  std::uint64_t ambiguous_bases = 0;
  /** FNV-1a-64 normalized-content fingerprint. */
  std::uint64_t fingerprint = 0;
  /** Complete persisted file size; zero for an unsaved built index. */
  std::uint64_t serialized_bytes = 0;
  /** Persisted ESA layout for a standalone SA. */
  SaAcceleration sa_acceleration = SaAcceleration::kNone;
  /** Effective standalone-SA resource profile. */
  SaResourceProfile sa_resource_profile = SaResourceProfile::kFast;
  /** Physical LCP representation. */
  SaLcpEncoding lcp_encoding = SaLcpEncoding::kNone;
  /** Resident auxiliary payload bytes, including derived query directories. */
  std::uint64_t auxiliary_bytes = 0;
  /** Resident logical-text payload bytes. */
  std::uint64_t text_bytes = 0;
  /** Resident suffix-array payload bytes. */
  std::uint64_t sa_bytes = 0;
  /** Resident inverse-suffix-array payload bytes. */
  std::uint64_t isa_bytes = 0;
  /** Resident LCP payload bytes, including the derived guide. */
  std::uint64_t lcp_bytes = 0;
  /** Resident one-byte LCP primary payload, or zero for raw LCP. */
  std::uint64_t lcp_primary_bytes = 0;
  /** Number of compact long-LCP range anchors. */
  std::uint64_t lcp_overflow_anchors = 0;
  /** Resident long-LCP anchor payload bytes. */
  std::uint64_t lcp_overflow_bytes = 0;
  /** Resident derived long-LCP guide bytes. */
  std::uint64_t lcp_guide_bytes = 0;
  /** Resident CHILD payload bytes. */
  std::uint64_t child_bytes = 0;
  /** Sum of resident text, index structures, and derived query payloads. */
  std::uint64_t resident_core_bytes = 0;
  /** Learned lookup capability, independent of ESA layout. */
  SaLookupAcceleration sa_lookup_acceleration = SaLookupAcceleration::kBinary;
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
  kInvalidInput,
  /** Filesystem or stream operation failed. */
  kIoError,
  /** Recognized capability is unavailable or not implemented. */
  kUnsupportedBackend,
  /** Persisted data violates structural or semantic invariants. */
  kCorruptIndex,
  /** Format or native dependency serialization version is incompatible. */
  kVersionMismatch,
  /** Backend construction or correctness-gated benchmark failed. */
  kBuildFailure
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
  ErrorCode Code() const noexcept;

 private:
  ErrorCode code_;
};

/** @param value Index kind. @return Stable lowercase name. */
SUFKIT_API const char* ToString(IndexKind value) noexcept;
/** @param value SA constructor. @return Stable selector name. */
SUFKIT_API const char* ToString(SaBackend value) noexcept;
/** @param value Persisted coordinate width. @return Stable selector name. */
SUFKIT_API const char* ToString(CoordinateStorageWidth value) noexcept;
/** @param value SA resource profile. @return Stable selector name. */
SUFKIT_API const char* ToString(SaResourceProfile value) noexcept;
/** @param value LCP representation. @return Stable representation name. */
SUFKIT_API const char* ToString(SaLcpEncoding value) noexcept;
/** @param value SA auxiliary layout. @return Stable name. */
SUFKIT_API const char* ToString(SaAcceleration value) noexcept;
/** @param value SA lookup capability. @return Stable name. */
SUFKIT_API const char* ToString(SaLookupAcceleration value) noexcept;
/** @param value Exact SA search algorithm. @return Stable name. */
SUFKIT_API const char* ToString(SaSearchAlgorithm value) noexcept;
/** @param value Right-maximal search algorithm. @return Stable name. */
SUFKIT_API const char* ToString(RightMaximalSearchAlgorithm value) noexcept;
/** @param value MEM search algorithm. @return Stable name. */
SUFKIT_API const char* ToString(MemSearchAlgorithm value) noexcept;
/** @param value FM backend. @return Stable selector name. */
SUFKIT_API const char* ToString(FmBackend value) noexcept;
/** @param value Result orientation. @return `+`, `-`, or `both`. */
SUFKIT_API const char* ToString(Strand value) noexcept;
/** @param value Strand mode. @return Stable selector name. */
SUFKIT_API const char* ToString(StrandMode value) noexcept;
/** @param value Error category. @return Stable lowercase name. */
SUFKIT_API const char* ToString(ErrorCode value) noexcept;

/** @} */

}  // namespace sufkit
