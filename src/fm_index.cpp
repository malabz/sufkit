#include <sufkit/fm_index.hpp>

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

#include <sufkit/version.hpp>

#include "genome_reference_internal.hpp"
#include "query.hpp"
#include "reference_data.hpp"
#include "serialization.hpp"

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

detail::StoredBackend stored_backend(FmBackend backend) {
    switch (backend) {
    case FmBackend::sdsl_csa_wt_huff:
        return detail::StoredBackend::sdsl_csa_wt_huff;
    case FmBackend::sdsl_csa_wt_balanced:
        return detail::StoredBackend::sdsl_csa_wt_balanced;
    case FmBackend::sdsl_csa_wt_epr:
        return detail::StoredBackend::sdsl_csa_wt_epr;
    case FmBackend::sdsl_csa_sada:
        throw Error(
            ErrorCode::unsupported_backend,
            "sdsl-csa-sada remains unavailable in the basic FM-index optimization branch");
    }
    throw Error(ErrorCode::unsupported_backend, "unknown FM-index backend");
}

std::uint32_t effective_batch_width(std::uint32_t requested) {
    if (requested == 0) return kAutomaticBatchWidth;
    if (requested > kMaximumBatchWidth) {
        throw Error(ErrorCode::invalid_input, "FM batch width must be in [1,256] or zero for automatic");
    }
    return requested;
}

detail::ReferenceData metadata_copy(const detail::ReferenceData& source) {
    detail::ReferenceData result;
    result.sequences = source.sequences;
    result.total_bases = source.total_bases;
    result.ambiguous_bases = source.ambiguous_bases;
    result.fingerprint = source.fingerprint;
    return result;
}

template <class Csa>
void validate_csa(const Csa& csa, std::uint64_t expected_size, ErrorCode code) {
    if (csa.size() != expected_size) {
        throw Error(code, "SDSL CSA has an unexpected logical text size");
    }
    if (csa.sigma < 2 || csa.comp2char[0] != detail::kSentinel) {
        throw Error(code, "SDSL CSA alphabet does not contain the expected sentinel");
    }
}

template <class Csa>
void construct_csa(Csa& csa, const std::vector<std::uint8_t>& input) {
    std::string encoded(
        reinterpret_cast<const char*>(input.data()),
        input.size());
    try {
        sdsl::construct_im(csa, std::move(encoded), 1);
    } catch (const std::exception& error) {
        throw Error(
            ErrorCode::build_failure,
            std::string("SDSL csa_wt construction failed: ") + error.what());
    }
    validate_csa(csa, input.size() + 1, ErrorCode::build_failure);
}

template <class Csa>
SuffixRange range_for(const Csa& csa, const std::vector<std::uint8_t>& pattern) {
    typename Csa::size_type begin = 0;
    typename Csa::size_type end = 0;
    const auto occurrences = sdsl::backward_search(
        csa,
        0,
        csa.size() - 1,
        pattern.begin(),
        pattern.end(),
        begin,
        end);
    if (occurrences == 0) return {0, 0};
    return {
        static_cast<std::uint64_t>(begin),
        static_cast<std::uint64_t>(begin + occurrences)};
}

