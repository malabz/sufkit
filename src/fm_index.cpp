// SPDX-License-Identifier: MIT

#include "sufkit/fm_index.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <sdsl/construct.hpp>
#include <sdsl/csa_wt.hpp>
#include <sdsl/io.hpp>
#include <sdsl/ram_fs.hpp>
#include <sdsl/suffix_array_algorithm.hpp>
#include <sdsl/version.hpp>
#include <sdsl/wt_blcd.hpp>
#include <sdsl/wt_epr.hpp>
#include <sdsl/wt_huff.hpp>

#include "genome_reference_internal.hpp"
#include "query.hpp"
#include "reference_data.hpp"
#include "serialization.hpp"
#include <sufkit/version.hpp>

namespace sufkit {
namespace {

static_assert(
    SDSL_VERSION == 30003,
    "sufkit 0.1.x requires SDSL 3.0.3 for native serialization compatibility");

using SdslFmHuffman = sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>;
using SdslFmBalanced = sdsl::csa_wt<sdsl::wt_blcd<>, 32, 64>;
using SdslFmEpr = sdsl::csa_wt<sdsl::wt_epr<8>, 32, 64>;
using SdslFmVariant = std::variant<SdslFmHuffman, SdslFmBalanced, SdslFmEpr>;

constexpr std::uint32_t kAutomaticBatchWidth = 16;
constexpr std::uint32_t kMaximumBatchWidth = 256;

enum class BatchOrientation : std::uint8_t {
  kForward = 0,
  kReverseComplement = 1,
};

// Query bytes are owned by one encoded-pattern array. Search descriptors keep
// only indices and orientation bits, so reverse-complement batch lanes do not
// require a second copy of every pattern.
struct BatchSearches {
  std::vector<std::size_t> pattern_indices;
  std::vector<std::size_t> result_indices;
  std::vector<BatchOrientation> orientations;

  void Reserve(std::size_t size) {
    pattern_indices.reserve(size);
    result_indices.reserve(size);
    orientations.reserve(size);
  }

  void Add(std::size_t pattern_index, std::size_t result_index,
           BatchOrientation orientation) {
    pattern_indices.push_back(pattern_index);
    result_indices.push_back(result_index);
    orientations.push_back(orientation);
  }

