#include "Benchmark.hpp"

#include "FlatArena.hpp"

#include <mach/mach.h>
#include <sys/resource.h>

#include <algorithm>
#include <cinttypes>
#include <memory>
#include <thread>

namespace diskprism {

namespace {

constexpr double kMiB = 1024.0 * 1024.0;

/// "12.34 GB" in decimal units, like Finder.
std::string formatBytes(uint64_t bytes) {
    static const char* const kUnits[] = {"bytes", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 5) {
        value /= 1000.0;
        ++unit;
    }
    char buffer[64];
    if (unit == 0) {
        std::snprintf(buffer, sizeof buffer, "%" PRIu64 " bytes", bytes);
    } else {
        std::snprintf(buffer, sizeof buffer, "%.2f %s", value, kUnits[unit]);
    }
    return buffer;
}

const char* statusName(ScanStatus status) noexcept {
    switch (status) {
        case ScanStatus::Completed:
            return "completed";
        case ScanStatus::Cancelled:
            return "cancelled";
        case ScanStatus::InvalidRoot:
            return "invalid root";
        case ScanStatus::ArenaFull:
            return "arena full (truncated)";
    }
    return "unknown";
}

}  // namespace

double peakResidentMegabytes() noexcept {
    struct rusage usage;
    if (::getrusage(RUSAGE_SELF, &usage) != 0) return 0.0;
    return static_cast<double>(usage.ru_maxrss) / kMiB;  // bytes on Darwin
}

double peakFootprintMegabytes() noexcept {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (::task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return 0.0;
#if defined(TASK_VM_INFO_REV5_COUNT)
    if (count >= TASK_VM_INFO_REV5_COUNT) return static_cast<double>(info.ledger_phys_footprint_peak) / kMiB;
#endif
    return static_cast<double>(info.phys_footprint) / kMiB;
}

int runBenchmark(const BenchmarkOptions& options, std::FILE* out, std::vector<BenchmarkRun>* runsOut) {
    if (options.path.empty()) {
        std::fprintf(stderr, "benchmark: no path given\n");
        return 2;
    }
    const int runCount = std::clamp(options.runs, 1, 100);
    const unsigned hardware = std::thread::hardware_concurrency();

    std::fprintf(out, "DiskPrism scan benchmark\n");
    std::fprintf(out, "  target            %s\n", options.path.c_str());
    std::fprintf(out, "  hardware threads  %u\n", hardware);
    std::fprintf(out, "  node size         %zu bytes\n", sizeof(FsNode));
    std::fprintf(out, "  runs              %d\n\n", runCount);

    std::vector<BenchmarkRun> runs;
    for (int i = 0; i < runCount; ++i) {
        BenchmarkRun run;
        {
            // A fresh arena per run: address space is reserved, pages are committed as nodes arrive.
            auto arena = std::make_unique<FlatArena>();
            BulkScanner scanner;
            ScanOptions scanOptions;
            scanOptions.thread_count = options.threads;
            scanOptions.sort_metric = options.metric;
            run.result = scanner.scan(options.path, *arena, scanOptions);
            if (run.result.status == ScanStatus::InvalidRoot) {
                std::fprintf(stderr, "benchmark: %s\n", run.result.error_message.c_str());
                return 1;
            }
            run.nodes = arena->liveNodeCount();
            run.node_bytes = static_cast<uint64_t>(arena->size()) * sizeof(FsNode);
            run.name_bytes = arena->strings().size();
            run.root_allocated = arena->node(arena->root()).allocated_bytes;
            run.root_logical = arena->node(arena->root()).logical_bytes;
            // Measured while the tree is still alive: this is the footprint of holding it.
            run.peak_rss_mb = peakResidentMegabytes();
            run.peak_footprint_mb = peakFootprintMegabytes();
        }

        const ScanProgress& t = run.result.totals;
        const double scanMs = run.result.scan_seconds * 1e3;
        const double finalizeMs = run.result.finalize_seconds * 1e3;
        std::fprintf(out, "run %d (%s, %u threads)\n", i + 1, statusName(run.result.status), run.result.threads);
        std::fprintf(out, "  files             %" PRIu64 "\n", t.files);
        std::fprintf(out, "  directories       %" PRIu64 "\n", t.directories);
        std::fprintf(out, "  elapsed           %.1f ms scan + %.1f ms aggregate/sort = %.1f ms\n", scanMs, finalizeMs, scanMs + finalizeMs);
        std::fprintf(out, "  throughput        %.0f files/sec (%.0f entries/sec)\n", t.files_per_second, t.items_per_second);
        std::fprintf(out, "  physical size     %s\n", formatBytes(run.root_allocated).c_str());
        std::fprintf(out, "  logical size      %s\n", formatBytes(run.root_logical).c_str());
        std::fprintf(out, "  getattrlistbulk   %" PRIu64 " calls (%.1f entries per call)\n", t.syscalls,
                     t.syscalls > 0 ? static_cast<double>(t.files + t.directories) / static_cast<double>(t.syscalls) : 0.0);
        std::fprintf(out, "  skipped           %" PRIu64 " denied, %" PRIu64 " mount points, %" PRIu64 " errors\n", t.denied,
                     t.mount_points_skipped, t.errors);
        std::fprintf(out, "  deduplicated      %" PRIu64 " hard-link names, %" PRIu64 " cloud-only files\n", t.hardlink_duplicates,
                     t.dataless_files);
        std::fprintf(out, "  arena             %u nodes: %.1f MB nodes + %.1f MB names = %.1f bytes/node\n", run.nodes,
                     static_cast<double>(run.node_bytes) / kMiB, static_cast<double>(run.name_bytes) / kMiB,
                     run.nodes > 0 ? static_cast<double>(run.node_bytes + run.name_bytes) / run.nodes : 0.0);
        std::fprintf(out, "  peak RSS          %.1f MB (peak footprint %.1f MB)\n\n", run.peak_rss_mb, run.peak_footprint_mb);
        std::fflush(out);
        runs.push_back(std::move(run));
    }

    // Summary: the fastest run is the steady-state (warm cache) throughput.
    const auto best = std::min_element(runs.begin(), runs.end(), [](const BenchmarkRun& a, const BenchmarkRun& b) {
        return a.result.scan_seconds < b.result.scan_seconds;
    });
    const BenchmarkRun& last = runs.back();
    std::fprintf(out, "summary\n");
    std::fprintf(out, "  best run          %.1f ms, %.0f files/sec\n", best->result.scan_seconds * 1e3,
                 best->result.totals.files_per_second);
    std::fprintf(out, "  first run         %.1f ms, %.0f files/sec\n", runs.front().result.scan_seconds * 1e3,
                 runs.front().result.totals.files_per_second);
    std::fprintf(out, "  peak RSS          %.1f MB\n", last.peak_rss_mb);
    // One machine-readable line for scripts and tests.
    std::fprintf(out,
                 "BENCHMARK files=%" PRIu64 " dirs=%" PRIu64 " best_ms=%.1f best_files_per_sec=%.0f peak_rss_mb=%.1f"
                 " peak_footprint_mb=%.1f nodes=%u\n",
                 best->result.totals.files, best->result.totals.directories, best->result.scan_seconds * 1e3,
                 best->result.totals.files_per_second, last.peak_rss_mb, last.peak_footprint_mb, best->nodes);
    std::fflush(out);
    if (runsOut != nullptr) *runsOut = std::move(runs);
    return 0;
}

}  // namespace diskprism
