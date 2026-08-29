// SPDX-License-Identifier: MIT

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

#include "reference_data.hpp"
#include <sufkit/types.hpp>

namespace sufkit::detail {

// Keep the container reader and writer on one version source. Individual
// index implementations may deliberately write an older compatible format.
inline constexpr std::uint16_t kCurrentSufidxFormatMinor = 4;

enum class StoredBackend : std::uint8_t {
  kDivsufsort32 = 1,
  kDivsufsort64 = 2,
  kCaps32 = 3,
  kCaps64 = 4,
  kSdslCsaWtHuff = 10,
  kSdslCsaWtBalanced = 11,
  kSdslCsaSada = 12,
  kSdslCsaWtEpr = 13
};

enum class SectionType : std::uint32_t {
  kMetadata = 1,
  kText = 2,
  kSuffixArray = 3,
  kSdslCsa = 4,
  kInverseSuffixArray = 5,
  kLcp = 6,
  kChild = 7,
  kLearnedSa = 8,
  kSaSampling = 9
};

struct SectionDescriptor {
  SectionType type = SectionType::kMetadata;
  std::uint32_t flags = 1;
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
  std::uint32_t crc32 = 0;
};

struct ContainerSpec {
  std::uint16_t format_minor = 0;
  IndexKind kind = IndexKind::kSuffixArray;
  StoredBackend backend = StoredBackend::kDivsufsort32;
  std::uint8_t coordinate_width = 0;
  std::uint8_t sdsl_major = 0;
  std::uint8_t sdsl_minor = 0;
  std::uint8_t sdsl_patch = 0;
  std::uint8_t library_major = 0;
  std::uint8_t library_minor = 0;
  std::uint8_t library_patch = 0;
  SaResourceProfile sa_resource_profile = SaResourceProfile::kFast;
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

// Restricts third-party payload decoders to one validated container section.
class SectionIStream : public std::istream {
 public:
  SectionIStream(const std::filesystem::path& path,
                 const SectionDescriptor& section);
  ~SectionIStream() override;

  // Includes bytes already buffered by the streambuf. Payload decoders use
  // this before allocating variable-length data from an untrusted section.
  std::uint64_t RemainingBytes() const noexcept;

 private:
  class LimitedBuffer : public std::streambuf {
   public:
    LimitedBuffer(std::ifstream& source, std::uint64_t limit,
                  std::size_t buffer_size);

    std::uint64_t RemainingBytes() const noexcept;

   protected:
    int_type underflow() override;

   private:
    std::ifstream& source_;
    std::uint64_t remaining_;
    std::size_t buffer_size_;
    std::unique_ptr<char[]> buffer_;
  };

  std::ifstream file_;
  LimitedBuffer buffer_;
};

void WriteContainer(const std::filesystem::path& path,
                    const SaveOptions& options, const ContainerSpec& spec,
                    const std::vector<SectionWriter>& writers);

ParsedContainer ReadContainer(const std::filesystem::path& path);
const SectionDescriptor& RequireSection(const ParsedContainer& container,
                                        SectionType type);
std::unique_ptr<SectionIStream> OpenSectionStream(
    const ParsedContainer& container, SectionType type);

void WriteMetadata(std::ostream& output, const ReferenceData& data);
ReferenceData ReadMetadata(const ParsedContainer& container);
IndexInfo IndexInfoFromContainer(const ParsedContainer& container);
const char* StoredBackendName(StoredBackend backend) noexcept;
const char* StoredBackendSignature(StoredBackend backend) noexcept;

void WriteU32(std::ostream& output, std::uint32_t value);
void WriteU64(std::ostream& output, std::uint64_t value);
std::uint32_t ReadU32(std::istream& input, const char* field);
std::uint64_t ReadU64(std::istream& input, const char* field);

}  // namespace sufkit::detail
