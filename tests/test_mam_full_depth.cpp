// SPDX-License-Identifier: MIT

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
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

using MatchTuple =
    std::tuple<std::uint64_t, sufkit::SequenceId, std::uint64_t,
               std::uint64_t, sufkit::Strand>;

std::string Normalize(std::string sequence) {
  for (char& base : sequence) {
    base =
        static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
    if (base != 'A' && base != 'C' && base != 'G' && base != 'T') {
      base = 'N';
    }
  }
  return sequence;
}

std::string ReverseComplement(const std::string& raw_sequence) {
  const auto sequence = Normalize(raw_sequence);
  std::string result(sequence.size(), 'N');
  for (std::size_t index = 0; index < sequence.size(); ++index) {
    switch (sequence[sequence.size() - index - 1]) {
      case 'A':
        result[index] = 'T';
        break;
      case 'C':
        result[index] = 'G';
        break;
      case 'G':
        result[index] = 'C';
        break;
      case 'T':
        result[index] = 'A';
        break;
      default:
        result[index] = 'N';
        break;
    }
  }
  return result;
}

std::uint64_t ReferenceOccurrences(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& pattern) {
  std::uint64_t count = 0;
  for (const auto& record : records) {
    const auto reference = Normalize(record.sequence);
    for (std::size_t position = 0;
         position + pattern.size() <= reference.size(); ++position) {
      if (reference.compare(position, pattern.size(), pattern) == 0) {
        ++count;
      }
    }
  }
  return count;
}

void AppendNaiveOneStrand(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& oriented_query, std::uint64_t original_query_length,
    std::uint64_t min_length, sufkit::Strand strand,
    std::vector<MatchTuple>& result) {
  const auto query = Normalize(oriented_query);
  for (std::size_t query_position = 0; query_position < query.size();
       ++query_position) {
    if (query[query_position] == 'N') {
      continue;
    }
    for (std::size_t sequence_id = 0; sequence_id < records.size();
         ++sequence_id) {
      const auto reference = Normalize(records[sequence_id].sequence);
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
            query_position > 0 && reference_position > 0 &&
            query[query_position - 1] != 'N' &&
            reference[reference_position - 1] != 'N' &&
            query[query_position - 1] == reference[reference_position - 1];
        if (left_extendable) {
          continue;
        }
        const auto pattern = query.substr(query_position, length);
        if (ReferenceOccurrences(records, pattern) != 1) {
          continue;
        }
        const auto output_position =
            strand == sufkit::Strand::kReverseComplement
                ? original_query_length - (query_position + length)
                : static_cast<std::uint64_t>(query_position);
        result.emplace_back(
            output_position, static_cast<sufkit::SequenceId>(sequence_id),
            reference_position, length, strand);
      }
    }
  }
}

std::vector<MatchTuple> NaiveMams(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& query, std::uint64_t min_length,
    sufkit::StrandMode strands) {
  std::vector<MatchTuple> result;
  if (strands == sufkit::StrandMode::kForward ||
      strands == sufkit::StrandMode::kBoth) {
    AppendNaiveOneStrand(records, query, query.size(), min_length,
                         sufkit::Strand::kForward, result);
  }
  if (strands == sufkit::StrandMode::kReverseComplement ||
      strands == sufkit::StrandMode::kBoth) {
    AppendNaiveOneStrand(records, ReverseComplement(query), query.size(),
                         min_length, sufkit::Strand::kReverseComplement,
                         result);
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<MatchTuple> Tuples(const sufkit::MamResult& result) {
  std::vector<MatchTuple> values;
  values.reserve(result.matches.size());
  for (const auto& match : result.matches) {
    values.emplace_back(match.query_position, match.sequence_id,
                        match.reference_position, match.length, match.strand);
  }
  return values;
}

sufkit::SuffixArray BuildFast(
    const std::vector<sufkit::SequenceRecord>& records) {
  auto options = sufkit::FastSuffixArrayBuildOptions();
  options.acceleration = sufkit::SaAcceleration::kFull;
  return sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records), options);
}

void GenerateStrings(const std::string& alphabet, std::size_t max_length,
                     std::vector<std::string>& values,
                     std::string prefix = {}) {
  if (!prefix.empty()) {
    values.push_back(prefix);
  }
  if (prefix.size() == max_length) {
    return;
  }
  for (const char base : alphabet) {
    GenerateStrings(alphabet, max_length, values, prefix + base);
  }
}

void MixByte(std::uint64_t& checksum, std::uint8_t value) {
  checksum ^= value;
  checksum *= 1099511628211ULL;
}

void MixU64(std::uint64_t& checksum, std::uint64_t value) {
  for (unsigned int byte = 0; byte < 8; ++byte) {
    MixByte(checksum,
            static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xffU));
  }
}

