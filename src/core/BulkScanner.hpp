// DiskPrism core — parallel directory scanner built on getattrlistbulk(2).
#pragma once

#include "CoreTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace diskprism {

class FlatArena;

struct ScanOptions {
    /// Worker threads; 0 means `std::thread::hardware_concurrency()`.
    unsigned thread_count = 0;
    /// Descend into other mounted volumes instead of stopping at mount points.
    bool cross_mount_points = false;
    /// Size of each worker's getattrlistbulk() result buffer.
    size_t buffer_bytes = 128 * 1024;
    /// Order of the child lists once the scan has been finalized.
    SizeMetric sort_metric = SizeMetric::Physical;
    /// Interval between progress callbacks (default ≈ 18 Hz).
    double progress_interval_seconds = 1.0 / 18.0;
};

/// Live counters; cheap to snapshot at any time from any thread.
struct ScanProgress {
    uint64_t files = 0;                 ///< non-directory entries seen
    uint64_t directories = 0;           ///< directories enumerated
    uint64_t allocated_bytes = 0;       ///< physical bytes (hard links and cloud files deduplicated)
    uint64_t logical_bytes = 0;         ///< apparent bytes
    uint64_t denied = 0;                ///< folders skipped because of EACCES / EPERM (permissions, TCC)
    uint64_t errors = 0;                ///< other per-entry or per-directory failures
    uint64_t hardlink_duplicates = 0;   ///< extra names of already counted inodes
    uint64_t dataless_files = 0;        ///< cloud-only files (SF_DATALESS)
    uint64_t mount_points_skipped = 0;  ///< volume boundaries that were not crossed
    uint64_t syscalls = 0;              ///< getattrlistbulk() invocations
    double elapsed_seconds = 0;
    double files_per_second = 0;        ///< files / elapsed
    double items_per_second = 0;        ///< (files + directories) / elapsed
    bool finished = false;

    [[nodiscard]] uint64_t bytes(SizeMetric metric) const noexcept {
        return metric == SizeMetric::Physical ? allocated_bytes : logical_bytes;
    }
};

enum class ScanStatus {
    Completed,
    Cancelled,    ///< cancel() was called; the arena holds the partial tree
    InvalidRoot,  ///< the root does not exist or is not a readable directory
    ArenaFull,    ///< the arena's capacity was reached; the tree is truncated
};

struct ScanResult {
    ScanStatus status = ScanStatus::Completed;
    ScanProgress totals;
    std::string root_path;       ///< canonical (symlink-free) path that was scanned
    std::string error_message;   ///< set for InvalidRoot
    int32_t root_dev = 0;        ///< unified device ID of the scan root
    unsigned threads = 0;
    double scan_seconds = 0;     ///< enumeration (wall clock)
    double finalize_seconds = 0; ///< aggregation + sorting + histogram

    [[nodiscard]] bool ok() const noexcept {
        return status == ScanStatus::Completed || status == ScanStatus::Cancelled || status == ScanStatus::ArenaFull;
    }
};

using ProgressCallback = std::function<void(const ScanProgress&)>;

/// Scans a directory tree into a `FlatArena`.
///
/// A pool of worker threads drains per-worker directory queues (LIFO for the owner,
/// oldest-half stealing for idle workers). Each worker opens a directory with
/// `open(O_RDONLY | O_DIRECTORY | O_CLOEXEC)`, reads all entries in batches with
/// getattrlistbulk() into its own 128 KiB buffer, decodes them into thread-local staging
/// vectors and then publishes the batch into the arena with two lock-free bump
/// allocations (node indices and name bytes) — workers never share a lock on the hot
/// path.
///
/// Accuracy rules enforced while scanning:
///   * symbolic links are recorded but never followed;
///   * mount points are never crossed (see `MountGuard`), which is also what prevents the
///     APFS data volume from being counted twice when `/` is scanned;
///   * SF_DATALESS (cloud-only) items contribute their logical size but zero physical
///     bytes, and dataless directories are not entered (that would trigger a download);
///   * a regular file with several hard links contributes its physical size only once;
///   * EACCES / EPERM are recorded on the node and counted, never fatal.
class BulkScanner {
public:
    BulkScanner();
    ~BulkScanner();

    BulkScanner(const BulkScanner&) = delete;
    BulkScanner& operator=(const BulkScanner&) = delete;

    /// Scans `rootPath` into `arena` (which must be empty) and finalizes the arena.
    /// Blocks until the scan is complete or cancelled. `onProgress` is invoked on the
    /// calling thread at `options.progress_interval_seconds` and once more at the end.
    ScanResult scan(const std::string& rootPath, FlatArena& arena, const ScanOptions& options = ScanOptions{},
                    const ProgressCallback& onProgress = ProgressCallback{});

    /// Requests cooperative cancellation. Safe to call from any thread at any time; a
    /// request made before `scan()` has started applies to that scan.
    void cancel() noexcept;
    [[nodiscard]] bool cancelRequested() const noexcept;

    /// Snapshot of the counters of the scan in progress (or of the last scan).
    [[nodiscard]] ScanProgress progress() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace diskprism
