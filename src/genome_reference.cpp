// SPDX-License-Identifier: MIT

#include "sufkit/genome_reference.hpp"

#include <zlib.h>

#include <array>
#include <limits>
#include <memory>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "kseq.h"

#include "genome_reference_internal.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
KSEQ_INIT(gzFile, gzread)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace sufkit {
namespace {

constexpr std::array<std::uint8_t, 256> MakeReferenceEncodingTable() {
  std::array<std::uint8_t, 256> table{};
  for (auto& value : table) {
    value = detail::kN;
  }
  table[static_cast<unsigned char>('A')] = detail::kA;
  table[static_cast<unsigned char>('a')] = detail::kA;
  table[static_cast<unsigned char>('C')] = detail::kC;
  table[static_cast<unsigned char>('c')] = detail::kC;
  table[static_cast<unsigned char>('G')] = detail::kG;
  table[static_cast<unsigned char>('g')] = detail::kG;
  table[static_cast<unsigned char>('T')] = detail::kT;
  table[static_cast<unsigned char>('t')] = detail::kT;
  return table;
}

constexpr auto kReferenceEncoding = MakeReferenceEncodingTable();

std::uint8_t NormalizeBase(char raw, bool& ambiguous) {
  const auto encoded =
      kReferenceEncoding[static_cast<unsigned char>(raw)];
  if (encoded == detail::kN) {
    // Strict ACGT queries cannot match this code, so every ambiguous input
    // symbol becomes a hard boundary without losing its base coordinate.
    ambiguous = true;
  }
  return encoded;
}

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

class ReferenceBuilder {
 public:
  ReferenceBuilder(std::size_t expected_records,
                   std::size_t expected_encoded_bytes) {
    data_.sequences.reserve(expected_records);
    data_.contig_starts.reserve(expected_records);
    data_.contig_lengths.reserve(expected_records);
    data_.encoded.reserve(expected_encoded_bytes);
    names_.reserve(expected_records);
  }

  void AddRecord(std::string name, std::string description,
                 std::string_view sequence) {
    ValidateRecord(name, sequence);
    AppendRecord(std::move(name), std::move(description), sequence);
  }

  void AddValidatedRecord(std::string name, std::string description,
                          std::string_view sequence) {
    AppendRecord(std::move(name), std::move(description), sequence);
  }

  detail::ReferenceData Finish() {
    if (data_.sequences.empty()) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference must contain at least one FASTA record");
    }
    data_.fingerprint = fingerprint_;
    return std::move(data_);
  }

 private:
  void ValidateRecord(const std::string& name, std::string_view sequence) {
    if (name.empty()) {
      throw Error(ErrorCode::kInvalidInput,
                  "FASTA record name must not be empty");
    }
    if (!names_.insert(name).second) {
      throw Error(ErrorCode::kInvalidInput,
                  "duplicate FASTA record name: " + name);
    }
    if (sequence.empty()) {
      throw Error(ErrorCode::kInvalidInput,
                  "FASTA record must not be empty: " + name);
    }
    if (data_.sequences.size() >
        static_cast<std::size_t>(std::numeric_limits<SequenceId>::max())) {
      throw Error(ErrorCode::kInvalidInput, "too many FASTA records");
    }
    const auto length = static_cast<std::uint64_t>(sequence.size());
    if (next_offset_ == std::numeric_limits<std::uint64_t>::max() ||
        length >
            std::numeric_limits<std::uint64_t>::max() - next_offset_ - 1) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference length overflows 64-bit coordinates");
    }
    if (sequence.size() >=
        data_.encoded.max_size() - data_.encoded.size()) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference exceeds the addressable byte-buffer size");
    }
  }

  void AppendByte(std::uint8_t value) {
    data_.encoded.push_back(value);
    fingerprint_ ^= value;
    fingerprint_ *= kFnvPrime;
  }

  void AppendRecord(std::string name, std::string description,
                    std::string_view sequence) {
    SequenceInfo info;
    info.id = static_cast<SequenceId>(data_.sequences.size());
    info.name = std::move(name);
    info.description = std::move(description);
    info.length = static_cast<std::uint64_t>(sequence.size());
    info.global_offset = next_offset_;

    for (const char raw : sequence) {
      bool ambiguous = false;
      AppendByte(NormalizeBase(raw, ambiguous));
      if (ambiguous) {
        ++info.ambiguous_bases;
      }
    }
    // A distinct non-queryable symbol prevents suffix matches from crossing
    // between adjacent contigs in the concatenated logical text.
    AppendByte(detail::kSeparator);
    data_.total_bases += info.length;
    data_.ambiguous_bases += info.ambiguous_bases;
    next_offset_ += info.length + 1;
    data_.contig_starts.push_back(info.global_offset);
    data_.contig_lengths.push_back(info.length);
    data_.sequences.push_back(std::move(info));
  }

  detail::ReferenceData data_;
  std::unordered_set<std::string> names_;
  std::uint64_t next_offset_ = 0;
  std::uint64_t fingerprint_ = kFnvOffsetBasis;
};

