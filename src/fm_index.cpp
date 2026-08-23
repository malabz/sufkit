#include <sufkit/fm_index.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <sdsl/construct.hpp>
#include <sdsl/csa_wt.hpp>
#include <sdsl/io.hpp>
#include <sdsl/suffix_array_algorithm.hpp>
#include <sdsl/version.hpp>
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

using SdslFmIndex = sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>;

detail::ReferenceData metadata_copy(const detail::ReferenceData& source) {
    detail::ReferenceData result;
    result.sequences = source.sequences;
    result.total_bases = source.total_bases;
    result.ambiguous_bases = source.ambiguous_bases;
    result.fingerprint = source.fingerprint;
    return result;
}

IndexInfo built_info(const detail::ReferenceData& data, const SdslFmIndex& csa) {
    IndexInfo info;
    info.kind = IndexKind::fm_index;
    info.library_version = SUFKIT_VERSION_STRING;
    info.backend = detail::stored_backend_name(detail::StoredBackend::sdsl_csa_wt_huff);
    info.backend_signature = detail::stored_backend_signature(detail::StoredBackend::sdsl_csa_wt_huff);
    info.sdsl_version = sdsl::sdsl_version;
    info.coordinate_width = 64;
    info.sequence_count = data.sequences.size();
    info.total_bases = data.total_bases;
    info.text_symbols = csa.size();
    info.ambiguous_bases = data.ambiguous_bases;
    info.fingerprint = data.fingerprint;
    return info;
}

} // namespace

struct FmIndex::Impl {
    detail::ReferenceData reference;
    SdslFmIndex csa;
    IndexInfo index_info;

    SuffixRange range(const std::vector<std::uint8_t>& pattern) const {
        SdslFmIndex::size_type begin = 0;
        SdslFmIndex::size_type end = 0;
        const auto occurrences = sdsl::backward_search(
            csa,
            0,
            csa.size() - 1,
            pattern.begin(),
            pattern.end(),
            begin,
            end);
        if (occurrences == 0) {
            return {0, 0};
        }
        return {
            static_cast<std::uint64_t>(begin),
            static_cast<std::uint64_t>(begin + occurrences)};
    }

    std::uint64_t collect(
        const std::vector<std::uint8_t>& pattern,
        Strand strand,
        const LocateOptions& options,
        std::vector<Match>& output) const {
        const auto interval = range(pattern);
        for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
            const auto global = static_cast<std::uint64_t>(csa[row]);
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
    if (options.backend != FmBackend::sdsl_csa_wt_huff) {
        throw Error(
            ErrorCode::unsupported_backend,
            "only sdsl-csa-wt-huff is implemented in sufkit 0.1.x");
    }
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
    const auto expected_size = reference.impl_->data.encoded.size() + 1;
    std::string encoded(
        reinterpret_cast<const char*>(reference.impl_->data.encoded.data()),
        reference.impl_->data.encoded.size());
    try {
        sdsl::construct_im(impl->csa, std::move(encoded), 1);
    } catch (const std::exception& error) {
        throw Error(
            ErrorCode::build_failure,
            std::string("SDSL csa_wt construction failed: ") + error.what());
    }
    if (impl->csa.size() != expected_size) {
        throw Error(ErrorCode::build_failure, "SDSL CSA has an unexpected logical text size");
    }
    if (impl->csa.sigma < 2 || impl->csa.comp2char[0] != detail::kSentinel) {
        throw Error(ErrorCode::build_failure, "SDSL CSA alphabet does not contain the expected sentinel");
    }
    impl->index_info = built_info(impl->reference, impl->csa);
    return FmIndex(std::move(impl));
}

SuffixRange FmIndex::equal_range(std::string_view pattern) const {
    return impl_->range(detail::encode_pattern(pattern));
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
    spec.backend = detail::StoredBackend::sdsl_csa_wt_huff;
    spec.coordinate_width = 64;
    spec.sdsl_major = SDSL_VERSION_MAJOR;
    spec.sdsl_minor = SDSL_VERSION_MINOR;
    spec.sdsl_patch = SDSL_VERSION_PATCH;
    spec.sequence_count = impl_->reference.sequences.size();
    spec.total_bases = impl_->reference.total_bases;
    spec.text_symbols = impl_->csa.size();
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
             [&](std::ostream& output) { impl_->csa.serialize(output); }}
        });
}

FmIndex FmIndex::load(const std::filesystem::path& path) {
    const auto container = detail::read_container(path);
    if (container.spec.kind != IndexKind::fm_index) {
        throw Error(ErrorCode::corrupt_index, "index does not contain an FM-index");
    }
    if (container.spec.backend != detail::StoredBackend::sdsl_csa_wt_huff) {
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
    impl->reference = detail::read_metadata(container);
    auto input = detail::open_section_stream(container, detail::SectionType::sdsl_csa);
    try {
        impl->csa.load(static_cast<std::istream&>(*input));
    } catch (const std::exception& error) {
        throw Error(
            ErrorCode::corrupt_index,
            std::string("cannot load SDSL CSA payload: ") + error.what());
    }
    if (input->peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::corrupt_index, "SDSL CSA section has trailing bytes");
    }
    if (impl->csa.size() != container.spec.text_symbols ||
        impl->csa.sigma < 2 || impl->csa.comp2char[0] != detail::kSentinel) {
        throw Error(ErrorCode::corrupt_index, "SDSL CSA metadata is inconsistent");
    }
    impl->index_info = detail::index_info_from_container(container);
    return FmIndex(std::move(impl));
}

} // namespace sufkit
