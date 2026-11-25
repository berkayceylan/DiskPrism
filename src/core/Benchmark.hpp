// DiskPrism core — headless scan benchmark (shared by `DiskPrism --benchmark` and the
// stand-alone `diskprism-bench` tool).
#pragma once

#include "BulkScanner.hpp"
#include "CoreTypes.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace diskprism {

struct BenchmarkOptions {
    std::string path;
    int runs = 3;           ///< the first run is usually "cold", later ones hit the kernel's caches
    unsigned threads = 0;   ///< 0: one worker per hardware thread
    SizeMetric metric = SizeMetric::Physical;
};

struct BenchmarkRun {
    ScanResult result;
    uint32_t nodes = 0;            ///< arena nodes (files + directories)
    uint64_t node_bytes = 0;       ///< nodes * sizeof(FsNode)
    uint64_t name_bytes = 0;       ///< string pool
    uint64_t root_allocated = 0;
    uint64_t root_logical = 0;
    double peak_rss_mb = 0;        ///< getrusage ru_maxrss, process lifetime peak so far
    double peak_footprint_mb = 0;  ///< Mach ledger peak physical footprint (what Activity Monitor calls "Memory")
};

/// Peak resident set size of this process in MiB.
[[nodiscard]] double peakResidentMegabytes() noexcept;

/// Peak physical footprint of this process in MiB (0 if the kernel does not report it).
[[nodiscard]] double peakFootprintMegabytes() noexcept;

/// Scans `options.path` `options.runs` times into fresh arenas and prints a report.
/// Returns a process exit code (0 on success).
int runBenchmark(const BenchmarkOptions& options, std::FILE* out, std::vector<BenchmarkRun>* runs = nullptr);

}  // namespace diskprism
