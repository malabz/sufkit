// SPDX-License-Identifier: MIT

#include "serialization.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>
#include <system_error>

#include <sufkit/version.hpp>

#include "caps_backend.hpp"
#include "sa_codec.hpp"

namespace sufkit::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{
    {'S', 'U', 'F', 'K', 'I', 'D', 'X', 0}};
constexpr std::uint16_t kFormatMajor = 1;
constexpr std::size_t kBaseHeaderSize = 80;
constexpr std::size_t kSectionEntrySize = 32;
constexpr std::size_t kHeaderCrcOffset = 72;
constexpr std::uint32_t kRequiredSection = 1;
constexpr std::uint32_t kMaxSections = 16;
constexpr std::uint32_t kMaxStringBytes = 1U << 30;
#ifndef SUFKIT_IO_BUFFER_KIB
#define SUFKIT_IO_BUFFER_KIB 1024
#endif
constexpr std::size_t kStreamBufferSize =
    static_cast<std::size_t>(SUFKIT_IO_BUFFER_KIB) << 10U;

std::atomic<std::uint64_t> g_temp_counter{0};

void UpdateCrc(uLong& crc, const unsigned char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto amount = static_cast<uInt>(
        std::min<std::size_t>(size - offset,
                              std::numeric_limits<uInt>::max()));
    crc = crc32(crc, data + offset, amount);
    offset += amount;
  }
}

// This stream buffer observes bytes as a section writer emits them. It avoids
// rereading every genome-scale payload solely to populate the section table.
class ChecksummingBuffer : public std::streambuf {
 public:
  explicit ChecksummingBuffer(std::streambuf* destination)
      : destination_(destination),
        crc_(crc32(0L, Z_NULL, 0)),
        buffer_(new char[kStreamBufferSize]) {
    setp(buffer_.get(), buffer_.get() + kStreamBufferSize);
  }

  bool Finish() { return FlushBuffer(); }

  std::uint32_t Crc32() const noexcept {
    return static_cast<std::uint32_t>(crc_);
  }

 protected:
  std::streamsize xsputn(const char* data, std::streamsize size) override {
    std::streamsize consumed = 0;
    while (consumed < size) {
      const auto remaining = size - consumed;
      const auto available = static_cast<std::streamsize>(epptr() - pptr());
      if (available == 0 && !FlushBuffer()) {
        break;
      }
      const auto buffer_size =
          static_cast<std::streamsize>(kStreamBufferSize);
      if (pptr() == pbase() && remaining >= buffer_size) {
        const auto written = destination_->sputn(data + consumed, remaining);
        if (written > 0) {
          UpdateCrc(crc_,
                    reinterpret_cast<const unsigned char*>(data + consumed),
                    static_cast<std::size_t>(written));
          consumed += written;
        }
        if (written != remaining) {
          break;
        }
        continue;
      }
      const auto copied = std::min(
          remaining, static_cast<std::streamsize>(epptr() - pptr()));
      std::memcpy(pptr(), data + consumed, static_cast<std::size_t>(copied));
      pbump(static_cast<int>(copied));
      consumed += copied;
    }
    return consumed;
  }

  int_type overflow(int_type character) override {
    if (traits_type::eq_int_type(character, traits_type::eof())) {
      return traits_type::not_eof(character);
    }
    if (!FlushBuffer()) {
      return traits_type::eof();
    }
    *pptr() = traits_type::to_char_type(character);
    pbump(1);
    return traits_type::not_eof(character);
  }

  int sync() override {
    return FlushBuffer() && destination_->pubsync() == 0 ? 0 : -1;
  }

 private:
  bool FlushBuffer() {
    const auto size = static_cast<std::streamsize>(pptr() - pbase());
    if (size == 0) {
      return true;
    }
    const auto written = destination_->sputn(pbase(), size);
    if (written > 0) {
      UpdateCrc(crc_, reinterpret_cast<const unsigned char*>(pbase()),
                static_cast<std::size_t>(written));
    }
    setp(buffer_.get(), buffer_.get() + kStreamBufferSize);
    return written == size;
  }

  std::streambuf* destination_;
  uLong crc_;
  std::unique_ptr<char[]> buffer_;
};

// Multi-byte integers in the container are encoded explicitly so the on-disk
// representation remains little-endian on every host architecture.
void AppendU16(std::vector<std::uint8_t>& output, std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void AppendU32(std::vector<std::uint8_t>& output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void AppendU64(std::vector<std::uint8_t>& output, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& input,
                     std::size_t offset) {
  if (offset > input.size() || input.size() - offset < 2) {
    throw Error(ErrorCode::kCorruptIndex, "index header is truncated");
  }
  const auto value = static_cast<std::uint32_t>(input[offset]) |
                     (static_cast<std::uint32_t>(input[offset + 1]) << 8U);
  return static_cast<std::uint16_t>(value);
}

std::uint32_t GetU32(const std::vector<std::uint8_t>& input,
                     std::size_t offset) {
  if (offset > input.size() || input.size() - offset < 4) {
    throw Error(ErrorCode::kCorruptIndex, "index header is truncated");
  }
  std::uint32_t value = 0;
  for (unsigned index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[offset + index]) << (8U * index);
  }
  return value;
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& input,
                     std::size_t offset) {
  if (offset > input.size() || input.size() - offset < 8) {
    throw Error(ErrorCode::kCorruptIndex, "index header is truncated");
  }
  std::uint64_t value = 0;
  for (unsigned index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(input[offset + index]) << (8U * index);
  }
  return value;
}

void PatchU32(std::vector<std::uint8_t>& output, std::size_t offset,
              std::uint32_t value) {
  if (offset > output.size() || output.size() - offset < 4) {
    throw Error(ErrorCode::kBuildFailure,
                "internal header patch offset is invalid");
  }
  for (unsigned index = 0; index < 4; ++index) {
    output[offset + index] =
        static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU);
  }
}

std::uint32_t CrcBytes(const std::vector<std::uint8_t>& bytes) {
  uLong value = crc32(0L, Z_NULL, 0);
  UpdateCrc(value, bytes.data(), bytes.size());
  return static_cast<std::uint32_t>(value);
}

