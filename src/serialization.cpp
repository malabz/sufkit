#include "serialization.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <set>
#include <system_error>

#include <unistd.h>
#include <zlib.h>

#include <sufkit/version.hpp>

namespace sufkit::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{{'S', 'U', 'F', 'K', 'I', 'D', 'X', 0}};
constexpr std::uint16_t kFormatMajor = 1;
constexpr std::uint16_t kFormatMinor = 2;
constexpr std::size_t kBaseHeaderSize = 80;
constexpr std::size_t kSectionEntrySize = 32;
constexpr std::size_t kHeaderCrcOffset = 72;
constexpr std::uint32_t kRequiredSection = 1;
constexpr std::uint32_t kMaxSections = 16;
constexpr std::uint32_t kMaxStringBytes = 1U << 30;

std::atomic<std::uint64_t> g_temp_counter{0};

void append_u16(std::vector<std::uint8_t>& output, std::uint16_t value) {
    output.push_back(static_cast<std::uint8_t>(value & 0xffU));
    output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 2) {
        throw Error(ErrorCode::corrupt_index, "index header is truncated");
    }
    const auto value = static_cast<std::uint32_t>(input[offset]) |
                       (static_cast<std::uint32_t>(input[offset + 1]) << 8U);
    return static_cast<std::uint16_t>(value);
}

std::uint32_t get_u32(const std::vector<std::uint8_t>& input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 4) {
        throw Error(ErrorCode::corrupt_index, "index header is truncated");
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(input[offset + index]) << (8U * index);
    }
    return value;
}

std::uint64_t get_u64(const std::vector<std::uint8_t>& input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 8) {
        throw Error(ErrorCode::corrupt_index, "index header is truncated");
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(input[offset + index]) << (8U * index);
    }
    return value;
}

void patch_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    if (offset > output.size() || output.size() - offset < 4) {
        throw Error(ErrorCode::build_failure, "internal header patch offset is invalid");
    }
    for (unsigned index = 0; index < 4; ++index) {
        output[offset + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU);
    }
}

std::uint32_t crc_bytes(const std::vector<std::uint8_t>& bytes) {
    uLong value = crc32(0L, Z_NULL, 0);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto amount = static_cast<uInt>(std::min<std::size_t>(bytes.size() - offset, 1U << 20));
        value = crc32(value, bytes.data() + offset, amount);
        offset += amount;
    }
    return static_cast<std::uint32_t>(value);
}

std::uint32_t crc_file_range(
    const std::filesystem::path& path,
    std::uint64_t offset,
    std::uint64_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error(ErrorCode::io_error, "cannot read index while calculating CRC: " + path.string());
    }
    input.seekg(static_cast<std::streamoff>(offset));
    if (!input) {
        throw Error(ErrorCode::io_error, "cannot seek index while calculating CRC: " + path.string());
    }
    std::array<unsigned char, 1U << 16> buffer{};
    uLong value = crc32(0L, Z_NULL, 0);
    std::uint64_t remaining = size;
    while (remaining != 0) {
        const auto amount = static_cast<std::streamsize>(
            std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(reinterpret_cast<char*>(buffer.data()), amount);
        if (input.gcount() != amount) {
            throw Error(ErrorCode::corrupt_index, "index section is truncated");
        }
        value = crc32(value, buffer.data(), static_cast<uInt>(amount));
        remaining -= static_cast<std::uint64_t>(amount);
    }
    return static_cast<std::uint32_t>(value);
}

std::filesystem::path temporary_path_for(const std::filesystem::path& target) {
    const auto counter = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    return std::filesystem::path(
        target.string() + ".partial." + std::to_string(static_cast<long long>(getpid())) +
        "." + std::to_string(ticks ^ counter));
}

