#include "benchmark.hpp"

#include <sufkit/types.hpp>

namespace sufkit::app {

int run_benchmark(const std::vector<std::string>&) {
    throw Error(
        ErrorCode::kUnsupportedBackend,
        "benchmark support was disabled at configure time");
}

} // namespace sufkit::app