  std::size_t Size() const noexcept { return pattern_indices.size(); }
};

std::uint8_t Complement(std::uint8_t symbol) {
  // The canonical encoding is contiguous: A=2, C=3, G=4, T=5.
  return static_cast<std::uint8_t>(7 - symbol);
}

bool IsReverseComplementPalindrome(
    const std::vector<std::uint8_t>& pattern) {
  for (std::size_t left = 0, right = pattern.size(); left < right; ++left) {
    --right;
    if (pattern[left] != Complement(pattern[right])) {
      return false;
    }
  }
  return true;
}

std::uint8_t BatchSymbol(const std::vector<std::uint8_t>& pattern,
                         BatchOrientation orientation,
                         std::size_t logical_index) {
  if (orientation == BatchOrientation::kForward) {
    return pattern[logical_index];
  }
  return Complement(pattern[pattern.size() - 1 - logical_index]);
}

struct GlobalFmHit {
  Position position = 0;
  Strand strand = Strand::kForward;
};

bool GlobalFmHitLess(const GlobalFmHit& left, const GlobalFmHit& right) {
  return std::tie(left.position, left.strand) <
         std::tie(right.position, right.strand);
}

void RetainGlobalFmHit(std::vector<GlobalFmHit>& hits, GlobalFmHit hit,
                       const std::optional<std::uint64_t>& limit) {
  if (!limit) {
    hits.push_back(hit);
    return;
  }
  if (*limit == 0) {
    return;
  }
  if (hits.size() < *limit) {
    hits.push_back(hit);
    std::push_heap(hits.begin(), hits.end(), GlobalFmHitLess);
    return;
  }
  if (GlobalFmHitLess(hit, hits.front())) {
    std::pop_heap(hits.begin(), hits.end(), GlobalFmHitLess);
    hits.back() = hit;
    std::push_heap(hits.begin(), hits.end(), GlobalFmHitLess);
  }
}

QueryResult FinalizeFmMatches(std::vector<GlobalFmHit> hits,
                              std::uint64_t total_hits,
                              const detail::ReferenceData& reference,
                              std::uint64_t pattern_length) {
  std::sort(hits.begin(), hits.end(), GlobalFmHitLess);

  // Merge global coordinates before touching contig metadata. Exact hits with
  // identical coordinates on opposite strands retain both-strand provenance.
  std::size_t output_size = 0;
  for (std::size_t index = 0; index < hits.size(); ++index) {
    if (output_size != 0 &&
        hits[output_size - 1].position == hits[index].position) {
      if (hits[output_size - 1].strand != hits[index].strand) {
        hits[output_size - 1].strand = Strand::kBoth;
      }
      continue;
    }
    if (output_size != index) {
      hits[output_size] = hits[index];
    }
    ++output_size;
  }
  hits.resize(output_size);

  const auto& starts = reference.contig_starts;
  const auto& lengths = reference.contig_lengths;
  if (!hits.empty() && (starts.empty() || starts.size() != lengths.size())) {
    throw Error(ErrorCode::kCorruptIndex,
                "FM-index contig coordinate metadata is invalid");
  }
  std::vector<Match> matches;
  matches.reserve(hits.size());
  std::size_t contig = 0;
  for (const auto& hit : hits) {
    while (contig + 1 < starts.size() && starts[contig + 1] <= hit.position) {
      ++contig;
    }
    if (hit.position < starts[contig]) {
      throw Error(ErrorCode::kCorruptIndex,
                  "SDSL CSA hit is outside a reference contig");
    }
    const auto local = hit.position - starts[contig];
    if (local > lengths[contig] ||
        pattern_length > lengths[contig] - local) {
      throw Error(ErrorCode::kCorruptIndex,
                  "SDSL CSA hit is outside a reference contig");
    }
    matches.push_back(
        {static_cast<SequenceId>(contig), local, pattern_length, hit.strand});
  }

  QueryResult result;
  result.total_hits = total_hits;
  result.hits = std::move(matches);
  result.truncated = result.hits.size() < result.total_hits;
  return result;
}

detail::StoredBackend StoredBackendFor(FmBackend backend) {
  switch (backend) {
    case FmBackend::kSdslCsaWtHuff:
      return detail::StoredBackend::kSdslCsaWtHuff;
    case FmBackend::kSdslCsaWtBalanced:
      return detail::StoredBackend::kSdslCsaWtBalanced;
    case FmBackend::kSdslCsaWtEpr:
      return detail::StoredBackend::kSdslCsaWtEpr;
    case FmBackend::kSdslCsaSada:
      throw Error(ErrorCode::kUnsupportedBackend,
                  "sdsl-csa-sada remains unavailable in the basic FM-index "
                  "optimization branch");
  }
  throw Error(ErrorCode::kUnsupportedBackend, "unknown FM-index backend");
}

std::uint32_t EffectiveBatchWidth(std::uint32_t requested) {
  if (requested == 0) {
    return kAutomaticBatchWidth;
  }
  if (requested > kMaximumBatchWidth) {
    throw Error(ErrorCode::kInvalidInput,
                "FM batch width must be in [1,256] or zero for automatic");
  }
  return requested;
}

detail::ReferenceData MetadataCopy(const detail::ReferenceData& source) {
  detail::ReferenceData result;
  result.sequences = source.sequences;
  result.contig_starts = source.contig_starts;
  result.contig_lengths = source.contig_lengths;
  result.total_bases = source.total_bases;
  result.ambiguous_bases = source.ambiguous_bases;
  result.fingerprint = source.fingerprint;
  return result;
}

template <class Csa>
void ValidateCsa(const Csa& csa, std::uint64_t expected_size, ErrorCode code) {
  if (csa.size() != expected_size) {
    throw Error(code, "SDSL CSA has an unexpected logical text size");
  }
  if (csa.sigma < 2 || csa.comp2char[0] != detail::kSentinel) {
    throw Error(code,
                "SDSL CSA alphabet does not contain the expected sentinel");
  }
}

template <class Csa>
void ConstructCsa(Csa& csa, const std::vector<std::uint8_t>& input) {
  // Byte zero is deliberately absent: SDSL appends the sole sentinel itself.
  const auto ram_file = sdsl::ram_file_name(
      sdsl::util::to_string(sdsl::util::pid()) + "_" +
      sdsl::util::to_string(sdsl::util::id()));
  struct RamFileGuard {
    std::string name;
    ~RamFileGuard() noexcept {
      try {
        sdsl::ram_fs::remove(name);
      } catch (...) {
        // Cleanup must not replace the original construction exception.
      }
    }
  } guard{ram_file};
  sdsl::cache_config cache(true, "@");
  cache.delete_data = true;
  struct CacheFileGuard {
    sdsl::cache_config& cache;
    std::array<std::string, 5> known_files;
    ~CacheFileGuard() noexcept {
      try {
        sdsl::util::delete_all_files(cache.file_map);
      } catch (...) {
        // Cleanup must not replace the original construction exception.
      }
      // SDSL registers a cache file only after finishing the construction
      // stage that creates it. Remove the deterministic names as well so an
      // exception between creation and registration cannot leak a RAM file.
      for (const auto& file : known_files) {
        try {
          sdsl::remove(file);
        } catch (...) {
          // Cleanup must not replace the original construction exception.
        }
      }
    }
  } cache_guard{
      cache,
      {sdsl::cache_file_name(sdsl::conf::KEY_TEXT, cache),
       sdsl::cache_file_name(sdsl::conf::KEY_SA, cache),
       sdsl::cache_file_name(sdsl::conf::KEY_BWT, cache),
       sdsl::cache_file_name(sdsl::conf::KEY_ISA, cache),
       sdsl::cache_file_name(sdsl::conf::KEY_SAMPLE_CHAR, cache)}};
  try {
    sdsl::ram_fs::content_type encoded;
    encoded.assign(input.begin(), input.end());
    sdsl::ram_fs::store(ram_file, std::move(encoded));
    sdsl::construct(csa, ram_file, cache, 1);
  } catch (const std::exception& error) {
    throw Error(
        ErrorCode::kBuildFailure,
        std::string("SDSL csa_wt construction failed: ") + error.what());
  }
  ValidateCsa(csa, input.size() + 1, ErrorCode::kBuildFailure);
}

template <class Csa>
SuffixRange RangeFor(const Csa& csa, const std::vector<std::uint8_t>& pattern) {
  typename Csa::size_type begin = 0;
  typename Csa::size_type end = 0;
  const auto occurrences = sdsl::backward_search(
      csa, 0, csa.size() - 1, pattern.begin(), pattern.end(), begin, end);
  if (occurrences == 0) {
    return {0, 0};
  }
  return {static_cast<std::uint64_t>(begin),
          static_cast<std::uint64_t>(begin + occurrences)};
}

template <class Csa, class Receiver>
void VisitRangeBatchFor(
    const Csa& csa, const std::vector<std::vector<std::uint8_t>>& patterns,
    const BatchSearches& searches, std::uint32_t batch_width,
    Receiver&& receiver) {
  if (searches.Size() == 0) {
    return;
  }

  const auto width = std::min<std::size_t>(batch_width, searches.Size());
  // The hot mutable lane state is structure-of-arrays. The workspace is sized
  // to the requested width and reused for every chunk rather than clearing a
  // fixed 256-lane object for small batches.
  std::vector<typename Csa::size_type> left(width);
  std::vector<typename Csa::size_type> right(width);
  std::vector<std::size_t> remaining(width);
  std::vector<std::uint8_t> matched(width);
  std::vector<std::size_t> active_lanes;
  std::vector<std::size_t> next_active_lanes;
  active_lanes.reserve(width);
  next_active_lanes.reserve(width);

  for (std::size_t chunk_begin = 0; chunk_begin < searches.Size();
       chunk_begin += width) {
    const auto chunk_end = std::min(searches.Size(), chunk_begin + width);
    const auto chunk_size = chunk_end - chunk_begin;
    active_lanes.clear();
    for (std::size_t local = 0; local < chunk_size; ++local) {
      const auto search_index = chunk_begin + local;
      left[local] = 0;
      right[local] = csa.size() - 1;
      remaining[local] =
          patterns[searches.pattern_indices[search_index]].size();
      matched[local] = 1;
      if (remaining[local] != 0) {
        active_lanes.push_back(local);
      }
    }

    // Empty and completed lanes are compacted out after each symbol, so later
    // passes touch only searches that can still advance.
    while (!active_lanes.empty()) {
      next_active_lanes.clear();
      for (const auto local : active_lanes) {
        const auto search_index = chunk_begin + local;
        const auto pattern_index = searches.pattern_indices[search_index];
        const auto& pattern = patterns[pattern_index];
        const auto symbol = BatchSymbol(
            pattern, searches.orientations[search_index], --remaining[local]);
        typename Csa::size_type next_left = 0;
        typename Csa::size_type next_right = 0;
        const auto occurrences = sdsl::backward_search(
            csa, left[local], right[local],
            static_cast<typename Csa::char_type>(symbol), next_left,
            next_right);
        if (occurrences == 0) {
          matched[local] = 0;
          continue;
        }
        left[local] = next_left;
        right[local] = next_right;
        if (remaining[local] != 0) {
          next_active_lanes.push_back(local);
        }
      }
      active_lanes.swap(next_active_lanes);
    }

    for (std::size_t local = 0; local < chunk_size; ++local) {
      const auto search_index = chunk_begin + local;
      SuffixRange range;
      if (matched[local] != 0) {
        range = {static_cast<std::uint64_t>(left[local]),
                 static_cast<std::uint64_t>(right[local] + 1)};
      }
      receiver(searches.result_indices[search_index], range);
    }
  }
}

template <class Csa>
std::vector<SuffixRange> RangeBatchFor(
    const Csa& csa, const std::vector<std::vector<std::uint8_t>>& patterns,
    const BatchSearches& searches, std::uint32_t batch_width,
    std::size_t result_size) {
  std::vector<SuffixRange> result(result_size);
  VisitRangeBatchFor(csa, patterns, searches, batch_width,
                     [&](std::size_t index, SuffixRange range) {
                       result[index] = range;
                     });
  return result;
}

template <class Csa>
void CountBatchFor(const Csa& csa,
                   const std::vector<std::vector<std::uint8_t>>& patterns,
                   const BatchSearches& searches, std::uint32_t batch_width,
                   std::vector<std::uint64_t>& counts) {
  VisitRangeBatchFor(csa, patterns, searches, batch_width,
                     [&](std::size_t index, SuffixRange range) {
                       counts[index] += range.Size();
                     });
}

IndexInfo BuiltInfo(const detail::ReferenceData& data, const SdslFmVariant& csa,
                    detail::StoredBackend backend) {
  IndexInfo info;
  info.kind = IndexKind::kFmIndex;
  info.library_version = SUFKIT_VERSION_STRING;
  info.backend = detail::StoredBackendName(backend);
  info.backend_signature = detail::StoredBackendSignature(backend);
  info.sdsl_version = sdsl::sdsl_version;
  info.coordinate_width = 64;
  info.sequence_count = data.sequences.size();
  info.total_bases = data.total_bases;
  info.text_symbols = std::visit(
      [](const auto& index) {
        return static_cast<std::uint64_t>(index.size());
      },
      csa);
  info.ambiguous_bases = data.ambiguous_bases;
  info.fingerprint = data.fingerprint;
  return info;
}

}  // namespace

struct FmIndex::Impl {
  detail::ReferenceData reference;
  SdslFmVariant csa;
  detail::StoredBackend backend = detail::StoredBackend::kSdslCsaWtHuff;
  IndexInfo index_info;

