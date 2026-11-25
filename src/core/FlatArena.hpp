// DiskPrism core — index-based flat arena holding the whole scanned file tree.
#pragma once

#include "CoreTypes.hpp"
#include "ExtensionTable.hpp"
#include "StringPool.hpp"
#include "VirtualRegion.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace diskprism {

/// Bit flags packed into `FsNode::flags`.
enum NodeFlags : uint32_t {
    kNodeDir = 1u << 0,           ///< directory
    kNodeDataless = 1u << 1,      ///< SF_DATALESS: iCloud / File Provider item not stored locally
    kNodeSymlink = 1u << 2,       ///< symbolic link (never followed)
    kNodeHardlinkDup = 1u << 3,   ///< extra name of an inode whose blocks are counted elsewhere
    kNodeAccessDenied = 1u << 4,  ///< EACCES / EPERM (Unix permissions or TCC)
    kNodeDeleted = 1u << 5,       ///< tombstone left by `removeNode()`
    kNodeMountPoint = 1u << 6,    ///< volume boundary that was deliberately not crossed
    kNodeHardlink = 1u << 7,      ///< regular file with link count > 1
    kNodeIOError = 1u << 8,       ///< any other error while reading the entry
    kNodeSpecial = 1u << 9,       ///< device node, socket, FIFO…
    kNodeFirmlink = 1u << 10,     ///< APFS firmlink (system → data volume); traversed once
};

/// One file-system object. Plain data, 56 bytes, no pointers: the tree is expressed with
/// 32-bit indices so millions of nodes stay cache-friendly and trivially relocatable.
struct FsNode {
    uint64_t allocated_bytes;   ///< physical bytes; for directories the subtree total
    uint64_t logical_bytes;     ///< apparent bytes; for directories the subtree total
    int64_t mtime;              ///< modification time, seconds since the Unix epoch
    uint32_t parent_idx;        ///< kInvalidNode for the root
    uint32_t first_child_idx;   ///< head of the child list (kInvalidNode if none)
    uint32_t next_sibling_idx;  ///< next entry in the parent's child list
    uint32_t name_offset;       ///< offset of the UTF-8 name in the string pool
    uint32_t file_count;        ///< files in the subtree (1 for a file itself)
    uint32_t child_count;       ///< direct live children
    uint32_t flags;             ///< NodeFlags
    uint16_t name_len;          ///< name length in bytes
    uint16_t ext_id;            ///< ExtensionTable ID

    [[nodiscard]] bool is_dir() const noexcept { return (flags & kNodeDir) != 0; }
    [[nodiscard]] bool is_dataless() const noexcept { return (flags & kNodeDataless) != 0; }
    [[nodiscard]] bool is_symlink() const noexcept { return (flags & kNodeSymlink) != 0; }
    [[nodiscard]] bool is_hardlink_dup() const noexcept { return (flags & kNodeHardlinkDup) != 0; }
    [[nodiscard]] bool access_denied() const noexcept { return (flags & kNodeAccessDenied) != 0; }
    [[nodiscard]] bool is_deleted() const noexcept { return (flags & kNodeDeleted) != 0; }
    [[nodiscard]] bool is_mount_point() const noexcept { return (flags & kNodeMountPoint) != 0; }

    [[nodiscard]] uint64_t bytes(SizeMetric metric) const noexcept {
        return metric == SizeMetric::Physical ? allocated_bytes : logical_bytes;
    }
};

static_assert(sizeof(FsNode) == 56, "FsNode must stay compact (target <= 64 bytes)");
static_assert(alignof(FsNode) == 8);
static_assert(std::is_trivially_copyable_v<FsNode> && std::is_standard_layout_v<FsNode>,
              "FsNode must be plain data");

/// Capacity limits of an arena. Capacity is virtual address space only — memory is
/// committed page by page as nodes are appended — so the defaults are generous.
struct ArenaConfig {
    uint32_t max_nodes = 1u << 28;                         ///< 268 M nodes (15 GiB of address space)
    uint64_t max_string_bytes = StringPool::kMaxCapacity;  ///< just under 4 GiB
};

