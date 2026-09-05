// SPDX-License-Identifier: MIT

#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr std::uint64_t kReportMagic = 0x5355464b4d454d31ULL;
constexpr std::uint32_t kReportVersion = 2;
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

struct Options {
  std::string method;
  std::string worker_phase;
  std::filesystem::path reference;
  std::filesystem::path index;
  std::filesystem::path report;
  std::filesystem::path output;
  bool help = false;
};

struct WorkerReport {
  std::uint64_t magic = kReportMagic;
  std::uint32_t version = kReportVersion;
  std::uint32_t reserved = 0;
  std::uint64_t dataset_fingerprint = 0;
  std::uint64_t total_bases = 0;
  std::uint64_t contigs = 0;
  std::uint64_t current_rss_bytes = 0;
  std::uint64_t index_ready_rss_bytes = 0;
  std::uint64_t after_rss_bytes = 0;
  std::uint64_t current_pss_bytes = 0;
  std::uint64_t index_ready_pss_bytes = 0;
  std::uint64_t after_pss_bytes = 0;
  std::uint64_t serialized_bytes = 0;
  std::uint64_t construction_coordinate_width = 0;
  std::uint64_t stored_coordinate_width = 0;
  std::uint64_t resource_profile = 0;
  std::uint64_t lcp_encoding = 0;
  std::uint64_t resident_core_bytes = 0;
  std::uint64_t query_count = 0;
  std::uint64_t total_hits = 0;
  std::uint64_t reported_hits = 0;
  std::uint64_t checksum = 0;
};

struct PhaseResult {
  WorkerReport report;
  std::uint64_t peak_rss_bytes = 0;
  std::uint64_t wait4_peak_rss_bytes = 0;
};

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& Path() const noexcept { return path_; }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

 private:
  std::filesystem::path path_;
};

std::string RequireValue(int argc, char** argv, int* position) {
  if (*position + 1 >= argc) {
    throw std::runtime_error(std::string("missing value for ") +
                             argv[*position]);
  }
  ++(*position);
  return argv[*position];
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int position = 1; position < argc; ++position) {
    const std::string option = argv[position];
    if (option == "--help") {
      options.help = true;
    } else if (option == "--method") {
      options.method = RequireValue(argc, argv, &position);
    } else if (option == "--reference") {
      options.reference = RequireValue(argc, argv, &position);
    } else if (option == "--output") {
      options.output = RequireValue(argc, argv, &position);
    } else if (option == "--worker") {
      options.worker_phase = RequireValue(argc, argv, &position);
    } else if (option == "--index") {
      options.index = RequireValue(argc, argv, &position);
    } else if (option == "--report") {
      options.report = RequireValue(argc, argv, &position);
    } else {
      throw std::runtime_error("unknown option: " + option);
    }
  }
  return options;
}

void ValidateMethod(const std::string& method) {
  if (method != "sa32" && method != "sa32-fast" &&
      method != "sa32-low-memory" && method != "sa-fast" &&
      method != "sa-low-memory" && method != "sa64-fast" &&
      method != "sa64-low-memory" &&
      method != "sa64-store32-fast" &&
      method != "sa64-store40-low-memory" &&
      method != "sa64-store48-low-memory" &&
      method != "sa64-store64-fast" && method != "fm-huff") {
    throw std::runtime_error("unsupported --method");
  }
}

bool IsSuffixArrayMethod(const std::string& method) {
  return method != "fm-huff";
}

void PrintHelp() {
  std::cout
      << "sufkit_query_memory_bench --reference REF.fa[.gz] "
         "--method METHOD --output RESULTS.tsv\n\n"
         "SA methods: sa32, sa32-fast, sa32-low-memory, sa-fast, "
         "sa-low-memory, sa64-fast, sa64-low-memory, "
         "sa64-store32-fast, sa64-store40-low-memory, "
         "sa64-store48-low-memory, sa64-store64-fast\n"
         "FM method: fm-huff\n";
}

