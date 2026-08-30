// SPDX-License-Identifier: MIT

#include <unistd.h>
#include <zlib.h>

#include <sys/wait.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <sufkit/sufkit.hpp>

namespace {

int failures = 0;

#define CHECK(condition)                               \
  do {                                                 \
    if (!(condition)) {                                \
      std::cerr << __FILE__ << ':' << __LINE__         \
                << ": CHECK failed: " #condition "\n"; \
      ++failures;                                      \
    }                                                  \
  } while (false)

template <class Function>
void CheckError(sufkit::ErrorCode expected, Function&& function) {
  try {
    function();
    std::cerr << "expected sufkit::Error was not thrown\n";
    ++failures;
  } catch (const sufkit::Error& error) {
    CHECK(error.Code() == expected);
  }
}

std::filesystem::path TestDirectory() {
  return std::filesystem::path("/tmp") /
         ("sufkit-tests-" + std::to_string(static_cast<long long>(getpid())));
}

sufkit::GenomeReference MakeReference() {
  return sufkit::GenomeReference::FromRecords({{"chr1", "first", "acgtNACGT"},
                                               {"chr2", "second", "TTTACGTAAA"},
                                               {"chr3", "third", "AAA"}});
}

void CheckMatchResults(const sufkit::QueryResult& result) {
  CHECK(result.total_hits == 3);
  CHECK(result.hits.size() == 3);
  CHECK(!result.truncated);
  CHECK(result.hits[0].sequence_id == 0 && result.hits[0].position == 0);
  CHECK(result.hits[1].sequence_id == 0 && result.hits[1].position == 5);
  CHECK(result.hits[2].sequence_id == 1 && result.hits[2].position == 3);
}

bool SameQueryResult(const sufkit::QueryResult& left,
                     const sufkit::QueryResult& right) {
  if (left.total_hits != right.total_hits ||
      left.truncated != right.truncated ||
      left.hits.size() != right.hits.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.hits.size(); ++index) {
    const auto& lhs = left.hits[index];
    const auto& rhs = right.hits[index];
    if (std::tie(lhs.sequence_id, lhs.position, lhs.length, lhs.strand) !=
        std::tie(rhs.sequence_id, rhs.position, rhs.length, rhs.strand)) {
      return false;
    }
  }
  return true;
}

void TestReferenceAndFasta(const std::filesystem::path& directory) {
  auto reference = MakeReference();
  CHECK(reference.SequenceCount() == 3);
  CHECK(reference.TotalBases() == 22);
  CHECK(reference.AmbiguousBases() == 1);
  CHECK(reference.GetSequenceInfo(1).name == "chr2");
  CHECK(reference.GetSequenceInfo(1).global_offset == 10);

  CheckError(sufkit::ErrorCode::kInvalidInput,
             [] { (void)sufkit::GenomeReference::FromRecords({}); });
  CheckError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::GenomeReference::FromRecords(
        {{"dup", "", "A"}, {"dup", "", "C"}});
  });
  CheckError(sufkit::ErrorCode::kInvalidInput, [] {
    (void)sufkit::GenomeReference::FromRecords({{"empty", "", ""}});
  });

  const auto fasta = directory / "reference.fa";
  {
    std::ofstream output(fasta, std::ios::binary);
    output << ">chr1 first\r\nacgtN\r\nACGT\r\n"
           << ">chr2 second\r\nTTTACGTAAA\r\n"
           << ">chr3 third\r\nAAA\r\n";
  }
  auto plain = sufkit::GenomeReference::FromFasta(fasta);
  CHECK(plain.TotalBases() == reference.TotalBases());
  CHECK(plain.Fingerprint() == reference.Fingerprint());

  const auto gzip = directory / "reference.fa.gz";
  gzFile gz = gzopen(gzip.c_str(), "wb");
  CHECK(gz != nullptr);
  const std::string contents =
      ">chr1 first\nacgtNACGT\n>chr2 second\nTTTACGTAAA\n>chr3 third\nAAA\n";
  CHECK(gzwrite(gz, contents.data(), static_cast<unsigned>(contents.size())) ==
        static_cast<int>(contents.size()));
  CHECK(gzclose(gz) == Z_OK);
  auto compressed = sufkit::GenomeReference::FromFasta(gzip);
  CHECK(compressed.Fingerprint() == reference.Fingerprint());

  std::vector<sufkit::SequenceRecord> many_records;
  many_records.reserve(257);
  const auto many_fasta = directory / "many-contigs.fa";
  {
    std::ofstream output(many_fasta, std::ios::binary);
    static constexpr std::array<char, 7> kInputBases{
        {'a', 'C', 'g', 'T', 'N', 'u', 'R'}};
    for (std::size_t index = 0; index < 257; ++index) {
      std::string bases;
      bases.reserve(1 + index % 97);
      for (std::size_t position = 0; position < 1 + index % 97; ++position) {
        bases.push_back(kInputBases[(index + position) % kInputBases.size()]);
      }
      const std::string name = "contig-" + std::to_string(index);
      const std::string description = "record " + std::to_string(index);
      output << '>' << name << ' ' << description << '\n' << bases << '\n';
      many_records.push_back({name, description, std::move(bases)});
    }
  }
  const auto many_from_records =
      sufkit::GenomeReference::FromRecords(many_records);
  const auto many_from_fasta = sufkit::GenomeReference::FromFasta(many_fasta);
  CHECK(many_from_fasta.SequenceCount() == many_from_records.SequenceCount());
  CHECK(many_from_fasta.TotalBases() == many_from_records.TotalBases());
  CHECK(many_from_fasta.AmbiguousBases() ==
        many_from_records.AmbiguousBases());
  CHECK(many_from_fasta.Fingerprint() == many_from_records.Fingerprint());
  CHECK(many_from_fasta.GetSequenceInfo(256).name == "contig-256");
  CHECK(many_from_fasta.GetSequenceInfo(256).description == "record 256");

  auto ambiguous =
      sufkit::GenomeReference::FromRecords({{"mixed", "", "aCuR-"}});
  CHECK(ambiguous.TotalBases() == 5);
  CHECK(ambiguous.AmbiguousBases() == 3);

  const auto empty_fasta = directory / "empty.fa";
  std::ofstream(empty_fasta, std::ios::binary);
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)sufkit::GenomeReference::FromFasta(empty_fasta); });
  CheckError(sufkit::ErrorCode::kIoError, [&] {
    (void)sufkit::GenomeReference::FromFasta(directory / "missing.fa");
  });

  const auto duplicate_fasta = directory / "duplicate.fa";
  {
    std::ofstream output(duplicate_fasta, std::ios::binary);
    output << ">same\nA\n>same\nC\n";
  }
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    (void)sufkit::GenomeReference::FromFasta(duplicate_fasta);
  });

  const auto broken_gzip = directory / "broken.fa.gz";
  {
    std::ofstream output(broken_gzip, std::ios::binary);
    const std::array<unsigned char, 8> bytes{
        {0x1f, 0x8b, 0x08, 0x00, 0xff, 0xff, 0xff, 0xff}};
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)sufkit::GenomeReference::FromFasta(broken_gzip); });
}

