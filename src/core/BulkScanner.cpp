#include "BulkScanner.hpp"

#include "BulkAttr.hpp"
#include "EntryClassifier.hpp"
#include "FlatArena.hpp"
#include "VolumeInfo.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/attr.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

namespace diskprism {

namespace {

/// Apple Silicon uses 128-byte cache lines (a superset of x86's 64); per-thread data is
/// padded to that so workers never write to a line another worker reads.
constexpr size_t kCacheLine = 128;
constexpr unsigned kMaxWorkers = 64;

// Cancellation is a three-state flag rather than a bool that scan() resets: a request that
// arrives *before* the scan thread gets going must cancel that scan, not be wiped by it.
constexpr int kCancelNone = 0;
constexpr int kCancelRequested = 1;  ///< stop the running scan (or the next one to start)
constexpr int kCancelConsumed = 2;   ///< the scan it applied to has ended

using Clock = std::chrono::steady_clock;

int64_t nowNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

bool isPermissionError(int error) noexcept {
    return error == EACCES || error == EPERM;
}

/// The directory vanished or changed type between being listed and being opened — an
/// ordinary event on a live file system, not an error worth reporting.
bool isBenignRace(int error) noexcept {
    return error == ENOENT || error == ENOTDIR || error == ELOOP;
}

// ---------------------------------------------------------------------------------------
// Live counters
// ---------------------------------------------------------------------------------------

/// One block of counters per worker. Only the owning worker writes them, so an increment
/// is a plain load + store (no locked instruction, no cache-line ping-pong); the progress
/// reporter sums the blocks whenever it wants a snapshot.
struct alignas(kCacheLine) WorkerCounters {
    std::atomic<uint64_t> files{0};
    std::atomic<uint64_t> directories{0};
    std::atomic<uint64_t> allocated{0};
    std::atomic<uint64_t> logical{0};
    std::atomic<uint64_t> denied{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> hardlinkDups{0};
    std::atomic<uint64_t> dataless{0};
    std::atomic<uint64_t> mounts{0};
    std::atomic<uint64_t> syscalls{0};

    void reset() noexcept {
        for (std::atomic<uint64_t>* c :
             {&files, &directories, &allocated, &logical, &denied, &errors, &hardlinkDups, &dataless, &mounts, &syscalls}) {
            c->store(0, std::memory_order_relaxed);
        }
    }
};

inline void bump(std::atomic<uint64_t>& counter, uint64_t amount) noexcept {
    if (amount != 0) counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------------------
// Hard-link deduplication
// ---------------------------------------------------------------------------------------

/// Thread-safe set of (device, inode) pairs, split into shards so that concurrent
/// inserts for different inodes almost never contend on the same mutex.
class HardLinkSet {
public:
    /// Returns true the first time an inode is presented, false for every later link.
    bool insert(int32_t dev, uint64_t fileId) {
        const Key key{fileId, dev};
        Shard& shard = shards_[KeyHash{}(key) >> 58];  // top 6 bits pick one of 64 shards
        std::lock_guard<std::mutex> lock(shard.mutex);
        return shard.seen.insert(key).second;
    }

private:
    struct Key {
        uint64_t file_id;
        int32_t dev;
        bool operator==(const Key& other) const noexcept { return file_id == other.file_id && dev == other.dev; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            uint64_t x = k.file_id ^ (static_cast<uint64_t>(static_cast<uint32_t>(k.dev)) * 0x9E3779B97F4A7C15ull);
            x ^= x >> 33;
            x *= 0xff51afd7ed558ccdull;
            x ^= x >> 33;
            x *= 0xc4ceb9fe1a85ec53ull;
            x ^= x >> 33;
            return static_cast<size_t>(x);
        }
    };
    struct alignas(kCacheLine) Shard {
        std::mutex mutex;
        std::unordered_set<Key, KeyHash> seen;
    };
    std::array<Shard, 64> shards_;
};

// ---------------------------------------------------------------------------------------
// Work-stealing directory queues
// ---------------------------------------------------------------------------------------

/// One queue of pending directory node indices per worker.
///
/// The owner pushes and pops at the back (depth-first, so the kernel's name cache stays
/// hot and the queue stays short); an idle worker steals the *older half* from the front
/// of a victim, which are the directories closest to the root and therefore the largest
/// pieces of remaining work. Each queue has its own mutex that is almost never
/// contended. Idle workers sleep on an epoch counter (futex-style `atomic::wait`).
class DirectoryQueues {
public:
    DirectoryQueues(unsigned workers, std::atomic<uint32_t>& epoch) : queues_(workers), epoch_(epoch) {}

    void push(unsigned worker, const uint32_t* dirs, size_t count) {
        {
            Queue& q = queues_[worker];
            std::lock_guard<std::mutex> lock(q.mutex);
            q.items.insert(q.items.end(), dirs, dirs + count);
        }
        epoch_.fetch_add(1, std::memory_order_release);
        if (count == 1) {
            epoch_.notify_one();
        } else {
            epoch_.notify_all();
        }
    }

    bool pop(unsigned worker, uint32_t& out) {
        Queue& q = queues_[worker];
        std::lock_guard<std::mutex> lock(q.mutex);
        if (q.items.empty()) return false;
        out = q.items.back();
        q.items.pop_back();
        return true;
    }

    bool steal(unsigned thief, uint32_t& out) {
        const unsigned count = static_cast<unsigned>(queues_.size());
        for (unsigned k = 1; k < count; ++k) {
            Queue& victim = queues_[(thief + k) % count];
            std::vector<uint32_t> loot;
            {
                std::lock_guard<std::mutex> lock(victim.mutex);
                if (victim.items.empty()) continue;
                const size_t take = (victim.items.size() + 1) / 2;
                loot.assign(victim.items.begin(), victim.items.begin() + static_cast<std::ptrdiff_t>(take));
                victim.items.erase(victim.items.begin(), victim.items.begin() + static_cast<std::ptrdiff_t>(take));
            }
            out = loot.back();
            loot.pop_back();
            if (!loot.empty()) {
                {
                    Queue& own = queues_[thief];
                    std::lock_guard<std::mutex> lock(own.mutex);
                    own.items.insert(own.items.end(), loot.begin(), loot.end());
                }
                // The thief now holds stealable work of its own.
                epoch_.fetch_add(1, std::memory_order_release);
                epoch_.notify_all();
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] uint32_t epoch() const noexcept { return epoch_.load(std::memory_order_acquire); }
    void wait(uint32_t seen) const noexcept { epoch_.wait(seen, std::memory_order_acquire); }
    void wakeAll() noexcept {
        epoch_.fetch_add(1, std::memory_order_release);
        epoch_.notify_all();
    }

private:
    struct alignas(kCacheLine) Queue {
        std::mutex mutex;
        std::vector<uint32_t> items;
    };

    std::vector<Queue> queues_;
    std::atomic<uint32_t>& epoch_;
};

// ---------------------------------------------------------------------------------------
// Opening directories
// ---------------------------------------------------------------------------------------

/// PATH_MAX is only 1024 bytes on Darwin but deeper trees do exist. Such a path is opened
/// in pieces that each fit, hopping from one directory descriptor to the next with
/// openat(). Returns -1 with errno set on failure.
int openLongPath(const std::string& path, int finalFlags) noexcept {
    constexpr size_t kChunk = PATH_MAX - 256;
    int dirfd = -1;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t end = path.size();
        if (end - pos > kChunk) {
            end = path.rfind('/', pos + kChunk);
            if (end == std::string::npos || end <= pos) {
                if (dirfd >= 0) ::close(dirfd);
                errno = ENAMETOOLONG;
                return -1;
            }
        }
        const std::string chunk = path.substr(pos, end - pos);
        const bool last = end == path.size();
        const int flags = last ? finalFlags : (O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        int next;
        do {
            next = dirfd < 0 ? ::open(chunk.c_str(), flags) : ::openat(dirfd, chunk.c_str(), flags);
        } while (next < 0 && errno == EINTR);
        const int savedErrno = errno;
        if (dirfd >= 0) ::close(dirfd);
        if (next < 0) {
            errno = savedErrno;
            return -1;
        }
        dirfd = next;
        pos = end + 1;  // step over the separator
    }
    return dirfd;
}

int openDirectory(const std::string& path, bool isRoot) noexcept {
    // The scan root was canonicalised with realpath(); below it a symlink must never be
    // followed, even if a directory is swapped for a link while the scan is running.
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (isRoot ? 0 : O_NOFOLLOW);
    int fd;
    do {
        fd = ::open(path.c_str(), flags);
    } while (fd < 0 && errno == EINTR);
    if (fd >= 0 || errno != ENAMETOOLONG) return fd;
    return openLongPath(path, flags);
}

// ---------------------------------------------------------------------------------------
// Shared scan state and workers
// ---------------------------------------------------------------------------------------

struct Shared {
    FlatArena& arena;
    const MountGuard& guard;
    DirectoryQueues& queues;
    HardLinkSet& hardlinks;
    WorkerCounters* counters;
    const std::atomic<int>& cancelState;
    std::atomic<bool> arenaFull{false};
    std::atomic<int64_t> pending{0};  ///< directories queued or currently being read
    size_t bufferBytes = 0;

    [[nodiscard]] bool stopping() const noexcept {
        return cancelState.load(std::memory_order_relaxed) == kCancelRequested || arenaFull.load(std::memory_order_relaxed);
    }
};

class Worker {
public:
    Worker(unsigned id, Shared& shared)
        : id_(id), shared_(shared), counters_(shared.counters[id]), extCache_(shared.arena.extensions()) {
        buffer_.resize(std::max<size_t>(shared.bufferBytes, 4096));
        fillBulkAttrList(attrList_);
    }

    void run() {
        // A metadata scan must never make a file provider download anything, and it
        // should not dirty access times either.
        ::setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD,
                         IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
        ::setiopolicy_np(IOPOL_TYPE_VFS_ATIME_UPDATES, IOPOL_SCOPE_THREAD, IOPOL_ATIME_UPDATES_OFF);
        ::pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        ::pthread_setname_np("diskprism.scan");

        int idleSpins = 0;
        for (;;) {
            if (shared_.stopping()) break;
            const uint32_t epoch = shared_.queues.epoch();  // read *before* looking for work
            uint32_t dir = kInvalidNode;
            if (shared_.queues.pop(id_, dir) || shared_.queues.steal(id_, dir)) {
                idleSpins = 0;
                scanDirectory(dir);
                if (shared_.pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    shared_.queues.wakeAll();  // that was the last directory: release sleepers
                }
                continue;
            }
            if (shared_.pending.load(std::memory_order_acquire) == 0) break;
            if (++idleSpins < 32) {
                std::this_thread::yield();  // work usually reappears within microseconds
                continue;
            }
            // Sleeps only if nothing has been pushed since `epoch` was read, so a push
            // that races with the failed pop/steal above can never be missed.
            shared_.queues.wait(epoch);
            idleSpins = 0;
        }
    }

private:
    enum class BatchOutcome { Ok, ForeignDevice, Malformed };

    struct BatchTotals {
        uint64_t files = 0;
        uint64_t allocated = 0;
        uint64_t logical = 0;
        uint64_t denied = 0;
        uint64_t errors = 0;
        uint64_t hardlinkDups = 0;
        uint64_t dataless = 0;
        uint64_t mounts = 0;
    };

    void recordFailure(uint32_t dirIdx, int error) noexcept {
        if (isBenignRace(error)) return;
        FsNode& dir = shared_.arena.node(dirIdx);
        if (isPermissionError(error)) {
            dir.flags |= kNodeAccessDenied;
            bump(counters_.denied, 1);
        } else {
            dir.flags |= kNodeIOError;
            bump(counters_.errors, 1);
        }
    }

    void scanDirectory(uint32_t dirIdx) {
        FlatArena& arena = shared_.arena;
        const bool isRoot = dirIdx == 0;

        path_.clear();
        arena.appendPath(dirIdx, path_);

        // Boundary check 2 (mount table): never even open another volume's mount point.
        if (!isRoot && shared_.guard.pathIsBoundary(path_)) {
            arena.node(dirIdx).flags |= kNodeMountPoint;
            bump(counters_.mounts, 1);
            return;
        }

        const int fd = openDirectory(path_, isRoot);
        if (fd < 0) {
            recordFailure(dirIdx, errno);
            return;
        }

        uint32_t lastChild = kInvalidNode;
        uint32_t childCount = 0;
        while (!shared_.stopping()) {
            const int entryCount = ::getattrlistbulk(fd, &attrList_, buffer_.data(), buffer_.size(), bulkOptions());
            bump(counters_.syscalls, 1);
            if (entryCount < 0) {
                if (errno == EINTR) continue;
                recordFailure(dirIdx, errno);
                break;
            }
            if (entryCount == 0) break;  // end of directory

            const BatchOutcome outcome = stageBatch(entryCount);
            if (outcome == BatchOutcome::ForeignDevice) {
                // Boundary check 3: this directory's entries live on another device, i.e.
                // it is the root of a mount that the first two checks did not flag.
                arena.node(dirIdx).flags |= kNodeMountPoint;
                bump(counters_.mounts, 1);
                break;
            }
            if (!commitBatch(dirIdx, lastChild, childCount)) break;
            if (outcome == BatchOutcome::Malformed) {
                arena.node(dirIdx).flags |= kNodeIOError;
                bump(counters_.errors, 1);
                break;
            }
        }
        ::close(fd);

        arena.node(dirIdx).child_count = childCount;
        bump(counters_.directories, 1);
    }

    /// Decodes one getattrlistbulk() result buffer into thread-local staging vectors.
    BatchOutcome stageBatch(int entryCount) {
        staged_.clear();
        stagedNames_.clear();
        stagedDirs_.clear();
        batch_ = BatchTotals{};

        BulkEntryReader reader(buffer_.data(), buffer_.size(), entryCount);
        BulkEntry entry;
        while (reader.next(entry)) {
            if (entry.name.empty() || entry.name == "." || entry.name == "..") continue;

            FsNode node{};
            node.parent_idx = kInvalidNode;  // filled in when the batch is committed
            node.first_child_idx = kInvalidNode;
            node.next_sibling_idx = kInvalidNode;
            node.name_len = static_cast<uint16_t>(std::min<size_t>(entry.name.size(), UINT16_MAX));
            node.name_offset = static_cast<uint32_t>(stagedNames_.size());
            node.ext_id = ExtensionTable::kNone;

            const EntryClass cls = classifyEntry(entry, shared_.guard);
            if (cls.foreign_device) return BatchOutcome::ForeignDevice;  // boundary check 3

            node.flags = cls.flags;
            if (!cls.readable) {
                // The kernel could not read this entry's attributes: keep the name so the
                // user can see it, with no size.
                node.file_count = 1;
                node.ext_id = extCache_.idForFilename(entry.name);
                ++batch_.files;
                if (cls.permission_denied) {
                    ++batch_.denied;
                } else {
                    ++batch_.errors;
                }
            } else if ((cls.flags & kNodeDir) != 0) {
                node.mtime = entry.mtime_sec;
                if ((cls.flags & kNodeMountPoint) != 0) ++batch_.mounts;  // boundary check 1
                if (cls.descend) stagedDirs_.push_back(static_cast<uint32_t>(staged_.size()));
            } else {
                node.mtime = entry.mtime_sec;
                node.file_count = 1;
                node.ext_id = extCache_.idForFilename(entry.name);

                uint64_t allocated = cls.allocated_bytes;
                if ((cls.flags & kNodeDataless) != 0) ++batch_.dataless;
                if (cls.hardlink_candidate && !shared_.hardlinks.insert(entry.dev_id, entry.file_id)) {
                    // Another name of an inode whose blocks are already accounted for.
                    node.flags |= kNodeHardlinkDup;
                    allocated = 0;
                    ++batch_.hardlinkDups;
                }
                node.allocated_bytes = allocated;
                node.logical_bytes = cls.logical_bytes;
                batch_.allocated += allocated;
                batch_.logical += cls.logical_bytes;
                ++batch_.files;
            }

            stagedNames_.insert(stagedNames_.end(), entry.name.begin(), entry.name.begin() + node.name_len);
            staged_.push_back(node);
        }
        return reader.malformed() ? BatchOutcome::Malformed : BatchOutcome::Ok;
    }

    /// Publishes the staged batch: one bump allocation for the node indices, one for the
    /// name bytes, then plain copies into memory no other thread can see yet.
    bool commitBatch(uint32_t dirIdx, uint32_t& lastChild, uint32_t& childCount) {
        const auto count = static_cast<uint32_t>(staged_.size());
        if (count == 0) return true;
        FlatArena& arena = shared_.arena;

        const uint32_t base = arena.allocNodes(count);
        if (base == kInvalidNode) return failArenaFull();

        const uint32_t nameBase = arena.strings().allocate(static_cast<uint32_t>(stagedNames_.size()));
        if (nameBase == StringPool::kInvalidOffset) {
            // The node slots are already reserved; turn them into tombstones so later
            // passes ignore them.
            FsNode tombstone{};
            tombstone.parent_idx = dirIdx;
            tombstone.first_child_idx = kInvalidNode;
            tombstone.next_sibling_idx = kInvalidNode;
            tombstone.flags = kNodeDeleted;
            for (uint32_t i = 0; i < count; ++i) arena.node(base + i) = tombstone;
            return failArenaFull();
        }
        if (!stagedNames_.empty()) {
            std::memcpy(arena.strings().mutableData(nameBase), stagedNames_.data(), stagedNames_.size());
        }

        for (uint32_t i = 0; i < count; ++i) {
            FsNode node = staged_[i];
            node.parent_idx = dirIdx;
            node.name_offset += nameBase;
            node.next_sibling_idx = (i + 1 < count) ? base + i + 1 : kInvalidNode;
            arena.node(base + i) = node;
        }

        // Splice the batch onto the directory's child list. Only this worker touches the
        // list while the directory is being read.
        if (lastChild == kInvalidNode) {
            arena.node(dirIdx).first_child_idx = base;
        } else {
            arena.node(lastChild).next_sibling_idx = base;
        }
        lastChild = base + count - 1;
        childCount += count;

        bump(counters_.files, batch_.files);
        bump(counters_.allocated, batch_.allocated);
        bump(counters_.logical, batch_.logical);
        bump(counters_.denied, batch_.denied);
        bump(counters_.errors, batch_.errors);
        bump(counters_.hardlinkDups, batch_.hardlinkDups);
        bump(counters_.dataless, batch_.dataless);
        bump(counters_.mounts, batch_.mounts);

        // Sub-directories become visible to other workers only now, after their nodes
        // have been fully written.
        if (!stagedDirs_.empty()) {
            newDirs_.clear();
            for (const uint32_t local : stagedDirs_) newDirs_.push_back(base + local);
            shared_.pending.fetch_add(static_cast<int64_t>(newDirs_.size()), std::memory_order_acq_rel);
            shared_.queues.push(id_, newDirs_.data(), newDirs_.size());
        }
        return true;
    }

    bool failArenaFull() noexcept {
        shared_.arenaFull.store(true, std::memory_order_relaxed);
        shared_.queues.wakeAll();
        return false;
    }

    unsigned id_;
    Shared& shared_;
    WorkerCounters& counters_;
    ExtensionCache extCache_;
    struct attrlist attrList_;

    std::vector<char> buffer_;         // getattrlistbulk() result buffer (heap: >= 16-byte aligned)
    std::string path_;                 // reused path buffer
    std::vector<FsNode> staged_;       // decoded entries of the current batch
    std::vector<char> stagedNames_;    // their names, back to back
    std::vector<uint32_t> stagedDirs_; // indices into staged_ of directories to descend into
    std::vector<uint32_t> newDirs_;
    BatchTotals batch_;
};

}  // namespace

// ---------------------------------------------------------------------------------------
// BulkScanner
// ---------------------------------------------------------------------------------------

struct BulkScanner::Impl {
    std::array<WorkerCounters, kMaxWorkers> counters;
    std::atomic<int> cancelState{kCancelNone};
    std::atomic<uint32_t> wakeEpoch{0};
    std::atomic<int64_t> startNanos{0};  ///< 0: no scan has started
    std::atomic<int64_t> endNanos{0};    ///< 0: scan still running
    std::atomic<bool> finished{false};
};

BulkScanner::BulkScanner() : impl_(std::make_unique<Impl>()) {}
BulkScanner::~BulkScanner() = default;

void BulkScanner::cancel() noexcept {
    impl_->cancelState.store(kCancelRequested, std::memory_order_relaxed);
    impl_->wakeEpoch.fetch_add(1, std::memory_order_release);
    impl_->wakeEpoch.notify_all();
}

bool BulkScanner::cancelRequested() const noexcept {
    return impl_->cancelState.load(std::memory_order_relaxed) != kCancelNone;
}

ScanProgress BulkScanner::progress() const noexcept {
    ScanProgress p;
    for (const WorkerCounters& c : impl_->counters) {
        p.files += c.files.load(std::memory_order_relaxed);
        p.directories += c.directories.load(std::memory_order_relaxed);
        p.allocated_bytes += c.allocated.load(std::memory_order_relaxed);
        p.logical_bytes += c.logical.load(std::memory_order_relaxed);
        p.denied += c.denied.load(std::memory_order_relaxed);
        p.errors += c.errors.load(std::memory_order_relaxed);
        p.hardlink_duplicates += c.hardlinkDups.load(std::memory_order_relaxed);
        p.dataless_files += c.dataless.load(std::memory_order_relaxed);
        p.mount_points_skipped += c.mounts.load(std::memory_order_relaxed);
        p.syscalls += c.syscalls.load(std::memory_order_relaxed);
    }
    const int64_t start = impl_->startNanos.load(std::memory_order_acquire);
    if (start != 0) {
        const int64_t end = impl_->endNanos.load(std::memory_order_acquire);
        const int64_t until = end != 0 ? end : nowNanos();
        p.elapsed_seconds = static_cast<double>(std::max<int64_t>(until - start, 0)) * 1e-9;
    }
    if (p.elapsed_seconds > 0) {
        p.files_per_second = static_cast<double>(p.files) / p.elapsed_seconds;
        p.items_per_second = static_cast<double>(p.files + p.directories) / p.elapsed_seconds;
    }
    p.finished = impl_->finished.load(std::memory_order_acquire);
    return p;
}

ScanResult BulkScanner::scan(const std::string& rootPath, FlatArena& arena, const ScanOptions& options,
                             const ProgressCallback& onProgress) {
    Impl& impl = *impl_;
    ScanResult result;

    int consumed = kCancelConsumed;
    impl.cancelState.compare_exchange_strong(consumed, kCancelNone, std::memory_order_relaxed);
    impl.finished.store(false, std::memory_order_release);
    impl.startNanos.store(0, std::memory_order_release);
    impl.endNanos.store(0, std::memory_order_release);
    for (WorkerCounters& c : impl.counters) c.reset();

    if (arena.size() != 0) {
        result.status = ScanStatus::InvalidRoot;
        result.error_message = "the arena already contains a tree";
        return result;
    }

    // Resolve symlinks in the root exactly once; nothing below it is ever followed.
    char* resolved = ::realpath(rootPath.c_str(), nullptr);
    if (resolved == nullptr) {
        result.status = ScanStatus::InvalidRoot;
        result.error_message = rootPath + ": " + std::strerror(errno);
        return result;
    }
    result.root_path = resolved;
    std::free(resolved);

    struct stat rootStat;
    if (::stat(result.root_path.c_str(), &rootStat) != 0) {
        result.status = ScanStatus::InvalidRoot;
        result.error_message = result.root_path + ": " + std::strerror(errno);
        return result;
    }
    if (!S_ISDIR(rootStat.st_mode)) {
        result.status = ScanStatus::InvalidRoot;
        result.error_message = result.root_path + ": not a directory";
        return result;
    }
    result.root_dev = rootStat.st_dev;

    const MountGuard guard = MountGuard::forSystem(result.root_path, rootStat.st_dev, options.cross_mount_points);

    if (arena.addRoot(result.root_path, static_cast<int64_t>(rootStat.st_mtimespec.tv_sec)) == kInvalidNode) {
        result.status = ScanStatus::ArenaFull;
        return result;
    }

    unsigned threadCount = options.thread_count != 0 ? options.thread_count : std::thread::hardware_concurrency();
    threadCount = std::clamp<unsigned>(threadCount, 1, kMaxWorkers);
    result.threads = threadCount;

    DirectoryQueues queues(threadCount, impl.wakeEpoch);
    HardLinkSet hardlinks;
    Shared shared{arena, guard, queues, hardlinks, impl.counters.data(), impl.cancelState};
    shared.bufferBytes = options.buffer_bytes;

    const uint32_t rootIdx = 0;
    shared.pending.store(1, std::memory_order_release);
    queues.push(0, &rootIdx, 1);

    const int64_t start = nowNanos();
    impl.startNanos.store(start, std::memory_order_release);

    std::mutex doneMutex;
    std::condition_variable doneCv;
    std::atomic<unsigned> active{threadCount};
    const auto workerMain = [&shared, &active, &doneMutex, &doneCv](unsigned id) {
        {
            Worker worker(id, shared);
            worker.run();
        }
        if (active.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(doneMutex);
            doneCv.notify_all();
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        try {
            threads.emplace_back(workerMain, i);
        } catch (const std::system_error&) {
            break;  // out of threads: carry on with the ones that did start
        }
    }
    const auto started = static_cast<unsigned>(threads.size());
    if (started < threadCount) {
        active.fetch_sub(threadCount - started, std::memory_order_acq_rel);
        if (started == 0) {
            active.store(1, std::memory_order_release);
            workerMain(0);  // last resort: scan on the calling thread
        }
    }

    {
        const auto interval = std::chrono::duration<double>(std::max(options.progress_interval_seconds, 0.005));
        std::unique_lock<std::mutex> lock(doneMutex);
        while (active.load(std::memory_order_acquire) != 0) {
            doneCv.wait_for(lock, interval);
            if (onProgress && active.load(std::memory_order_acquire) != 0) {
                lock.unlock();
                onProgress(progress());
                lock.lock();
            }
        }
    }
    for (std::thread& t : threads) t.join();

    const int64_t scanEnd = nowNanos();
    result.scan_seconds = static_cast<double>(scanEnd - start) * 1e-9;

    // A cancelled or truncated scan still leaves a consistent (partial) tree behind.
    arena.finalize(options.sort_metric);

    const int64_t finalizeEnd = nowNanos();
    result.finalize_seconds = static_cast<double>(finalizeEnd - scanEnd) * 1e-9;
    impl.endNanos.store(scanEnd, std::memory_order_release);
    impl.finished.store(true, std::memory_order_release);

    result.totals = progress();
    if (shared.arenaFull.load(std::memory_order_relaxed)) {
        result.status = ScanStatus::ArenaFull;
    } else if (int requested = kCancelRequested;
               impl.cancelState.compare_exchange_strong(requested, kCancelConsumed, std::memory_order_relaxed)) {
        result.status = ScanStatus::Cancelled;
    } else {
        result.status = ScanStatus::Completed;
    }
    if (onProgress) onProgress(result.totals);
    return result;
}

}  // namespace diskprism