std::uint32_t CrcFileRange(const std::filesystem::path& path,
                           std::uint64_t offset, std::uint64_t size) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error(ErrorCode::kIoError,
                "cannot read index while calculating CRC: " + path.string());
  }
  input.seekg(static_cast<std::streamoff>(offset));
  if (!input) {
    throw Error(ErrorCode::kIoError,
                "cannot seek index while calculating CRC: " + path.string());
  }
  std::unique_ptr<unsigned char[]> buffer(
      new unsigned char[kStreamBufferSize]);
  // Stream large payloads through a fixed buffer instead of duplicating an
  // SDSL index or suffix array solely to calculate its checksum.
  uLong value = crc32(0L, Z_NULL, 0);
  std::uint64_t remaining = size;
  while (remaining != 0) {
    const auto amount = static_cast<std::streamsize>(
        std::min<std::uint64_t>(remaining, kStreamBufferSize));
    input.read(reinterpret_cast<char*>(buffer.get()), amount);
    if (input.gcount() != amount) {
      throw Error(ErrorCode::kCorruptIndex, "index section is truncated");
    }
    UpdateCrc(value, buffer.get(), static_cast<std::size_t>(amount));
    remaining -= static_cast<std::uint64_t>(amount);
  }
  return static_cast<std::uint32_t>(value);
}

std::filesystem::path CreateTemporaryPathFor(
    const std::filesystem::path& target) {
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const auto counter =
        g_temp_counter.fetch_add(1, std::memory_order_relaxed);
    const auto ticks = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto temporary = std::filesystem::path(
        target.string() + ".partial." +
        std::to_string(static_cast<long long>(getpid())) + "." +
        std::to_string(ticks ^ counter));
    const int descriptor =
        open(temporary.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0666);
    if (descriptor >= 0) {
      if (close(descriptor) != 0) {
        const auto close_error = errno;
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw Error(ErrorCode::kIoError,
                    "cannot close temporary index: " +
                        std::error_code(close_error, std::generic_category())
                            .message());
      }
      return temporary;
    }
    if (errno != EEXIST) {
      throw Error(ErrorCode::kIoError,
                  "cannot create temporary index: " +
                      std::error_code(errno, std::generic_category())
                          .message());
    }
  }
  throw Error(ErrorCode::kIoError,
              "cannot allocate a unique temporary index path");
}

void PublishTemporary(const std::filesystem::path& temporary,
                      const std::filesystem::path& target, bool overwrite) {
  if (overwrite) {
    std::filesystem::rename(temporary, target);
    return;
  }

  // A same-directory hard link is an atomic no-replace publication on the
  // supported Linux/WSL platform. Unlike exists()+rename(), it cannot overwrite
  // a target created by another process after the initial validation.
  if (link(temporary.c_str(), target.c_str()) != 0) {
    if (errno == EEXIST) {
      throw Error(ErrorCode::kIoError,
                  "index already exists: " + target.string());
    }
    throw Error(ErrorCode::kIoError,
                "cannot publish index: " +
                    std::error_code(errno, std::generic_category()).message());
  }
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
}

std::vector<std::uint8_t> BuildHeader(
    const ContainerSpec& spec, const std::vector<SectionDescriptor>& sections) {
  std::vector<std::uint8_t> header;
  header.reserve(kBaseHeaderSize + sections.size() * kSectionEntrySize);
  header.insert(header.end(), kMagic.begin(), kMagic.end());
  AppendU16(header, kFormatMajor);
  if (spec.format_minor > kCurrentSufidxFormatMinor) {
    throw Error(ErrorCode::kBuildFailure,
                "unsupported output sufidx format version");
  }
  AppendU16(header, spec.format_minor);
  header.push_back(1);  // little endian
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
  std::uint8_t resource_profile = 0;
  if (spec.kind == IndexKind::kSuffixArray && spec.format_minor >= 4) {
    resource_profile =
        static_cast<std::uint8_t>(spec.sa_resource_profile);
    if (resource_profile >
        static_cast<std::uint8_t>(SaResourceProfile::kLowMemory)) {
      throw Error(ErrorCode::kBuildFailure,
                  "invalid suffix-array resource profile");
    }
  }
  header.push_back(resource_profile);
  AppendU32(header, static_cast<std::uint32_t>(sections.size()));
  AppendU32(header, static_cast<std::uint32_t>(
                        kBaseHeaderSize + sections.size() * kSectionEntrySize));
  AppendU64(header, spec.sequence_count);
  AppendU64(header, spec.total_bases);
  AppendU64(header, spec.text_symbols);
  AppendU64(header, spec.ambiguous_bases);
  AppendU64(header, spec.fingerprint);
  AppendU32(header, 0);  // header CRC placeholder
  AppendU32(header, 0);
  for (const auto& section : sections) {
    AppendU32(header, static_cast<std::uint32_t>(section.type));
    AppendU32(header, section.flags);
    AppendU64(header, section.offset);
    AppendU64(header, section.size);
    AppendU32(header, section.crc32);
    AppendU32(header, 0);
  }
  if (header.size() != kBaseHeaderSize + sections.size() * kSectionEntrySize) {
    throw Error(ErrorCode::kBuildFailure,
                "internal index header size mismatch");
  }
  PatchU32(header, kHeaderCrcOffset, CrcBytes(header));
  return header;
}

std::vector<std::uint8_t> ReadPrefix(const std::filesystem::path& path,
                                     std::size_t size) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw Error(ErrorCode::kIoError, "cannot open index: " + path.string());
  }
  std::vector<std::uint8_t> bytes(size);
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(size));
  if (input.gcount() != static_cast<std::streamsize>(size)) {
    throw Error(ErrorCode::kCorruptIndex,
                "index header is truncated: " + path.string());
  }
  return bytes;
}

std::string ReadString(SectionIStream& input, const char* field) {
  const auto size = ReadU32(input, field);
  if (size > kMaxStringBytes) {
    throw Error(ErrorCode::kCorruptIndex, std::string(field) + " is too large");
  }
  if (size > input.RemainingBytes()) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(field) + " is truncated");
  }
  std::string value;
  try {
    value.resize(size);
  } catch (const std::bad_alloc&) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string("cannot allocate ") + field);
  } catch (const std::length_error&) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(field) + " is too large");
  }
  if (size != 0) {
    input.read(value.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
      throw Error(ErrorCode::kCorruptIndex,
                  std::string(field) + " is truncated");
    }
  }
  return value;
}

void WriteString(std::ostream& output, const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw Error(ErrorCode::kBuildFailure,
                "metadata string exceeds the index format limit");
  }
  WriteU32(output, static_cast<std::uint32_t>(value.size()));
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

}  // namespace