void MixMatches(std::uint64_t& checksum,
                const std::vector<MatchTuple>& matches) {
  MixU64(checksum, matches.size());
  for (const auto& match : matches) {
    MixU64(checksum, std::get<0>(match));
    MixU64(checksum, std::get<1>(match));
    MixU64(checksum, std::get<2>(match));
    MixU64(checksum, std::get<3>(match));
    MixU64(checksum, static_cast<std::uint8_t>(std::get<4>(match)));
  }
}

std::vector<MatchTuple> Search(
    const sufkit::SuffixArray& index, const std::string& query,
    std::uint64_t min_length, sufkit::StrandMode strands,
    sufkit::MemSearchAlgorithm algorithm) {
  sufkit::MamOptions options;
  options.min_length = min_length;
  options.strands = strands;
  options.algorithm = algorithm;
  options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  return Tuples(index.FindMams(query, options));
}

void CheckAllAlgorithms(
    const sufkit::SuffixArray& index,
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& query, std::uint64_t min_length,
    sufkit::StrandMode strands, std::uint64_t* oracle_checksum = nullptr,
    std::uint64_t* baseline_checksum = nullptr,
    std::uint64_t* suffix_link_checksum = nullptr) {
  const auto expected = NaiveMams(records, query, min_length, strands);
  if (oracle_checksum) {
    MixMatches(*oracle_checksum, expected);
  }
  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kChild,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    const auto observed =
        Search(index, query, min_length, strands, algorithm);
    CHECK(observed == expected);
    if (algorithm == sufkit::MemSearchAlgorithm::kBaseline &&
        baseline_checksum) {
      MixMatches(*baseline_checksum, observed);
    }
    if (algorithm == sufkit::MemSearchAlgorithm::kSuffixLink &&
        suffix_link_checksum) {
      MixMatches(*suffix_link_checksum, observed);
    }
  }
}

void TestExhaustiveOldNewDifferential() {
  std::vector<std::string> references;
  std::vector<std::string> queries;
  GenerateStrings("AC", 3, references);
  GenerateStrings("AC", 4, queries);

  std::uint64_t oracle_checksum = 1469598103934665603ULL;
  std::uint64_t baseline_checksum = 1469598103934665603ULL;
  std::uint64_t suffix_link_checksum = 1469598103934665603ULL;
  std::uint64_t case_id = 0;
  for (const auto& first : references) {
    for (const auto& second : references) {
      const std::vector<sufkit::SequenceRecord> records{
          {"r0", "", first}, {"r1", "", second}};
      const auto index = BuildFast(records);
      for (const auto& query : queries) {
        for (std::uint64_t min_length = 1;
             min_length <= query.size(); ++min_length) {
          MixU64(oracle_checksum, case_id);
          MixU64(baseline_checksum, case_id);
          MixU64(suffix_link_checksum, case_id);
          CheckAllAlgorithms(index, records, query, min_length,
                             sufkit::StrandMode::kForward, &oracle_checksum,
                             &baseline_checksum, &suffix_link_checksum);
          ++case_id;
        }
      }
    }
  }

  CHECK(baseline_checksum == oracle_checksum);
  CHECK(suffix_link_checksum == oracle_checksum);
  // This fingerprint freezes the exhaustive corpus and the legacy root-search
  // result set. A full-depth suffix-link sweep must remain byte-for-byte
  // equivalent at the public tuple boundary.
  constexpr std::uint64_t kExpectedChecksum = 16812603661132351543ULL;
  CHECK(oracle_checksum == kExpectedChecksum);
}

std::uint64_t NextRandom(std::uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 2685821657736338717ULL;
}

std::string RandomSequence(std::uint64_t& state, std::size_t length,
                           bool query) {
  static constexpr char kReferenceAlphabet[] = "ACGTN";
  static constexpr char kQueryAlphabet[] = "ACGTNxacgt";
  std::string value(length, 'A');
  for (char& base : value) {
    const auto random = NextRandom(state);
    if (query) {
      base = kQueryAlphabet[random % (sizeof(kQueryAlphabet) - 1)];
    } else {
      base = kReferenceAlphabet[random % (sizeof(kReferenceAlphabet) - 1)];
    }
  }
  return value;
}