std::uint64_t CurrentRssBytes() {
  const int descriptor = open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    throw std::runtime_error("cannot open /proc/self/statm");
  }
  std::array<char, 128> buffer{};
  ssize_t bytes_read = -1;
  do {
    bytes_read = read(descriptor, buffer.data(), buffer.size() - 1);
  } while (bytes_read < 0 && errno == EINTR);
  const int close_status = close(descriptor);
  if (bytes_read <= 0 || close_status != 0) {
    throw std::runtime_error("cannot read /proc/self/statm");
  }

  std::uint64_t virtual_pages = 0;
  std::uint64_t resident_pages = 0;
  const char* begin = buffer.data();
  const char* end = begin + bytes_read;
  const auto virtual_result =
      std::from_chars(begin, end, virtual_pages);
  if (virtual_result.ec != std::errc{}) {
    throw std::runtime_error("cannot parse /proc/self/statm");
  }
  begin = virtual_result.ptr;
  while (begin != end && (*begin == ' ' || *begin == '\t')) {
    ++begin;
  }
  const auto resident_result =
      std::from_chars(begin, end, resident_pages);
  if (resident_result.ec != std::errc{}) {
    throw std::runtime_error("cannot read /proc/self/statm");
  }
  (void)virtual_pages;
  const long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    throw std::runtime_error("cannot determine the system page size");
  }
  const auto page_size = static_cast<std::uint64_t>(page_size_value);
  if (resident_pages >
      std::numeric_limits<std::uint64_t>::max() / page_size) {
    throw std::runtime_error("current RSS byte count overflow");
  }
  return resident_pages * page_size;
}

std::uint64_t CurrentPssBytes() {
  std::ifstream input("/proc/self/smaps_rollup");
  if (!input) {
    throw std::runtime_error("cannot open /proc/self/smaps_rollup");
  }
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("Pss:", 0) != 0) {
      continue;
    }
    std::istringstream parser(line.substr(4));
    std::uint64_t kib = 0;
    std::string unit;
    if (!(parser >> kib >> unit) || unit != "kB") {
      throw std::runtime_error("cannot parse /proc/self/smaps_rollup Pss");
    }
    if (kib > std::numeric_limits<std::uint64_t>::max() / 1024U) {
      throw std::runtime_error("current PSS byte count overflow");
    }
    return kib * 1024U;
  }
  throw std::runtime_error("Pss is absent from /proc/self/smaps_rollup");
}

void MixByte(std::uint64_t* checksum, std::uint8_t value) {
  *checksum ^= value;
  *checksum *= kFnvPrime;
}

void MixValue(std::uint64_t* checksum, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    MixByte(checksum,
            static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void MixText(std::uint64_t* checksum, std::string_view value) {
  MixValue(checksum, static_cast<std::uint64_t>(value.size()));
  for (const unsigned char character : value) {
    MixByte(checksum, character);
  }
}

void MixQueryResult(std::uint64_t* checksum,
                    const sufkit::QueryResult& result) {
  MixValue(checksum, result.total_hits);
  MixValue(checksum, static_cast<std::uint64_t>(result.hits.size()));
  for (const auto& match : result.hits) {
    MixValue(checksum, match.sequence_id);
    MixValue(checksum, match.position);
    MixValue(checksum, match.length);
    MixByte(checksum, static_cast<std::uint8_t>(match.strand));
  }
}

template <class Index>
void ExecuteQueries(const Index& index, WorkerReport* report) {
  static constexpr std::array<std::string_view, 3> kPatterns{{
      "ACGTACGTACGTACGTACGT",
      "AAAAAAAAAAAAAAAAAAAA",
      "TGCATGCATGCATGCATGCA",
  }};

  std::uint64_t checksum = kFnvOffset;
  for (const auto pattern : kPatterns) {
    MixByte(&checksum, 1);
    MixText(&checksum, pattern);
    const auto hits = index.Count(pattern);
    report->total_hits += hits;
    ++report->query_count;
    MixValue(&checksum, hits);
  }

  for (const auto limit : {std::uint64_t{0}, std::uint64_t{10}}) {
    sufkit::LocateOptions options;
    options.max_hits = limit;
    for (const auto pattern : kPatterns) {
      MixByte(&checksum, limit == 0 ? 2 : 3);
      MixText(&checksum, pattern);
      const auto result = index.Locate(pattern, options);
      report->total_hits += result.total_hits;
      report->reported_hits +=
          static_cast<std::uint64_t>(result.hits.size());
      ++report->query_count;
      MixQueryResult(&checksum, result);
    }
  }
  report->checksum = checksum;
}

void WriteWorkerReport(const std::filesystem::path& path,
                       const WorkerReport& report) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(&report), sizeof(report));
  output.close();
  if (!output) {
    throw std::runtime_error("cannot write phase worker report");
  }
}