SectionIStream::LimitedBuffer::LimitedBuffer(std::ifstream& source,
                                             std::uint64_t limit,
                                             std::size_t buffer_size)
    : source_(source),
      remaining_(limit),
      buffer_size_(buffer_size),
      buffer_(new char[buffer_size_]) {
  setg(buffer_.get(), buffer_.get(), buffer_.get());
}

std::uint64_t SectionIStream::LimitedBuffer::RemainingBytes() const noexcept {
  const auto buffered = static_cast<std::uint64_t>(egptr() - gptr());
  return remaining_ + buffered;
}

SectionIStream::LimitedBuffer::int_type
SectionIStream::LimitedBuffer::underflow() {
  if (gptr() < egptr()) {
    return traits_type::to_int_type(*gptr());
  }
  if (remaining_ == 0) {
    return traits_type::eof();
  }
  // EOF at the declared section boundary prevents a payload decoder from
  // reading bytes owned by a later section.
  const auto amount = static_cast<std::streamsize>(
      std::min<std::uint64_t>(remaining_, buffer_size_));
  source_.read(buffer_.get(), amount);
  const auto read = source_.gcount();
  if (read <= 0) {
    return traits_type::eof();
  }
  remaining_ -= static_cast<std::uint64_t>(read);
  setg(buffer_.get(), buffer_.get(), buffer_.get() + read);
  return traits_type::to_int_type(*gptr());
}

SectionIStream::SectionIStream(const std::filesystem::path& path,
                               const SectionDescriptor& section)
    : std::istream(nullptr),
      file_(path, std::ios::binary),
      buffer_(file_, section.size, kStreamBufferSize) {
  if (!file_) {
    throw Error(ErrorCode::kIoError,
                "cannot open index section: " + path.string());
  }
  file_.seekg(static_cast<std::streamoff>(section.offset));
  if (!file_) {
    throw Error(ErrorCode::kCorruptIndex, "cannot seek to index section");
  }
  rdbuf(&buffer_);
}

SectionIStream::~SectionIStream() = default;

std::uint64_t SectionIStream::RemainingBytes() const noexcept {
  return buffer_.RemainingBytes();
}

void WriteU32(std::ostream& output, std::uint32_t value) {
  std::array<char, 4> bytes{};
  for (unsigned index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<char>((value >> (8U * index)) & 0xffU);
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw Error(ErrorCode::kIoError, "failed to write index data");
  }
}

void WriteU64(std::ostream& output, std::uint64_t value) {
  std::array<char, 8> bytes{};
  for (unsigned index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<char>((value >> (8U * index)) & 0xffU);
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw Error(ErrorCode::kIoError, "failed to write index data");
  }
}

std::uint32_t ReadU32(std::istream& input, const char* field) {
  std::array<unsigned char, 4> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw Error(ErrorCode::kCorruptIndex, std::string(field) + " is truncated");
  }
  std::uint32_t value = 0;
  for (unsigned index = 0; index < bytes.size(); ++index) {
    value |= static_cast<std::uint32_t>(bytes[index]) << (8U * index);
  }
  return value;
}

std::uint64_t ReadU64(std::istream& input, const char* field) {
  std::array<unsigned char, 8> bytes{};
  input.read(reinterpret_cast<char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw Error(ErrorCode::kCorruptIndex, std::string(field) + " is truncated");
  }
  std::uint64_t value = 0;
  for (unsigned index = 0; index < bytes.size(); ++index) {
    value |= static_cast<std::uint64_t>(bytes[index]) << (8U * index);
  }
  return value;
}

void WriteContainer(const std::filesystem::path& path,
                    const SaveOptions& options, const ContainerSpec& spec,
                    const std::vector<SectionWriter>& writers) {
  if (writers.empty() || writers.size() > kMaxSections) {
    throw Error(ErrorCode::kBuildFailure, "invalid number of index sections");
  }
  if (path.empty()) {
    throw Error(ErrorCode::kInvalidInput,
                "index output path must not be empty");
  }
  if (!options.overwrite && std::filesystem::exists(path)) {
    throw Error(ErrorCode::kIoError, "index already exists: " + path.string());
  }

  const auto temporary = CreateTemporaryPathFor(path);
  try {
    const std::size_t header_size =
        kBaseHeaderSize + writers.size() * kSectionEntrySize;
    std::unique_ptr<char[]> output_buffer(new char[kStreamBufferSize]);
    std::fstream output;
    output.rdbuf()->pubsetbuf(
        output_buffer.get(), static_cast<std::streamsize>(kStreamBufferSize));
    output.open(temporary, std::ios::binary | std::ios::in | std::ios::out |
                               std::ios::trunc);
    if (!output) {
      throw Error(ErrorCode::kIoError,
                  "cannot create temporary index: " + temporary.string());
    }
    std::vector<char> placeholder(header_size, '\0');
    output.write(placeholder.data(),
                 static_cast<std::streamsize>(placeholder.size()));

    // Section writers stream directly after the placeholder, avoiding a second
    // in-memory copy of potentially genome-scale payloads.
    std::vector<SectionDescriptor> sections;
    sections.reserve(writers.size());
    std::set<SectionType> unique_types;
    for (const auto& writer : writers) {
      if (!unique_types.insert(writer.type).second) {
        throw Error(ErrorCode::kBuildFailure, "duplicate index section type");
      }
      const auto begin = output.tellp();
      if (begin < 0) {
        throw Error(ErrorCode::kIoError,
                    "cannot determine index section offset");
      }
      ChecksummingBuffer checksumming_buffer(output.rdbuf());
      std::ostream section_output(&checksumming_buffer);
      writer.write(section_output);
      if (!section_output || !checksumming_buffer.Finish() || !output) {
        throw Error(ErrorCode::kIoError, "failed while writing index section");
      }
      const auto end = output.tellp();
      if (end < begin) {
        throw Error(ErrorCode::kIoError, "invalid index section length");
      }
      SectionDescriptor section;
      section.type = writer.type;
      section.flags = kRequiredSection;
      section.offset = static_cast<std::uint64_t>(begin);
      section.size = static_cast<std::uint64_t>(end - begin);
      section.crc32 = checksumming_buffer.Crc32();
      sections.push_back(section);
    }
    output.flush();
    if (!output) {
      throw Error(ErrorCode::kIoError, "failed to flush temporary index");
    }
    // CRCs were accumulated while each payload was streamed. The completed
    // header can therefore replace the placeholder without a first full-file
    // payload reread; final self-validation below remains unchanged.
    const auto header = BuildHeader(spec, sections);
    output.seekp(0);
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    output.flush();
    output.close();
    if (!output) {
      throw Error(ErrorCode::kIoError, "failed to finalize temporary index");
    }

    // Validate the complete temporary container before publication so readers
    // can never observe a partially written target.
    (void)ReadContainer(temporary);
    PublishTemporary(temporary, path, options.overwrite);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw;
  }
}