void TestSuffixArray(const std::filesystem::path& directory) {
  auto reference = MakeReference();
  auto sa32 = sufkit::SuffixArray::Build(
      reference,
      {sufkit::SaBackend::kDivsufsort, sufkit::CoordinateWidth::kBits32, 1});
  auto sa64 = sufkit::SuffixArray::Build(
      reference,
      {sufkit::SaBackend::kDivsufsort, sufkit::CoordinateWidth::kBits64, 1});
  CHECK(sa32.GetInfo().coordinate_width == 32);
  CHECK(sa64.GetInfo().coordinate_width == 64);
  CHECK(sa32.GetInfo().stored_coordinate_width == 32);
  // Construction width is independent of the final resident representation:
  // a 64-bit constructor may safely down-pack a small index to uint32_t.
  CHECK(sa64.GetInfo().stored_coordinate_width == 32);
  CHECK(sa32.GetInfo().text_symbols ==
        reference.TotalBases() + reference.SequenceCount() + 1);
  CHECK(sa32.GetInfo().sa_bytes == sa32.GetInfo().suffix_count * 4ULL);
  CHECK(sa64.GetInfo().sa_bytes == sa64.GetInfo().suffix_count * 4ULL);
  constexpr auto kExpectedLcpEncoding = sufkit::SaLcpEncoding::kRaw;
  CHECK(sa32.GetInfo().lcp_encoding == kExpectedLcpEncoding);
  CHECK(sa64.GetInfo().lcp_encoding == kExpectedLcpEncoding);
  CHECK(sa32.GetInfo().auxiliary_bytes ==
        sa32.GetInfo().isa_bytes + sa32.GetInfo().lcp_bytes +
            sa32.GetInfo().child_bytes);
  CHECK(sa64.GetInfo().auxiliary_bytes ==
        sa64.GetInfo().isa_bytes + sa64.GetInfo().lcp_bytes +
            sa64.GetInfo().child_bytes);

  const std::vector<std::uint8_t> logical_text{2, 3, 4, 5, 6, 2, 3, 4, 5,
                                               1, 5, 5, 5, 2, 3, 4, 5, 2,
                                               2, 2, 1, 2, 2, 2, 1, 0};
  std::vector<bool> seen(logical_text.size(), false);
  std::uint64_t previous = 0;
  for (std::uint64_t row = 0; row < logical_text.size(); ++row) {
    const auto suffix = sa32.SuffixAt(row);
    CHECK(suffix < logical_text.size());
    if (suffix < logical_text.size()) {
      CHECK(!seen[static_cast<std::size_t>(suffix)]);
      seen[static_cast<std::size_t>(suffix)] = true;
      if (row != 0) {
        CHECK(std::lexicographical_compare(
            logical_text.begin() + static_cast<std::ptrdiff_t>(previous),
            logical_text.end(),
            logical_text.begin() + static_cast<std::ptrdiff_t>(suffix),
            logical_text.end()));
      }
      previous = suffix;
    }
  }
  CHECK(
      std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
  CHECK(sa32.Count("ACGT") == 3);
  CHECK(sa64.Count("acgt") == 3);
  CHECK(sa32.EqualRange("ACGT").Size() == 3);
  CheckMatchResults(sa32.Locate("ACGT"));
  CheckMatchResults(sa64.Locate("ACGT"));
  CHECK(sa32.Count("GTTT") == 0);  // would require crossing a contig separator
  CHECK(sa32.Count("GTAC") == 0);  // would require crossing the N in chr1
  CHECK(sa32.Count("AAA", sufkit::StrandMode::kBoth) == 3);
  const auto invalid_strand = static_cast<sufkit::StrandMode>(255);
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)sa32.Count("ACGT", invalid_strand); });
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    sufkit::LocateOptions invalid;
    invalid.strands = invalid_strand;
    (void)sa32.Locate("ACGT", invalid);
  });

  sufkit::LocateOptions limited;
  limited.max_hits = 1;
  const auto truncated = sa32.Locate("ACGT", limited);
  CHECK(truncated.total_hits == 3);
  CHECK(truncated.hits.size() == 1);
  CHECK(truncated.truncated);
  limited.max_hits = 0;
  const auto omitted = sa32.Locate("ACGT", limited);
  CHECK(omitted.total_hits == 3);
  CHECK(omitted.hits.empty());
  CHECK(omitted.truncated);
  limited.max_hits = 10;
  CheckMatchResults(sa32.Locate("ACGT", limited));

  sufkit::LocateOptions both;
  both.strands = sufkit::StrandMode::kBoth;
  const auto palindrome = sa32.Locate("ACGT", both);
  CHECK(palindrome.total_hits == 3);
  CHECK(std::all_of(
      palindrome.hits.begin(), palindrome.hits.end(),
      [](const auto& match) { return match.strand == sufkit::Strand::kBoth; }));

  const auto directional = sa32.Locate("AAA", both);
  CHECK(directional.total_hits == 3);
  CHECK(directional.hits.size() == 3);
  CHECK(!directional.truncated);
  CHECK(directional.hits[0].sequence_id == 1 &&
        directional.hits[0].position == 0 &&
        directional.hits[0].strand == sufkit::Strand::kReverseComplement);
  CHECK(directional.hits[1].sequence_id == 1 &&
        directional.hits[1].position == 7 &&
        directional.hits[1].strand == sufkit::Strand::kForward);
  CHECK(directional.hits[2].sequence_id == 2 &&
        directional.hits[2].position == 0 &&
        directional.hits[2].strand == sufkit::Strand::kForward);
  both.max_hits = 1;
  const auto directional_first = sa32.Locate("AAA", both);
  CHECK(directional_first.total_hits == directional.total_hits);
  CHECK(directional_first.hits.size() == 1);
  CHECK(directional_first.truncated);
  CHECK(directional_first.hits[0].sequence_id == 1 &&
        directional_first.hits[0].position == 0 &&
        directional_first.hits[0].strand ==
            sufkit::Strand::kReverseComplement);
  const auto palindrome_first = sa32.Locate("ACGT", both);
  CHECK(palindrome_first.total_hits == palindrome.total_hits);
  CHECK(palindrome_first.hits.size() == 1);
  CHECK(palindrome_first.hits[0].strand == sufkit::Strand::kBoth);
  both.max_hits = 2;
  const auto directional_limited = sa32.Locate("AAA", both);
  CHECK(directional_limited.total_hits == 3);
  CHECK(directional_limited.hits.size() == 2);
  CHECK(directional_limited.truncated);
  CHECK(directional_limited.hits[0].sequence_id == 1 &&
        directional_limited.hits[0].position == 0);
  CHECK(directional_limited.hits[1].sequence_id == 1 &&
        directional_limited.hits[1].position == 7);
  both.max_hits = 3;
  const auto directional_exact_limit = sa32.Locate("AAA", both);
  CHECK(directional_exact_limit.hits.size() == 3);
  CHECK(!directional_exact_limit.truncated);
  sufkit::LocateOptions huge_limit;
  huge_limit.max_hits = std::numeric_limits<std::uint64_t>::max();
  const auto absent_with_huge_limit = sa32.Locate("GTTT", huge_limit);
  CHECK(absent_with_huge_limit.total_hits == 0);
  CHECK(absent_with_huge_limit.hits.empty());
  CHECK(!absent_with_huge_limit.truncated);

  CheckError(sufkit::ErrorCode::kInvalidInput, [&] { (void)sa32.Count(""); });
  CheckError(sufkit::ErrorCode::kInvalidInput,
             [&] { (void)sa32.Count("ACNT"); });
  const auto path = directory / "reference.sa.sufidx";
  sa32.Save(path);
  CHECK(std::filesystem::exists(path));
  CheckError(sufkit::ErrorCode::kIoError, [&] { sa32.Save(path); });
  const auto inspected = sufkit::InspectIndex(path);
  CHECK(inspected.kind == sufkit::IndexKind::kSuffixArray);
  CHECK(inspected.backend == "divsufsort32");
  auto loaded = sufkit::SuffixArray::Load(path);
  CheckMatchResults(loaded.Locate("ACGT"));

  // The container records the 64-bit constructor independently from the
  // down-packed 32-bit resident SA representation.
  const auto path64 = directory / "reference.sa64.sufidx";
  sa64.Save(path64);
  auto loaded64 = sufkit::SuffixArray::Load(path64);
  CHECK(loaded64.GetInfo().coordinate_width == 64);
  CHECK(loaded64.GetInfo().stored_coordinate_width == 32);
  CHECK(loaded64.GetInfo().auxiliary_bytes ==
        sa64.GetInfo().auxiliary_bytes);
  CheckMatchResults(loaded64.Locate("ACGT"));

  // A 32-bit SA block at this size crosses the 256 KiB container-buffer
  // boundary, covering both buffered and direct checksummed writes.
  std::string buffered_sequence(65535, 'A');
  static constexpr std::array<char, 4> kBases{{'A', 'C', 'G', 'T'}};
  for (std::size_t index = 0; index < buffered_sequence.size(); ++index) {
    buffered_sequence[index] =
        kBases[(index * 17 + index / 11) % kBases.size()];
  }
  const auto buffered_reference = sufkit::GenomeReference::FromRecords(
      {{"buffered", "", std::move(buffered_sequence)}});
  sufkit::SuffixArrayBuildOptions buffered_options;
  buffered_options.coordinate_width = sufkit::CoordinateWidth::kBits32;
  buffered_options.acceleration = sufkit::SaAcceleration::kNone;
  auto buffered_index =
      sufkit::SuffixArray::Build(buffered_reference, buffered_options);
  const auto buffered_path = directory / "buffered-boundary.sufidx";
  buffered_index.Save(buffered_path);
  CHECK(std::filesystem::file_size(buffered_path) > 256U * 1024U);
  auto buffered_loaded = sufkit::SuffixArray::Load(buffered_path);
  CHECK(buffered_loaded.Count("ACGTACGT") ==
        buffered_index.Count("ACGTACGT"));

  // Two writers racing without overwrite permission must not both publish.
  const auto concurrent_path = directory / "concurrent-save.sufidx";
  int gate[2]{};
  CHECK(pipe(gate) == 0);
  std::array<pid_t, 2> children{};
  for (auto& child : children) {
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
      close(gate[1]);
      char token = 0;
      if (read(gate[0], &token, 1) != 1) {
        _exit(20);
      }
      try {
        buffered_index.Save(concurrent_path);
        _exit(0);
      } catch (const sufkit::Error& error) {
        _exit(error.Code() == sufkit::ErrorCode::kIoError ? 10 : 11);
      } catch (...) {
        _exit(12);
      }
    }
  }
  close(gate[0]);
  CHECK(write(gate[1], "xx", 2) == 2);
  close(gate[1]);
  unsigned successes = 0;
  unsigned conflicts = 0;
  for (const auto child : children) {
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      ++successes;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 10) {
      ++conflicts;
    }
  }
  CHECK(successes == 1);
  CHECK(conflicts == 1);
  auto concurrent_loaded = sufkit::SuffixArray::Load(concurrent_path);
  CHECK(concurrent_loaded.Count("ACGTACGT") ==
        buffered_index.Count("ACGTACGT"));
}