  SuffixRange Range(const std::vector<std::uint8_t>& pattern) const {
    return std::visit(
        [&](const auto& index) { return RangeFor(index, pattern); }, csa);
  }

  std::vector<SuffixRange> RangeBatch(
      const std::vector<std::vector<std::uint8_t>>& patterns,
      const BatchSearches& searches, std::uint32_t batch_width,
      std::size_t result_size) const {
    return std::visit(
        [&](const auto& index) {
          return RangeBatchFor(index, patterns, searches, batch_width,
                               result_size);
        },
        csa);
  }

  void CountBatch(const std::vector<std::vector<std::uint8_t>>& patterns,
                  const BatchSearches& searches, std::uint32_t batch_width,
                  std::vector<std::uint64_t>& counts) const {
    std::visit(
        [&](const auto& index) {
          CountBatchFor(index, patterns, searches, batch_width, counts);
        },
        csa);
  }

  void Collect(SuffixRange interval, Strand strand,
               const std::optional<std::uint64_t>& limit,
               std::vector<GlobalFmHit>& output) const {
    std::visit(
        [&](const auto& index) {
          for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
            const auto global = static_cast<std::uint64_t>(index[row]);
            RetainGlobalFmHit(output, {global, strand}, limit);
          }
        },
        csa);
  }
};