ParsedContainer ReadContainer(const std::filesystem::path& path) {
  std::error_code size_error;
  const auto file_size = std::filesystem::file_size(path, size_error);
  if (size_error) {
    throw Error(ErrorCode::kIoError, "cannot stat index: " + path.string());
  }
  if (file_size < kBaseHeaderSize) {
    throw Error(ErrorCode::kCorruptIndex,
                "index is smaller than its fixed header");
  }
  auto prefix = ReadPrefix(path, kBaseHeaderSize);
  if (!std::equal(kMagic.begin(), kMagic.end(), prefix.begin())) {
    throw Error(ErrorCode::kCorruptIndex, "index magic is invalid");
  }
  const auto major = GetU16(prefix, 8);
  const auto minor = GetU16(prefix, 10);
  if (major != kFormatMajor || minor > kCurrentSufidxFormatMinor) {
    throw Error(ErrorCode::kVersionMismatch,
                "unsupported sufidx format version");
  }
  if (prefix[12] != 1) {
    throw Error(ErrorCode::kCorruptIndex, "unsupported index byte order");
  }
  if (prefix[16] != kNormalizationId) {
    throw Error(ErrorCode::kVersionMismatch,
                "unsupported reference normalization version");
  }
  const auto section_count = GetU32(prefix, 24);
  const auto header_size = GetU32(prefix, 28);
  if (section_count == 0 || section_count > kMaxSections ||
      header_size != kBaseHeaderSize + section_count * kSectionEntrySize ||
      header_size > file_size) {
    throw Error(ErrorCode::kCorruptIndex, "invalid index section table size");
  }
  auto header = ReadPrefix(path, header_size);
  const auto expected_crc = GetU32(header, kHeaderCrcOffset);
  PatchU32(header, kHeaderCrcOffset, 0);
  if (CrcBytes(header) != expected_crc) {
    throw Error(ErrorCode::kCorruptIndex, "index header CRC mismatch");
  }
  if (GetU32(header, kHeaderCrcOffset + sizeof(std::uint32_t)) != 0) {
    throw Error(ErrorCode::kCorruptIndex,
                "reserved index header field is non-zero");
  }

  ParsedContainer result;
  result.path = path;
  result.file_size = file_size;
  result.spec.format_minor = minor;
  const auto kind = header[13];
  if (kind != static_cast<std::uint8_t>(IndexKind::kSuffixArray) &&
      kind != static_cast<std::uint8_t>(IndexKind::kFmIndex)) {
    throw Error(ErrorCode::kCorruptIndex, "unknown index kind");
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
  const auto raw_resource_profile = header[23];
  if (result.spec.kind == IndexKind::kSuffixArray && minor >= 4) {
    if (raw_resource_profile >
        static_cast<std::uint8_t>(SaResourceProfile::kLowMemory)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "invalid suffix-array resource profile");
    }
    result.spec.sa_resource_profile =
        static_cast<SaResourceProfile>(raw_resource_profile);
  } else if (raw_resource_profile != 0) {
    throw Error(ErrorCode::kCorruptIndex,
                "reserved index header field is non-zero");
  }
  result.spec.sequence_count = GetU64(header, 32);
  result.spec.total_bases = GetU64(header, 40);
  result.spec.text_symbols = GetU64(header, 48);
  result.spec.ambiguous_bases = GetU64(header, 56);
  result.spec.fingerprint = GetU64(header, 64);
  const auto backend = result.spec.backend;
  if ((result.spec.kind == IndexKind::kSuffixArray &&
       backend != StoredBackend::kDivsufsort32 &&
       backend != StoredBackend::kDivsufsort64 &&
       backend != StoredBackend::kCaps32 &&
       backend != StoredBackend::kCaps64) ||
      (result.spec.kind == IndexKind::kFmIndex &&
       backend != StoredBackend::kSdslCsaWtHuff &&
       backend != StoredBackend::kSdslCsaWtBalanced &&
       backend != StoredBackend::kSdslCsaSada &&
       backend != StoredBackend::kSdslCsaWtEpr)) {
    throw Error(ErrorCode::kUnsupportedBackend, "unsupported index backend id");
  }

  std::set<SectionType> unique_types;
  for (std::uint32_t index = 0; index < section_count; ++index) {
    const std::size_t offset = kBaseHeaderSize + index * kSectionEntrySize;
    SectionDescriptor section;
    section.type = static_cast<SectionType>(GetU32(header, offset));
    section.flags = GetU32(header, offset + 4);
    section.offset = GetU64(header, offset + 8);
    section.size = GetU64(header, offset + 16);
    section.crc32 = GetU32(header, offset + 24);
    if (GetU32(header, offset + 28) != 0) {
      throw Error(ErrorCode::kCorruptIndex,
                  "reserved section descriptor field is non-zero");
    }
    const auto raw_type = static_cast<std::uint32_t>(section.type);
    const bool known_type =
        raw_type >= static_cast<std::uint32_t>(SectionType::kMetadata) &&
        raw_type <= static_cast<std::uint32_t>(SectionType::kSaSampling);
    if (!known_type && (section.flags & kRequiredSection) != 0) {
      throw Error(ErrorCode::kCorruptIndex, "unknown required index section");
    }
    if (!unique_types.insert(section.type).second) {
      throw Error(ErrorCode::kCorruptIndex, "duplicate index section type");
    }
    if (section.offset < header_size || section.offset > file_size ||
        section.size > file_size - section.offset) {
      throw Error(ErrorCode::kCorruptIndex, "index section is out of bounds");
    }
    result.sections.push_back(section);
  }
  auto ordered = result.sections;
  std::sort(ordered.begin(), ordered.end(),
            [](const auto& left, const auto& right) {
              return left.offset < right.offset;
            });
  for (std::size_t index = 1; index < ordered.size(); ++index) {
    // Bounds checks alone do not prevent two valid-looking sections from
    // aliasing the same payload bytes.
    if (ordered[index - 1].offset + ordered[index - 1].size >
        ordered[index].offset) {
      throw Error(ErrorCode::kCorruptIndex, "index sections overlap");
    }
  }
  for (const auto& section : result.sections) {
    if (CrcFileRange(path, section.offset, section.size) != section.crc32) {
      throw Error(ErrorCode::kCorruptIndex, "index section CRC mismatch");
    }
  }
  return result;
}

