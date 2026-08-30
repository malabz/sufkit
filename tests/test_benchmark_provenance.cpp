// SPDX-License-Identifier: MIT

#include "benchmark_provenance.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

#define CHECK(condition)                                                    \
  do {                                                                      \
    if (!(condition)) {                                                     \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__       \
                << ": " #condition << '\n';                               \
      std::exit(1);                                                         \
    }                                                                       \
  } while (false)

void TestQuotedAndSeparatedPathsAreRedacted() {
  const std::string flags =
      "-O3 -I\"/mnt/d/private path/include\" "
      "-isystem \"/opt/sdk include\" -DKEEP=1";
  const auto redacted =
      sufkit::app::bench::RedactCompilerFlagPaths(flags);

  CHECK(redacted ==
        "-O3 <path-flag> -isystem <path-flag> -DKEEP=1");
  CHECK(redacted.find("/mnt/d") == std::string::npos);
  CHECK(redacted.find("/opt/sdk") == std::string::npos);
}

void TestWindowsAndDefinitionPathsAreRedacted() {
  const std::string flags =
      "global=-O2 -I\"C:\\Users\\person\\private include\" "
      "-DROOT=/home/person -Wshadow";
  const auto redacted =
      sufkit::app::bench::RedactCompilerFlagPaths(flags);

  CHECK(redacted ==
        "global=-O2 <path-flag> <path-flag> -Wshadow");
  CHECK(redacted.find("person") == std::string::npos);
}

void TestNonPathFlagsRemainReadable() {
  const auto redacted = sufkit::app::bench::RedactCompilerFlagPaths(
      "global=-O3 config=-DNDEBUG cli-private=-Wall -Wextra");
  CHECK(redacted ==
        "global=-O3 config=-DNDEBUG cli-private=-Wall -Wextra");
}

}  // namespace

int main() {
  TestQuotedAndSeparatedPathsAreRedacted();
  TestWindowsAndDefinitionPathsAreRedacted();
  TestNonPathFlagsRemainReadable();
  return 0;
}