std::vector<std::uint8_t> build_header(
    const ContainerSpec& spec,
    const std::vector<SectionDescriptor>& sections) {
    std::vector<std::uint8_t> header;
    header.reserve(kBaseHeaderSize + sections.size() * kSectionEntrySize);
    header.insert(header.end(), kMagic.begin(), kMagic.end());
    append_u16(header, kFormatMajor);
    if (spec.format_minor > kFormatMinor) {
        throw Error(ErrorCode::build_failure, "unsupported output sufidx format version");
    }
    append_u16(header, spec.format_minor);
    header.push_back(1); // little endian
    header.push_back(static_cast<std::uint8_t>(spec.kind));
    header.push_back(static_cast<std::uint8_t>(spec.backend));
    header.push_back(spec.coordinate_width);
    header.push_back(kNormalizationId);
    header.push_back(SUFKIT_VERSION_MAJOR);
    header.push_back(SUFKIT_VERSION_MINOR);
    header.push_back(SUFKIT_VERSION_PATCH);
    header.push_back(spec.sdsl_major);
    header.push_back(spec.sdsl_minor);
    header.push_back(spec.sdsl_patch);
    header.push_back(0);
    append_u32(header, static_cast<std::uint32_t>(sections.size()));
    append_u32(header, static_cast<std::uint32_t>(
        kBaseHeaderSize + sections.size() * kSectionEntrySize));
    append_u64(header, spec.sequence_count);
    append_u64(header, spec.total_bases);
    append_u64(header, spec.text_symbols);
    append_u64(header, spec.ambiguous_bases);
    append_u64(header, spec.fingerprint);
    append_u32(header, 0); // header CRC placeholder
    append_u32(header, 0);
    for (const auto& section : sections) {
        append_u32(header, static_cast<std::uint32_t>(section.type));
        append_u32(header, section.flags);
        append_u64(header, section.offset);
        append_u64(header, section.size);
        append_u32(header, section.crc32);
        append_u32(header, 0);
    }
    if (header.size() != kBaseHeaderSize + sections.size() * kSectionEntrySize) {
        throw Error(ErrorCode::build_failure, "internal index header size mismatch");
    }
    patch_u32(header, kHeaderCrcOffset, crc_bytes(header));
    return header;
}

std::vector<std::uint8_t> read_prefix(const std::filesystem::path& path, std::size_t size) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw Error(ErrorCode::io_error, "cannot open index: " + path.string());
    }
    std::vector<std::uint8_t> bytes(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        throw Error(ErrorCode::corrupt_index, "index header is truncated: " + path.string());
    }
    return bytes;
}

std::string read_string(std::istream& input, const char* field) {
    const auto size = read_u32(input, field);
    if (size > kMaxStringBytes) {
        throw Error(ErrorCode::corrupt_index, std::string(field) + " is too large");
    }
    std::string value(size, '\0');
    if (size != 0) {
        input.read(value.data(), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size)) {
            throw Error(ErrorCode::corrupt_index, std::string(field) + " is truncated");
        }
    }
    return value;
}

void write_string(std::ostream& output, const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::build_failure, "metadata string exceeds the index format limit");
    }
    write_u32(output, static_cast<std::uint32_t>(value.size()));
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace

SectionIStream::LimitedBuffer::LimitedBuffer(std::ifstream& source, std::uint64_t limit)
    : source_(source), remaining_(limit) {
    setg(buffer_, buffer_, buffer_);
}

SectionIStream::LimitedBuffer::int_type SectionIStream::LimitedBuffer::underflow() {
    if (gptr() < egptr()) {
        return traits_type::to_int_type(*gptr());
    }
    if (remaining_ == 0) {
        return traits_type::eof();
    }
    const auto amount = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining_, sizeof(buffer_)));
    source_.read(buffer_, amount);
    const auto read = source_.gcount();
    if (read <= 0) {
        return traits_type::eof();
    }
    remaining_ -= static_cast<std::uint64_t>(read);
    setg(buffer_, buffer_, buffer_ + read);
    return traits_type::to_int_type(*gptr());
}

SectionIStream::SectionIStream(
    const std::filesystem::path& path,
    const SectionDescriptor& section)
    : std::istream(nullptr), file_(path, std::ios::binary), buffer_(file_, section.size) {
    if (!file_) {
        throw Error(ErrorCode::io_error, "cannot open index section: " + path.string());
    }
    file_.seekg(static_cast<std::streamoff>(section.offset));
    if (!file_) {
        throw Error(ErrorCode::corrupt_index, "cannot seek to index section");
    }
    rdbuf(&buffer_);
}

SectionIStream::~SectionIStream() = default;

void write_u32(std::ostream& output, std::uint32_t value) {
    std::array<char, 4> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (8U * index)) & 0xffU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw Error(ErrorCode::io_error, "failed to write index data");
    }
}

void write_u64(std::ostream& output, std::uint64_t value) {
    std::array<char, 8> bytes{};
    for (unsigned index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<char>((value >> (8U * index)) & 0xffU);
    }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw Error(ErrorCode::io_error, "failed to write index data");
    }
}