/// Initial attributes for `FlatArena::addChild`.
struct NodeInit {
    uint64_t allocated_bytes = 0;
    uint64_t logical_bytes = 0;
    int64_t mtime = 0;
    uint32_t flags = 0;
};

/// What `FlatArena::removeNode` took out of the tree.
struct RemovalResult {
    bool removed = false;
    uint64_t allocated_bytes = 0;
    uint64_t logical_bytes = 0;
    uint32_t file_count = 0;
    uint32_t node_count = 0;

    [[nodiscard]] uint64_t bytes(SizeMetric metric) const noexcept {
        return metric == SizeMetric::Physical ? allocated_bytes : logical_bytes;
    }
};

/// The scanned tree: a contiguous array of `FsNode`, a contiguous pool of names and the
/// extension table.
///
/// Invariant: a node's index is always greater than its parent's index. Nodes are only
/// ever appended, and a child can only be created once its parent exists, so this holds
/// by construction — and it turns bottom-up aggregation into a single reverse pass over
/// the array with no recursion, no stack and perfectly sequential memory access.
///
/// Thread-safety: `allocNodes()` and the string pool are lock-free and may be used from
/// many scanner threads at once. A thread may write a node only while it exclusively
/// owns it (right after allocating it, or while it is the one scanning that directory).
/// Every other member is for the single thread that owns the finished tree.
class FlatArena {
public:
    explicit FlatArena(const ArenaConfig& config = ArenaConfig{});

    FlatArena(const FlatArena&) = delete;
    FlatArena& operator=(const FlatArena&) = delete;

    // --- Construction -------------------------------------------------------------------

    /// Creates the root node. `name` is the absolute path that was scanned.
    uint32_t addRoot(std::string_view name, int64_t mtime = 0);

    /// Appends one child (O(1)). Returns kInvalidNode when the arena is full.
    /// Directory totals are not updated until `finalize()`.
    uint32_t addChild(uint32_t parent, std::string_view name, const NodeInit& init);

    /// Reserves `count` consecutive node indices; lock-free. Returns kInvalidNode when
    /// the arena cannot hold them (nothing is reserved in that case).
    [[nodiscard]] uint32_t allocNodes(uint32_t count) noexcept;

    /// Aggregates directory totals bottom-up, sorts every child list by `metric`
    /// (largest first), rebuilds the extension histogram and assigns extension colors.
    void finalize(SizeMetric metric = SizeMetric::Physical);

    /// Re-sorts every child list by `metric`, largest first.
    void sortChildren(SizeMetric metric);

    /// Metric the child lists are currently ordered by (set by finalize/sortChildren).
    [[nodiscard]] SizeMetric sortedMetric() const noexcept { return sorted_metric_; }
    [[nodiscard]] bool isSorted() const noexcept { return sorted_; }

    // --- Incremental update -------------------------------------------------------------

    /// Removes a node (and its subtree) from the totals without rescanning anything.
    ///
    /// The node is tombstoned and its bytes and file count are subtracted from every
    /// ancestor up to the root: O(depth). Removing a directory additionally walks that
    /// subtree once so the extension histogram stays exact. The root cannot be removed.
    RemovalResult removeNode(uint32_t idx);

    /// Restores "largest first" order in the child lists along the path from `idx` to
    /// the root (ancestors shrink when something below them is removed) and unlinks
    /// tombstones from those lists.
    void resortAncestorChain(uint32_t idx);

    // --- Access -------------------------------------------------------------------------

    [[nodiscard]] uint32_t root() const noexcept { return size() == 0 ? kInvalidNode : 0; }

    /// Number of node slots in use, including tombstones.
    [[nodiscard]] uint32_t size() const noexcept { return node_count_.load(std::memory_order_acquire); }
    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint32_t liveNodeCount() const noexcept { return size() - deleted_count_; }

