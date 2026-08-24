// SPDX-License-Identifier: MIT

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_probe {

#if defined(__GNUC__) || defined(__clang__)
#define SUFKIT_ALLOCATION_NOINLINE __attribute__((noinline))
#else
#define SUFKIT_ALLOCATION_NOINLINE
#endif

std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> allocation_count{0};
std::atomic<std::uint64_t> allocated_bytes{0};
volatile std::uintptr_t pointer_sink = 0;

void Record(std::size_t size) noexcept {
  if (!enabled.load(std::memory_order_relaxed)) {
    return;
  }
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  allocated_bytes.fetch_add(static_cast<std::uint64_t>(size),
                            std::memory_order_relaxed);
}

void Reset() noexcept {
  enabled.store(false, std::memory_order_relaxed);
  allocation_count.store(0, std::memory_order_relaxed);
  allocated_bytes.store(0, std::memory_order_relaxed);
}

}  // namespace allocation_probe

SUFKIT_ALLOCATION_NOINLINE void* operator new(std::size_t size) {
  const auto effective_size = std::max<std::size_t>(size, 1);
  void* memory = std::malloc(effective_size);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  allocation_probe::Record(effective_size);
  return memory;
}

SUFKIT_ALLOCATION_NOINLINE void* operator new[](std::size_t size) {
  return ::operator new(size);
}

SUFKIT_ALLOCATION_NOINLINE void* operator new(
    std::size_t size, std::align_val_t alignment) {
  const auto effective_size = std::max<std::size_t>(size, 1);
  const auto effective_alignment = std::max<std::size_t>(
      static_cast<std::size_t>(alignment), sizeof(void*));
  void* memory = nullptr;
  if (posix_memalign(&memory, effective_alignment, effective_size) != 0) {
    throw std::bad_alloc();
  }
  allocation_probe::Record(effective_size);
  return memory;
}