void TestRandomDifferential() {
  std::uint64_t state = 2026083003ULL;
  for (std::uint64_t trial = 0; trial < 80; ++trial) {
    const auto record_count = 1 + NextRandom(state) % 3;
    std::vector<sufkit::SequenceRecord> records;
    for (std::uint64_t id = 0; id < record_count; ++id) {
      const auto length = 8 + NextRandom(state) % 33;
      records.push_back({"r" + std::to_string(id), "",
                         RandomSequence(state, length, false)});
    }
    const auto query_length = 4 + NextRandom(state) % 45;
    const auto query = RandomSequence(state, query_length, true);
    const auto index = BuildFast(records);
    for (const auto strands : {sufkit::StrandMode::kForward,
                               sufkit::StrandMode::kReverseComplement,
                               sufkit::StrandMode::kBoth}) {
      for (const auto min_length : {1ULL, 2ULL, 4ULL, 7ULL, 12ULL}) {
        CheckAllAlgorithms(index, records, query, min_length, strands);
      }
    }
  }
}

void TestBoundariesStrandsAndUniqueness() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTTGATTACAGGGNACGTAC"},
      {"r1", "", "CCCGATTACATTTNACGTTC"},
      {"r2", "", "GGGTGTAATCGGG"}};
  const auto index = BuildFast(records);
  for (const auto& query :
       {std::string("NNgattacaGGGNACGTACnn"),
        std::string("CAGATTACATTTTGTAATC"),
        std::string("ACGTACNACGTTC"), std::string("NNNN")}) {
    for (const auto strands : {sufkit::StrandMode::kForward,
                               sufkit::StrandMode::kReverseComplement,
                               sufkit::StrandMode::kBoth}) {
      for (const auto min_length : {1ULL, 4ULL, 7ULL}) {
        CheckAllAlgorithms(index, records, query, min_length, strands);
      }
    }
  }

  // The seven-base GATTACA prefix occurs in two contigs. Only the longer
  // flanks may become reference-unique; no candidate may cross either N or a
  // contig separator while proving uniqueness.
  const std::string query = "AAGATTACAGGGNCCGATTACATTT";
  const auto expected =
      NaiveMams(records, query, 7, sufkit::StrandMode::kForward);
  const auto observed = Search(index, query, 7,
                               sufkit::StrandMode::kForward,
                               sufkit::MemSearchAlgorithm::kSuffixLink);
  CHECK(observed == expected);
  CHECK(std::none_of(observed.begin(), observed.end(), [](const auto& match) {
    return std::get<3>(match) == 7;
  }));
}

void TestUniqueChainMustResumeTraversal() {
  // q=0 has the unique MAM ACGT. After deleting its first character, CGT is
  // no longer unique because the second TCGTAAA occurrence joins the suffix
  // interval. Traversal must resume immediately so q=1 can discover the
  // longer, internally starting MAM CGTAAA instead of skipping over it.
  const std::vector<sufkit::SequenceRecord> records{
      {"ref", "", "ACGTCTCGTAAA"}};
  const std::string query = "ACGTAAA";
  const auto index = BuildFast(records);
  const std::vector<MatchTuple> expected_forward{
      {0, 0, 0, 4, sufkit::Strand::kForward},
      {1, 0, 6, 6, sufkit::Strand::kForward}};
  CHECK(NaiveMams(records, query, 3, sufkit::StrandMode::kForward) ==
        expected_forward);
  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kChild,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    CHECK(Search(index, query, 3, sufkit::StrandMode::kForward, algorithm) ==
          expected_forward);
  }

  sufkit::MamOptions options;
  options.min_length = 3;
  options.strands = sufkit::StrandMode::kBoth;
  options.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  const auto expected_both =
      NaiveMams(records, query, 3, sufkit::StrandMode::kBoth);
  CHECK(Tuples(index.FindMams(query, options)) == expected_both);
  std::vector<MatchTuple> streamed;
  index.ForEachMam(query, options, [&](const sufkit::MamMatch& match) {
    streamed.emplace_back(match.query_position, match.sequence_id,
                          match.reference_position, match.length,
                          match.strand);
  });
  std::sort(streamed.begin(), streamed.end());
  CHECK(streamed == expected_both);
}

void TestSuffixLinkScanBudgetFallsBackToRoot() {
  // The production suffix-link scan budget is 4096 LCP probes. C followed by a
  // longer A-run makes q=0 reference-unique, while deleting C expands A^k to
  // more than 4096 suffixes. The q=1 A^kG match is still a valid internal MAM
  // and must be recovered by the exact root fallback after budget exhaustion.
  constexpr std::size_t kProbeBudget = 4096;
  constexpr std::size_t kMinLength = 32;
  constexpr std::size_t kRepeatBases =
      kProbeBudget + kMinLength + 16;
  const std::vector<sufkit::SequenceRecord> records{
      {"ref", "", "C" + std::string(kRepeatBases, 'A') + "G"}};
  const std::string query = "C" + std::string(kMinLength, 'A') + "G";
  const auto index = BuildFast(records);
  const std::vector<MatchTuple> expected{
      {0, 0, 0, kMinLength + 1, sufkit::Strand::kForward},
      {1, 0, 1 + kRepeatBases - kMinLength, kMinLength + 1,
       sufkit::Strand::kForward}};

  for (const auto algorithm :
       {sufkit::MemSearchAlgorithm::kBaseline,
        sufkit::MemSearchAlgorithm::kLcp,
        sufkit::MemSearchAlgorithm::kSuffixLink,
        sufkit::MemSearchAlgorithm::kFull,
        sufkit::MemSearchAlgorithm::kAutoSelect}) {
    CHECK(Search(index, query, kMinLength, sufkit::StrandMode::kForward,
                 algorithm) == expected);
  }
}

