// SPDX-License-Identifier: MIT

#include "sufkit/fm_index.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sdsl/construct.hpp>
#include <sdsl/csa_wt.hpp>
#include <sdsl/io.hpp>
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
  std::string encoded(reinterpret_cast<const char*>(input.data()),
                      input.size());
  try {
    sdsl::construct_im(csa, std::move(encoded), 1);
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

template <class Csa>
std::vector<SuffixRange> RangeBatchFor(
    const Csa& csa, const std::vector<std::vector<std::uint8_t>>& patterns,
    std::uint32_t batch_width) {
  std::vector<SuffixRange> result(patterns.size());
  if (patterns.empty()) {
    return result;
  }

  struct State {
    typename Csa::size_type left = 0;
    typename Csa::size_type right = 0;
    std::size_t remaining = 0;
    bool empty = false;
  };

  // Each lane advances by one symbol per pass. Finished and empty lanes remain
  // stable while the active lanes continue their independent backward search.
  const auto width = static_cast<std::size_t>(batch_width);
  for (std::size_t chunk_begin = 0; chunk_begin < patterns.size();
       chunk_begin += width) {
    const auto chunk_end = std::min(patterns.size(), chunk_begin + width);
    const auto chunk_size = chunk_end - chunk_begin;
    std::array<State, kMaximumBatchWidth> states{};
    for (std::size_t local = 0; local < chunk_size; ++local) {
      states[local] = State{};
      states[local].right = csa.size() - 1;
      states[local].remaining = patterns[chunk_begin + local].size();
    }

    bool active = true;
    while (active) {
      active = false;
      for (std::size_t local = 0; local < chunk_size; ++local) {
        auto& state = states[local];
        if (state.empty || state.remaining == 0) {
          continue;
        }
        active = true;
        const auto& pattern = patterns[chunk_begin + local];
        const auto symbol = pattern[--state.remaining];
        typename Csa::size_type next_left = 0;
        typename Csa::size_type next_right = 0;
        const auto occurrences =
            sdsl::backward_search(csa, state.left, state.right,
                                  static_cast<typename Csa::char_type>(symbol),
                                  next_left, next_right);
        if (occurrences == 0) {
          state.empty = true;
          state.remaining = 0;
        } else {
          state.left = next_left;
          state.right = next_right;
        }
      }
    }

    for (std::size_t local = 0; local < chunk_size; ++local) {
      const auto& state = states[local];
      if (!state.empty) {
        result[chunk_begin + local] = {
            static_cast<std::uint64_t>(state.left),
            static_cast<std::uint64_t>(state.right + 1)};
      }
    }
  }
  return result;
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
      std::uint32_t batch_width) const {
    return std::visit(
        [&](const auto& index) {
          return RangeBatchFor(index, patterns, batch_width);
        },
        csa);
  }

  std::uint64_t Collect(const std::vector<std::uint8_t>& pattern, Strand strand,
                        const LocateOptions& options,
                        std::vector<Match>& output) const {
    return std::visit(
        [&](const auto& index) -> std::uint64_t {
          const auto interval = RangeFor(index, pattern);
          for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
            const auto global = static_cast<std::uint64_t>(index[row]);
            // This validation also prevents sentinels and separators from ever
            // escaping as public contig-local coordinates.
            const auto mapped = detail::MapGlobalPosition(
                reference.sequences, global, pattern.size());
            if (!mapped) {
              throw Error(ErrorCode::kCorruptIndex,
                          "SDSL CSA hit is outside a reference contig");
            }
            detail::RetainMatch(
                output,
                {mapped->first, mapped->second,
                 static_cast<std::uint64_t>(pattern.size()), strand},
                options);
          }
          return interval.Size();
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
  return impl_->RangeBatch(encoded, width);
}

std::uint64_t FmIndex::Count(std::string_view pattern,
                             StrandMode strands) const {
  const auto encoded = detail::EncodePattern(pattern);
  if (strands == StrandMode::kForward) {
    return impl_->Range(encoded).Size();
  }
  const auto reverse = detail::ReverseComplement(encoded);
  if (strands == StrandMode::kReverseComplement) {
    return impl_->Range(reverse).Size();
  }
  // A reverse-complement palindrome denotes one search, not two identical
  // sets of occurrences.
  if (encoded == reverse) {
    return impl_->Range(encoded).Size();
  }
  return impl_->Range(encoded).Size() + impl_->Range(reverse).Size();
}

std::vector<std::uint64_t> FmIndex::CountBatch(
    const std::vector<std::string_view>& patterns,
    const FmBatchOptions& options) const {
  const auto width = EffectiveBatchWidth(options.batch_width);
  std::vector<std::vector<std::uint8_t>> encoded;
  encoded.reserve(patterns.size());
  for (const auto pattern : patterns) {
    encoded.push_back(detail::EncodePattern(pattern));
  }

  std::vector<std::vector<std::uint8_t>> searches;
  std::vector<std::size_t> owners;
  searches.reserve(encoded.size() * 2);
  owners.reserve(encoded.size() * 2);
  if (options.strands == StrandMode::kForward) {
    searches = std::move(encoded);
    for (std::size_t index = 0; index < searches.size(); ++index) {
      owners.push_back(index);
    }
  } else {
    for (std::size_t index = 0; index < encoded.size(); ++index) {
      const auto reverse = detail::ReverseComplement(encoded[index]);
      if (options.strands == StrandMode::kReverseComplement) {
        searches.push_back(reverse);
        owners.push_back(index);
      } else if (options.strands == StrandMode::kBoth) {
        searches.push_back(encoded[index]);
        owners.push_back(index);
        // Omitting the duplicate palindrome search preserves scalar Count()
        // semantics while owners maps every remaining lane to its input.
        if (encoded[index] != reverse) {
          searches.push_back(reverse);
          owners.push_back(index);
        }
      } else {
        throw Error(ErrorCode::kInvalidInput,
                    "invalid strand mode for FM batch query");
      }
    }
  }

  const auto ranges = impl_->RangeBatch(searches, width);
  std::vector<std::uint64_t> counts(patterns.size(), 0);
  for (std::size_t index = 0; index < ranges.size(); ++index) {
    counts[owners[index]] += ranges[index].Size();
  }
  return counts;
}

QueryResult FmIndex::Locate(std::string_view pattern,
                            const LocateOptions& options) const {
  const auto encoded = detail::EncodePattern(pattern);
  std::vector<Match> matches;
  std::uint64_t total_hits = 0;
  if (options.strands == StrandMode::kForward) {
    total_hits = impl_->Collect(encoded, Strand::kForward, options, matches);
  } else {
    const auto reverse = detail::ReverseComplement(encoded);
    if (options.strands == StrandMode::kReverseComplement) {
      total_hits =
          impl_->Collect(reverse, Strand::kReverseComplement, options, matches);
    } else if (encoded == reverse) {
      // Palindromic hits are represented once and explicitly retain both-strand
      // provenance.
      total_hits = impl_->Collect(encoded, Strand::kBoth, options, matches);
    } else {
      total_hits = impl_->Collect(encoded, Strand::kForward, options, matches);
      total_hits +=
          impl_->Collect(reverse, Strand::kReverseComplement, options, matches);
    }
  }
  return detail::FinalizeMatches(std::move(matches), total_hits);
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