FmIndex::FmIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FmIndex::FmIndex(FmIndex&&) noexcept = default;
FmIndex& FmIndex::operator=(FmIndex&&) noexcept = default;
FmIndex::~FmIndex() = default;

FmIndex FmIndex::Build(const GenomeReference& reference,
                       const FmIndexBuildOptions& options) {
  if (reference.impl_->data.encoded.empty()) {
    throw Error(ErrorCode::kInvalidInput, "reference text is empty");
  }
  if (std::find(reference.impl_->data.encoded.begin(),
                reference.impl_->data.encoded.end(),
                detail::kSentinel) != reference.impl_->data.encoded.end()) {
    throw Error(ErrorCode::kInvalidInput,
                "SDSL construction input contains a zero symbol");
  }

  auto impl = std::make_unique<Impl>();
  impl->reference = MetadataCopy(reference.impl_->data);
  impl->backend = StoredBackendFor(options.backend);
  switch (options.backend) {
    case FmBackend::kSdslCsaWtHuff:
      impl->csa.emplace<SdslFmHuffman>();
      break;
    case FmBackend::kSdslCsaWtBalanced:
      impl->csa.emplace<SdslFmBalanced>();
      break;
    case FmBackend::kSdslCsaWtEpr:
      impl->csa.emplace<SdslFmEpr>();
      break;
    case FmBackend::kSdslCsaSada:
      throw Error(ErrorCode::kUnsupportedBackend,
                  "sdsl-csa-sada is not implemented");
  }
  std::visit(
      [&](auto& index) { ConstructCsa(index, reference.impl_->data.encoded); },
      impl->csa);
  impl->index_info = BuiltInfo(impl->reference, impl->csa, impl->backend);
  return FmIndex(std::move(impl));
}