void TestManyContigCoordinateMapping(
    const std::filesystem::path& directory) {
  std::vector<sufkit::SequenceRecord> records;
  records.reserve(1024);
  for (std::size_t index = 0; index < 1023; ++index) {
    records.push_back(
        {"many-" + std::to_string(index), "", "AAAAAAAA"});
  }
  records.push_back({"many-1023", "", "CCCGGGTT"});
  const auto reference =
      sufkit::GenomeReference::FromRecords(std::move(records));

  auto check = [](const auto& index) {
    const auto result = index.Locate("CCCGGGTT");
    CHECK(result.total_hits == 1);
    CHECK(result.hits.size() == 1);
    CHECK(result.hits[0].sequence_id == 1023);
    CHECK(result.hits[0].position == 0);
    CHECK(index.Count("AAC") == 0);  // cannot cross a contig separator
  };

  auto sa = sufkit::SuffixArray::Build(reference);
  auto fm = sufkit::FmIndex::Build(reference);
  check(sa);
  check(fm);

  const auto sa_path = directory / "many-contigs.sa.sufidx";
  const auto fm_path = directory / "many-contigs.fm.sufidx";
  sa.Save(sa_path);
  fm.Save(fm_path);
  auto loaded_sa = sufkit::SuffixArray::Load(sa_path);
  auto loaded_fm = sufkit::FmIndex::Load(fm_path);
  check(loaded_sa);
  check(loaded_fm);
}

void TestFmIndex(const std::filesystem::path& directory) {
  auto reference = MakeReference();
  const auto available = sufkit::AvailableFmBackends();
  CHECK(
      std::any_of(available.begin(), available.end(), [](const auto& backend) {
        return backend.name == "sdsl-csa-wt-balanced" && backend.available;
      }));
  CHECK(
      std::any_of(available.begin(), available.end(), [](const auto& backend) {
        return backend.name == "sdsl-csa-wt-epr" && backend.available;
      }));
  CHECK(
      std::any_of(available.begin(), available.end(), [](const auto& backend) {
        return backend.name == "sdsl-csa-sada" && !backend.available;
      }));
  auto fm = sufkit::FmIndex::Build(reference);
  CHECK(fm.GetInfo().backend == "sdsl-csa-wt-huff");
  CHECK(fm.GetInfo().backend_signature ==
        "sdsl::csa_wt<sdsl::wt_huff<>,32,64>");
  CHECK(fm.GetInfo().sdsl_version == "3.0.3");
  CHECK(fm.GetInfo().text_symbols ==
        reference.TotalBases() + reference.SequenceCount() + 1);
  CHECK(fm.EqualRange("ACGT").Size() == 3);
  CHECK(fm.Count("ACGT") == 3);
  CheckMatchResults(fm.Locate("ACGT"));
  CHECK(fm.Count("GTTT") == 0);

  sufkit::LocateOptions limited;
  limited.max_hits = 0;
  const auto omitted = fm.Locate("ACGT", limited);
  CHECK(omitted.total_hits == 3);
  CHECK(omitted.hits.empty());
  CHECK(omitted.truncated);
  limited.max_hits = 1;
  const auto first = fm.Locate("ACGT", limited);
  CHECK(first.total_hits == 3);
  CHECK(first.hits.size() == 1);
  CHECK(first.hits[0].sequence_id == 0 && first.hits[0].position == 0);
  CHECK(first.truncated);
  limited.max_hits = 3;
  CheckMatchResults(fm.Locate("ACGT", limited));
  limited.max_hits = 10;
  CheckMatchResults(fm.Locate("ACGT", limited));
  limited.max_hits = 0;
  const auto omitted_no_hit = fm.Locate("GTTT", limited);
  CHECK(omitted_no_hit.total_hits == 0);
  CHECK(omitted_no_hit.hits.empty());
  CHECK(!omitted_no_hit.truncated);

  sufkit::LocateOptions both;
  both.strands = sufkit::StrandMode::kBoth;
  const auto aaa = fm.Locate("AAA", both);
  CHECK(aaa.total_hits == 3);
  CHECK(aaa.hits[0].sequence_id == 1 && aaa.hits[0].position == 0 &&
        aaa.hits[0].strand == sufkit::Strand::kReverseComplement);
  CHECK(aaa.hits[1].sequence_id == 1 && aaa.hits[1].position == 7 &&
        aaa.hits[1].strand == sufkit::Strand::kForward);
  CHECK(aaa.hits[2].sequence_id == 2 && aaa.hits[2].position == 0 &&
        aaa.hits[2].strand == sufkit::Strand::kForward);
  both.max_hits = 1;
  const auto aaa_first = fm.Locate("AAA", both);
  CHECK(aaa_first.total_hits == 3 && aaa_first.hits.size() == 1);
  CHECK(aaa_first.hits[0].sequence_id == 1 &&
        aaa_first.hits[0].position == 0 &&
        aaa_first.hits[0].strand == sufkit::Strand::kReverseComplement);
  const auto palindrome_first = fm.Locate("ACGT", both);
  CHECK(palindrome_first.total_hits == 3 &&
        palindrome_first.hits.size() == 1);
  CHECK(palindrome_first.hits[0].sequence_id == 0 &&
        palindrome_first.hits[0].position == 0 &&
        palindrome_first.hits[0].strand == sufkit::Strand::kBoth);
  both.max_hits = 0;
  const auto both_omitted = fm.Locate("AAA", both);
  CHECK(both_omitted.total_hits == 3);
  CHECK(both_omitted.hits.empty());
  CHECK(both_omitted.truncated);
  both.max_hits.reset();

  // Mixed pattern lengths exercise active-lane compaction, while more patterns
  // than the smaller widths cover workspace reuse across multiple chunks.
  const std::vector<std::string_view> batch_patterns{
      "ACGT", "A",   "AC", "ACG", "AAA",   "GTTT",
      "TTT",  "CGT", "C",  "TACGT", "ACGTACGTACGT"};
  const std::array<std::uint32_t, 6> batch_widths{{1, 2, 3, 4, 16, 256}};
  struct FmCase {
    sufkit::FmBackend backend;
    const char* name;
    const char* signature;
  };
  const std::array<FmCase, 3> fm_cases{
      {{sufkit::FmBackend::kSdslCsaWtHuff, "sdsl-csa-wt-huff",
        "sdsl::csa_wt<sdsl::wt_huff<>,32,64>"},
       {sufkit::FmBackend::kSdslCsaWtBalanced, "sdsl-csa-wt-balanced",
        "sdsl::csa_wt<sdsl::wt_blcd<>,32,64>"},
       {sufkit::FmBackend::kSdslCsaWtEpr, "sdsl-csa-wt-epr",
        "sdsl::csa_wt<sdsl::wt_epr<8>,32,64>"}}};
  for (const auto& fm_case : fm_cases) {
    auto candidate = sufkit::FmIndex::Build(reference, {fm_case.backend});
    CHECK(candidate.GetInfo().backend == fm_case.name);
    CHECK(candidate.GetInfo().backend_signature == fm_case.signature);
    CHECK(candidate.GetInfo().format_version == "1.0");
    CheckMatchResults(candidate.Locate("ACGT"));
    for (const auto width : batch_widths) {
      const auto ranges = candidate.EqualRangeBatch(batch_patterns, width);
      CHECK(ranges.size() == batch_patterns.size());
      for (std::size_t index = 0; index < batch_patterns.size(); ++index) {
        const auto scalar = candidate.EqualRange(batch_patterns[index]);
        CHECK(ranges[index].begin == scalar.begin);
        CHECK(ranges[index].end == scalar.end);
      }
      for (const auto strands : {sufkit::StrandMode::kForward,
                                 sufkit::StrandMode::kReverseComplement,
                                 sufkit::StrandMode::kBoth}) {
        sufkit::FmBatchOptions batch_options;
        batch_options.strands = strands;
        batch_options.batch_width = width;
        const auto counts = candidate.CountBatch(batch_patterns, batch_options);
        CHECK(counts.size() == batch_patterns.size());
        for (std::size_t index = 0; index < batch_patterns.size(); ++index) {
          CHECK(counts[index] ==
                candidate.Count(batch_patterns[index], strands));
        }
      }
    }
    CHECK(candidate.CountBatch({}, {}).empty());
    CheckError(sufkit::ErrorCode::kInvalidInput,
               [&] { (void)candidate.EqualRangeBatch(batch_patterns, 257); });
    CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
      sufkit::FmBatchOptions invalid;
      invalid.batch_width = 257;
      (void)candidate.CountBatch(batch_patterns, invalid);
    });
    CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
      const std::vector<std::string_view> invalid{"ACGT", "ACNT", "TTT"};
      (void)candidate.CountBatch(invalid);
    });
    const auto invalid_strand = static_cast<sufkit::StrandMode>(255);
    CheckError(sufkit::ErrorCode::kInvalidInput,
               [&] { (void)candidate.Count("ACGT", invalid_strand); });
    CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
      sufkit::LocateOptions invalid;
      invalid.strands = invalid_strand;
      (void)candidate.Locate("ACGT", invalid);
    });
    CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
      sufkit::FmBatchOptions invalid;
      invalid.strands = invalid_strand;
      (void)candidate.CountBatch({}, invalid);
    });

    const auto backend_path =
        directory / (std::string(fm_case.name) + ".sufidx");
    candidate.Save(backend_path);
    const auto backend_info = sufkit::InspectIndex(backend_path);
    CHECK(backend_info.format_version == "1.0");
    CHECK(backend_info.backend == fm_case.name);
    CHECK(backend_info.backend_signature == fm_case.signature);
    auto backend_loaded = sufkit::FmIndex::Load(backend_path);
    CHECK(backend_loaded.CountBatch(batch_patterns).size() ==
          batch_patterns.size());
    CheckMatchResults(backend_loaded.Locate("ACGT"));
  }

  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)sufkit::FmIndex::Build(reference, {sufkit::FmBackend::kSdslCsaSada});
  });

  const auto path = directory / "reference.fm.sufidx";
  fm.Save(path);
  const auto deterministic_path = directory / "reference.fm.copy.sufidx";
  fm.Save(deterministic_path);
  {
    std::ifstream left(path, std::ios::binary);
    std::ifstream right(deterministic_path, std::ios::binary);
    const std::vector<char> left_bytes{std::istreambuf_iterator<char>(left),
                                       std::istreambuf_iterator<char>()};
    const std::vector<char> right_bytes{std::istreambuf_iterator<char>(right),
                                        std::istreambuf_iterator<char>()};
    CHECK(left_bytes == right_bytes);
  }
  fm.Save(path, {true});
  auto loaded = sufkit::FmIndex::Load(path);
  CheckMatchResults(loaded.Locate("ACGT"));
  const auto inspected = sufkit::InspectIndex(path);
  CHECK(inspected.kind == sufkit::IndexKind::kFmIndex);
  CHECK(inspected.sdsl_version == "3.0.3");
  CheckError(sufkit::ErrorCode::kCorruptIndex,
             [&] { (void)sufkit::SuffixArray::Load(path); });

  std::atomic<bool> concurrent_ok{true};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        const auto batch = loaded.CountBatch(batch_patterns);
        if (loaded.Count("ACGT") != 3 ||
            loaded.Locate("AAA", both).total_hits != 3 ||
            batch.size() != batch_patterns.size() || batch.front() != 3) {
          concurrent_ok.store(false);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  CHECK(concurrent_ok.load());

  const auto damaged = directory / "damaged.sufidx";
  std::filesystem::copy_file(path, damaged);
  {
    std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    file.clear();
    file.seekp(-1, std::ios::end);
    value ^= 0x55;
    file.write(&value, 1);
  }
  CheckError(sufkit::ErrorCode::kCorruptIndex,
             [&] { (void)sufkit::FmIndex::Load(damaged); });

  const auto truncated = directory / "truncated.sufidx";
  std::filesystem::copy_file(path, truncated);
  std::filesystem::resize_file(truncated,
                               std::filesystem::file_size(truncated) - 1);
  CheckError(sufkit::ErrorCode::kCorruptIndex,
             [&] { (void)sufkit::FmIndex::Load(truncated); });

  const auto bad_magic = directory / "bad-magic.sufidx";
  std::filesystem::copy_file(path, bad_magic);
  {
    std::fstream file(bad_magic,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.put('X');
  }
  CheckError(sufkit::ErrorCode::kCorruptIndex,
             [&] { (void)sufkit::InspectIndex(bad_magic); });

  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    CHECK(entry.path().filename().string().find(".partial.") ==
          std::string::npos);
  }
}

