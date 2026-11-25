// diskprism-bench — stand-alone headless scan benchmark.
// Links only the pure C++ core (no AppKit, no Metal), which also makes it the smallest
// binary to run under sanitizers or a profiler.
//
//   diskprism-bench <path> [--runs N] [--threads N] [--logical]
#include "Benchmark.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv) {
    diskprism::BenchmarkOptions options;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--runs") == 0 && i + 1 < argc) {
            options.runs = std::atoi(argv[++i]);
        } else if (std::strcmp(arg, "--threads") == 0 && i + 1 < argc) {
            options.threads = static_cast<unsigned>(std::max(0, std::atoi(argv[++i])));
        } else if (std::strcmp(arg, "--logical") == 0) {
            options.metric = diskprism::SizeMetric::Logical;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            std::printf("usage: diskprism-bench <path> [--runs N] [--threads N] [--logical]\n");
            return 0;
        } else if (arg[0] != '-' && options.path.empty()) {
            options.path = arg;
        } else {
            std::fprintf(stderr, "diskprism-bench: unexpected argument '%s'\n", arg);
            return 2;
        }
    }
    if (options.path.empty()) {
        std::fprintf(stderr, "usage: diskprism-bench <path> [--runs N] [--threads N] [--logical]\n");
        return 2;
    }
    return diskprism::runBenchmark(options, stdout);
}