SuffixRange FmIndex::EqualRange(std::string_view pattern) const {
  return impl_->Range(detail::EncodePattern(pattern));
}

std::vector<SuffixRange> FmIndex::EqualRangeBatch(
    const std::vector<std::string_view>& patterns,
    std::uint32_t batch_width) const {
  const auto width = EffectiveBatchWidth(batch_width);
  std::vector<std::vector<std::uint8_t>> encoded;
  encoded.reserve(patterns.size());
  for (const auto pattern : patterns) {
    encoded.push_back(detail::EncodePattern(pattern));
  }
  BatchSearches searches;
  searches.Reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    searches.Add(index, index, BatchOrientation::kForward);
  }
  return impl_->RangeBatch(encoded, searches, width, patterns.size());
}

std::uint64_t FmIndex::Count(std::string_view pattern,
                             StrandMode strands) const {
  const auto encoded = detail::EncodePattern(pattern);
  switch (strands) {
    case StrandMode::kForward:
      return impl_->Range(encoded).Size();
    case StrandMode::kReverseComplement:
      return impl_->Range(detail::ReverseComplement(encoded)).Size();
    case StrandMode::kBoth: {
      const auto reverse = detail::ReverseComplement(encoded);
      // A reverse-complement palindrome denotes one search, not two identical
      // sets of occurrences.
      if (encoded == reverse) {
        return impl_->Range(encoded).Size();
      }
      return impl_->Range(encoded).Size() + impl_->Range(reverse).Size();
    }
  }
  throw Error(ErrorCode::kInvalidInput,
              "invalid strand mode for FM count query");
}