const SectionDescriptor& RequireSection(const ParsedContainer& container,
                                        SectionType type) {
  const auto it =
      std::find_if(container.sections.begin(), container.sections.end(),
                   [type](const SectionDescriptor& section) {
                     return section.type == type;
                   });
  if (it == container.sections.end()) {
    throw Error(ErrorCode::kCorruptIndex, "required index section is missing");
  }
  return *it;
}

std::unique_ptr<SectionIStream> OpenSectionStream(
    const ParsedContainer& container, SectionType type) {
  return std::make_unique<SectionIStream>(container.path,
                                          RequireSection(container, type));
}

void WriteMetadata(std::ostream& output, const ReferenceData& data) {
  if (data.sequences.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw Error(ErrorCode::kBuildFailure,
                "too many reference sequences for index metadata");
  }
  WriteU32(output, static_cast<std::uint32_t>(data.sequences.size()));
  for (const auto& sequence : data.sequences) {
    WriteU32(output, sequence.id);
    WriteU64(output, sequence.global_offset);
    WriteU64(output, sequence.length);
    WriteU64(output, sequence.ambiguous_bases);
    WriteString(output, sequence.name);
    WriteString(output, sequence.description);
  }
}

ReferenceData ReadMetadata(const ParsedContainer& container) {
  const auto& section = RequireSection(container, SectionType::kMetadata);
  auto input = OpenSectionStream(container, SectionType::kMetadata);
  ReferenceData data;
  const auto count = ReadU32(*input, "metadata sequence count");
  constexpr std::uint64_t kFixedBytesPerSequence =
      sizeof(std::uint32_t) + 3 * sizeof(std::uint64_t) +
      2 * sizeof(std::uint32_t);
  if (count == 0 || count != container.spec.sequence_count ||
      section.size < sizeof(std::uint32_t) ||
      count > (section.size - sizeof(std::uint32_t)) /
                  kFixedBytesPerSequence) {
    throw Error(ErrorCode::kCorruptIndex, "metadata sequence count mismatch");
  }
  try {
    data.sequences.reserve(count);
    data.contig_starts.reserve(count);
    data.contig_lengths.reserve(count);
  } catch (const std::bad_alloc&) {
    throw Error(ErrorCode::kCorruptIndex,
                "cannot allocate sequence metadata");
  } catch (const std::length_error&) {
    throw Error(ErrorCode::kCorruptIndex, "sequence metadata is too large");
  }
  std::set<std::string> names;
  std::uint64_t expected_offset = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    SequenceInfo sequence;
    sequence.id = ReadU32(*input, "sequence id");
    sequence.global_offset = ReadU64(*input, "sequence global offset");
    sequence.length = ReadU64(*input, "sequence length");
    sequence.ambiguous_bases = ReadU64(*input, "sequence ambiguous bases");
    sequence.name = ReadString(*input, "sequence name");
    sequence.description = ReadString(*input, "sequence description");
    if (sequence.id != index || sequence.name.empty() || sequence.length == 0 ||
        sequence.global_offset != expected_offset ||
        !names.insert(sequence.name).second ||
        sequence.ambiguous_bases > sequence.length) {
      throw Error(ErrorCode::kCorruptIndex, "invalid sequence metadata");
    }
    if (expected_offset == std::numeric_limits<std::uint64_t>::max() ||
        sequence.length >
            std::numeric_limits<std::uint64_t>::max() - expected_offset - 1) {
      throw Error(ErrorCode::kCorruptIndex,
                  "sequence metadata overflows coordinates");
    }
    expected_offset += sequence.length + 1;
    data.total_bases += sequence.length;
    data.ambiguous_bases += sequence.ambiguous_bases;
    data.contig_starts.push_back(sequence.global_offset);
    data.contig_lengths.push_back(sequence.length);
    data.sequences.push_back(std::move(sequence));
  }
  if (input->peek() != std::char_traits<char>::eof()) {
    throw Error(ErrorCode::kCorruptIndex,
                "metadata section has trailing bytes");
  }
  if (expected_offset == std::numeric_limits<std::uint64_t>::max()) {
    throw Error(ErrorCode::kCorruptIndex,
                "sequence metadata overflows the sentinel coordinate");
  }
  if (data.total_bases != container.spec.total_bases ||
      data.ambiguous_bases != container.spec.ambiguous_bases ||
      expected_offset + 1 != container.spec.text_symbols) {
    throw Error(ErrorCode::kCorruptIndex,
                "metadata totals do not match the index header");
  }
  data.fingerprint = container.spec.fingerprint;
  return data;
}

const char* StoredBackendName(StoredBackend backend) noexcept {
  switch (backend) {
    case StoredBackend::kDivsufsort32:
      return "divsufsort32";
    case StoredBackend::kDivsufsort64:
      return "divsufsort64";
    case StoredBackend::kCaps32:
      return "caps32";
    case StoredBackend::kCaps64:
      return "caps64";
    case StoredBackend::kSdslCsaWtHuff:
      return "sdsl-csa-wt-huff";
    case StoredBackend::kSdslCsaWtBalanced:
      return "sdsl-csa-wt-balanced";
    case StoredBackend::kSdslCsaSada:
      return "sdsl-csa-sada";
    case StoredBackend::kSdslCsaWtEpr:
      return "sdsl-csa-wt-epr";
  }
  return "unknown";
}

