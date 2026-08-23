// SPDX-License-Identifier: MIT

#include "sufkit/genome_reference.hpp"

#include <zlib.h>

#include <cctype>
#include <limits>
#include <memory>
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

std::uint8_t NormalizeBase(char raw, bool& ambiguous) {
  const auto value = static_cast<unsigned char>(raw);
  switch (static_cast<char>(std::toupper(value))) {
    case 'A':
      return detail::kA;
    case 'C':
      return detail::kC;
    case 'G':
      return detail::kG;
    case 'T':
      return detail::kT;
    default:
      // Strict ACGT queries cannot match this code, so every ambiguous input
      // symbol becomes a hard boundary without losing its base coordinate.
      ambiguous = true;
      return detail::kN;
  }
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

  std::vector<SequenceRecord> records;
  int status = 0;
  while ((status = kseq_read(sequence.get())) >= 0) {
    SequenceRecord record;
    if (sequence->name.s != nullptr) {
      record.name.assign(sequence->name.s, sequence->name.l);
    }
    if (sequence->comment.s != nullptr) {
      record.description.assign(sequence->comment.s, sequence->comment.l);
    }
    if (sequence->seq.s != nullptr) {
      record.sequence.assign(sequence->seq.s, sequence->seq.l);
    }
    records.push_back(std::move(record));
  }
  if (status < -1) {
    throw Error(ErrorCode::kInvalidInput, "malformed FASTA input: " + native);
  }
  return FromRecords(std::move(records));
}

GenomeReference GenomeReference::FromRecords(
    std::vector<SequenceRecord> records) {
  if (records.empty()) {
    throw Error(ErrorCode::kInvalidInput,
                "reference must contain at least one FASTA record");
  }

  auto impl = std::make_unique<Impl>();
  std::unordered_set<std::string> names;
  std::uint64_t next_offset = 0;
  for (std::size_t index = 0; index < records.size(); ++index) {
    auto& record = records[index];
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

    SequenceInfo info;
    info.id = static_cast<SequenceId>(index);
    info.name = std::move(record.name);
    info.description = std::move(record.description);
    info.length = static_cast<std::uint64_t>(record.sequence.size());
    info.global_offset = next_offset;

    if (next_offset == std::numeric_limits<std::uint64_t>::max() ||
        info.length >
            std::numeric_limits<std::uint64_t>::max() - next_offset - 1) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference length overflows 64-bit coordinates");
    }
    if (record.sequence.size() >=
        impl->data.encoded.max_size() - impl->data.encoded.size()) {
      throw Error(ErrorCode::kInvalidInput,
                  "reference exceeds the addressable byte-buffer size");
    }
    impl->data.encoded.reserve(impl->data.encoded.size() +
                               record.sequence.size() + 1);
    for (const char raw : record.sequence) {
      bool ambiguous = false;
      impl->data.encoded.push_back(NormalizeBase(raw, ambiguous));
      if (ambiguous) {
        ++info.ambiguous_bases;
      }
    }
    // A distinct non-queryable symbol prevents suffix matches from crossing
    // between adjacent contigs in the concatenated logical text.
    impl->data.encoded.push_back(detail::kSeparator);
    impl->data.total_bases += info.length;
    impl->data.ambiguous_bases += info.ambiguous_bases;
    next_offset += info.length + 1;
    impl->data.sequences.push_back(std::move(info));
  }
  // The fingerprint covers normalized bases and separators, making it stable
  // across FASTA wrapping and letter case while retaining contig boundaries.
  impl->data.fingerprint = detail::ContentFingerprint(impl->data.encoded);
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