void TestSaStorageProfiles(const std::filesystem::path& directory) {
  const auto reference = MakeReference();
  const std::array<std::string, 6> patterns{
      {"A", "ACGT", "AAA", "TTT", "GTTT", "CCCC"}};

  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    auto invalid = sufkit::FastSuffixArrayBuildOptions();
    invalid.resource_profile = static_cast<sufkit::SaResourceProfile>(255);
    (void)sufkit::SuffixArray::Build(reference, invalid);
  });
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    auto invalid = sufkit::FastSuffixArrayBuildOptions();
    invalid.acceleration = static_cast<sufkit::SaAcceleration>(255);
    (void)sufkit::SuffixArray::Build(reference, invalid);
  });

  auto fast_options = sufkit::FastSuffixArrayBuildOptions();
  fast_options.backend = sufkit::SaBackend::kDivsufsort;
  fast_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  auto fast = sufkit::SuffixArray::Build(reference, fast_options);
  CHECK(fast.GetInfo().coordinate_width == 64);
  CHECK(fast.GetInfo().stored_coordinate_width == 32);
  CHECK(fast.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kFast);
  CHECK(fast.GetInfo().format_version == "1.4");

  auto low_options = sufkit::LowMemorySuffixArrayBuildOptions();
  low_options.backend = sufkit::SaBackend::kDivsufsort;
  low_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  auto low = sufkit::SuffixArray::Build(reference, low_options);
  CHECK(low.GetInfo().coordinate_width == 64);
  CHECK(low.GetInfo().stored_coordinate_width == 32);
  CHECK(low.GetInfo().sa_resource_profile ==
        sufkit::SaResourceProfile::kLowMemory);
  CHECK(low.GetInfo().sa_acceleration == sufkit::SaAcceleration::kLcp);
  CHECK(low.GetInfo().isa_bytes == 0);
  CHECK(low.GetInfo().child_bytes == 0);
  CHECK(low.GetInfo().learned_index_bytes == 0);
  CHECK(low.GetInfo().resident_core_bytes <
        fast.GetInfo().resident_core_bytes);

  for (const auto& pattern : patterns) {
    CHECK(fast.Count(pattern) == low.Count(pattern));
    CHECK(SameQueryResult(fast.Locate(pattern), low.Locate(pattern)));
  }

  const std::array<sufkit::CoordinateStorageWidth, 4> storage_widths{
      {sufkit::CoordinateStorageWidth::kBits32,
       sufkit::CoordinateStorageWidth::kBits40,
       sufkit::CoordinateStorageWidth::kBits48,
       sufkit::CoordinateStorageWidth::kBits64}};
  const std::array<std::uint8_t, 4> expected_widths{{32, 40, 48, 64}};
  for (std::size_t index = 0; index < storage_widths.size(); ++index) {
    auto options = sufkit::FastSuffixArrayBuildOptions();
    options.backend = sufkit::SaBackend::kDivsufsort;
    options.coordinate_width = sufkit::CoordinateWidth::kBits64;
    options.storage_width = storage_widths[index];
    auto candidate = sufkit::SuffixArray::Build(reference, options);
    const auto info = candidate.GetInfo();
    CHECK(info.coordinate_width == 64);
    CHECK(info.stored_coordinate_width == expected_widths[index]);
    CHECK(info.sa_bytes ==
          info.suffix_count * (expected_widths[index] / 8ULL));
    CHECK(info.format_version == "1.4");
    for (const auto& pattern : patterns) {
      CHECK(SameQueryResult(candidate.Locate(pattern), fast.Locate(pattern)));
    }

    const auto path =
        directory /
        ("storage-" + std::to_string(expected_widths[index]) + ".sufidx");
    candidate.Save(path);
    const auto inspected = sufkit::InspectIndex(path);
    CHECK(inspected.format_version == "1.4");
    CHECK(inspected.coordinate_width == 64);
    CHECK(inspected.stored_coordinate_width == expected_widths[index]);
    auto loaded = sufkit::SuffixArray::Load(path);
    CHECK(loaded.GetInfo().stored_coordinate_width == expected_widths[index]);
    for (const auto& pattern : patterns) {
      CHECK(SameQueryResult(loaded.Locate(pattern),
                            candidate.Locate(pattern)));
    }
  }
}

