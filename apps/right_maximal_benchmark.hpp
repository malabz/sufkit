#pragma once

#include <string>
#include <vector>

namespace sufkit::app::right_maximal_bench {

int run(const std::vector<std::string>& arguments);

// Internal clean-exec entry point for measured maximal-match phases.
int run_worker(const std::vector<std::string>& arguments);

} // namespace sufkit::app::right_maximal_bench