std::size_t ValidateAndMeasureRecords(
    const std::vector<SequenceRecord>& records) {
  if (records.empty()) {
    throw Error(ErrorCode::kInvalidInput,
                "reference must contain at least one FASTA record");
  }

  std::unordered_set<std::string_view> names;
  names.reserve(records.size());
  const auto max_encoded_size = std::vector<std::uint8_t>{}.max_size();
  std::uint64_t next_offset = 0;
  std::size_t encoded_size = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (record.name.empty()) {
      throw Error(ErrorCode::kInvalidInput,
                  "FASTA record name must not be empty");
    }
    if (!names.insert(record.name).second) {
      throw Error(ErrorCode::kInvalidInput,
                  "duplicate FASTA record name: " + record.name);
    }
    if (record.sequence.empty()) {
      throw Error(ErrorCode::kInvalidInput,
                  "FASTA record must not be empty: " + record.name);
    }
    if (index >
        static_cast<std::size_t>(std::numeric_limits<SequenceId>::max())) {
      throw Error(ErrorCode::kInvalidInput, "too many FASTA records");
    }

    const auto length = static_cast<std::uint64_t>(record.sequence.size());
    if (next_offset == std::numeric_limits<std::uint64_t>::max() ||
        length >
            std::numeric_limits<std::uint64_t>::max() - next_offset - 1) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference length overflows 64-bit coordinates");
    }
    if (record.sequence.size() >= max_encoded_size - encoded_size) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference exceeds the addressable byte-buffer size");
    }
    encoded_size += record.sequence.size() + 1;
    next_offset += length + 1;
  }
  return encoded_size;
}

}  // namespace

GenomeReference::GenomeReference(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

GenomeReference::GenomeReference(GenomeReference&&) noexcept = default;
GenomeReference& GenomeReference::operator=(GenomeReference&&) noexcept =
    default;
GenomeReference::~GenomeReference() = default;

GenomeReference GenomeReference::FromFasta(const std::filesystem::path& path) {
  const std::string native = path.string();
  gzFile file = gzopen(native.c_str(), "rb");
  if (file == nullptr) {
    throw Error(ErrorCode::kIoError, "cannot open FASTA: " + native);
  }
  struct GzCloser {
    void operator()(gzFile_s* value) const noexcept {
      if (value != nullptr) {
        gzclose(value);
      }
    }
  };
  std::unique_ptr<gzFile_s, GzCloser> file_guard(file);
  kseq_t* raw_sequence = kseq_init(file);
  if (raw_sequence == nullptr) {
    throw Error(ErrorCode::kIoError, "cannot initialize kseq for: " + native);
  }
  struct KseqCloser {
    void operator()(kseq_t* value) const noexcept { kseq_destroy(value); }
  };
  std::unique_ptr<kseq_t, KseqCloser> sequence(raw_sequence);

  ReferenceBuilder builder(0, 0);
  int status = 0;
  while ((status = kseq_read(sequence.get())) >= 0) {
    std::string name;
    if (sequence->name.s != nullptr) {
      name.assign(sequence->name.s, sequence->name.l);
    }
    std::string description;
    if (sequence->comment.s != nullptr) {
      description.assign(sequence->comment.s, sequence->comment.l);
    }
    const char* sequence_data =
        sequence->seq.s == nullptr ? "" : sequence->seq.s;
    builder.AddRecord(
        std::move(name), std::move(description),
        std::string_view(sequence_data, sequence->seq.l));
  }
  if (status < -1) {
    throw Error(ErrorCode::kInvalidInput, "malformed FASTA input: " + native);
  }
  auto impl = std::make_unique<Impl>();
  impl->data = builder.Finish();
  return GenomeReference(std::move(impl));
}

GenomeReference GenomeReference::FromRecords(
    std::vector<SequenceRecord> records) {
  const auto encoded_size = ValidateAndMeasureRecords(records);
  ReferenceBuilder builder(records.size(), encoded_size);
  for (auto& record : records) {
    builder.AddValidatedRecord(std::move(record.name),
                               std::move(record.description), record.sequence);
  }
  auto impl = std::make_unique<Impl>();
  impl->data = builder.Finish();
  return GenomeReference(std::move(impl));
}

std::uint64_t GenomeReference::SequenceCount() const noexcept {
  return static_cast<std::uint64_t>(impl_->data.sequences.size());
}

std::uint64_t GenomeReference::TotalBases() const noexcept {
  return impl_->data.total_bases;
}

std::uint64_t GenomeReference::AmbiguousBases() const noexcept {
  return impl_->data.ambiguous_bases;
}

std::uint64_t GenomeReference::Fingerprint() const noexcept {
  return impl_->data.fingerprint;
}

SequenceInfo GenomeReference::GetSequenceInfo(SequenceId id) const {
  const auto index = static_cast<std::size_t>(id);
  if (index >= impl_->data.sequences.size()) {
    throw Error(ErrorCode::kInvalidInput, "sequence id is out of range");
  }
  return impl_->data.sequences[index];
}

}  // namespace sufkit