void TestFastPrefixDirectoryIntegration(
    const std::filesystem::path& directory) {
  std::string sequence(262144, 'A');
  std::uint64_t state = 20260830;
  constexpr std::array<char, 4> kBases{'A', 'C', 'G', 'T'};
  for (auto& base : sequence) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    base = kBases[static_cast<std::size_t>(state >> 62U)];
  }
  const auto reference = sufkit::GenomeReference::FromRecords(
      {{"fast-prefix", "", sequence}});
  auto options = sufkit::FastSuffixArrayBuildOptions();
  options.backend = sufkit::SaBackend::kDivsufsort;
  auto index = sufkit::SuffixArray::Build(reference, options);

  const auto info = index.GetInfo();
  const auto persistent_core = info.text_bytes + info.sa_bytes +
                               info.isa_bytes + info.lcp_bytes +
                               info.child_bytes;
  // This input selects the private k=8 table: 4^8 packed 32-bit intervals.
  CHECK(info.resident_core_bytes - persistent_core == (512U << 10U));
  CHECK(info.auxiliary_bytes >= info.isa_bytes + info.lcp_bytes +
                                    (512U << 10U));

  const auto pattern = sequence.substr(12345, 50);
  sufkit::SaSearchStatistics binary_stats;
  const auto binary = index.EqualRange(
      pattern, sufkit::SaSearchAlgorithm::kBinary, &binary_stats);
  sufkit::SaSearchStatistics auto_stats;
  const auto automatic = index.EqualRange(
      pattern, sufkit::SaSearchAlgorithm::kAutoSelect, &auto_stats);
  CHECK(binary.begin == automatic.begin && binary.end == automatic.end);
  CHECK(auto_stats.suffix_comparisons < binary_stats.suffix_comparisons);

  const auto query = sequence.substr(12345, 200);
  const auto same_matches = [](const auto& left, const auto& right) {
    if (left.total_matches != right.total_matches ||
        left.truncated != right.truncated ||
        left.matches.size() != right.matches.size()) {
      return false;
    }
    for (std::size_t i = 0; i < left.matches.size(); ++i) {
      const auto& a = left.matches[i];
      const auto& b = right.matches[i];
      if (a.sequence_id != b.sequence_id ||
          a.reference_position != b.reference_position ||
          a.query_position != b.query_position || a.length != b.length ||
          a.strand != b.strand) {
        return false;
      }
    }
    return true;
  };
  sufkit::MemOptions auto_mem;
  auto_mem.min_length = 20;
  auto binary_mem = auto_mem;
  binary_mem.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  CHECK(same_matches(index.FindMems(query, auto_mem),
                     index.FindMems(query, binary_mem)));
  sufkit::MamOptions auto_mam;
  auto_mam.min_length = 20;
  auto binary_mam = auto_mam;
  binary_mam.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  CHECK(same_matches(index.FindMams(query, auto_mam),
                     index.FindMams(query, binary_mam)));

  const auto path = directory / "fast-prefix.sa.sufidx";
  index.Save(path);
  const auto inspected = sufkit::InspectIndex(path);
  auto loaded = sufkit::SuffixArray::Load(path);
  CHECK(inspected.resident_core_bytes == info.resident_core_bytes);
  CHECK(loaded.GetInfo().resident_core_bytes == info.resident_core_bytes);
  const auto loaded_range = loaded.EqualRange(pattern);
  CHECK(loaded_range.begin == binary.begin && loaded_range.end == binary.end);
}

void TestPackedSpanLocateEnumeration() {
  const auto reference = sufkit::GenomeReference::FromRecords(
      {{"repeat", "", std::string(700, 'A')}});
  const std::array<sufkit::CoordinateStorageWidth, 4> storage_widths{
      {sufkit::CoordinateStorageWidth::kBits32,
       sufkit::CoordinateStorageWidth::kBits40,
       sufkit::CoordinateStorageWidth::kBits48,
       sufkit::CoordinateStorageWidth::kBits64}};

  auto native_options = sufkit::FastSuffixArrayBuildOptions();
  native_options.backend = sufkit::SaBackend::kDivsufsort;
  native_options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  native_options.storage_width =
      sufkit::CoordinateStorageWidth::kBits32;
  const auto native = sufkit::SuffixArray::Build(reference, native_options);
  for (const std::size_t length : {511U, 512U, 513U, 600U}) {
    const std::string long_pattern(length, 'A');
    const auto expected_hits = 701U - length;
    CHECK(native.Count(long_pattern) == expected_hits);
    CHECK(native.Count(long_pattern, sufkit::StrandMode::kBoth) ==
          expected_hits);
    CHECK(native.EqualRange(long_pattern).Size() == expected_hits);
  }
  const std::array<std::optional<std::uint64_t>, 5> limits{
      {std::nullopt, 0, 1, 10, 600}};
  std::vector<sufkit::QueryResult> expected;
  for (const auto limit : limits) {
    sufkit::LocateOptions locate;
    locate.max_hits = limit;
    expected.push_back(native.Locate("AAA", locate));
  }
  CHECK(expected.front().total_hits > 512);

  for (const auto storage_width : storage_widths) {
    auto options = sufkit::FastSuffixArrayBuildOptions();
    options.backend = sufkit::SaBackend::kDivsufsort;
    options.coordinate_width = sufkit::CoordinateWidth::kBits64;
    options.storage_width = storage_width;
    const auto candidate = sufkit::SuffixArray::Build(reference, options);
    CHECK(candidate.Count("AAA") == expected.front().total_hits);
    std::size_t result_index = 0;
    for (const auto limit : limits) {
      sufkit::LocateOptions locate;
      locate.max_hits = limit;
      CHECK(SameQueryResult(candidate.Locate("AAA", locate),
                            expected[result_index]));
      ++result_index;
    }
  }
}

void TestLegacySaFixtures() {
  const auto fixture_directory =
      std::filesystem::path(__FILE__).parent_path() / "data";
  struct LegacyFixture {
    const char* filename;
    const char* format;
    std::uint32_t sampling_rate;
    sufkit::SaAcceleration acceleration;
  };
  const std::array<LegacyFixture, 4> fixtures{
      {{"legacy-sa-v1.0.fixture", "1.0", 1,
        sufkit::SaAcceleration::kNone},
       {"legacy-sa-v1.1.fixture", "1.1", 1,
        sufkit::SaAcceleration::kFull},
       {"legacy-sa-v1.2.fixture", "1.2", 1,
        sufkit::SaAcceleration::kLcpSuffixLink},
       {"legacy-sa-v1.3.fixture", "1.3", 2,
        sufkit::SaAcceleration::kFull}}};

  for (const auto& fixture : fixtures) {
    const auto path = fixture_directory / fixture.filename;
    CHECK(std::filesystem::is_regular_file(path));
    const auto inspected = sufkit::InspectIndex(path);
    CHECK(inspected.format_version == fixture.format);
    CHECK(inspected.sa_sampling_rate == fixture.sampling_rate);
    auto loaded = sufkit::SuffixArray::Load(path);
    CHECK(loaded.GetInfo().format_version == fixture.format);
    CHECK(loaded.GetInfo().sa_acceleration == fixture.acceleration);
    CHECK(loaded.SamplingRate() == fixture.sampling_rate);
    CHECK(loaded.Count("ACGT") == 3);
    CheckMatchResults(loaded.Locate("ACGT"));
  }
}

using RightMaximalTuple =
    std::tuple<std::uint64_t, sufkit::SequenceId, sufkit::Position,
               std::uint64_t, sufkit::Strand>;

std::vector<RightMaximalTuple> RightMaximalTuples(
    const sufkit::RightMaximalResult& result);
