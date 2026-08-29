#pragma once

#include <string>
#include <vector>

namespace sufkit::app {

int run_benchmark(const std::vector<std::string>& arguments);

// Internal clean-exec entry point used by benchmark phase workers. This is
// deliberately not shown in the public CLI help.
int run_benchmark_worker(const std::vector<std::string>& arguments);

// Internal clean-exec entry point used by maximal-match phase workers.
int run_maximal_benchmark_worker(const std::vector<std::string>& arguments);

} // namespace sufkit::app