    [[nodiscard]] FsNode& node(uint32_t idx) noexcept {
        assert(idx < size());
        return nodes_[idx];
    }
    [[nodiscard]] const FsNode& node(uint32_t idx) const noexcept {
        assert(idx < size());
        return nodes_[idx];
    }
    [[nodiscard]] bool isValid(uint32_t idx) const noexcept {
        return idx < size() && !nodes_[idx].is_deleted();
    }

    [[nodiscard]] std::string_view name(uint32_t idx) const noexcept {
        const FsNode& n = node(idx);
        return strings_.view(n.name_offset, n.name_len);
    }

    /// First live child / next live sibling (tombstones are skipped).
    [[nodiscard]] uint32_t firstChild(uint32_t idx) const noexcept {
        return skipDeleted(node(idx).first_child_idx);
    }
    [[nodiscard]] uint32_t nextSibling(uint32_t idx) const noexcept {
        return skipDeleted(node(idx).next_sibling_idx);
    }

    /// Calls `fn(childIdx)` for every live child, in list order.
    template <typename Fn>
    void forEachChild(uint32_t idx, Fn&& fn) const {
        for (uint32_t c = firstChild(idx); c != kInvalidNode; c = nextSibling(c)) fn(c);
    }

    /// Absolute path of a node.
    [[nodiscard]] std::string path(uint32_t idx) const;
    void appendPath(uint32_t idx, std::string& out) const;

    /// Path of `idx` relative to `base` (which must be `idx` or one of its ancestors).
    /// Returns the node's own name when `idx == base`.
    [[nodiscard]] std::string relativePath(uint32_t idx, uint32_t base) const;

    [[nodiscard]] uint32_t depth(uint32_t idx) const noexcept;
    [[nodiscard]] bool isAncestorOrSelf(uint32_t ancestor, uint32_t idx) const noexcept;

    /// The child of `ancestor` that lies on the path to `idx`; kInvalidNode if `idx` is
    /// not strictly below `ancestor`.
    [[nodiscard]] uint32_t childToward(uint32_t ancestor, uint32_t idx) const noexcept;

    /// Largest file found by always descending into the largest child; used to give
    /// aggregated directory blocks a representative color. kInvalidNode if none.
    [[nodiscard]] uint32_t representativeFile(uint32_t idx) const noexcept;

    [[nodiscard]] ExtensionTable& extensions() noexcept { return extensions_; }
    [[nodiscard]] const ExtensionTable& extensions() const noexcept { return extensions_; }
    [[nodiscard]] StringPool& strings() noexcept { return strings_; }
    [[nodiscard]] const StringPool& strings() const noexcept { return strings_; }

    /// Recomputes the extension histogram from scratch (O(n)); `removeNode` keeps it in
    /// sync incrementally, this is the reference the tests compare against.
    void rebuildExtensionStats();

    /// Bytes of committed memory used by nodes and names.
    [[nodiscard]] uint64_t memoryBytes() const noexcept;

private:
    [[nodiscard]] uint32_t skipDeleted(uint32_t idx) const noexcept {
        while (idx != kInvalidNode && nodes_[idx].is_deleted()) idx = nodes_[idx].next_sibling_idx;
        return idx;
    }
    void sortChildrenOf(uint32_t dir, SizeMetric metric, std::vector<std::pair<uint64_t, uint32_t>>& scratch);

    VirtualRegion region_;
    FsNode* nodes_ = nullptr;
    uint32_t capacity_ = 0;
    std::atomic<uint32_t> node_count_{0};
    uint32_t deleted_count_ = 0;

    StringPool strings_;
    ExtensionTable extensions_;

    SizeMetric sorted_metric_ = SizeMetric::Physical;
    bool sorted_ = false;
    uint32_t pending_resorts_ = 0;  ///< removals whose ancestor chain is not re-sorted yet
};

}  // namespace diskprism