void TestStreamingExceptionsPersistenceAndConcurrency() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "AACCGGTTAACCGATTACAGGGTTTACGTCAGTACGATC"},
      {"r1", "", "TTTTGATTACATTTCCCGATCGTAGCTAG"}};
  const std::string query =
      "NNacgTCAGTACGATCccGATTACAGGGNNCTAGCTACGATC";
  const auto index = BuildFast(records);
  sufkit::MamOptions options;
  options.min_length = 6;
  options.strands = sufkit::StrandMode::kBoth;
  options.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
  options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
  const auto expected =
      NaiveMams(records, query, options.min_length, options.strands);
  CHECK(!expected.empty());
  CHECK(Tuples(index.FindMams(query, options)) == expected);

  std::vector<MatchTuple> streamed;
  index.ForEachMam(query, options, [&](const sufkit::MamMatch& match) {
    streamed.emplace_back(match.query_position, match.sequence_id,
                          match.reference_position, match.length,
                          match.strand);
  });
  std::sort(streamed.begin(), streamed.end());
  streamed.erase(std::unique(streamed.begin(), streamed.end()), streamed.end());
  CHECK(streamed == expected);

  bool propagated = false;
  try {
    index.ForEachMam(query, options, [](const sufkit::MamMatch&) {
      throw std::runtime_error("full-depth MAM callback sentinel");
    });
  } catch (const std::runtime_error& error) {
    propagated =
        std::string(error.what()) == "full-depth MAM callback sentinel";
  }
  CHECK(propagated);

  const auto path =
      std::filesystem::current_path() /
      ("sufkit-mam-full-depth-roundtrip-" +
       std::to_string(static_cast<long long>(getpid())) + ".sufidx");
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  index.Save(path);
  const auto loaded = sufkit::SuffixArray::Load(path);
  std::filesystem::remove(path, ignored);
  CHECK(Tuples(loaded.FindMams(query, options)) == expected);

  std::atomic<bool> consistent{true};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 50; ++iteration) {
        if (Tuples(loaded.FindMams(query, options)) != expected) {
          consistent = false;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  CHECK(consistent.load());
}

void TestLowMemoryMatchesFast() {
  const std::vector<sufkit::SequenceRecord> records{
      {"r0", "", "TTTGATTACAGGGACGTCAGTACGATC"},
      {"r1", "", "CCCGATTACATTTGATCGTAGCTAG"}};
  const std::string query = "NNACGTCAGTACGATCCCGATTACAGGGNN";
  const auto fast = BuildFast(records);
  const auto low = sufkit::SuffixArray::Build(
      sufkit::GenomeReference::FromRecords(records),
      sufkit::LowMemorySuffixArrayBuildOptions());
  for (const auto strands : {sufkit::StrandMode::kForward,
                             sufkit::StrandMode::kReverseComplement,
                             sufkit::StrandMode::kBoth}) {
    sufkit::MamOptions fast_options;
    fast_options.min_length = 5;
    fast_options.strands = strands;
    fast_options.algorithm = sufkit::MemSearchAlgorithm::kSuffixLink;
    fast_options.lookup_algorithm = sufkit::SaSearchAlgorithm::kBinary;
    auto low_options = fast_options;
    low_options.algorithm = sufkit::MemSearchAlgorithm::kLcp;
    const auto expected = NaiveMams(records, query, 5, strands);
    CHECK(Tuples(fast.FindMams(query, fast_options)) == expected);
    CHECK(Tuples(low.FindMams(query, low_options)) == expected);
  }
}

}  // namespace

int main() {
  TestExhaustiveOldNewDifferential();
  TestRandomDifferential();
  TestBoundariesStrandsAndUniqueness();
  TestUniqueChainMustResumeTraversal();
  TestSuffixLinkScanBudgetFallsBackToRoot();
  TestStreamingExceptionsPersistenceAndConcurrency();
  TestLowMemoryMatchesFast();
  if (failures != 0) {
    std::cerr << failures << " full-depth MAM assertion(s) failed\n";
    return 1;
  }
  std::cout << "Full-depth MAM regression tests passed\n";
  return 0;
}