std::uint32_t read_u32(std::istream& input, const char* field) {
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw Error(ErrorCode::corrupt_index, std::string(field) + " is truncated");
    }
    std::uint32_t value = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
    }
    return value;
}

std::uint64_t read_u64(std::istream& input, const char* field) {
    std::array<unsigned char, 8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw Error(ErrorCode::corrupt_index, std::string(field) + " is truncated");
    }
    std::uint64_t value = 0;
    for (unsigned index = 0; index < bytes.size(); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
    }
    return value;
}

void write_container(
    const std::filesystem::path& path,
    const SaveOptions& options,
    const ContainerSpec& spec,
    const std::vector<SectionWriter>& writers) {
    if (writers.empty() || writers.size() > kMaxSections) {
        throw Error(ErrorCode::build_failure, "invalid number of index sections");
    }
    if (path.empty()) {
        throw Error(ErrorCode::invalid_input, "index output path must not be empty");
    }
    if (!options.overwrite && std::filesystem::exists(path)) {
        throw Error(ErrorCode::io_error, "index already exists: " + path.string());
    }

    const auto temporary = temporary_path_for(path);
    try {
        const std::size_t header_size = kBaseHeaderSize + writers.size() * kSectionEntrySize;
        std::fstream output(
            temporary,
            std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!output) {
            throw Error(ErrorCode::io_error, "cannot create temporary index: " + temporary.string());
        }
        std::vector<char> placeholder(header_size, '\0');
        output.write(placeholder.data(), static_cast<std::streamsize>(placeholder.size()));

        std::vector<SectionDescriptor> sections;
        sections.reserve(writers.size());
        std::set<SectionType> unique_types;
        for (const auto& writer : writers) {
            if (!unique_types.insert(writer.type).second) {
                throw Error(ErrorCode::build_failure, "duplicate index section type");
            }
            const auto begin = output.tellp();
            if (begin < 0) {
                throw Error(ErrorCode::io_error, "cannot determine index section offset");
            }
            writer.write(output);
            if (!output) {
                throw Error(ErrorCode::io_error, "failed while writing index section");
            }
            const auto end = output.tellp();
            if (end < begin) {
                throw Error(ErrorCode::io_error, "invalid index section length");
            }
            SectionDescriptor section;
            section.type = writer.type;
            section.flags = kRequiredSection;
            section.offset = static_cast<std::uint64_t>(begin);
            section.size = static_cast<std::uint64_t>(end - begin);
            sections.push_back(section);
        }
        output.flush();
        if (!output) {
            throw Error(ErrorCode::io_error, "failed to flush temporary index");
        }
        for (auto& section : sections) {
            section.crc32 = crc_file_range(temporary, section.offset, section.size);
        }
        const auto header = build_header(spec, sections);
        output.seekp(0);
        output.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        output.flush();
        output.close();
        if (!output) {
            throw Error(ErrorCode::io_error, "failed to finalize temporary index");
        }

        (void)read_container(temporary);
        if (!options.overwrite && std::filesystem::exists(path)) {
            throw Error(ErrorCode::io_error, "index appeared during save: " + path.string());
        }
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

ParsedContainer read_container(const std::filesystem::path& path) {
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        throw Error(ErrorCode::io_error, "cannot stat index: " + path.string());
    }
    if (file_size < kBaseHeaderSize) {
        throw Error(ErrorCode::corrupt_index, "index is smaller than its fixed header");
    }
    auto prefix = read_prefix(path, kBaseHeaderSize);
    if (!std::equal(kMagic.begin(), kMagic.end(), prefix.begin())) {
        throw Error(ErrorCode::corrupt_index, "index magic is invalid");
    }
    const auto major = get_u16(prefix, 8);
    const auto minor = get_u16(prefix, 10);
    if (major != kFormatMajor || minor > kFormatMinor) {
        throw Error(ErrorCode::version_mismatch, "unsupported sufidx format version");
    }
    if (prefix[12] != 1) {
        throw Error(ErrorCode::corrupt_index, "unsupported index byte order");
    }
    if (prefix[16] != kNormalizationId) {
        throw Error(ErrorCode::version_mismatch, "unsupported reference normalization version");
    }
    const auto section_count = get_u32(prefix, 24);
    const auto header_size = get_u32(prefix, 28);
    if (section_count == 0 || section_count > kMaxSections ||
        header_size != kBaseHeaderSize + section_count * kSectionEntrySize ||
        header_size > file_size) {
        throw Error(ErrorCode::corrupt_index, "invalid index section table size");
    }
    auto header = read_prefix(path, header_size);
    const auto expected_crc = get_u32(header, kHeaderCrcOffset);
    patch_u32(header, kHeaderCrcOffset, 0);
    if (crc_bytes(header) != expected_crc) {
        throw Error(ErrorCode::corrupt_index, "index header CRC mismatch");
    }

    ParsedContainer result;
    result.path = path;
    result.file_size = file_size;
    result.spec.format_minor = minor;
    const auto kind = header[13];
    if (kind != static_cast<std::uint8_t>(IndexKind::suffix_array) &&
        kind != static_cast<std::uint8_t>(IndexKind::fm_index)) {
        throw Error(ErrorCode::corrupt_index, "unknown index kind");
    }
    result.spec.kind = static_cast<IndexKind>(kind);
    result.spec.backend = static_cast<StoredBackend>(header[14]);
    result.spec.coordinate_width = header[15];
    result.spec.library_major = header[17];
    result.spec.library_minor = header[18];
    result.spec.library_patch = header[19];
    result.spec.sdsl_major = header[20];
    result.spec.sdsl_minor = header[21];
    result.spec.sdsl_patch = header[22];
    result.spec.sequence_count = get_u64(header, 32);
    result.spec.total_bases = get_u64(header, 40);
    result.spec.text_symbols = get_u64(header, 48);
    result.spec.ambiguous_bases = get_u64(header, 56);
    result.spec.fingerprint = get_u64(header, 64);
    const auto backend = result.spec.backend;
    if ((result.spec.kind == IndexKind::suffix_array &&
         backend != StoredBackend::divsufsort32 && backend != StoredBackend::divsufsort64) ||
        (result.spec.kind == IndexKind::fm_index &&
         backend != StoredBackend::sdsl_csa_wt_huff &&
         backend != StoredBackend::sdsl_csa_wt_balanced &&
         backend != StoredBackend::sdsl_csa_sada &&
         backend != StoredBackend::sdsl_csa_wt_epr)) {
        throw Error(ErrorCode::unsupported_backend, "unsupported index backend id");
    }

    std::set<SectionType> unique_types;
    for (std::uint32_t index = 0; index < section_count; ++index) {
        const std::size_t offset = kBaseHeaderSize + index * kSectionEntrySize;
        SectionDescriptor section;
        section.type = static_cast<SectionType>(get_u32(header, offset));
        section.flags = get_u32(header, offset + 4);
        section.offset = get_u64(header, offset + 8);
        section.size = get_u64(header, offset + 16);
        section.crc32 = get_u32(header, offset + 24);
        const auto raw_type = static_cast<std::uint32_t>(section.type);
        const bool known_type = raw_type >= static_cast<std::uint32_t>(SectionType::metadata) &&
                                raw_type <= static_cast<std::uint32_t>(SectionType::learned_sa);
        if (!known_type && (section.flags & kRequiredSection) != 0) {
            throw Error(ErrorCode::corrupt_index, "unknown required index section");
        }
        if (!unique_types.insert(section.type).second) {
            throw Error(ErrorCode::corrupt_index, "duplicate index section type");
        }
        if (section.offset < header_size || section.offset > file_size ||
            section.size > file_size - section.offset) {
            throw Error(ErrorCode::corrupt_index, "index section is out of bounds");
        }
        result.sections.push_back(section);
    }
    auto ordered = result.sections;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.offset < right.offset;
    });
    for (std::size_t index = 1; index < ordered.size(); ++index) {
        if (ordered[index - 1].offset + ordered[index - 1].size > ordered[index].offset) {
            throw Error(ErrorCode::corrupt_index, "index sections overlap");
        }
    }
    for (const auto& section : result.sections) {
        if (crc_file_range(path, section.offset, section.size) != section.crc32) {
            throw Error(ErrorCode::corrupt_index, "index section CRC mismatch");
        }
    }
    return result;
}