template <class Csa>
std::vector<SuffixRange> range_batch_for(
    const Csa& csa,
    const std::vector<std::vector<std::uint8_t>>& patterns,
    std::uint32_t batch_width) {
    std::vector<SuffixRange> result(patterns.size());
    if (patterns.empty()) return result;

    struct State {
        typename Csa::size_type left = 0;
        typename Csa::size_type right = 0;
        std::size_t remaining = 0;
        bool empty = false;
    };

    const auto width = static_cast<std::size_t>(batch_width);
    for (std::size_t chunk_begin = 0; chunk_begin < patterns.size(); chunk_begin += width) {
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
                if (state.empty || state.remaining == 0) continue;
                active = true;
                const auto& pattern = patterns[chunk_begin + local];
                const auto symbol = pattern[--state.remaining];
                typename Csa::size_type next_left = 0;
                typename Csa::size_type next_right = 0;
                const auto occurrences = sdsl::backward_search(
                    csa,
                    state.left,
                    state.right,
                    static_cast<typename Csa::char_type>(symbol),
                    next_left,
                    next_right);
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

IndexInfo built_info(
    const detail::ReferenceData& data,
    const SdslFmVariant& csa,
    detail::StoredBackend backend) {
    IndexInfo info;
    info.kind = IndexKind::fm_index;
    info.library_version = SUFKIT_VERSION_STRING;
    info.backend = detail::stored_backend_name(backend);
    info.backend_signature = detail::stored_backend_signature(backend);
    info.sdsl_version = sdsl::sdsl_version;
    info.coordinate_width = 64;
    info.sequence_count = data.sequences.size();
    info.total_bases = data.total_bases;
    info.text_symbols = std::visit([](const auto& index) {
        return static_cast<std::uint64_t>(index.size());
    }, csa);
    info.ambiguous_bases = data.ambiguous_bases;
    info.fingerprint = data.fingerprint;
    return info;
}

} // namespace

struct FmIndex::Impl {
    detail::ReferenceData reference;
    SdslFmVariant csa;
    detail::StoredBackend backend = detail::StoredBackend::sdsl_csa_wt_huff;
    IndexInfo index_info;

    SuffixRange range(const std::vector<std::uint8_t>& pattern) const {
        return std::visit([&](const auto& index) {
            return range_for(index, pattern);
        }, csa);
    }

    std::vector<SuffixRange> range_batch(
        const std::vector<std::vector<std::uint8_t>>& patterns,
        std::uint32_t batch_width) const {
        return std::visit([&](const auto& index) {
            return range_batch_for(index, patterns, batch_width);
        }, csa);
    }

    std::uint64_t collect(
        const std::vector<std::uint8_t>& pattern,
        Strand strand,
        const LocateOptions& options,
        std::vector<Match>& output) const {
        return std::visit([&](const auto& index) -> std::uint64_t {
            const auto interval = range_for(index, pattern);
            for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
                const auto global = static_cast<std::uint64_t>(index[row]);
                const auto mapped = detail::map_global_position(
                    reference.sequences, global, pattern.size());
                if (!mapped) {
                    throw Error(ErrorCode::corrupt_index, "SDSL CSA hit is outside a reference contig");
                }
                detail::retain_match(output, {
                    mapped->first,
                    mapped->second,
                    static_cast<std::uint64_t>(pattern.size()),
                    strand}, options);
            }
            return interval.size();
        }, csa);
    }
};

