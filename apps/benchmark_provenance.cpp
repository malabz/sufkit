// SPDX-License-Identifier: MIT

#include "benchmark_provenance.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <sched.h>
#include <unistd.h>

#ifndef SUFKIT_BENCH_GIT_COMMIT
#define SUFKIT_BENCH_GIT_COMMIT "unknown"
#endif
#ifndef SUFKIT_BENCH_GIT_DIRTY
#define SUFKIT_BENCH_GIT_DIRTY "unknown"
#endif
#ifndef SUFKIT_BENCH_COMPILE_FLAGS
#define SUFKIT_BENCH_COMPILE_FLAGS "unknown"
#endif
#ifndef SUFKIT_BENCH_SSE42_COMPILED
#define SUFKIT_BENCH_SSE42_COMPILED 0
#endif

namespace sufkit::app::bench {
namespace {

std::string SanitizeField(std::string value) {
  std::replace(value.begin(), value.end(), '\t', ' ');
  std::replace(value.begin(), value.end(), '\r', ' ');
  std::replace(value.begin(), value.end(), '\n', ' ');
  return value.empty() ? "unknown" : value;
}

std::vector<std::string> SplitCompilerFlags(std::string_view flags) {
  std::vector<std::string> tokens;
  std::string token;
  char quote = '\0';
  for (const char character : flags) {
    if (quote != '\0') {
      if (character == quote) {
        quote = '\0';
      } else {
        token.push_back(character);
      }
      continue;
    }
    if (character == '\'' || character == '"') {
      quote = character;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(character)) != 0) {
      if (!token.empty()) {
        tokens.push_back(std::move(token));
        token.clear();
      }
      continue;
    }
    token.push_back(character);
  }
  if (!token.empty()) {
    tokens.push_back(std::move(token));
  }
  return tokens;
}

bool ContainsPathComponent(std::string_view token) {
  // This intentionally also hides relative paths and URL-like values. The
  // metadata remains useful for optimization switches while never exposing a
  // local directory through a quoted include or definition value.
  return token.find('/') != std::string_view::npos ||
         token.find('\\') != std::string_view::npos;
}

std::string CpuFlags() {
  std::ifstream input("/proc/cpuinfo");
  std::string line;
  while (std::getline(input, line)) {
    if (line.rfind("flags", 0) != 0) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    auto flags = line.substr(colon + 1);
    const auto first = flags.find_first_not_of(' ');
    return first == std::string::npos ? "unknown" : flags.substr(first);
  }
  return "unknown";
}

std::string CpuAffinity() {
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
    return "unknown";
  }
  std::ostringstream output;
  bool first_range = true;
  int cpu = 0;
  while (cpu < CPU_SETSIZE) {
    while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &affinity)) {
      ++cpu;
    }
    if (cpu == CPU_SETSIZE) {
      break;
    }
    const int begin = cpu;
    while (cpu + 1 < CPU_SETSIZE && CPU_ISSET(cpu + 1, &affinity)) {
      ++cpu;
    }
    if (!first_range) {
      output << ',';
    }
    first_range = false;
    output << begin;
    if (cpu != begin) {
      output << '-' << cpu;
    }
    ++cpu;
  }
  return first_range ? "unknown" : output.str();
}

std::string ExecutableSha256() {
  std::array<char, 256> buffer{};
  std::string output;
  const auto command = "/usr/bin/sha256sum /proc/" +
                       std::to_string(static_cast<long long>(getpid())) +
                       "/exe";
  FILE* process = popen(command.c_str(), "r");
  if (process == nullptr) {
    return "unknown";
  }
  while (fgets(buffer.data(), static_cast<int>(buffer.size()), process) !=
         nullptr) {
    output.append(buffer.data());
  }
  const auto status = pclose(process);
  std::istringstream parsed(output);
  std::string hash;
  parsed >> hash;
  return status == 0 && hash.size() == 64 ? hash : "unknown";
}

bool RuntimeSupportsSse42() {
#if (defined(__GNUC__) || defined(__clang__)) && \
    (defined(__x86_64__) || defined(__i386__))
  __builtin_cpu_init();
  return __builtin_cpu_supports("sse4.2") != 0 &&
         __builtin_cpu_supports("popcnt") != 0;
#else
  return false;
#endif
}

std::string QuoteArgument(std::string_view value) {
  if (value.find_first_of(" \t'\"") == std::string_view::npos) {
    return std::string(value);
  }
  std::string result{"'"};
  for (const char character : value) {
    if (character == '\'') {
      result.append("'\\''");
    } else {
      result.push_back(character);
    }
  }
  result.push_back('\'');
  return result;
}

std::string RedactedCommandLine(const std::vector<std::string>& arguments) {
  static const std::set<std::string> path_options{
      "--reference", "--queries", "--output", "--output-dir", "--mummer4",
      "--mummer4-runtime", "--minibwa"};
  std::ostringstream output;
  output << "sufkit bench";
  bool redact_next = false;
  for (const auto& argument : arguments) {
    output << ' ';
    if (redact_next) {
      output << "<path>";
      redact_next = false;
      continue;
    }
    output << QuoteArgument(argument);
    redact_next = path_options.count(argument) != 0;
  }
  return output.str();
}

}  // namespace

std::string RedactCompilerFlagPaths(std::string_view flags) {
  std::ostringstream output;
  bool first = true;
  for (const auto& token : SplitCompilerFlags(flags)) {
    if (!first) {
      output << ' ';
    }
    first = false;
    output << (ContainsPathComponent(token) ? "<path-flag>" : token);
  }
  return output.str();
}

BenchmarkProvenance CollectBenchmarkProvenance(
    const std::vector<std::string>& arguments) {
  BenchmarkProvenance result;
  result.git_commit = SanitizeField(SUFKIT_BENCH_GIT_COMMIT);
  result.git_dirty = SanitizeField(SUFKIT_BENCH_GIT_DIRTY);
  result.compile_flags = SanitizeField(
      RedactCompilerFlagPaths(SUFKIT_BENCH_COMPILE_FLAGS));
  result.cpu_flags = SanitizeField(CpuFlags());
  result.executable_sha256 = ExecutableSha256();
  result.cpu_affinity = SanitizeField(CpuAffinity());
  result.sse42_compiled = SUFKIT_BENCH_SSE42_COMPILED ? "1" : "0";
  result.sse42_runtime = RuntimeSupportsSse42() ? "1" : "0";
  result.command_line_redacted = SanitizeField(RedactedCommandLine(arguments));
  return result;
}

}  // namespace sufkit::app::bench
