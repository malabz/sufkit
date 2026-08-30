// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sufkit::app::bench {

struct BenchmarkProvenance {
  std::string git_commit;
  std::string git_dirty;
  std::string compile_flags;
  std::string cpu_flags;
  std::string executable_sha256;
  std::string cpu_affinity;
  std::string sse42_compiled;
  std::string sse42_runtime;
  std::string command_line_redacted;
  std::string peak_rss_scope = "method_process_lifetime";
};

BenchmarkProvenance CollectBenchmarkProvenance(
    const std::vector<std::string>& arguments);

// Conservatively removes path-bearing compiler-flag tokens before they are
// written to a shareable benchmark evidence file.
std::string RedactCompilerFlagPaths(std::string_view flags);

}  // namespace sufkit::app::bench
