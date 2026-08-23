#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <memory>
#include <ostream>
#include <streambuf>
#include <string>
#include <vector>

#include <sufkit/types.hpp>

#include "reference_data.hpp"

namespace sufkit::detail {

enum class StoredBackend : std::uint8_t {
    divsufsort32 = 1,
    divsufsort64 = 2,
    sdsl_csa_wt_huff = 10
};

enum class SectionType : std::uint32_t {
    metadata = 1,
    text = 2,
    suffix_array = 3,
    sdsl_csa = 4,
    inverse_suffix_array = 5,
    lcp = 6,
    child = 7
};

struct SectionDescriptor {
    SectionType type = SectionType::metadata;
    std::uint32_t flags = 1;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
};

struct ContainerSpec {
    std::uint16_t format_minor = 0;
    IndexKind kind = IndexKind::suffix_array;
    StoredBackend backend = StoredBackend::divsufsort32;
    std::uint8_t coordinate_width = 0;
    std::uint8_t sdsl_major = 0;
    std::uint8_t sdsl_minor = 0;
    std::uint8_t sdsl_patch = 0;
    std::uint8_t library_major = 0;
    std::uint8_t library_minor = 0;
    std::uint8_t library_patch = 0;
    std::uint64_t sequence_count = 0;
    std::uint64_t total_bases = 0;
    std::uint64_t text_symbols = 0;
    std::uint64_t ambiguous_bases = 0;
    std::uint64_t fingerprint = 0;
};

struct SectionWriter {
    SectionType type;
    std::function<void(std::ostream&)> write;
};

struct ParsedContainer {
    std::filesystem::path path;
    ContainerSpec spec;
    std::vector<SectionDescriptor> sections;
    std::uint64_t file_size = 0;
};

class SectionIStream : public std::istream {
public:
    SectionIStream(const std::filesystem::path& path, const SectionDescriptor& section);
    ~SectionIStream() override;

private:
    class LimitedBuffer : public std::streambuf {
    public:
        LimitedBuffer(std::ifstream& source, std::uint64_t limit);

    protected:
        int_type underflow() override;

    private:
        std::ifstream& source_;
        std::uint64_t remaining_;
        char buffer_[8192];
    };

    std::ifstream file_;
    LimitedBuffer buffer_;
};

void write_container(
    const std::filesystem::path& path,
    const SaveOptions& options,
    const ContainerSpec& spec,
    const std::vector<SectionWriter>& writers);

ParsedContainer read_container(const std::filesystem::path& path);
const SectionDescriptor& require_section(const ParsedContainer& container, SectionType type);
std::unique_ptr<SectionIStream> open_section_stream(
    const ParsedContainer& container,
    SectionType type);

void write_metadata(std::ostream& output, const ReferenceData& data);
ReferenceData read_metadata(const ParsedContainer& container);
IndexInfo index_info_from_container(const ParsedContainer& container);
const char* stored_backend_name(StoredBackend backend) noexcept;
const char* stored_backend_signature(StoredBackend backend) noexcept;

void write_u32(std::ostream& output, std::uint32_t value);
void write_u64(std::ostream& output, std::uint64_t value);
std::uint32_t read_u32(std::istream& input, const char* field);
std::uint64_t read_u64(std::istream& input, const char* field);

} // namespace sufkit::detail