const char* StoredBackendSignature(StoredBackend backend) noexcept {
  switch (backend) {
    case StoredBackend::kDivsufsort32:
      return "libdivsufsort-2.0.2/saidx_t";
    case StoredBackend::kDivsufsort64:
      return "libdivsufsort-2.0.2/saidx64_t";
    case StoredBackend::kCaps32:
      return "CaPS-SA@2597b373/uint32_t+ParlayLib@e1f1dc0";
    case StoredBackend::kCaps64:
      return "CaPS-SA@2597b373/uint64_t+ParlayLib@e1f1dc0";
    case StoredBackend::kSdslCsaWtHuff:
      return "sdsl::csa_wt<sdsl::wt_huff<>,32,64>";
    case StoredBackend::kSdslCsaWtBalanced:
      return "sdsl::csa_wt<sdsl::wt_blcd<>,32,64>";
    case StoredBackend::kSdslCsaSada:
      return "sdsl::csa_sada<>";
    case StoredBackend::kSdslCsaWtEpr:
      return "sdsl::csa_wt<sdsl::wt_epr<8>,32,64>";
  }
  return "unknown";
}

namespace {

constexpr std::uint64_t kCoordinateCodecHeaderBytes = 56;
constexpr std::uint64_t kLcpCodecHeaderBytes = 80;
constexpr std::uint64_t kLegacySaHeaderBytes = 8;
constexpr std::uint64_t kLegacyAuxHeaderBytes = 9;
constexpr std::uint64_t kLearnedSaHeaderBytes = 36;

std::uint64_t CheckedMetricAdd(std::uint64_t left, std::uint64_t right,
                               const char* label) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) + " byte count overflows");
  }
  return left + right;
}

std::uint64_t CheckedMetricMultiply(std::uint64_t count,
                                    std::uint64_t width,
                                    const char* label) {
  if (width != 0 &&
      count > std::numeric_limits<std::uint64_t>::max() / width) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) + " byte count overflows");
  }
  return count * width;
}

void RequireExactSectionSize(const SectionDescriptor& section,
                             std::uint64_t expected, const char* label) {
  if (section.size != expected) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) + " section size is inconsistent");
  }
}

bool IsThirtyTwoBitSaBackend(StoredBackend backend) noexcept {
  return backend == StoredBackend::kDivsufsort32 ||
         backend == StoredBackend::kCaps32;
}

void ValidateConstructionWidth(const ParsedContainer& container) {
  const bool is_32_bit = IsThirtyTwoBitSaBackend(container.spec.backend);
  const auto expected = static_cast<std::uint8_t>(is_32_bit ? 32 : 64);
  if (container.spec.coordinate_width != expected) {
    throw Error(ErrorCode::kCorruptIndex,
                "SA construction width disagrees with its backend");
  }
  const bool is_caps = container.spec.backend == StoredBackend::kCaps32 ||
                       container.spec.backend == StoredBackend::kCaps64;
  const auto backend =
      is_caps ? SaBackend::kCaps : SaBackend::kDivsufsort;
  const auto width = is_32_bit ? CoordinateWidth::kBits32
                               : CoordinateWidth::kBits64;
  if (!SaConstructionCanRepresent(backend, width,
                                  container.spec.text_symbols)) {
    throw Error(ErrorCode::kCorruptIndex,
                "SA text exceeds its recorded construction width");
  }
}

SaCoordinateCodecHeader InspectCoordinateSection(
    const ParsedContainer& container, SectionType type,
    std::uint64_t expected_elements, std::uint64_t expected_symbols,
    const char* label) {
  const auto& section = RequireSection(container, type);
  auto input = OpenSectionStream(container, type);
  const auto header = ReadCoordinateCodecHeader(*input);
  if (header.element_count != expected_elements ||
      header.symbol_count != expected_symbols) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string(label) +
                    " codec metadata disagrees with the container");
  }
  auto expected_size = CheckedMetricAdd(
      kCoordinateCodecHeaderBytes, header.low_plane_bytes, label);
  expected_size =
      CheckedMetricAdd(expected_size, header.high_plane_bytes, label);
  RequireExactSectionSize(section, expected_size, label);
  return header;
}

std::uint64_t InspectLegacySaSection(const ParsedContainer& container,
                                     std::uint64_t expected_count,
                                     std::uint8_t width) {
  const auto& section =
      RequireSection(container, SectionType::kSuffixArray);
  auto input = OpenSectionStream(container, SectionType::kSuffixArray);
  if (ReadU64(*input, "suffix-array length") != expected_count) {
    throw Error(ErrorCode::kCorruptIndex,
                "suffix-array length does not match sampling metadata");
  }
  const auto payload = CheckedMetricMultiply(expected_count, width / 8U,
                                             "suffix array");
  RequireExactSectionSize(
      section, CheckedMetricAdd(kLegacySaHeaderBytes, payload, "suffix array"),
      "suffix array");
  return payload;
}

std::uint64_t InspectLegacyAuxSection(
    const ParsedContainer& container, SectionType type,
    std::uint64_t expected_count, std::uint8_t expected_width,
    const char* label) {
  const auto& section = RequireSection(container, type);
  auto input = OpenSectionStream(container, type);
  const auto count = ReadU64(*input, label);
  const int raw_width = input->get();
  if (count != expected_count || raw_width == std::char_traits<char>::eof() ||
      static_cast<std::uint8_t>(raw_width) != expected_width) {
    throw Error(ErrorCode::kCorruptIndex,
                std::string("invalid ") + label + " header");
  }
  const auto payload = CheckedMetricMultiply(
      expected_count, expected_width / 8U, label);
  RequireExactSectionSize(
      section, CheckedMetricAdd(kLegacyAuxHeaderBytes, payload, label), label);
  return payload;
}

void SkipSectionBytes(std::istream& input, std::uint64_t byte_count,
                      const char* label) {
  std::array<char, 4096> buffer{};
  while (byte_count != 0) {
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(byte_count, buffer.size()));
    input.read(buffer.data(), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      throw Error(ErrorCode::kCorruptIndex,
                  std::string(label) + " is truncated");
    }
    byte_count -= chunk;
  }
}

}  // namespace

