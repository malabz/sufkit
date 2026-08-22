#include <sufkit/suffix_array.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <variant>
#include <vector>

extern "C" {
#include "divsufsort.h"
#include "divsufsort64.h"
}

#include <sufkit/version.hpp>

#include "genome_reference_internal.hpp"
#include "query.hpp"
#include "reference_data.hpp"
#include "serialization.hpp"

namespace sufkit {
namespace {

using Sa32 = std::vector<std::int32_t>;
using Sa64 = std::vector<std::int64_t>;
using SaStorage = std::variant<Sa32, Sa64>;

detail::ReferenceData metadata_copy(const detail::ReferenceData& source) {
    detail::ReferenceData result;
    result.sequences = source.sequences;
    result.total_bases = source.total_bases;
    result.ambiguous_bases = source.ambiguous_bases;
    result.fingerprint = source.fingerprint;
    return result;
}

int compare_suffix_pattern(
    const std::vector<std::uint8_t>& text,
    std::uint64_t suffix,
    const std::vector<std::uint8_t>& pattern) {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (suffix >= text.size() || index > text.size() - static_cast<std::size_t>(suffix) - 1) {
            return -1;
        }
        const auto left = text[static_cast<std::size_t>(suffix) + index];
        const auto right = pattern[index];
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
    }
    return 0;
}

template <class SaVector>
SuffixRange range_for(
    const std::vector<std::uint8_t>& text,
    const SaVector& suffix_array,
    const std::vector<std::uint8_t>& pattern) {
    const auto lower = std::lower_bound(
        suffix_array.begin(), suffix_array.end(), 0,
        [&](const auto suffix, int) {
            return compare_suffix_pattern(
                text, static_cast<std::uint64_t>(suffix), pattern) < 0;
        });
    const auto upper = std::upper_bound(
        lower, suffix_array.end(), 0,
        [&](int, const auto suffix) {
            return compare_suffix_pattern(
                text, static_cast<std::uint64_t>(suffix), pattern) > 0;
        });
    return {
        static_cast<std::uint64_t>(std::distance(suffix_array.begin(), lower)),
        static_cast<std::uint64_t>(std::distance(suffix_array.begin(), upper))};
}

std::uint64_t sa_value(const SaStorage& storage, std::uint64_t row) {
    return std::visit(
        [row](const auto& suffix_array) -> std::uint64_t {
            if (row >= suffix_array.size()) {
                throw Error(ErrorCode::invalid_input, "suffix-array row is out of range");
            }
            return static_cast<std::uint64_t>(suffix_array[static_cast<std::size_t>(row)]);
        },
        storage);
}

IndexInfo built_info(
    const detail::ReferenceData& data,
    detail::StoredBackend backend,
    std::uint8_t width,
    std::uint64_t text_symbols) {
    IndexInfo info;
    info.kind = IndexKind::suffix_array;
    info.library_version = SUFKIT_VERSION_STRING;
    info.backend = detail::stored_backend_name(backend);
    info.backend_signature = detail::stored_backend_signature(backend);
    info.coordinate_width = width;
    info.sequence_count = data.sequences.size();
    info.total_bases = data.total_bases;
    info.text_symbols = text_symbols;
    info.ambiguous_bases = data.ambiguous_bases;
    info.fingerprint = data.fingerprint;
    return info;
}

} // namespace

struct SuffixArray::Impl {
    detail::ReferenceData reference;
    std::vector<std::uint8_t> text;
    SaStorage suffix_array;
    detail::StoredBackend backend = detail::StoredBackend::divsufsort32;
    IndexInfo index_info;

    SuffixRange range(const std::vector<std::uint8_t>& pattern) const {
        return std::visit(
            [&](const auto& values) { return range_for(text, values, pattern); },
            suffix_array);
    }