WorkerReport ReadWorkerReport(const std::filesystem::path& path) {
  WorkerReport report;
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char*>(&report), sizeof(report));
  if (input.gcount() != static_cast<std::streamsize>(sizeof(report)) ||
      input.peek() != std::char_traits<char>::eof()) {
    throw std::runtime_error("invalid phase worker report size");
  }
  if (report.magic != kReportMagic || report.version != kReportVersion) {
    throw std::runtime_error("invalid phase worker report header");
  }
  return report;
}

sufkit::SuffixArrayBuildOptions SuffixArrayOptionsForMethod(
    const std::string& method) {
  const bool low_memory = method.find("low-memory") != std::string::npos;
  auto options = low_memory ? sufkit::LowMemorySuffixArrayBuildOptions()
                            : sufkit::FastSuffixArrayBuildOptions();
  options.backend = sufkit::SaBackend::kDivsufsort;
  options.sampling_rate = 1;

  if (method == "sa32" || method.rfind("sa32-", 0) == 0) {
    options.coordinate_width = sufkit::CoordinateWidth::kBits32;
  } else if (method.rfind("sa64", 0) == 0) {
    options.coordinate_width = sufkit::CoordinateWidth::kBits64;
  }

  if (method.find("store32") != std::string::npos) {
    options.storage_width = sufkit::CoordinateStorageWidth::kBits32;
  } else if (method.find("store40") != std::string::npos) {
    options.storage_width = sufkit::CoordinateStorageWidth::kBits40;
  } else if (method.find("store48") != std::string::npos) {
    options.storage_width = sufkit::CoordinateStorageWidth::kBits48;
  } else if (method.find("store64") != std::string::npos) {
    options.storage_width = sufkit::CoordinateStorageWidth::kBits64;
  }
  return options;
}

void RecordIndexInfo(const sufkit::IndexInfo& info, WorkerReport* report) {
  report->construction_coordinate_width = info.coordinate_width;
  report->stored_coordinate_width = info.stored_coordinate_width;
  report->resource_profile =
      static_cast<std::uint64_t>(info.sa_resource_profile);
  report->lcp_encoding = static_cast<std::uint64_t>(info.lcp_encoding);
  report->resident_core_bytes = info.resident_core_bytes;
}

WorkerReport BuildIndex(const Options& options) {
  auto reference = sufkit::GenomeReference::FromFasta(options.reference);
  WorkerReport report;
  report.dataset_fingerprint = reference.Fingerprint();
  report.total_bases = reference.TotalBases();
  report.contigs = reference.SequenceCount();
  if (IsSuffixArrayMethod(options.method)) {
    const auto build_options = SuffixArrayOptionsForMethod(options.method);
    auto index = sufkit::SuffixArray::Build(reference, build_options);
    RecordIndexInfo(index.GetInfo(), &report);
    index.Save(options.index);
    report.serialized_bytes = static_cast<std::uint64_t>(
        std::filesystem::file_size(options.index));
    report.current_rss_bytes = CurrentRssBytes();
    report.current_pss_bytes = CurrentPssBytes();
  } else {
    sufkit::FmIndexBuildOptions build_options;
    build_options.backend = sufkit::FmBackend::kSdslCsaWtHuff;
    auto index = sufkit::FmIndex::Build(reference, build_options);
    index.Save(options.index);
    report.serialized_bytes = static_cast<std::uint64_t>(
        std::filesystem::file_size(options.index));
    report.current_rss_bytes = CurrentRssBytes();
    report.current_pss_bytes = CurrentPssBytes();
  }
  return report;
}