const SectionDescriptor& require_section(
    const ParsedContainer& container,
    SectionType type) {
    const auto it = std::find_if(
        container.sections.begin(), container.sections.end(),
        [type](const SectionDescriptor& section) { return section.type == type; });
    if (it == container.sections.end()) {
        throw Error(ErrorCode::corrupt_index, "required index section is missing");
    }
    return *it;
}

std::unique_ptr<SectionIStream> open_section_stream(
    const ParsedContainer& container,
    SectionType type) {
    return std::make_unique<SectionIStream>(container.path, require_section(container, type));
}

void write_metadata(std::ostream& output, const ReferenceData& data) {
    if (data.sequences.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw Error(ErrorCode::build_failure, "too many reference sequences for index metadata");
    }
    write_u32(output, static_cast<std::uint32_t>(data.sequences.size()));
    for (const auto& sequence : data.sequences) {
        write_u32(output, sequence.id);
        write_u64(output, sequence.global_offset);
        write_u64(output, sequence.length);
        write_u64(output, sequence.ambiguous_bases);
        write_string(output, sequence.name);
        write_string(output, sequence.description);
    }
}

ReferenceData read_metadata(const ParsedContainer& container) {
    auto input = open_section_stream(container, SectionType::metadata);
    ReferenceData data;
    const auto count = read_u32(*input, "metadata sequence count");
    if (count != container.spec.sequence_count) {
        throw Error(ErrorCode::corrupt_index, "metadata sequence count mismatch");
    }
    data.sequences.reserve(count);
    std::set<std::string> names;
    std::uint64_t expected_offset = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        SequenceInfo sequence;
        sequence.id = read_u32(*input, "sequence id");
        sequence.global_offset = read_u64(*input, "sequence global offset");
        sequence.length = read_u64(*input, "sequence length");
        sequence.ambiguous_bases = read_u64(*input, "sequence ambiguous bases");
        sequence.name = read_string(*input, "sequence name");
        sequence.description = read_string(*input, "sequence description");
        if (sequence.id != index || sequence.name.empty() || sequence.length == 0 ||
            sequence.global_offset != expected_offset || !names.insert(sequence.name).second ||
            sequence.ambiguous_bases > sequence.length) {
            throw Error(ErrorCode::corrupt_index, "invalid sequence metadata");
        }
        if (sequence.length > std::numeric_limits<std::uint64_t>::max() - expected_offset - 1) {
            throw Error(ErrorCode::corrupt_index, "sequence metadata overflows coordinates");
        }
        expected_offset += sequence.length + 1;
        data.total_bases += sequence.length;
        data.ambiguous_bases += sequence.ambiguous_bases;
        data.sequences.push_back(std::move(sequence));
    }
    if (input->peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::corrupt_index, "metadata section has trailing bytes");
    }
    if (data.total_bases != container.spec.total_bases ||
        data.ambiguous_bases != container.spec.ambiguous_bases ||
        expected_offset + 1 != container.spec.text_symbols) {
        throw Error(ErrorCode::corrupt_index, "metadata totals do not match the index header");
    }
    data.fingerprint = container.spec.fingerprint;
    return data;
}