FmIndex::FmIndex(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

FmIndex::FmIndex(FmIndex&&) noexcept = default;
FmIndex& FmIndex::operator=(FmIndex&&) noexcept = default;
FmIndex::~FmIndex() = default;

FmIndex FmIndex::build(
    const GenomeReference& reference,
    const FmIndexBuildOptions& options) {
    if (reference.impl_->data.encoded.empty()) {
        throw Error(ErrorCode::invalid_input, "reference text is empty");
    }
    if (std::find(
            reference.impl_->data.encoded.begin(),
            reference.impl_->data.encoded.end(),
            detail::kSentinel) != reference.impl_->data.encoded.end()) {
        throw Error(ErrorCode::invalid_input, "SDSL construction input contains a zero symbol");
    }

    auto impl = std::make_unique<Impl>();
    impl->reference = metadata_copy(reference.impl_->data);
    impl->backend = stored_backend(options.backend);
    switch (options.backend) {
    case FmBackend::sdsl_csa_wt_huff:
        impl->csa.emplace<SdslFmHuffman>();
        break;
    case FmBackend::sdsl_csa_wt_balanced:
        impl->csa.emplace<SdslFmBalanced>();
        break;
    case FmBackend::sdsl_csa_wt_epr:
        impl->csa.emplace<SdslFmEpr>();
        break;
    case FmBackend::sdsl_csa_sada:
        throw Error(ErrorCode::unsupported_backend, "sdsl-csa-sada is not implemented");
    }
    std::visit([&](auto& index) {
        construct_csa(index, reference.impl_->data.encoded);
    }, impl->csa);
    impl->index_info = built_info(impl->reference, impl->csa, impl->backend);
    return FmIndex(std::move(impl));
}

SuffixRange FmIndex::equal_range(std::string_view pattern) const {
    return impl_->range(detail::encode_pattern(pattern));
}

std::vector<SuffixRange> FmIndex::equal_range_batch(
    const std::vector<std::string_view>& patterns,
    std::uint32_t batch_width) const {
    const auto width = effective_batch_width(batch_width);
    std::vector<std::vector<std::uint8_t>> encoded;
    encoded.reserve(patterns.size());
    for (const auto pattern : patterns) encoded.push_back(detail::encode_pattern(pattern));
    return impl_->range_batch(encoded, width);
}

std::uint64_t FmIndex::count(std::string_view pattern, StrandMode strands) const {
    const auto encoded = detail::encode_pattern(pattern);
    if (strands == StrandMode::forward) {
        return impl_->range(encoded).size();
    }
    const auto reverse = detail::reverse_complement(encoded);
    if (strands == StrandMode::reverse_complement) {
        return impl_->range(reverse).size();
    }
    if (encoded == reverse) {
        return impl_->range(encoded).size();
    }
    return impl_->range(encoded).size() + impl_->range(reverse).size();
}

std::vector<std::uint64_t> FmIndex::count_batch(
    const std::vector<std::string_view>& patterns,
    const FmBatchOptions& options) const {
    const auto width = effective_batch_width(options.batch_width);
    std::vector<std::vector<std::uint8_t>> encoded;
    encoded.reserve(patterns.size());
    for (const auto pattern : patterns) encoded.push_back(detail::encode_pattern(pattern));

    std::vector<std::vector<std::uint8_t>> searches;
    std::vector<std::size_t> owners;
    searches.reserve(encoded.size() * 2);
    owners.reserve(encoded.size() * 2);
    if (options.strands == StrandMode::forward) {
        searches = std::move(encoded);
        for (std::size_t index = 0; index < searches.size(); ++index) owners.push_back(index);
    } else for (std::size_t index = 0; index < encoded.size(); ++index) {
        const auto reverse = detail::reverse_complement(encoded[index]);
        if (options.strands == StrandMode::reverse_complement) {
            searches.push_back(reverse);
            owners.push_back(index);
        } else if (options.strands == StrandMode::both) {
            searches.push_back(encoded[index]);
            owners.push_back(index);
            if (encoded[index] != reverse) {
                searches.push_back(reverse);
                owners.push_back(index);
            }
        } else {
            throw Error(ErrorCode::invalid_input, "invalid strand mode for FM batch query");
        }
    }

    const auto ranges = impl_->range_batch(searches, width);
    std::vector<std::uint64_t> counts(patterns.size(), 0);
    for (std::size_t index = 0; index < ranges.size(); ++index) {
        counts[owners[index]] += ranges[index].size();
    }
    return counts;
}

QueryResult FmIndex::locate(
    std::string_view pattern,
    const LocateOptions& options) const {
    const auto encoded = detail::encode_pattern(pattern);
    std::vector<Match> matches;
    std::uint64_t total_hits = 0;
    if (options.strands == StrandMode::forward) {
        total_hits = impl_->collect(encoded, Strand::forward, options, matches);
    } else {
        const auto reverse = detail::reverse_complement(encoded);
        if (options.strands == StrandMode::reverse_complement) {
            total_hits = impl_->collect(
                reverse, Strand::reverse_complement, options, matches);
        } else if (encoded == reverse) {
            total_hits = impl_->collect(encoded, Strand::both, options, matches);
        } else {
            total_hits = impl_->collect(encoded, Strand::forward, options, matches);
            total_hits += impl_->collect(
                reverse, Strand::reverse_complement, options, matches);
        }
    }
    return detail::finalize_matches(std::move(matches), total_hits);
}

SequenceInfo FmIndex::sequence_info(SequenceId id) const {
    const auto index = static_cast<std::size_t>(id);
    if (index >= impl_->reference.sequences.size()) {
        throw Error(ErrorCode::invalid_input, "sequence id is out of range");
    }
    return impl_->reference.sequences[index];
}

IndexInfo FmIndex::info() const { return impl_->index_info; }

void FmIndex::save(
    const std::filesystem::path& path,
    const SaveOptions& options) const {
    detail::ContainerSpec spec;
    spec.kind = IndexKind::fm_index;
    spec.backend = impl_->backend;
    spec.coordinate_width = 64;
    spec.sdsl_major = SDSL_VERSION_MAJOR;
    spec.sdsl_minor = SDSL_VERSION_MINOR;
    spec.sdsl_patch = SDSL_VERSION_PATCH;
    spec.sequence_count = impl_->reference.sequences.size();
    spec.total_bases = impl_->reference.total_bases;
    spec.text_symbols = std::visit([](const auto& index) {
        return static_cast<std::uint64_t>(index.size());
    }, impl_->csa);
    spec.ambiguous_bases = impl_->reference.ambiguous_bases;
    spec.fingerprint = impl_->reference.fingerprint;

    detail::write_container(
        path,
        options,
        spec,
        {
            {detail::SectionType::metadata,
             [&](std::ostream& output) { detail::write_metadata(output, impl_->reference); }},
            {detail::SectionType::sdsl_csa,
             [&](std::ostream& output) {
                 std::visit([&](const auto& index) { index.serialize(output); }, impl_->csa);
             }}
        });
}

FmIndex FmIndex::load(const std::filesystem::path& path) {
    const auto container = detail::read_container(path);
    if (container.spec.kind != IndexKind::fm_index) {
        throw Error(ErrorCode::corrupt_index, "index does not contain an FM-index");
    }
    if (container.spec.backend == detail::StoredBackend::sdsl_csa_sada) {
        throw Error(ErrorCode::unsupported_backend, "sdsl-csa-sada is not implemented");
    }
    if (container.spec.backend != detail::StoredBackend::sdsl_csa_wt_huff &&
        container.spec.backend != detail::StoredBackend::sdsl_csa_wt_balanced &&
        container.spec.backend != detail::StoredBackend::sdsl_csa_wt_epr) {
        throw Error(ErrorCode::unsupported_backend, "unsupported FM-index payload");
    }
    if (container.spec.coordinate_width != 64) {
        throw Error(ErrorCode::corrupt_index, "SDSL CSA coordinate width must be 64 bits");
    }
    if (container.spec.sdsl_major != SDSL_VERSION_MAJOR ||
        container.spec.sdsl_minor != SDSL_VERSION_MINOR ||
        container.spec.sdsl_patch != SDSL_VERSION_PATCH) {
        throw Error(ErrorCode::version_mismatch, "SDSL serialization version mismatch");
    }

    auto impl = std::make_unique<Impl>();
    impl->backend = container.spec.backend;
    if (impl->backend == detail::StoredBackend::sdsl_csa_wt_huff) {
        impl->csa.emplace<SdslFmHuffman>();
    } else if (impl->backend == detail::StoredBackend::sdsl_csa_wt_balanced) {
        impl->csa.emplace<SdslFmBalanced>();
    } else {
        impl->csa.emplace<SdslFmEpr>();
    }
    impl->reference = detail::read_metadata(container);
    auto input = detail::open_section_stream(container, detail::SectionType::sdsl_csa);
    try {
        std::visit([&](auto& index) { index.load(static_cast<std::istream&>(*input)); }, impl->csa);
    } catch (const std::exception& error) {
        throw Error(
            ErrorCode::corrupt_index,
            std::string("cannot load SDSL CSA payload: ") + error.what());
    }
    if (input->peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::corrupt_index, "SDSL CSA section has trailing bytes");
    }
    std::visit([&](const auto& index) {
        validate_csa(index, container.spec.text_symbols, ErrorCode::corrupt_index);
    }, impl->csa);
    impl->index_info = detail::index_info_from_container(container);
    return FmIndex(std::move(impl));
}

} // namespace sufkit