WorkerReport LoadIndex(const Options& options) {
  WorkerReport report;
  if (IsSuffixArrayMethod(options.method)) {
    auto index = sufkit::SuffixArray::Load(options.index);
    RecordIndexInfo(index.GetInfo(), &report);
    report.index_ready_rss_bytes = CurrentRssBytes();
    report.index_ready_pss_bytes = CurrentPssBytes();
  } else {
    auto index = sufkit::FmIndex::Load(options.index);
    (void)index.GetInfo();
    report.index_ready_rss_bytes = CurrentRssBytes();
    report.index_ready_pss_bytes = CurrentPssBytes();
  }
  report.current_rss_bytes = report.index_ready_rss_bytes;
  report.current_pss_bytes = report.index_ready_pss_bytes;
  return report;
}

WorkerReport QueryIndex(const Options& options) {
  WorkerReport report;
  if (IsSuffixArrayMethod(options.method)) {
    auto index = sufkit::SuffixArray::Load(options.index);
    RecordIndexInfo(index.GetInfo(), &report);
    report.index_ready_rss_bytes = CurrentRssBytes();
    report.index_ready_pss_bytes = CurrentPssBytes();
    ExecuteQueries(index, &report);
    report.after_rss_bytes = CurrentRssBytes();
    report.after_pss_bytes = CurrentPssBytes();
  } else {
    auto index = sufkit::FmIndex::Load(options.index);
    (void)index.GetInfo();
    report.index_ready_rss_bytes = CurrentRssBytes();
    report.index_ready_pss_bytes = CurrentPssBytes();
    ExecuteQueries(index, &report);
    report.after_rss_bytes = CurrentRssBytes();
    report.after_pss_bytes = CurrentPssBytes();
  }
  report.current_rss_bytes = report.after_rss_bytes;
  report.current_pss_bytes = report.after_pss_bytes;
  return report;
}

int RunWorker(const Options& options) {
  ValidateMethod(options.method);
  if (options.index.empty() || options.report.empty()) {
    throw std::runtime_error("phase worker requires --index and --report");
  }
  WorkerReport report;
  if (options.worker_phase == "build") {
    if (options.reference.empty()) {
      throw std::runtime_error("build worker requires --reference");
    }
    report = BuildIndex(options);
  } else if (options.worker_phase == "load") {
    report = LoadIndex(options);
  } else if (options.worker_phase == "query") {
    report = QueryIndex(options);
  } else {
    throw std::runtime_error("unknown phase worker");
  }
  WriteWorkerReport(options.report, report);
  return 0;
}