std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> NaivePositions(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& pattern);

void TestLearnedSa(const std::filesystem::path& directory) {
  const auto reference = sufkit::GenomeReference::FromRecords(
      {{"learned-1", "", "ACGTACGTACGTAAAACCCCGGGGTTTTACGTACGT"},
       {"learned-2", "", "TTTTGGGGCCCCAAAANACGTTGCATGCAACGT"}});
  sufkit::SuffixArrayBuildOptions options;
  CHECK(options.acceleration == sufkit::SaAcceleration::kLcpSuffixLink);
  options.learned_index.enabled = true;
  options.learned_index.k = 4;
  options.learned_index.bucket_bits = 3;
  auto index = sufkit::SuffixArray::Build(reference, options);
  CHECK(index.Acceleration() == sufkit::SaAcceleration::kLcpSuffixLink);
  CHECK(index.LookupAcceleration() ==
        sufkit::SaLookupAcceleration::kSaplingPwl);
  CHECK(index.GetInfo().format_version == "1.4");
  CHECK(index.GetInfo().learned_k == 4);
  CHECK(index.GetInfo().learned_bucket_bits == 3);
  CHECK(index.GetInfo().learned_index_bytes != 0);

  const std::array<std::string, 10> patterns{{"A", "ACG", "ACGT", "ACGTA",
                                              "TTTT", "CCCCAAAA", "TGCA",
                                              "GGGGG", "AAAA", "CGTT"}};
  for (const auto& pattern : patterns) {
    const auto binary =
        index.EqualRange(pattern, sufkit::SaSearchAlgorithm::kBinary);
    const auto lcp =
        index.EqualRange(pattern, sufkit::SaSearchAlgorithm::kLcpBinary);
    sufkit::SaSearchStatistics learned_stats;
    const auto learned = index.EqualRange(
        pattern, sufkit::SaSearchAlgorithm::kSaplingPwl, &learned_stats);
    const auto automatic = index.EqualRange(pattern);
    CHECK(binary.begin == lcp.begin && binary.end == lcp.end);
    CHECK(binary.begin == learned.begin && binary.end == learned.end);
    CHECK(binary.begin == automatic.begin && binary.end == automatic.end);
    CHECK(index.Count(pattern, sufkit::StrandMode::kForward,
                      sufkit::SaSearchAlgorithm::kSaplingPwl) == binary.Size());
    sufkit::LocateOptions locate_options;
    const auto binary_hits = index.Locate(pattern, locate_options,
                                          sufkit::SaSearchAlgorithm::kBinary);
    const auto learned_hits = index.Locate(
        pattern, locate_options, sufkit::SaSearchAlgorithm::kSaplingPwl);
    CHECK(binary_hits.total_hits == learned_hits.total_hits);
    std::vector<std::pair<sufkit::SequenceId, sufkit::Position>>
        learned_positions;
    for (const auto& match : learned_hits.hits) {
      learned_positions.emplace_back(match.sequence_id, match.position);
    }
    CHECK(learned_positions ==
          NaivePositions(
              {{"learned-1", "", "ACGTACGTACGTAAAACCCCGGGGTTTTACGTACGT"},
               {"learned-2", "", "TTTTGGGGCCCCAAAANACGTTGCATGCAACGT"}},
              pattern));
    if (pattern.size() < 4) {
      CHECK(learned_stats.full_binary_fallbacks != 0);
    }
  }

  sufkit::RightMaximalOptions right_maximal_options;
  right_maximal_options.min_length = 4;
  right_maximal_options.algorithm =
      sufkit::RightMaximalSearchAlgorithm::kSuffixLink;
  right_maximal_options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  const auto binary_right_maximal = index.FindRightMaximalMatches(
      "GGACGTACGTNNNTGCATG", right_maximal_options);
  sufkit::RightMaximalSearchStatistics right_maximal_stats;
  right_maximal_options.lookup_algorithm =
      sufkit::SaSearchAlgorithm::kSaplingPwl;
  right_maximal_options.statistics = &right_maximal_stats;
  const auto learned_right_maximal = index.FindRightMaximalMatches(
      "GGACGTACGTNNNTGCATG", right_maximal_options);
  CHECK(RightMaximalTuples(binary_right_maximal) ==
        RightMaximalTuples(learned_right_maximal));
  CHECK(right_maximal_stats.learned_lookup_calls != 0);

  const auto path = directory / "learned.sa.sufidx";
  index.Save(path);
  const auto deterministic_path = directory / "learned.sa.copy.sufidx";
  index.Save(deterministic_path);
  {
    std::ifstream left(path, std::ios::binary);
    std::ifstream right(deterministic_path, std::ios::binary);
    const std::vector<char> left_bytes{std::istreambuf_iterator<char>(left),
                                       std::istreambuf_iterator<char>()};
    const std::vector<char> right_bytes{std::istreambuf_iterator<char>(right),
                                        std::istreambuf_iterator<char>()};
    CHECK(left_bytes == right_bytes);
  }
  const auto inspected = sufkit::InspectIndex(path);
  CHECK(inspected.format_version == "1.4");
  CHECK(inspected.sa_lookup_acceleration ==
        sufkit::SaLookupAcceleration::kSaplingPwl);
  CHECK(inspected.learned_k == 4);
  CHECK(inspected.learned_bucket_bits == 3);
  auto loaded = sufkit::SuffixArray::Load(path);
  CHECK(index.GetInfo().resident_core_bytes == inspected.resident_core_bytes);
  CHECK(index.GetInfo().resident_core_bytes ==
        loaded.GetInfo().resident_core_bytes);
  for (const auto& pattern : patterns) {
    const auto expected =
        index.EqualRange(pattern, sufkit::SaSearchAlgorithm::kBinary);
    const auto observed =
        loaded.EqualRange(pattern, sufkit::SaSearchAlgorithm::kSaplingPwl);
    CHECK(expected.begin == observed.begin && expected.end == observed.end);
  }
  std::atomic<bool> learned_concurrent_ok{true};
  std::vector<std::thread> learned_workers;
  for (int worker = 0; worker < 4; ++worker) {
    learned_workers.emplace_back([&] {
      for (int repetition = 0; repetition < 100; ++repetition) {
        const auto binary =
            loaded.EqualRange("ACGTAC", sufkit::SaSearchAlgorithm::kBinary);
        const auto learned_range =
            loaded.EqualRange("ACGTAC", sufkit::SaSearchAlgorithm::kSaplingPwl);
        if (binary.begin != learned_range.begin ||
            binary.end != learned_range.end) {
          learned_concurrent_ok.store(false);
        }
      }
    });
  }
  for (auto& worker : learned_workers) {
    worker.join();
  }
  CHECK(learned_concurrent_ok.load());

  const auto damaged = directory / "learned-damaged.sufidx";
  std::filesystem::copy_file(path, damaged);
  {
    std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char value = 0;
    file.read(&value, 1);
    file.clear();
    file.seekp(-1, std::ios::end);
    value ^= 0x31;
    file.write(&value, 1);
  }
  CheckError(sufkit::ErrorCode::kCorruptIndex,
             [&] { (void)sufkit::SuffixArray::Load(damaged); });

  std::string budget_sequence(10000, 'A');
  for (std::size_t position = 0; position < budget_sequence.size();
       ++position) {
    static constexpr std::array<char, 4> kBases{{'A', 'C', 'G', 'T'}};
    budget_sequence[position] =
        kBases[(position * 17 + position / 7) % kBases.size()];
  }
  const auto budget_reference = sufkit::GenomeReference::FromRecords(
      {{"budget", "", std::move(budget_sequence)}});
  sufkit::SuffixArrayBuildOptions budget_options;
  budget_options.acceleration = sufkit::SaAcceleration::kNone;
  budget_options.learned_index.enabled = true;
  budget_options.learned_index.k = 4;
  auto budget_index =
      sufkit::SuffixArray::Build(budget_reference, budget_options);
  const auto budget_info = budget_index.GetInfo();
  const auto sa_payload_bytes = budget_info.text_symbols * 4ULL;
  CHECK(budget_info.learned_index_bytes * 10000ULL <=
        sa_payload_bytes * 100ULL);

  const auto short_reference = sufkit::GenomeReference::FromRecords(
      {{"short-1", "", "ACGTACGTACGTACGT"},
       {"short-2", "", "NNNNNNNNNNNNNNNN"}});
  sufkit::SuffixArrayBuildOptions short_options;
  short_options.acceleration = sufkit::SaAcceleration::kNone;
  short_options.learned_index.enabled = true;
  short_options.learned_index.k = 20;
  short_options.learned_index.bucket_bits = 2;
  auto short_index = sufkit::SuffixArray::Build(short_reference, short_options);
  CHECK(short_index.LookupAcceleration() ==
        sufkit::SaLookupAcceleration::kSaplingPwl);
  CHECK(short_index.Count(std::string(20, 'A'), sufkit::StrandMode::kForward,
                          sufkit::SaSearchAlgorithm::kSaplingPwl) == 0);

  sufkit::SuffixArrayBuildOptions no_learned;
  no_learned.acceleration = sufkit::SaAcceleration::kFull;
  auto full = sufkit::SuffixArray::Build(reference, no_learned);
  sufkit::SaSearchStatistics auto_stats;
  const auto automatic = full.EqualRange(
      "ACGT", sufkit::SaSearchAlgorithm::kAutoSelect, &auto_stats);
  sufkit::SaSearchStatistics lcp_stats;
  const auto lcp = full.EqualRange(
      "ACGT", sufkit::SaSearchAlgorithm::kLcpBinary, &lcp_stats);
  CHECK(automatic.begin == lcp.begin && automatic.end == lcp.end);
  CHECK(auto_stats.suffix_comparisons == lcp_stats.suffix_comparisons);
  CHECK(auto_stats.character_comparisons == lcp_stats.character_comparisons);
  CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
    (void)full.EqualRange("ACGT", sufkit::SaSearchAlgorithm::kSaplingPwl);
  });
}

