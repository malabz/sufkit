// SPDX-License-Identifier: MIT

#include "app_support.hpp"

#include <zlib.h>

#include <limits>
#include <memory>
#include <stdexcept>

#include "kseq.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
KSEQ_INIT(gzFile, gzread)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace sufkit::app {

std::vector<SequenceRecord> ReadFastaRecords(
    const std::filesystem::path& path) {
  const auto native = path.string();
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
  kseq_t* raw = kseq_init(file);
  if (raw == nullptr) {
    throw Error(ErrorCode::kIoError, "cannot initialize kseq for: " + native);
  }
  struct KseqCloser {
    void operator()(kseq_t* value) const noexcept { kseq_destroy(value); }
  };
  std::unique_ptr<kseq_t, KseqCloser> sequence(raw);

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
  return records;
}

std::uint64_t ParseUnsigned(const std::string& text,
                            const std::string& option_name) {
  if (text.empty() || text.front() == '-') {
    throw Error(ErrorCode::kInvalidInput,
                option_name + " requires a non-negative integer");
  }
  std::size_t consumed = 0;
  std::uint64_t value = 0;
  try {
    value = std::stoull(text, &consumed, 10);
  } catch (const std::exception&) {
    throw Error(ErrorCode::kInvalidInput, option_name + " requires an integer");
  }
  if (consumed != text.size()) {
    throw Error(ErrorCode::kInvalidInput, option_name + " requires an integer");
  }
  return value;
}

}  // namespace sufkit::app