IndexInfo IndexInfoFromContainer(const ParsedContainer& container) {
  const auto has = [&](SectionType type) {
    return std::any_of(container.sections.begin(), container.sections.end(),
                       [type](const SectionDescriptor& section) {
                         return section.type == type;
                       });
  };
  (void)RequireSection(container, SectionType::kMetadata);
  if (container.spec.kind == IndexKind::kSuffixArray) {
    (void)RequireSection(container, SectionType::kText);
    (void)RequireSection(container, SectionType::kSuffixArray);
    if (has(SectionType::kSdslCsa)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array container has an FM-index section");
    }
  } else {
    (void)RequireSection(container, SectionType::kSdslCsa);
    if (has(SectionType::kText) || has(SectionType::kSuffixArray) ||
        has(SectionType::kInverseSuffixArray) || has(SectionType::kLcp) ||
        has(SectionType::kChild) || has(SectionType::kLearnedSa) ||
        has(SectionType::kSaSampling)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "FM-index container has suffix-array-only sections");
    }
  }
  IndexInfo info;
  info.kind = container.spec.kind;
  info.format_version = "1." + std::to_string(container.spec.format_minor);
  info.library_version = std::to_string(container.spec.library_major) + "." +
                         std::to_string(container.spec.library_minor) + "." +
                         std::to_string(container.spec.library_patch);
  info.backend = StoredBackendName(container.spec.backend);
  info.backend_signature = StoredBackendSignature(container.spec.backend);
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
  if (container.spec.kind == IndexKind::kSuffixArray) {
    ValidateConstructionWidth(container);
    if (info.text_symbols < 2) {
      throw Error(ErrorCode::kCorruptIndex,
                  "suffix-array logical text is too short");
    }
    const auto& text_section =
        RequireSection(container, SectionType::kText);
    RequireExactSectionSize(text_section, info.text_symbols, "text");
    info.text_bytes = info.text_symbols;

    const bool isa = has(SectionType::kInverseSuffixArray);
    const bool lcp = has(SectionType::kLcp);
    const bool child = has(SectionType::kChild);
    const bool learned = has(SectionType::kLearnedSa);
    const bool sampled = has(SectionType::kSaSampling);
    if (!isa && !lcp && !child) {
      info.sa_acceleration = SaAcceleration::kNone;
    } else if (!isa && lcp && !child) {
      info.sa_acceleration = SaAcceleration::kLcp;
    } else if (!isa && lcp && child) {
      info.sa_acceleration = SaAcceleration::kLcpChild;
    } else if (isa && lcp && !child) {
      info.sa_acceleration = SaAcceleration::kLcpSuffixLink;
    } else if (isa && lcp && child) {
      info.sa_acceleration = SaAcceleration::kFull;
    } else {
      throw Error(ErrorCode::kCorruptIndex,
                  "invalid suffix-array auxiliary section combination");
    }
    if (info.sa_acceleration != SaAcceleration::kNone &&
        container.spec.format_minor < 1) {
      throw Error(ErrorCode::kCorruptIndex,
                  "auxiliary sections require sufidx format 1.1");
    }
    if (sampled) {
      if (container.spec.format_minor < 3) {
        throw Error(ErrorCode::kCorruptIndex,
                    "sampled SA data requires sufidx format 1.3");
      }
      auto input = OpenSectionStream(container, SectionType::kSaSampling);
      info.sa_sampling_rate = ReadU32(*input, "SA sampling rate");
      info.suffix_count = ReadU64(*input, "sampled suffix count");
      if (info.sa_sampling_rate <= 1) {
        throw Error(ErrorCode::kCorruptIndex, "invalid sampled SA metadata");
      }
      const auto expected_count =
          info.text_symbols == 0
              ? 0
              : 1 + (info.text_symbols - 1) / info.sa_sampling_rate;
      if (info.suffix_count != expected_count ||
          input->peek() != std::char_traits<char>::eof()) {
        throw Error(ErrorCode::kCorruptIndex, "invalid sampled SA metadata");
      }
    } else {
      info.sa_sampling_rate = 1;
      info.suffix_count = info.text_symbols;
    }
    if (container.spec.sa_resource_profile ==
            SaResourceProfile::kLowMemory &&
        (info.sa_acceleration != SaAcceleration::kLcp || learned || sampled)) {
      throw Error(ErrorCode::kCorruptIndex,
                  "low-memory profile has incompatible SA sections");
    }

    if (container.spec.format_minor >= 4) {
      const auto sa_header = InspectCoordinateSection(
          container, SectionType::kSuffixArray, info.suffix_count,
          info.text_symbols, "suffix array");
      info.stored_coordinate_width =
          static_cast<std::uint8_t>(sa_header.width);
      info.sa_bytes = CheckedMetricAdd(sa_header.low_plane_bytes,
                                       sa_header.high_plane_bytes,
                                       "suffix array");

      if (isa) {
        const auto header = InspectCoordinateSection(
            container, SectionType::kInverseSuffixArray, info.suffix_count,
            info.suffix_count, "inverse suffix array");
        info.isa_bytes = CheckedMetricAdd(header.low_plane_bytes,
                                          header.high_plane_bytes,
                                          "inverse suffix array");
      }
      if (lcp) {
        const auto& section = RequireSection(container, SectionType::kLcp);
        auto input = OpenSectionStream(container, SectionType::kLcp);
        const auto header = ReadLcpCodecHeader(*input);
        if (header.row_count != info.suffix_count ||
            header.sampling_rate != info.sa_sampling_rate ||
            header.text_symbols != info.text_symbols) {
          throw Error(ErrorCode::kCorruptIndex,
                      "LCP codec metadata disagrees with the container");
        }
        auto expected_size = CheckedMetricAdd(
            kLcpCodecHeaderBytes, header.primary_bytes, "LCP");
        expected_size = CheckedMetricAdd(expected_size,
                                         header.anchor_position_bytes, "LCP");
        expected_size = CheckedMetricAdd(expected_size,
                                         header.anchor_value_bytes, "LCP");
        RequireExactSectionSize(section, expected_size, "LCP");
        if (header.codec == SaLcpCodec::kByteCoded) {
          info.lcp_encoding = SaLcpEncoding::kByteCoded;
          info.lcp_primary_bytes = header.primary_bytes;
          info.lcp_overflow_anchors = header.anchor_count;
          info.lcp_overflow_bytes = CheckedMetricAdd(
              header.anchor_position_bytes, header.anchor_value_bytes,
              "LCP overflow");
          const auto block_count =
              1 + (info.text_symbols - 1) / LcpStorage::kGuideBlockSize;
          const auto guide_entries =
              CheckedMetricAdd(block_count, 1, "LCP guide");
          info.lcp_guide_bytes = CheckedMetricMultiply(
              guide_entries, header.coordinate_width / 8U, "LCP guide");
          info.lcp_bytes = CheckedMetricAdd(
              CheckedMetricAdd(info.lcp_primary_bytes,
                               info.lcp_overflow_bytes, "LCP"),
              info.lcp_guide_bytes, "LCP");
        } else {
          info.lcp_encoding = SaLcpEncoding::kRaw;
          info.lcp_bytes = header.primary_bytes;
        }
      }
      if (child) {
        if (info.suffix_count == std::numeric_limits<std::uint64_t>::max()) {
          throw Error(ErrorCode::kCorruptIndex,
                      "CHILD coordinate domain overflows");
        }
        const auto header = InspectCoordinateSection(
            container, SectionType::kChild, info.suffix_count,
            info.suffix_count + 1U, "CHILD");
        info.child_bytes = CheckedMetricAdd(header.low_plane_bytes,
                                            header.high_plane_bytes, "CHILD");
      }
    } else {
      info.stored_coordinate_width = info.coordinate_width;
      info.sa_bytes = InspectLegacySaSection(
          container, info.suffix_count, info.coordinate_width);
      if (isa) {
        info.isa_bytes = InspectLegacyAuxSection(
            container, SectionType::kInverseSuffixArray, info.suffix_count,
            info.coordinate_width, "inverse suffix array");
      }
      if (lcp) {
        const auto lcp_width = static_cast<std::uint8_t>(
            info.text_symbols <= std::numeric_limits<std::uint32_t>::max()
                ? 32
                : 64);
        info.lcp_encoding = SaLcpEncoding::kRaw;
        info.lcp_bytes = InspectLegacyAuxSection(
            container, SectionType::kLcp, info.suffix_count, lcp_width, "LCP");
      }
      if (child) {
        info.child_bytes = InspectLegacyAuxSection(
            container, SectionType::kChild, info.suffix_count,
            info.coordinate_width, "CHILD");
      }
    }

    std::uint64_t learned_resident_bytes = 0;
    if (learned) {
      if (container.spec.format_minor < 2) {
        throw Error(ErrorCode::kCorruptIndex,
                    "learned SA data requires sufidx format 1.2");
      }
      const auto& section =
          RequireSection(container, SectionType::kLearnedSa);
      auto input = OpenSectionStream(container, SectionType::kLearnedSa);
      const auto model_id = ReadU32(*input, "learned SA model ID");
      info.learned_k = ReadU32(*input, "learned SA k");
      info.learned_bucket_bits = ReadU32(*input, "learned SA bucket bits");
      info.learned_memory_overhead_basis_points =
          ReadU32(*input, "learned SA memory budget");
      const auto width = ReadU32(*input, "learned SA coordinate width");
      const auto anchors = ReadU64(*input, "learned SA anchor count");
      const auto fingerprint = ReadU64(*input, "learned SA fingerprint");
      if (model_id != 1) {
        throw Error(ErrorCode::kUnsupportedBackend,
                    "unsupported learned SA model ID");
      }
      const bool valid_coordinate_width =
          container.spec.format_minor >= 4
              ? width == 32 || width == 40 || width == 48 || width == 64
              : width == 32 || width == 64;
      if (info.learned_k == 0 || info.learned_k > 31 ||
          info.learned_bucket_bits > 2U * info.learned_k ||
          info.learned_bucket_bits > 31 ||
          !valid_coordinate_width ||
          (container.spec.format_minor < 4 &&
           width != info.coordinate_width) ||
          (container.spec.format_minor < 4 && width == 32 &&
           info.suffix_count > std::numeric_limits<std::uint32_t>::max()) ||
          anchors != (1ULL << info.learned_bucket_bits) + 1 ||
          fingerprint != info.fingerprint) {
        throw Error(ErrorCode::kCorruptIndex, "invalid learned SA header");
      }
      const auto key_bytes =
          CheckedMetricMultiply(anchors, 8, "learned SA keys");
      learned_resident_bytes = key_bytes;
      auto learned_size = CheckedMetricAdd(kLearnedSaHeaderBytes, key_bytes,
                                           "learned SA");
      if (container.spec.format_minor >= 4) {
        if (info.suffix_count == std::numeric_limits<std::uint64_t>::max()) {
          throw Error(ErrorCode::kCorruptIndex,
                      "learned SA coordinate domain overflows");
        }
        SkipSectionBytes(*input, key_bytes, "learned SA keys");
        const auto coordinate_header = ReadCoordinateCodecHeader(*input);
        if (coordinate_header.element_count != anchors ||
            coordinate_header.symbol_count != info.suffix_count + 1U ||
            static_cast<std::uint32_t>(coordinate_header.width) != width) {
          throw Error(ErrorCode::kCorruptIndex,
                      "learned SA coordinate codec is inconsistent");
        }
        learned_size = CheckedMetricAdd(
            learned_size, kCoordinateCodecHeaderBytes, "learned SA");
        learned_size = CheckedMetricAdd(
            learned_size, coordinate_header.low_plane_bytes, "learned SA");
        learned_size = CheckedMetricAdd(
            learned_size, coordinate_header.high_plane_bytes, "learned SA");
        learned_resident_bytes = CheckedMetricAdd(
            learned_resident_bytes,
            CheckedMetricAdd(coordinate_header.low_plane_bytes,
                             coordinate_header.high_plane_bytes,
                             "learned SA resident rows"),
            "learned SA resident payload");
      } else {
        const auto row_bytes =
            CheckedMetricMultiply(anchors, width / 8U, "learned SA rows");
        learned_size =
            CheckedMetricAdd(learned_size, row_bytes, "learned SA");
        learned_resident_bytes = CheckedMetricAdd(
            learned_resident_bytes, row_bytes,
            "learned SA resident payload");
      }
      RequireExactSectionSize(section, learned_size, "learned SA");
      info.sa_lookup_acceleration = SaLookupAcceleration::kSaplingPwl;
      info.learned_index_bytes = section.size;
    }

    info.sa_resource_profile = container.spec.sa_resource_profile;
    info.auxiliary_bytes = CheckedMetricAdd(
        CheckedMetricAdd(info.isa_bytes, info.lcp_bytes, "auxiliary"),
        info.child_bytes, "auxiliary");
    info.resident_core_bytes = CheckedMetricAdd(
        CheckedMetricAdd(
            CheckedMetricAdd(info.text_bytes, info.sa_bytes, "resident core"),
            info.auxiliary_bytes, "resident core"),
        learned_resident_bytes, "resident core");
  } else {
    if (info.coordinate_width != 64) {
      throw Error(ErrorCode::kCorruptIndex,
                  "SDSL CSA coordinate width must be 64 bits");
    }
    info.stored_coordinate_width = info.coordinate_width;
  }
  return info;
}

}  // namespace sufkit::detail
