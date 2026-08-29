#include "benchmark.hpp"

#include <sufkit/types.hpp>

namespace sufkit::app {

int run_benchmark(const std::vector<std::string>&) {
    throw Error(
        ErrorCode::kUnsupportedBackend,
        "benchmark support was disabled at configure time");
}

int run_benchmark_worker(const std::vector<std::string>&) {
    throw Error(
        ErrorCode::kUnsupportedBackend,
        "benchmark support was disabled at configure time");
}

int run_maximal_benchmark_worker(const std::vector<std::string>&) {
    throw Error(
        ErrorCode::kUnsupportedBackend,
        "benchmark support was disabled at configure time");
}

} // namespace sufkit::app