std::vector<std::uint64_t> FmIndex::CountBatch(
    const std::vector<std::string_view>& patterns,
    const FmBatchOptions& options) const {
  const auto width = EffectiveBatchWidth(options.batch_width);
  if (options.strands != StrandMode::kForward &&
      options.strands != StrandMode::kReverseComplement &&
      options.strands != StrandMode::kBoth) {
    throw Error(ErrorCode::kInvalidInput,
                "invalid strand mode for FM batch query");
  }
  std::vector<std::vector<std::uint8_t>> encoded;
  encoded.reserve(patterns.size());
  for (const auto pattern : patterns) {
    encoded.push_back(detail::EncodePattern(pattern));
  }

  BatchSearches searches;
  searches.Reserve(encoded.size() * 2);
  if (options.strands == StrandMode::kForward) {
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      searches.Add(index, index, BatchOrientation::kForward);
    }
  } else {
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      if (options.strands == StrandMode::kReverseComplement) {
        searches.Add(index, index, BatchOrientation::kReverseComplement);
      } else if (options.strands == StrandMode::kBoth) {
        searches.Add(index, index, BatchOrientation::kForward);
        // Omitting the duplicate palindrome search preserves scalar Count()
        // semantics while each remaining lane maps back to its input result.
        if (!IsReverseComplementPalindrome(encoded[index])) {
          searches.Add(index, index,
                       BatchOrientation::kReverseComplement);
        }
      }
    }
  }

  std::vector<std::uint64_t> counts(patterns.size(), 0);
  impl_->CountBatch(encoded, searches, width, counts);
  return counts;
}

QueryResult FmIndex::Locate(std::string_view pattern,
                            const LocateOptions& options) const {
  const auto encoded = detail::EncodePattern(pattern);
  SuffixRange first_range;
  SuffixRange second_range;
  Strand first_strand = Strand::kForward;
  Strand second_strand = Strand::kForward;
  bool has_second_range = false;
  switch (options.strands) {
    case StrandMode::kForward:
      first_range = impl_->Range(encoded);
      break;
    case StrandMode::kReverseComplement:
      first_range = impl_->Range(detail::ReverseComplement(encoded));
      first_strand = Strand::kReverseComplement;
      break;
    case StrandMode::kBoth: {
      const auto reverse = detail::ReverseComplement(encoded);
      if (encoded == reverse) {
        // Palindromic hits are represented once and explicitly retain
        // both-strand provenance.
        first_range = impl_->Range(encoded);
        first_strand = Strand::kBoth;
      } else {
        first_range = impl_->Range(encoded);
        second_range = impl_->Range(reverse);
        second_strand = Strand::kReverseComplement;
        has_second_range = true;
      }
      break;
    }
    default:
      throw Error(ErrorCode::kInvalidInput,
                  "invalid strand mode for FM locate query");
  }

  const auto total_hits =
      first_range.Size() +
      (has_second_range ? second_range.Size() : std::uint64_t{0});
  // max_hits=0 promises an exact count but no materialized coordinates. Both
  // backward searches above are still required; CSA position recovery is not.
  if (options.max_hits && *options.max_hits == 0) {
    return FinalizeFmMatches({}, total_hits, impl_->reference, encoded.size());
  }

  std::optional<std::uint64_t> retained_limit;
  if (options.max_hits && *options.max_hits < total_hits) {
    retained_limit = *options.max_hits;
  }
  std::vector<GlobalFmHit> hits;
  const auto expected_size = retained_limit ? *retained_limit : total_hits;
  if (expected_size <= hits.max_size()) {
    hits.reserve(static_cast<std::size_t>(expected_size));
  }
  impl_->Collect(first_range, first_strand, retained_limit, hits);
  if (has_second_range) {
    impl_->Collect(second_range, second_strand, retained_limit, hits);
  }
  return FinalizeFmMatches(std::move(hits), total_hits, impl_->reference,
                           encoded.size());
}

SequenceInfo FmIndex::GetSequenceInfo(SequenceId id) const {
  const auto index = static_cast<std::size_t>(id);
  if (index >= impl_->reference.sequences.size()) {
    throw Error(ErrorCode::kInvalidInput, "sequence id is out of range");
  }
  return impl_->reference.sequences[index];
}