SUFKIT_ALLOCATION_NOINLINE void* operator new[](
    std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

SUFKIT_ALLOCATION_NOINLINE void* operator new(
    std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

SUFKIT_ALLOCATION_NOINLINE void* operator new[](
    std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

SUFKIT_ALLOCATION_NOINLINE void* operator new(
    std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}

SUFKIT_ALLOCATION_NOINLINE void* operator new[](
    std::size_t size, std::align_val_t alignment,
    const std::nothrow_t&) noexcept {
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}

SUFKIT_ALLOCATION_NOINLINE void operator delete(void* memory) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](void* memory) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete(void* memory,
                                                 std::size_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](void* memory,
                                                   std::size_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete(
    void* memory, std::align_val_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](
    void* memory, std::align_val_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete(
    void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](
    void* memory, std::size_t, std::align_val_t) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete(
    void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](
    void* memory, const std::nothrow_t&) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete(
    void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(memory);
}
SUFKIT_ALLOCATION_NOINLINE void operator delete[](
    void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  std::free(memory);
}

#undef SUFKIT_ALLOCATION_NOINLINE

namespace {

struct Options {
  bool verify_only = false;
  std::filesystem::path output;
};

struct AllocationRow {
  std::string operation;
  std::uint64_t allocation_count = 0;
  std::uint64_t allocated_bytes = 0;
  std::uint64_t checksum = 0;
};

class CountingScope {
 public:
  CountingScope() {
    allocation_probe::Reset();
    allocation_probe::enabled.store(true, std::memory_order_relaxed);
  }

  ~CountingScope() {
    allocation_probe::enabled.store(false, std::memory_order_relaxed);
  }

  CountingScope(const CountingScope&) = delete;
  CountingScope& operator=(const CountingScope&) = delete;
};

void VerifyCounterCoverage() {
  {
    CountingScope scope;
    void* ordinary = ::operator new(3);
    allocation_probe::pointer_sink =
        reinterpret_cast<std::uintptr_t>(ordinary);
    ::operator delete(ordinary);
    void* array = ::operator new[](5);
    allocation_probe::pointer_sink = reinterpret_cast<std::uintptr_t>(array);
    ::operator delete[](array);
    void* aligned = ::operator new(64, std::align_val_t{64});
    allocation_probe::pointer_sink =
        reinterpret_cast<std::uintptr_t>(aligned);
    if (reinterpret_cast<std::uintptr_t>(aligned) % 64 != 0) {
      ::operator delete(aligned, std::align_val_t{64});
      throw std::runtime_error("aligned allocation is not 64-byte aligned");
    }
    ::operator delete(aligned, std::align_val_t{64});
    void* aligned_array = ::operator new[](96, std::align_val_t{64});
    allocation_probe::pointer_sink =
        reinterpret_cast<std::uintptr_t>(aligned_array);
    if (reinterpret_cast<std::uintptr_t>(aligned_array) % 64 != 0) {
      ::operator delete[](aligned_array, std::align_val_t{64});
      throw std::runtime_error(
          "aligned array allocation is not 64-byte aligned");
    }
    ::operator delete[](aligned_array, std::align_val_t{64});
  }
  const auto count =
      allocation_probe::allocation_count.load(std::memory_order_relaxed);
  const auto bytes =
      allocation_probe::allocated_bytes.load(std::memory_order_relaxed);
  if (count != 4 || bytes != 168) {
    throw std::runtime_error(
        "ordinary/array/aligned allocation counter coverage failed");
  }
  allocation_probe::Reset();
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help") {
      std::cout << "sufkit_query_allocation_bench [--verify-only] "
                   "[--output FILE]\n";
      std::exit(0);
    }
    if (name == "--verify-only") {
      options.verify_only = true;
      continue;
    }
    if (name == "--output") {
      if (++index >= argc) {
        throw std::runtime_error("--output requires a path");
      }
      options.output = argv[index];
      continue;
    }
    throw std::runtime_error("unknown option: " + name);
  }
  if (!options.verify_only && options.output.empty()) {
    throw std::runtime_error(
        "--output is required unless --verify-only is used");
  }
  return options;
}

void Mix(std::uint64_t& checksum, std::uint64_t value) noexcept {
  for (unsigned int byte = 0; byte < 8; ++byte) {
    checksum ^= (value >> (byte * 8U)) & 0xffU;
    checksum *= 1099511628211ULL;
  }
}

std::uint64_t ChecksumSeed() noexcept { return 1469598103934665603ULL; }

void MixQueryResult(std::uint64_t& checksum,
                    const sufkit::QueryResult& result) noexcept {
  Mix(checksum, result.total_hits);
  Mix(checksum, result.hits.size());
  Mix(checksum, result.truncated ? 1 : 0);
  for (const auto& match : result.hits) {
    Mix(checksum, match.sequence_id);
    Mix(checksum, match.position);
    Mix(checksum, match.length);
    Mix(checksum, static_cast<std::uint8_t>(match.strand));
  }
}

void MixRightMaximalResult(
    std::uint64_t& checksum,
    const sufkit::RightMaximalResult& result) noexcept {
  Mix(checksum, result.total_matches);
  Mix(checksum, result.matches.size());
  Mix(checksum, result.truncated ? 1 : 0);
  for (const auto& match : result.matches) {
    Mix(checksum, match.sequence_id);
    Mix(checksum, match.reference_position);
    Mix(checksum, match.query_position);
    Mix(checksum, match.length);
    Mix(checksum, static_cast<std::uint8_t>(match.strand));
  }
}

template <class Operation>
AllocationRow Measure(const char* name, Operation&& operation) {
  std::uint64_t checksum = 0;
  {
    CountingScope scope;
    checksum = operation();
  }
  AllocationRow row;
  row.operation = name;
  row.allocation_count =
      allocation_probe::allocation_count.load(std::memory_order_relaxed);
  row.allocated_bytes =
      allocation_probe::allocated_bytes.load(std::memory_order_relaxed);
  row.checksum = checksum;
  return row;
}

std::string Repeated(std::string_view motif, std::size_t copies) {
  std::string sequence;
  sequence.reserve(motif.size() * copies);
  for (std::size_t copy = 0; copy < copies; ++copy) {
    sequence.append(motif);
  }
  return sequence;
}

std::uint64_t CountChecksum(
    const sufkit::SuffixArray& index,
    const std::vector<std::string_view>& patterns) {
  auto checksum = ChecksumSeed();
  for (const auto pattern : patterns) {
    Mix(checksum, index.Count(pattern));
  }
  return checksum;
}

std::uint64_t CountChecksum(
    const sufkit::FmIndex& index,
    const std::vector<std::string_view>& patterns) {
  auto checksum = ChecksumSeed();
  for (const auto pattern : patterns) {
    Mix(checksum, index.Count(pattern));
  }
  return checksum;
}

std::uint64_t BatchCountChecksum(
    const sufkit::FmIndex& index,
    const std::vector<std::string_view>& patterns) {
  auto checksum = ChecksumSeed();
  for (const auto count : index.CountBatch(patterns)) {
    Mix(checksum, count);
  }
  return checksum;
}

std::uint64_t LocateChecksum(const sufkit::SuffixArray& index,
                             std::string_view pattern,
                             std::optional<std::uint64_t> max_hits) {
  sufkit::LocateOptions options;
  options.max_hits = max_hits;
  auto checksum = ChecksumSeed();
  MixQueryResult(checksum, index.Locate(pattern, options));
  return checksum;
}

std::uint64_t LocateChecksum(const sufkit::FmIndex& index,
                             std::string_view pattern,
                             std::optional<std::uint64_t> max_hits) {
  sufkit::LocateOptions options;
  options.max_hits = max_hits;
  auto checksum = ChecksumSeed();
  MixQueryResult(checksum, index.Locate(pattern, options));
  return checksum;
}

std::uint64_t RightMaximalChecksum(
    const sufkit::SuffixArray& index, std::string_view query,
    const sufkit::RightMaximalOptions& options,
    std::optional<std::uint64_t> max_matches) {
  auto checksum = ChecksumSeed();
  MixRightMaximalResult(
      checksum, index.FindRightMaximalMatches(query, options, max_matches));
  return checksum;
}

const AllocationRow& FindRow(const std::vector<AllocationRow>& rows,
                             std::string_view operation) {
  const auto iterator = std::find_if(
      rows.begin(), rows.end(), [&](const AllocationRow& row) {
        return row.operation == operation;
      });
  if (iterator == rows.end()) {
    throw std::runtime_error("missing allocation row: " +
                             std::string(operation));
  }
  return *iterator;
}

void VerifyRows(const std::vector<AllocationRow>& rows,
                const sufkit::SuffixArray& suffix_array,
                const sufkit::FmIndex& fm_index,
                const std::vector<std::string_view>& patterns,
                std::string_view locate_pattern,
                std::string_view right_maximal_query,
                const sufkit::RightMaximalOptions& right_options) {
  if (rows.size() != 11) {
    throw std::runtime_error("unexpected allocation benchmark row count");
  }
  const auto sa_count = FindRow(rows, "sa_count").checksum;
  if (sa_count != FindRow(rows, "fm_count_scalar").checksum ||
      sa_count != FindRow(rows, "fm_count_batch").checksum) {
    throw std::runtime_error("SA/FM scalar/batch count checksum mismatch");
  }
  for (const auto suffix : {"locate_0", "locate_10", "locate_all"}) {
    if (FindRow(rows, std::string("sa_") + suffix).checksum !=
        FindRow(rows, std::string("fm_") + suffix).checksum) {
      throw std::runtime_error(std::string("SA/FM ") + suffix +
                               " checksum mismatch");
    }
  }

  allocation_probe::enabled.store(false, std::memory_order_relaxed);
  if (CountChecksum(suffix_array, patterns) !=
          CountChecksum(fm_index, patterns) ||
      CountChecksum(fm_index, patterns) !=
          BatchCountChecksum(fm_index, patterns)) {
    throw std::runtime_error("disabled-counter count verification failed");
  }
  const auto bounded = suffix_array.FindRightMaximalMatches(
      right_maximal_query, right_options, 8);
  const auto unbounded = suffix_array.FindRightMaximalMatches(
      right_maximal_query, right_options);
  if (bounded.total_matches != unbounded.total_matches ||
      bounded.matches.size() > unbounded.matches.size() ||
      !std::equal(bounded.matches.begin(), bounded.matches.end(),
                  unbounded.matches.begin(), [](const auto& left,
                                                 const auto& right) {
                    return left.sequence_id == right.sequence_id &&
                           left.reference_position ==
                               right.reference_position &&
                           left.query_position == right.query_position &&
                           left.length == right.length &&
                           left.strand == right.strand;
                  })) {
    throw std::runtime_error("bounded right-maximal result is not a prefix");
  }

  const auto sa_locate = suffix_array.Locate(locate_pattern);
  const auto fm_locate = fm_index.Locate(locate_pattern);
  auto sa_checksum = ChecksumSeed();
  auto fm_checksum = ChecksumSeed();
  MixQueryResult(sa_checksum, sa_locate);
  MixQueryResult(fm_checksum, fm_locate);
  if (sa_checksum != fm_checksum) {
    throw std::runtime_error("disabled-counter locate verification failed");
  }
}

std::vector<AllocationRow> RunMeasurements(
    const sufkit::SuffixArray& suffix_array, const sufkit::FmIndex& fm_index,
    const std::vector<std::string_view>& patterns,
    std::string_view locate_pattern, std::string_view right_maximal_query,
    const sufkit::RightMaximalOptions& right_options) {
  std::vector<AllocationRow> rows;
  rows.reserve(11);
  rows.push_back(Measure("sa_count", [&] {
    return CountChecksum(suffix_array, patterns);
  }));
  rows.push_back(Measure("sa_locate_0", [&] {
    return LocateChecksum(suffix_array, locate_pattern, 0);
  }));
  rows.push_back(Measure("sa_locate_10", [&] {
    return LocateChecksum(suffix_array, locate_pattern, 10);
  }));
  rows.push_back(Measure("sa_locate_all", [&] {
    return LocateChecksum(suffix_array, locate_pattern, std::nullopt);
  }));
  rows.push_back(Measure("right_maximal_bounded", [&] {
    return RightMaximalChecksum(suffix_array, right_maximal_query,
                                right_options, 8);
  }));
  rows.push_back(Measure("right_maximal_unbounded", [&] {
    return RightMaximalChecksum(suffix_array, right_maximal_query,
                                right_options, std::nullopt);
  }));
  rows.push_back(Measure("fm_count_scalar", [&] {
    return CountChecksum(fm_index, patterns);
  }));
  rows.push_back(Measure("fm_count_batch", [&] {
    return BatchCountChecksum(fm_index, patterns);
  }));
  rows.push_back(Measure("fm_locate_0", [&] {
    return LocateChecksum(fm_index, locate_pattern, 0);
  }));
  rows.push_back(Measure("fm_locate_10", [&] {
    return LocateChecksum(fm_index, locate_pattern, 10);
  }));
  rows.push_back(Measure("fm_locate_all", [&] {
    return LocateChecksum(fm_index, locate_pattern, std::nullopt);
  }));
  return rows;
}

void WriteRows(const std::filesystem::path& path,
               const std::vector<AllocationRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create allocation benchmark output");
  }
  output << "operation\tallocation_count\tallocated_bytes\tchecksum\n";
  for (const auto& row : rows) {
    output << row.operation << '\t' << row.allocation_count << '\t'
           << row.allocated_bytes << '\t' << std::hex << row.checksum
           << std::dec << '\n';
  }
  if (!output) {
    throw std::runtime_error("cannot finish allocation benchmark output");
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    allocation_probe::Reset();
    const auto options = ParseOptions(argc, argv);
    VerifyCounterCoverage();

    std::vector<sufkit::SequenceRecord> records;
    auto first = Repeated("ACGT", 128);
    first.append("NNNN");
    first.append(Repeated("TGCA", 64));
    records.push_back({"contig1", "", std::move(first)});
    records.push_back(
        {"contig2", "", Repeated("GATTACACCGT", 64)});
    const auto reference = sufkit::GenomeReference::FromRecords(records);

    sufkit::SuffixArrayBuildOptions sa_options;
    sa_options.acceleration = sufkit::SaAcceleration::kLcpSuffixLink;
    const auto suffix_array = sufkit::SuffixArray::Build(reference, sa_options);
    const auto fm_index = sufkit::FmIndex::Build(reference);

    const std::string frequent_pattern = "ACGTACGTACGTACGTACGT";
    const std::string second_pattern = "GATTACACCGTGATTACACC";
    const std::string absent_pattern = "AAAAAAAAAAAAAAAAAAAA";
    const std::vector<std::string_view> patterns{
        frequent_pattern, second_pattern, absent_pattern};
    const std::string right_maximal_query =
        "TTACGTACGTACGTACGTACGTGGATTACACCGTGATTACA";
    sufkit::RightMaximalOptions right_options;
    right_options.min_length = 12;
    right_options.algorithm =
        sufkit::RightMaximalSearchAlgorithm::kSuffixLink;

    const auto rows = RunMeasurements(
        suffix_array, fm_index, patterns, frequent_pattern,
        right_maximal_query, right_options);
    VerifyRows(rows, suffix_array, fm_index, patterns, frequent_pattern,
               right_maximal_query, right_options);
    if (options.verify_only) {
      std::cout << "verified " << rows.size()
                << " query allocation observations\n";
      return 0;
    }
    WriteRows(options.output, rows);
    return 0;
  } catch (const std::exception& error) {
    allocation_probe::enabled.store(false, std::memory_order_relaxed);
    std::cerr << "sufkit_query_allocation_bench: " << error.what() << '\n';
    return 2;
  }
}