const char* stored_backend_name(StoredBackend backend) noexcept {
    switch (backend) {
    case StoredBackend::divsufsort32: return "divsufsort32";
    case StoredBackend::divsufsort64: return "divsufsort64";
    case StoredBackend::sdsl_csa_wt_huff: return "sdsl-csa-wt-huff";
    case StoredBackend::sdsl_csa_wt_balanced: return "sdsl-csa-wt-balanced";
    case StoredBackend::sdsl_csa_sada: return "sdsl-csa-sada";
    case StoredBackend::sdsl_csa_wt_epr: return "sdsl-csa-wt-epr";
    }
    return "unknown";
}

const char* stored_backend_signature(StoredBackend backend) noexcept {
    switch (backend) {
    case StoredBackend::divsufsort32: return "libdivsufsort-2.0.2/saidx_t";
    case StoredBackend::divsufsort64: return "libdivsufsort-2.0.2/saidx64_t";
    case StoredBackend::sdsl_csa_wt_huff:
        return "sdsl::csa_wt<sdsl::wt_huff<>,32,64>";
    case StoredBackend::sdsl_csa_wt_balanced:
        return "sdsl::csa_wt<sdsl::wt_blcd<>,32,64>";
    case StoredBackend::sdsl_csa_sada:
        return "sdsl::csa_sada<>";
    case StoredBackend::sdsl_csa_wt_epr:
        return "sdsl::csa_wt<sdsl::wt_epr<8>,32,64>";
    }
    return "unknown";
}