IndexInfo FmIndex::GetInfo() const { return impl_->index_info; }

void FmIndex::Save(const std::filesystem::path& path,
                   const SaveOptions& options) const {
  detail::ContainerSpec spec;
  spec.kind = IndexKind::kFmIndex;
  spec.backend = impl_->backend;
  spec.coordinate_width = 64;
  spec.sdsl_major = SDSL_VERSION_MAJOR;
  spec.sdsl_minor = SDSL_VERSION_MINOR;
  spec.sdsl_patch = SDSL_VERSION_PATCH;
  spec.sequence_count = impl_->reference.sequences.size();
  spec.total_bases = impl_->reference.total_bases;
  spec.text_symbols = std::visit(
      [](const auto& index) {
        return static_cast<std::uint64_t>(index.size());
      },
      impl_->csa);
  spec.ambiguous_bases = impl_->reference.ambiguous_bases;
  spec.fingerprint = impl_->reference.fingerprint;

  detail::WriteContainer(
      path, options, spec,
      {{detail::SectionType::kMetadata,
        [&](std::ostream& output) {
          detail::WriteMetadata(output, impl_->reference);
        }},
       {detail::SectionType::kSdslCsa, [&](std::ostream& output) {
          std::visit([&](const auto& index) { index.serialize(output); },
                     impl_->csa);
        }}});
}

FmIndex FmIndex::Load(const std::filesystem::path& path) {
  const auto container = detail::ReadContainer(path);
  if (container.spec.kind != IndexKind::kFmIndex) {
    throw Error(ErrorCode::kCorruptIndex, "index does not contain an FM-index");
  }
  if (container.spec.backend == detail::StoredBackend::kSdslCsaSada) {
    throw Error(ErrorCode::kUnsupportedBackend,
                "sdsl-csa-sada is not implemented");
  }
  if (container.spec.backend != detail::StoredBackend::kSdslCsaWtHuff &&
      container.spec.backend != detail::StoredBackend::kSdslCsaWtBalanced &&
      container.spec.backend != detail::StoredBackend::kSdslCsaWtEpr) {
    throw Error(ErrorCode::kUnsupportedBackend, "unsupported FM-index payload");
  }
  if (container.spec.coordinate_width != 64) {
    throw Error(ErrorCode::kCorruptIndex,
                "SDSL CSA coordinate width must be 64 bits");
  }
  if (container.spec.sdsl_major != SDSL_VERSION_MAJOR ||
      container.spec.sdsl_minor != SDSL_VERSION_MINOR ||
      container.spec.sdsl_patch != SDSL_VERSION_PATCH) {
    throw Error(ErrorCode::kVersionMismatch,
                "SDSL serialization version mismatch");
  }

  auto impl = std::make_unique<Impl>();
  impl->backend = container.spec.backend;
  if (impl->backend == detail::StoredBackend::kSdslCsaWtHuff) {
    impl->csa.emplace<SdslFmHuffman>();
  } else if (impl->backend == detail::StoredBackend::kSdslCsaWtBalanced) {
    impl->csa.emplace<SdslFmBalanced>();
  } else {
    impl->csa.emplace<SdslFmEpr>();
  }
  impl->reference = detail::ReadMetadata(container);
  // The section stream prevents SDSL from consuming bytes belonging to a
  // following container section when loading a corrupt payload.
  auto input =
      detail::OpenSectionStream(container, detail::SectionType::kSdslCsa);
  try {
    std::visit(
        [&](auto& index) { index.load(static_cast<std::istream&>(*input)); },
        impl->csa);
  } catch (const std::exception& error) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string("cannot load SDSL CSA payload: ") + error.what());
  }
  if (input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                "SDSL CSA section has trailing bytes");
  }
  std::visit(
      [&](const auto& index) {
        ValidateCsa(index, container.spec.text_symbols,
                    ErrorCode::kCorruptIndex);
      },
      impl->csa);
  impl->index_info = detail::IndexInfoFromContainer(container);
  return FmIndex(std::move(impl));
}

}  // namespace sufkit