    std::uint64_t collect(
        const std::vector<std::uint8_t>& pattern,
        Strand strand,
        const LocateOptions& options,
        std::vector<Match>& output) const {
        const auto interval = range(pattern);
        for (std::uint64_t row = interval.begin; row < interval.end; ++row) {
            const auto global = sa_value(suffix_array, row);
            const auto mapped = detail::map_global_position(
                reference.sequences, global, pattern.size());
            if (!mapped) {
                throw Error(ErrorCode::corrupt_index, "suffix-array hit is outside a reference contig");
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

SuffixArray::SuffixArray(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

SuffixArray::SuffixArray(SuffixArray&&) noexcept = default;
SuffixArray& SuffixArray::operator=(SuffixArray&&) noexcept = default;
SuffixArray::~SuffixArray() = default;

SuffixArray SuffixArray::build(
    const GenomeReference& reference,
    const SuffixArrayBuildOptions& options) {
    if (options.threads == 0) {
        throw Error(ErrorCode::invalid_input, "suffix-array thread count must be greater than zero");
    }
    if (options.backend == SaBackend::caps) {
        throw Error(ErrorCode::unsupported_backend, "CaPS is reserved for sufkit V1.1");
    }

    auto impl = std::make_unique<Impl>();
    impl->reference = metadata_copy(reference.impl_->data);
    impl->text = reference.impl_->data.encoded;
    impl->text.push_back(detail::kSentinel);
    if (impl->text.size() < 2) {
        throw Error(ErrorCode::invalid_input, "reference text is empty");
    }

    CoordinateWidth width = options.coordinate_width;
    if (width == CoordinateWidth::auto_select) {
        width = impl->text.size() <= static_cast<std::size_t>(std::numeric_limits<saidx_t>::max())
                    ? CoordinateWidth::bits32
                    : CoordinateWidth::bits64;
    }
    if (width == CoordinateWidth::bits32) {
        if (impl->text.size() > static_cast<std::size_t>(std::numeric_limits<saidx_t>::max())) {
            throw Error(ErrorCode::invalid_input, "reference is too large for divsufsort32");
        }
        Sa32 values(impl->text.size());
        const int status = divsufsort(
            reinterpret_cast<const sauchar_t*>(impl->text.data()),
            reinterpret_cast<saidx_t*>(values.data()),
            static_cast<saidx_t>(impl->text.size()));
        if (status != 0) {
            throw Error(ErrorCode::build_failure, "divsufsort32 failed");
        }
        impl->suffix_array = std::move(values);
        impl->backend = detail::StoredBackend::divsufsort32;
    } else if (width == CoordinateWidth::bits64) {
        if (impl->text.size() > static_cast<std::size_t>(std::numeric_limits<saidx64_t>::max())) {
            throw Error(ErrorCode::invalid_input, "reference is too large for divsufsort64");
        }
        Sa64 values(impl->text.size());
        const int status = divsufsort64(
            reinterpret_cast<const sauchar_t*>(impl->text.data()),
            reinterpret_cast<saidx64_t*>(values.data()),
            static_cast<saidx64_t>(impl->text.size()));
        if (status != 0) {
            throw Error(ErrorCode::build_failure, "divsufsort64 failed");
        }
        impl->suffix_array = std::move(values);
        impl->backend = detail::StoredBackend::divsufsort64;
    } else {
        throw Error(ErrorCode::invalid_input, "invalid suffix-array coordinate width");
    }

    impl->index_info = built_info(
        impl->reference,
        impl->backend,
        static_cast<std::uint8_t>(width),
        impl->text.size());
    return SuffixArray(std::move(impl));
}

SuffixRange SuffixArray::equal_range(std::string_view pattern) const {
    return impl_->range(detail::encode_pattern(pattern));
}

std::uint64_t SuffixArray::count(std::string_view pattern, StrandMode strands) const {
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

QueryResult SuffixArray::locate(
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

Position SuffixArray::suffix_at(std::uint64_t row) const {
    return sa_value(impl_->suffix_array, row);
}

SequenceInfo SuffixArray::sequence_info(SequenceId id) const {
    const auto index = static_cast<std::size_t>(id);
    if (index >= impl_->reference.sequences.size()) {
        throw Error(ErrorCode::invalid_input, "sequence id is out of range");
    }
    return impl_->reference.sequences[index];
}

IndexInfo SuffixArray::info() const { return impl_->index_info; }

void SuffixArray::save(
    const std::filesystem::path& path,
    const SaveOptions& options) const {
    detail::ContainerSpec spec;
    spec.kind = IndexKind::suffix_array;
    spec.backend = impl_->backend;
    spec.coordinate_width = impl_->index_info.coordinate_width;
    spec.sequence_count = impl_->reference.sequences.size();
    spec.total_bases = impl_->reference.total_bases;
    spec.text_symbols = impl_->text.size();
    spec.ambiguous_bases = impl_->reference.ambiguous_bases;
    spec.fingerprint = impl_->reference.fingerprint;

    detail::write_container(
        path,
        options,
        spec,
        {
            {detail::SectionType::metadata,
             [&](std::ostream& output) { detail::write_metadata(output, impl_->reference); }},
            {detail::SectionType::text,
             [&](std::ostream& output) {
                 output.write(
                     reinterpret_cast<const char*>(impl_->text.data()),
                     static_cast<std::streamsize>(impl_->text.size()));
             }},
            {detail::SectionType::suffix_array,
             [&](std::ostream& output) {
                 std::visit(
                     [&](const auto& values) {
                         detail::write_u64(output, values.size());
                         for (const auto value : values) {
                             if constexpr (sizeof(value) == 4) {
                                 detail::write_u32(output, static_cast<std::uint32_t>(value));
                             } else {
                                 detail::write_u64(output, static_cast<std::uint64_t>(value));
                             }
                         }
                     },
                     impl_->suffix_array);
             }}
        });
}

SuffixArray SuffixArray::load(const std::filesystem::path& path) {
    const auto container = detail::read_container(path);
    if (container.spec.kind != IndexKind::suffix_array) {
        throw Error(ErrorCode::corrupt_index, "index does not contain a suffix array");
    }
    if (container.spec.backend != detail::StoredBackend::divsufsort32 &&
        container.spec.backend != detail::StoredBackend::divsufsort64) {
        throw Error(ErrorCode::unsupported_backend, "unsupported suffix-array payload");
    }

    auto impl = std::make_unique<Impl>();
    impl->reference = detail::read_metadata(container);
    impl->backend = container.spec.backend;

    const auto& text_section = detail::require_section(container, detail::SectionType::text);
    if (text_section.size != container.spec.text_symbols || text_section.size < 2) {
        throw Error(ErrorCode::corrupt_index, "invalid suffix-array text section size");
    }
    auto text_input = detail::open_section_stream(container, detail::SectionType::text);
    impl->text.resize(static_cast<std::size_t>(text_section.size));
    text_input->read(
        reinterpret_cast<char*>(impl->text.data()),
        static_cast<std::streamsize>(impl->text.size()));
    if (text_input->gcount() != static_cast<std::streamsize>(impl->text.size()) ||
        impl->text.back() != detail::kSentinel ||
        std::find(impl->text.begin(), std::prev(impl->text.end()), detail::kSentinel) !=
            std::prev(impl->text.end())) {
        throw Error(ErrorCode::corrupt_index, "suffix-array text has an invalid sentinel");
    }
    for (const auto& sequence : impl->reference.sequences) {
        const auto separator = sequence.global_offset + sequence.length;
        if (separator >= impl->text.size() || impl->text[static_cast<std::size_t>(separator)] != detail::kSeparator) {
            throw Error(ErrorCode::corrupt_index, "suffix-array text has an invalid contig separator");
        }
        for (std::uint64_t position = sequence.global_offset; position < separator; ++position) {
            const auto symbol = impl->text[static_cast<std::size_t>(position)];
            if (symbol < detail::kA || symbol > detail::kN) {
                throw Error(ErrorCode::corrupt_index, "suffix-array text has an invalid base symbol");
            }
        }
    }
    if (detail::content_fingerprint(impl->text.data(), impl->text.size() - 1) !=
        impl->reference.fingerprint) {
        throw Error(ErrorCode::corrupt_index, "suffix-array text fingerprint mismatch");
    }

    auto sa_input = detail::open_section_stream(container, detail::SectionType::suffix_array);
    const auto count = detail::read_u64(*sa_input, "suffix-array length");
    if (count != impl->text.size()) {
        throw Error(ErrorCode::corrupt_index, "suffix-array length does not match text length");
    }
    std::vector<bool> seen(static_cast<std::size_t>(count), false);
    if (container.spec.backend == detail::StoredBackend::divsufsort32) {
        if (container.spec.coordinate_width != 32) {
            throw Error(ErrorCode::corrupt_index, "invalid divsufsort32 coordinate width");
        }
        Sa32 values(static_cast<std::size_t>(count));
        for (auto& value : values) {
            const auto raw = detail::read_u32(*sa_input, "suffix-array value");
            if (raw >= count || seen[raw]) {
                throw Error(ErrorCode::corrupt_index, "suffix array is not a permutation");
            }
            seen[raw] = true;
            value = static_cast<std::int32_t>(raw);
        }
        impl->suffix_array = std::move(values);
    } else {
        if (container.spec.coordinate_width != 64) {
            throw Error(ErrorCode::corrupt_index, "invalid divsufsort64 coordinate width");
        }
        Sa64 values(static_cast<std::size_t>(count));
        for (auto& value : values) {
            const auto raw = detail::read_u64(*sa_input, "suffix-array value");
            if (raw >= count || seen[static_cast<std::size_t>(raw)]) {
                throw Error(ErrorCode::corrupt_index, "suffix array is not a permutation");
            }
            seen[static_cast<std::size_t>(raw)] = true;
            value = static_cast<std::int64_t>(raw);
        }
        impl->suffix_array = std::move(values);
    }
    if (sa_input->peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::corrupt_index, "suffix-array section has trailing bytes");
    }
    impl->index_info = detail::index_info_from_container(container);
    return SuffixArray(std::move(impl));
}

} // namespace sufkit