std::filesystem::path CreateTemporaryDirectory(
    const std::filesystem::path& output) {
  const auto directory = output.parent_path();
  const auto base = "." + output.filename().string() + ".memory-bench." +
                    std::to_string(static_cast<long long>(getpid())) + "." +
                    std::to_string(static_cast<unsigned long long>(
                        std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    const auto candidate =
        directory / (base + "." + std::to_string(attempt));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
    if (error) {
      throw std::runtime_error("cannot create temporary artifact directory: " +
                               error.message());
    }
  }
  throw std::runtime_error("cannot allocate a temporary artifact directory");
}

std::uint64_t PeakRssBytes(const rusage& usage) {
  if (usage.ru_maxrss <= 0) {
    return 0;
  }
  const auto kib = static_cast<std::uint64_t>(usage.ru_maxrss);
  if (kib > std::numeric_limits<std::uint64_t>::max() / 1024U) {
    throw std::runtime_error("peak RSS byte count overflow");
  }
  return kib * 1024U;
}

PhaseResult RunPhase(const std::string& phase,
                     const Options& options,
                     const std::filesystem::path& index_path,
                     const std::filesystem::path& report_path) {
  std::vector<std::string> arguments{
      "sufkit_query_memory_bench", "--worker", phase,
      "--method", options.method, "--index", index_path.string(),
      "--report", report_path.string()};
  if (phase == "build") {
    arguments.push_back("--reference");
    arguments.push_back(options.reference.string());
  }

  const pid_t child = fork();
  if (child < 0) {
    throw std::runtime_error("cannot fork phase worker");
  }
  if (child == 0) {
    std::vector<char*> raw_arguments;
    raw_arguments.reserve(arguments.size() + 1);
    for (auto& argument : arguments) {
      raw_arguments.push_back(argument.data());
    }
    raw_arguments.push_back(nullptr);
    execv("/proc/self/exe", raw_arguments.data());
    _exit(127);
  }

  int status = 0;
  rusage usage{};
  pid_t waited = -1;
  do {
    waited = wait4(child, &status, 0, &usage);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) {
    throw std::runtime_error("cannot wait for " + phase + " phase worker");
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    throw std::runtime_error(phase + " phase worker failed");
  }
  PhaseResult result;
  result.report = ReadWorkerReport(report_path);
  result.wait4_peak_rss_bytes = PeakRssBytes(usage);
  // Linux's batched RSS accounting can leave wait4's high-water estimate
  // below an actual /proc sample for short-lived workers. Preserve both
  // measurements and report the maximum observed lifetime RSS.
  result.peak_rss_bytes = std::max({result.wait4_peak_rss_bytes,
      result.report.current_rss_bytes, result.report.index_ready_rss_bytes,
      result.report.after_rss_bytes});
  return result;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream output;
  output << std::hex << std::setw(16) << std::setfill('0') << value;
  return output.str();
}

std::string RssMib(std::uint64_t bytes) {
  if (bytes == 0) {
    return "NA";
  }
  std::ostringstream output;
  output << std::fixed << std::setprecision(6)
         << static_cast<long double>(bytes) / (1024.0L * 1024.0L);
  return output.str();
}

std::string CoordinateWidthText(std::uint64_t width) {
  return width == 0 ? "NA" : std::to_string(width);
}

std::string ProfileText(const WorkerReport& report) {
  if (report.construction_coordinate_width == 0) {
    return "NA";
  }
  return sufkit::ToString(
      static_cast<sufkit::SaResourceProfile>(report.resource_profile));
}

std::string LcpEncodingText(const WorkerReport& report) {
  if (report.construction_coordinate_width == 0) {
    return "NA";
  }
  return sufkit::ToString(
      static_cast<sufkit::SaLcpEncoding>(report.lcp_encoding));
}

void WriteRow(std::ostream& output,
              const std::string& method,
              const std::string& phase,
              const WorkerReport& dataset,
              const PhaseResult& result) {
  const char* current_scope = "after_index_save";
  const char* peak_scope = "build_worker_lifetime";
  if (phase == "load") {
    current_scope = "index_ready";
    peak_scope = "load_worker_lifetime";
  } else if (phase == "query") {
    current_scope = "after_queries";
    peak_scope = "query_worker_lifetime_including_load";
  }
  output << method << '\t' << phase << '\t'
         << Hex(dataset.dataset_fingerprint) << '\t'
         << dataset.total_bases << '\t' << dataset.contigs << '\t'
         << RssMib(result.report.current_rss_bytes) << '\t'
         << current_scope << '\t'
         << RssMib(result.report.index_ready_rss_bytes) << '\t'
         << RssMib(result.report.after_rss_bytes) << '\t'
         << RssMib(result.report.current_pss_bytes) << '\t'
         << RssMib(result.report.index_ready_pss_bytes) << '\t'
         << RssMib(result.report.after_pss_bytes) << '\t'
         << RssMib(result.peak_rss_bytes) << '\t' << peak_scope << '\t'
         << dataset.serialized_bytes << '\t'
         << CoordinateWidthText(result.report.construction_coordinate_width)
         << '\t'
         << CoordinateWidthText(result.report.stored_coordinate_width) << '\t'
         << ProfileText(result.report) << '\t'
         << LcpEncodingText(result.report) << '\t'
         << result.report.resident_core_bytes << '\t'
         << result.report.query_count << '\t'
         << result.report.total_hits << '\t'
         << result.report.reported_hits << '\t'
         << (result.report.query_count == 0 ? "NA"
                                            : Hex(result.report.checksum))
         << "\tok\t" << RssMib(result.wait4_peak_rss_bytes)
         << "\tmax-wait4-and-proc-samples\n";
}

int RunParent(const Options& original_options) {
  ValidateMethod(original_options.method);
  if (original_options.reference.empty() || original_options.output.empty()) {
    throw std::runtime_error(
        "--reference, --method, and --output are required");
  }

  Options options = original_options;
  options.output = std::filesystem::absolute(options.output);
  if (std::filesystem::exists(options.output)) {
    throw std::runtime_error("output already exists");
  }
  std::filesystem::create_directories(options.output.parent_path());
  TemporaryDirectory temporary_directory(
      CreateTemporaryDirectory(options.output));
  const auto& directory = temporary_directory.Path();
  const auto index_path = directory / "index.sufidx";
  const auto build_report_path = directory / "build.report";
  const auto load_report_path = directory / "load.report";
  const auto query_report_path = directory / "query.report";
  const auto partial_output = directory / "output.partial";

  const auto build =
      RunPhase("build", options, index_path, build_report_path);
  const auto load = RunPhase("load", options, index_path, load_report_path);
  const auto query =
      RunPhase("query", options, index_path, query_report_path);
  if (load.report.index_ready_rss_bytes == 0 ||
      load.report.index_ready_pss_bytes == 0 ||
      query.report.index_ready_rss_bytes == 0 ||
      query.report.index_ready_pss_bytes == 0 ||
      query.report.after_rss_bytes == 0 ||
      query.report.after_pss_bytes == 0 || query.report.query_count != 9) {
    throw std::runtime_error("phase worker returned incomplete RSS metrics");
  }

  std::ofstream output(partial_output, std::ios::trunc);
  if (!output) {
    throw std::runtime_error("cannot create output TSV");
  }
  output << "method\tphase\tdataset_fingerprint\ttotal_bases\tcontigs\t"
            "current_rss_mb\tcurrent_rss_scope\tindex_ready_rss_mb\t"
            "after_rss_mb\tcurrent_pss_mb\tindex_ready_pss_mb\t"
            "after_pss_mb\tpeak_rss_mb\tpeak_rss_scope\tserialized_bytes\t"
            "construction_coordinate_width\tstored_coordinate_width\t"
            "sa_profile\tlcp_encoding\tresident_core_bytes\t"
            "query_count\ttotal_hits\treported_hits\tresult_checksum\tstatus\t"
            "wait4_peak_rss_mb\tpeak_rss_source\n";
  WriteRow(output, options.method, "build", build.report, build);
  WriteRow(output, options.method, "load", build.report, load);
  WriteRow(output, options.method, "query", build.report, query);
  output.close();
  if (!output) {
    throw std::runtime_error("cannot finish output TSV");
  }
  // Publish without replacing an output created by a concurrent invocation.
  if (link(partial_output.c_str(), options.output.c_str()) != 0) {
    if (errno == EEXIST) {
      throw std::runtime_error("output already exists");
    }
    throw std::runtime_error(
        "cannot publish output TSV: " +
        std::error_code(errno, std::generic_category()).message());
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto options = ParseOptions(argc, argv);
    if (options.help) {
      PrintHelp();
      return 0;
    }
    if (!options.worker_phase.empty()) {
      return RunWorker(options);
    }
    return RunParent(options);
  } catch (const std::exception& error) {
    std::cerr << "sufkit_query_memory_bench: " << error.what() << '\n';
    return 2;
  }
}