IndexInfo index_info_from_container(const ParsedContainer& container) {
    const auto has = [&](SectionType type) {
        return std::any_of(container.sections.begin(), container.sections.end(),
            [type](const SectionDescriptor& section) { return section.type == type; });
    };
    (void)require_section(container, SectionType::metadata);
    if (container.spec.kind == IndexKind::suffix_array) {
        (void)require_section(container, SectionType::text);
        (void)require_section(container, SectionType::suffix_array);
        if (has(SectionType::sdsl_csa))
            throw Error(ErrorCode::corrupt_index,
                        "suffix-array container has an FM-index section");
    } else {
        (void)require_section(container, SectionType::sdsl_csa);
        if (has(SectionType::text) || has(SectionType::suffix_array) ||
            has(SectionType::inverse_suffix_array) || has(SectionType::lcp) ||
            has(SectionType::child) || has(SectionType::learned_sa))
            throw Error(ErrorCode::corrupt_index,
                        "FM-index container has suffix-array-only sections");
    }
    IndexInfo info;
    info.kind = container.spec.kind;
    info.format_version = "1." + std::to_string(container.spec.format_minor);
    info.library_version = std::to_string(container.spec.library_major) + "." +
                           std::to_string(container.spec.library_minor) + "." +
                           std::to_string(container.spec.library_patch);
    info.backend = stored_backend_name(container.spec.backend);
    info.backend_signature = stored_backend_signature(container.spec.backend);
    if (container.spec.sdsl_major != 0 || container.spec.sdsl_minor != 0 ||
        container.spec.sdsl_patch != 0) {
        info.sdsl_version = std::to_string(container.spec.sdsl_major) + "." +
                            std::to_string(container.spec.sdsl_minor) + "." +
                            std::to_string(container.spec.sdsl_patch);
    }
    info.coordinate_width = container.spec.coordinate_width;
    info.sequence_count = container.spec.sequence_count;
    info.total_bases = container.spec.total_bases;
    info.text_symbols = container.spec.text_symbols;
    info.ambiguous_bases = container.spec.ambiguous_bases;
    info.fingerprint = container.spec.fingerprint;
    info.serialized_bytes = container.file_size;
    if (container.spec.kind == IndexKind::suffix_array) {
        const bool isa = has(SectionType::inverse_suffix_array);
        const bool lcp = has(SectionType::lcp);
        const bool child = has(SectionType::child);
        const bool learned = has(SectionType::learned_sa);
        if (!isa && !lcp && !child) info.sa_acceleration = SaAcceleration::none;
        else if (!isa && lcp && !child) info.sa_acceleration = SaAcceleration::lcp;
        else if (!isa && lcp && child) info.sa_acceleration = SaAcceleration::lcp_child;
        else if (isa && lcp && !child) info.sa_acceleration = SaAcceleration::lcp_suffix_link;
        else if (isa && lcp && child) info.sa_acceleration = SaAcceleration::full;
        else throw Error(ErrorCode::corrupt_index,
                         "invalid suffix-array auxiliary section combination");
        for (const auto& section : container.sections) {
            if (section.type == SectionType::inverse_suffix_array ||
                section.type == SectionType::lcp || section.type == SectionType::child) {
                info.auxiliary_bytes += section.size;
            }
        }
        if (learned) {
            if (container.spec.format_minor < 2) {
                throw Error(ErrorCode::corrupt_index,
                            "learned SA data requires sufidx format 1.2");
            }
            auto input = open_section_stream(container, SectionType::learned_sa);
            const auto model_id = read_u32(*input, "learned SA model ID");
            info.learned_k = read_u32(*input, "learned SA k");
            info.learned_bucket_bits = read_u32(*input, "learned SA bucket bits");
            info.learned_memory_overhead_basis_points =
                read_u32(*input, "learned SA memory budget");
            const auto width = read_u32(*input, "learned SA coordinate width");
            const auto anchors = read_u64(*input, "learned SA anchor count");
            const auto fingerprint = read_u64(*input, "learned SA fingerprint");
            if (model_id != 1) {
                throw Error(ErrorCode::unsupported_backend,
                            "unsupported learned SA model ID");
            }
            if (info.learned_k == 0 || info.learned_k > 31 ||
                info.learned_bucket_bits > 2U * info.learned_k ||
                info.learned_bucket_bits > 31 || width != info.coordinate_width ||
                anchors != (1ULL << info.learned_bucket_bits) + 1 ||
                fingerprint != info.fingerprint) {
                throw Error(ErrorCode::corrupt_index, "invalid learned SA header");
            }
            info.sa_lookup_acceleration = SaLookupAcceleration::sapling_pwl;
            info.learned_index_bytes = require_section(container, SectionType::learned_sa).size;
        }
    }
    return info;
}

} // namespace sufkit::detail