std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> NaivePositions(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& pattern) {
  std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> positions;
  for (std::size_t sequence_id = 0; sequence_id < records.size();
       ++sequence_id) {
    std::string sequence = records[sequence_id].sequence;
    for (auto& base : sequence) {
      base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
      if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
        base = 'N';
      }
    }
    auto position = sequence.find(pattern);
    while (position != std::string::npos) {
      positions.emplace_back(static_cast<sufkit::SequenceId>(sequence_id),
                             position);
      position = sequence.find(pattern, position + 1);
    }
  }
  return positions;
}

template <class Index>
std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> IndexPositions(
    const Index& index, const std::string& pattern) {
  std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> positions;
  for (const auto& match : index.Locate(pattern).hits) {
    positions.emplace_back(match.sequence_id, match.position);
  }
  return positions;
}

void TestRandomizedDifferential() {
  std::uint64_t state = 0x20260822ULL;
  const auto next = [&] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
  };
  const std::array<char, 5> alphabet{{'A', 'C', 'G', 'T', 'N'}};
  const std::array<char, 4> query_alphabet{{'A', 'C', 'G', 'T'}};
  for (int trial = 0; trial < 25; ++trial) {
    std::vector<sufkit::SequenceRecord> records;
    for (int sequence_id = 0; sequence_id < 3; ++sequence_id) {
      std::string sequence(40, 'A');
      for (auto& base : sequence) {
        base = alphabet[next() % alphabet.size()];
      }
      records.push_back(
          {"r" + std::to_string(sequence_id), "", std::move(sequence)});
    }
    auto reference = sufkit::GenomeReference::FromRecords(records);
    auto sa32 = sufkit::SuffixArray::Build(
        reference,
        {sufkit::SaBackend::kDivsufsort, sufkit::CoordinateWidth::kBits32, 1});
    auto sa64 = sufkit::SuffixArray::Build(
        reference,
        {sufkit::SaBackend::kDivsufsort, sufkit::CoordinateWidth::kBits64, 1});
    sufkit::SuffixArrayBuildOptions learned_options;
    learned_options.coordinate_width = sufkit::CoordinateWidth::kBits32;
    learned_options.acceleration = sufkit::SaAcceleration::kFull;
    learned_options.learned_index.enabled = true;
    learned_options.learned_index.k = 4;
    learned_options.learned_index.bucket_bits = 3;
    auto learned = sufkit::SuffixArray::Build(reference, learned_options);
    auto fm = sufkit::FmIndex::Build(reference);
    for (int query_index = 0; query_index < 12; ++query_index) {
      std::string pattern(static_cast<std::size_t>(1 + next() % 8), 'A');
      for (auto& base : pattern) {
        base = query_alphabet[next() % query_alphabet.size()];
      }
      const auto expected = NaivePositions(records, pattern);
      CHECK(sa32.Count(pattern) == expected.size());
      CHECK(sa64.Count(pattern) == expected.size());
      CHECK(fm.Count(pattern) == expected.size());
      const auto binary_range =
          learned.EqualRange(pattern, sufkit::SaSearchAlgorithm::kBinary);
      for (const auto algorithm : {sufkit::SaSearchAlgorithm::kLcpBinary,
                                   sufkit::SaSearchAlgorithm::kSaplingPwl,
                                   sufkit::SaSearchAlgorithm::kChild,
                                   sufkit::SaSearchAlgorithm::kAutoSelect}) {
        const auto observed = learned.EqualRange(pattern, algorithm);
        CHECK(observed.begin == binary_range.begin &&
              observed.end == binary_range.end);
      }
      CHECK(IndexPositions(sa32, pattern) == expected);
      CHECK(IndexPositions(sa64, pattern) == expected);
      CHECK(IndexPositions(fm, pattern) == expected);
    }
  }
}

std::vector<RightMaximalTuple> RightMaximalTuples(
    const sufkit::RightMaximalResult& result) {
  std::vector<RightMaximalTuple> values;
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length, match.strand);
  }
  return values;
}

std::vector<RightMaximalTuple> NaiveRightMaximalMatches(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& raw_query, std::uint64_t min_length) {
  std::string query = raw_query;
  for (auto& base : query) {
    base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
    if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
      base = 'N';
    }
  }
  std::vector<RightMaximalTuple> result;
  for (std::size_t sequence_id = 0; sequence_id < records.size();
       ++sequence_id) {
    std::string reference = records[sequence_id].sequence;
    for (auto& base : reference) {
      base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
      if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
        base = 'N';
      }
    }
    for (std::size_t query_position = 0; query_position < query.size();
         ++query_position) {
      if (query[query_position] == 'N') {
        continue;
      }
      for (std::size_t reference_position = 0;
           reference_position < reference.size(); ++reference_position) {
        if (reference[reference_position] == 'N') {
          continue;
        }
        std::size_t length = 0;
        while (query_position + length < query.size() &&
               reference_position + length < reference.size() &&
               query[query_position + length] != 'N' &&
               reference[reference_position + length] != 'N' &&
               query[query_position + length] ==
                   reference[reference_position + length]) {
          ++length;
        }
        if (length < min_length) {
          continue;
        }
        const bool left_extendable =
            query_position != 0 && reference_position != 0 &&
            query[query_position - 1] != 'N' &&
            reference[reference_position - 1] != 'N' &&
            query[query_position - 1] == reference[reference_position - 1];
        if (!left_extendable) {
          result.emplace_back(
              query_position, static_cast<sufkit::SequenceId>(sequence_id),
              reference_position, length, sufkit::Strand::kForward);
        }
      }
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

void TestRightMaximalSearch(const std::filesystem::path& directory) {
  const auto reference =
      sufkit::GenomeReference::FromRecords({{"r1", "", "GATTACAGTC"},
                                            {"r2", "", "TTACAGGG"},
                                            {"r3", "", "AAAANCCCC"}});
  sufkit::SuffixArrayBuildOptions build_options;
  build_options.acceleration = sufkit::SaAcceleration::kFull;
  auto index = sufkit::SuffixArray::Build(reference, build_options);
  CHECK(index.Acceleration() == sufkit::SaAcceleration::kFull);
  CHECK(index.GetInfo().format_version == "1.4");
  CHECK(index.GetInfo().auxiliary_bytes != 0);

  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    sufkit::RightMaximalOptions invalid;
    invalid.strands = static_cast<sufkit::StrandMode>(255);
    (void)index.FindRightMaximalMatches("GATTACA", invalid);
  });

  sufkit::RightMaximalOptions options;
  options.min_length = 4;
  options.strands = sufkit::StrandMode::kForward;
  options.algorithm = sufkit::RightMaximalSearchAlgorithm::kBaseline;
  const auto baseline = index.FindRightMaximalMatches("CCGATTACAT", options);
  CHECK(baseline.total_matches == 2);
  CHECK(baseline.matches.size() == 2);
  CHECK(baseline.matches[0].query_position == 2);
  CHECK(baseline.matches[0].sequence_id == 0);
  CHECK(baseline.matches[0].reference_position == 0);
  CHECK(baseline.matches[0].length == 7);

  for (const auto algorithm :
       {sufkit::RightMaximalSearchAlgorithm::kLcp,
        sufkit::RightMaximalSearchAlgorithm::kChild,
        sufkit::RightMaximalSearchAlgorithm::kSuffixLink,
        sufkit::RightMaximalSearchAlgorithm::kFull,
        sufkit::RightMaximalSearchAlgorithm::kAutoSelect}) {
    options.algorithm = algorithm;
    CHECK(RightMaximalTuples(index.FindRightMaximalMatches(
              "CCGATTACAT", options)) == RightMaximalTuples(baseline));
  }
  sufkit::RightMaximalSearchStatistics automatic_statistics;
  options.algorithm = sufkit::RightMaximalSearchAlgorithm::kAutoSelect;
  options.statistics = &automatic_statistics;
  (void)index.FindRightMaximalMatches("CCGATTACAT", options);
  CHECK(automatic_statistics.suffix_link_attempts !=
        0);  // full index still auto-selects suffix-link
  options.statistics = nullptr;

  options.algorithm = sufkit::RightMaximalSearchAlgorithm::kFull;
  options.min_length = 3;
  const auto hard_break = index.FindRightMaximalMatches("AAAANCCCC", options);
  CHECK(std::any_of(hard_break.matches.begin(), hard_break.matches.end(),
                    [](const auto& match) {
                      return match.sequence_id == 2 &&
                             match.reference_position == 0 &&
                             match.query_position == 0 && match.length == 4;
                    }));
  CHECK(std::any_of(hard_break.matches.begin(), hard_break.matches.end(),
                    [](const auto& match) {
                      return match.sequence_id == 2 &&
                             match.reference_position == 5 &&
                             match.query_position == 5 && match.length == 4;
                    }));
  CHECK(std::none_of(hard_break.matches.begin(), hard_break.matches.end(),
                     [](const auto& match) {
                       return match.query_position < 4 &&
                              match.query_position + match.length > 4;
                     }));

  options.min_length = 4;
  options.strands = sufkit::StrandMode::kBoth;
  const auto both = index.FindRightMaximalMatches("TGTAATC", options);
  CHECK(std::any_of(both.matches.begin(), both.matches.end(),
                    [](const auto& match) {
                      return match.strand == sufkit::Strand::kReverseComplement;
                    }));

  const auto limited = index.FindRightMaximalMatches("CCGATTACAT", options, 1);
  CHECK(limited.total_matches >= limited.matches.size());
  CHECK(limited.matches.size() == 1);
  CHECK(limited.truncated);
  const auto unlimited =
      index.FindRightMaximalMatches("CCGATTACAT", options);
  const auto exact_limit = index.FindRightMaximalMatches(
      "CCGATTACAT", options, unlimited.total_matches);
  CHECK(RightMaximalTuples(exact_limit) == RightMaximalTuples(unlimited));
  CHECK(!exact_limit.truncated);
  const auto count_only =
      index.FindRightMaximalMatches("CCGATTACAT", options, 0);
  CHECK(count_only.matches.empty());
  CHECK(count_only.truncated == (count_only.total_matches != 0));

  std::uint64_t streamed = 0;
  index.ForEachRightMaximalMatch(
      "CCGATTACAT", options,
      [&](const sufkit::RightMaximalMatch&) { ++streamed; });
  CHECK(streamed ==
        index.FindRightMaximalMatches("CCGATTACAT", options).total_matches);
  try {
    index.ForEachRightMaximalMatch(
        "CCGATTACAT", options, [](const sufkit::RightMaximalMatch&) {
          throw std::runtime_error("callback sentinel");
        });
    CHECK(false);
  } catch (const std::runtime_error& error) {
    CHECK(std::string(error.what()) == "callback sentinel");
  }
  std::atomic<bool> concurrent_ok{true};
  std::vector<std::thread> workers;
  const auto expected_both =
      RightMaximalTuples(index.FindRightMaximalMatches("CCGATTACAT", options));
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int repetition = 0; repetition < 50; ++repetition) {
        if (RightMaximalTuples(index.FindRightMaximalMatches(
                "CCGATTACAT", options)) != expected_both) {
          concurrent_ok.store(false);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  CHECK(concurrent_ok.load());
  CheckError(sufkit::ErrorCode::kInvalidInput, [&] {
    auto invalid = options;
    invalid.min_length = 0;
    (void)index.FindRightMaximalMatches("ACGT", invalid);
  });

  const std::array<sufkit::SaAcceleration, 5> modes{
      {sufkit::SaAcceleration::kNone, sufkit::SaAcceleration::kLcp,
       sufkit::SaAcceleration::kLcpChild,
       sufkit::SaAcceleration::kLcpSuffixLink, sufkit::SaAcceleration::kFull}};
  for (std::size_t mode_index = 0; mode_index < modes.size(); ++mode_index) {
    build_options.acceleration = modes[mode_index];
    auto built = sufkit::SuffixArray::Build(reference, build_options);
    const auto path =
        directory / ("right-maximal-" + std::to_string(mode_index) + ".sufidx");
    built.Save(path);
    const auto inspected = sufkit::InspectIndex(path);
    CHECK(inspected.sa_acceleration == modes[mode_index]);
    CHECK(inspected.format_version == "1.4");
    auto loaded = sufkit::SuffixArray::Load(path);
    auto baseline_options = options;
    baseline_options.strands = sufkit::StrandMode::kForward;
    baseline_options.algorithm = sufkit::RightMaximalSearchAlgorithm::kBaseline;
    CHECK(RightMaximalTuples(loaded.FindRightMaximalMatches(
              "CCGATTACAT", baseline_options)) == RightMaximalTuples(baseline));
    if (modes[mode_index] == sufkit::SaAcceleration::kNone) {
      auto unavailable = baseline_options;
      unavailable.algorithm = sufkit::RightMaximalSearchAlgorithm::kChild;
      CheckError(sufkit::ErrorCode::kUnsupportedBackend, [&] {
        (void)loaded.FindRightMaximalMatches("CCGATTACAT", unavailable);
      });
    }
  }
}

void TestRandomizedRightMaximalDifferential() {
  std::uint64_t state = 0x51f17e5aULL;
  const auto next = [&] {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
  };
  const std::array<char, 5> alphabet{{'A', 'C', 'G', 'T', 'N'}};
  for (int trial = 0; trial < 30; ++trial) {
    std::vector<sufkit::SequenceRecord> records;
    for (int sequence_id = 0; sequence_id < 2; ++sequence_id) {
      std::string sequence(28, 'A');
      for (auto& base : sequence) {
        base = alphabet[next() % alphabet.size()];
      }
      records.push_back({"right-maximal-r" + std::to_string(sequence_id), "",
                         std::move(sequence)});
    }
    std::string query(24, 'A');
    for (auto& base : query) {
      base = alphabet[next() % alphabet.size()];
    }
    const auto reference = sufkit::GenomeReference::FromRecords(records);
    sufkit::SuffixArrayBuildOptions build_options;
    build_options.acceleration = sufkit::SaAcceleration::kFull;
    auto index = sufkit::SuffixArray::Build(reference, build_options);
    for (std::uint64_t min_length = 1; min_length <= 5; ++min_length) {
      sufkit::RightMaximalOptions options;
      options.min_length = min_length;
      const auto expected =
          NaiveRightMaximalMatches(records, query, min_length);
      for (const auto algorithm :
           {sufkit::RightMaximalSearchAlgorithm::kBaseline,
            sufkit::RightMaximalSearchAlgorithm::kLcp,
            sufkit::RightMaximalSearchAlgorithm::kChild,
            sufkit::RightMaximalSearchAlgorithm::kSuffixLink,
            sufkit::RightMaximalSearchAlgorithm::kFull}) {
        options.algorithm = algorithm;
        const auto observed =
            RightMaximalTuples(index.FindRightMaximalMatches(query, options));
        CHECK(observed == expected);
      }
    }
  }
}

}  // namespace

int main() {
  const auto directory = TestDirectory();
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  try {
    TestReferenceAndFasta(directory);
    TestSuffixArray(directory);
    TestSaStorageProfiles(directory);
    TestFastPrefixDirectoryIntegration(directory);
    TestPackedSpanLocateEnumeration();
    TestLegacySaFixtures();
    TestManyContigCoordinateMapping(directory);
    TestLearnedSa(directory);
    TestFmIndex(directory);
    TestRandomizedDifferential();
    TestRightMaximalSearch(directory);
    TestRandomizedRightMaximalDifferential();
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    ++failures;
  }
  std::filesystem::remove_all(directory);
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "all sufkit tests passed\n";
  return 0;
}
