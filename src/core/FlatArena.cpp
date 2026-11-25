#include "FlatArena.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace diskprism {

namespace {

constexpr uint32_t kMaxNodeCapacity = UINT32_MAX - 1;  // kInvalidNode must never be a valid index

uint16_t clampNameLength(size_t length) noexcept {
    return static_cast<uint16_t>(std::min<size_t>(length, UINT16_MAX));
}

}  // namespace

FlatArena::FlatArena(const ArenaConfig& config) : strings_(config.max_string_bytes) {
    const uint32_t wanted = std::clamp<uint32_t>(config.max_nodes, 1, kMaxNodeCapacity);
    const size_t preferred = static_cast<size_t>(wanted) * sizeof(FsNode);
    const size_t minimum = std::min<size_t>(preferred, size_t{4096} * sizeof(FsNode));
    region_ = VirtualRegion::reserveUpTo(preferred, minimum);
    capacity_ = static_cast<uint32_t>(std::min<size_t>(region_.size() / sizeof(FsNode), wanted));
    // FsNode is trivial, so this begins the array's lifetime without touching a page.
    nodes_ = new (region_.data()) FsNode[capacity_];
}

uint32_t FlatArena::allocNodes(uint32_t count) noexcept {
    uint32_t current = node_count_.load(std::memory_order_relaxed);
    for (;;) {
        if (count > capacity_ - current) return kInvalidNode;
        if (node_count_.compare_exchange_weak(current, current + count, std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
            return current;
        }
    }
}

uint32_t FlatArena::addRoot(std::string_view name, int64_t mtime) {
    assert(size() == 0 && "the root must be the first node");
    const uint32_t idx = allocNodes(1);
    if (idx == kInvalidNode) return kInvalidNode;

    const uint16_t length = clampNameLength(name.size());
    uint32_t offset = strings_.append(name.substr(0, length));
    FsNode n{};
    n.mtime = mtime;
    n.parent_idx = kInvalidNode;
    n.first_child_idx = kInvalidNode;
    n.next_sibling_idx = kInvalidNode;
    n.name_offset = offset == StringPool::kInvalidOffset ? 0 : offset;
    n.name_len = offset == StringPool::kInvalidOffset ? uint16_t{0} : length;
    n.flags = kNodeDir;
    n.ext_id = ExtensionTable::kNone;
    nodes_[idx] = n;
    return idx;
}

uint32_t FlatArena::addChild(uint32_t parent, std::string_view name, const NodeInit& init) {
    assert(parent < size() && nodes_[parent].is_dir());
    const uint32_t idx = allocNodes(1);
    if (idx == kInvalidNode) return kInvalidNode;

    const uint16_t length = clampNameLength(name.size());
    const uint32_t offset = strings_.append(name.substr(0, length));
    const bool isDir = (init.flags & kNodeDir) != 0;

    FsNode n{};
    n.allocated_bytes = isDir ? 0 : init.allocated_bytes;
    n.logical_bytes = isDir ? 0 : init.logical_bytes;
    n.mtime = init.mtime;
    n.parent_idx = parent;
    n.first_child_idx = kInvalidNode;
    n.next_sibling_idx = nodes_[parent].first_child_idx;
    n.name_offset = offset == StringPool::kInvalidOffset ? 0 : offset;
    n.name_len = offset == StringPool::kInvalidOffset ? uint16_t{0} : length;
    n.file_count = isDir ? 0 : 1;
    n.flags = init.flags & ~static_cast<uint32_t>(kNodeDeleted);
    n.ext_id = isDir ? ExtensionTable::kNone : extensions_.internFilename(name);
    nodes_[idx] = n;

    nodes_[parent].first_child_idx = idx;
    nodes_[parent].child_count += 1;
    sorted_ = false;
    pending_resorts_ = 0;
    return idx;
}

void FlatArena::finalize(SizeMetric metric) {
    const uint32_t count = size();

    // Directories start from zero so that finalize() is idempotent.
    for (uint32_t i = 0; i < count; ++i) {
        FsNode& n = nodes_[i];
        if (n.is_dir()) {
            n.allocated_bytes = 0;
            n.logical_bytes = 0;
            n.file_count = 0;
        }
    }

    // Children always have a larger index than their parent, so one reverse sweep folds
    // every subtree into its ancestors: by the time node i is added to its parent, all
    // of i's descendants (which have indices > i) have already been added to i.
    for (uint32_t i = count; i-- > 1;) {
        const FsNode& n = nodes_[i];
        if (n.is_deleted()) continue;
        assert(n.parent_idx < i && "child index must exceed parent index");
        FsNode& parent = nodes_[n.parent_idx];
        parent.allocated_bytes += n.allocated_bytes;
        parent.logical_bytes += n.logical_bytes;
        parent.file_count += n.file_count;
    }

    sortChildren(metric);
    rebuildExtensionStats();
    extensions_.assignColors();
}

void FlatArena::sortChildrenOf(uint32_t dir, SizeMetric metric,
                               std::vector<std::pair<uint64_t, uint32_t>>& scratch) {
    scratch.clear();
    for (uint32_t c = nodes_[dir].first_child_idx; c != kInvalidNode; c = nodes_[c].next_sibling_idx) {
        if (nodes_[c].is_deleted()) continue;
        scratch.emplace_back(nodes_[c].bytes(metric), c);
    }
    if (scratch.size() > 1) {
        // Largest first. Equal sizes keep ascending index order, which makes the result
        // deterministic for a given arena.
        std::sort(scratch.begin(), scratch.end(),
                  [](const std::pair<uint64_t, uint32_t>& a, const std::pair<uint64_t, uint32_t>& b) {
                      if (a.first != b.first) return a.first > b.first;
                      return a.second < b.second;
                  });
    }
    // Relinking only live nodes also unlinks any tombstones from the list.
    nodes_[dir].first_child_idx = scratch.empty() ? kInvalidNode : scratch.front().second;
    for (size_t k = 0; k < scratch.size(); ++k) {
        nodes_[scratch[k].second].next_sibling_idx = (k + 1 < scratch.size()) ? scratch[k + 1].second : kInvalidNode;
    }
    nodes_[dir].child_count = static_cast<uint32_t>(scratch.size());
}

void FlatArena::sortChildren(SizeMetric metric) {
    std::vector<std::pair<uint64_t, uint32_t>> scratch;
    const uint32_t count = size();
    for (uint32_t i = 0; i < count; ++i) {
        const FsNode& n = nodes_[i];
        if (!n.is_dir() || n.is_deleted() || n.first_child_idx == kInvalidNode) continue;
        sortChildrenOf(i, metric, scratch);
    }
    sorted_metric_ = metric;
    sorted_ = true;
    pending_resorts_ = 0;
}

RemovalResult FlatArena::removeNode(uint32_t idx) {
    RemovalResult result;
    if (idx == kInvalidNode || idx >= size() || idx == 0) return result;  // the root stays
    FsNode& target = nodes_[idx];
    if (target.is_deleted()) return result;

    result.allocated_bytes = target.allocated_bytes;
    result.logical_bytes = target.logical_bytes;
    result.file_count = target.file_count;

    // Pre-order walk of the subtree using only the links stored in the nodes (no stack,
    // no recursion): tombstone every node and take each file out of the histogram.
    // Children removed earlier are already tombstoned and are skipped with their subtrees.
    uint32_t current = idx;
    for (;;) {
        FsNode& n = nodes_[current];
        if (!n.is_dir()) extensions_.removeFile(n.ext_id, n.allocated_bytes, n.logical_bytes);
        n.flags |= kNodeDeleted;
        ++result.node_count;

        uint32_t next = skipDeleted(n.first_child_idx);
        if (next == kInvalidNode) {
            uint32_t at = current;
            while (at != idx) {
                next = skipDeleted(nodes_[at].next_sibling_idx);
                if (next != kInvalidNode) break;
                at = nodes_[at].parent_idx;
            }
        }
        if (next == kInvalidNode) break;
        current = next;
    }

    // O(depth): roll the totals back along the ancestor chain.
    for (uint32_t p = target.parent_idx; p != kInvalidNode; p = nodes_[p].parent_idx) {
        FsNode& ancestor = nodes_[p];
        ancestor.allocated_bytes -= std::min(ancestor.allocated_bytes, result.allocated_bytes);
        ancestor.logical_bytes -= std::min(ancestor.logical_bytes, result.logical_bytes);
        ancestor.file_count -= std::min(ancestor.file_count, result.file_count);
    }
    FsNode& parent = nodes_[target.parent_idx];
    if (parent.child_count > 0) --parent.child_count;

    deleted_count_ += result.node_count;
    if (sorted_ || pending_resorts_ > 0) {
        // Ancestors just shrank, so they may no longer be in "largest first" position
        // among their siblings until resortAncestorChain() runs.
        sorted_ = false;
        ++pending_resorts_;
    }
    result.removed = true;
    return result;
}

void FlatArena::resortAncestorChain(uint32_t idx) {
    if (idx == kInvalidNode || idx >= size()) return;
    std::vector<std::pair<uint64_t, uint32_t>> scratch;
    uint32_t dir = nodes_[idx].is_dir() && !nodes_[idx].is_deleted() ? idx : nodes_[idx].parent_idx;
    for (; dir != kInvalidNode; dir = nodes_[dir].parent_idx) {
        sortChildrenOf(dir, sorted_metric_, scratch);
    }
    if (pending_resorts_ > 0 && --pending_resorts_ == 0) sorted_ = true;
}

void FlatArena::appendPath(uint32_t idx, std::string& out) const {
    if (idx == kInvalidNode || idx >= size()) return;

    // Collect the chain idx → root, then emit it root → idx.
    constexpr size_t kInlineDepth = 96;
    uint32_t inlineChain[kInlineDepth];
    std::vector<uint32_t> heapChain;
    uint32_t* chain = inlineChain;
    size_t depthCount = 0;
    for (uint32_t i = idx; i != kInvalidNode; i = nodes_[i].parent_idx) {
        if (depthCount == kInlineDepth && heapChain.empty()) {
            heapChain.assign(inlineChain, inlineChain + kInlineDepth);
        }
        if (!heapChain.empty()) {
            heapChain.push_back(i);
            chain = heapChain.data();
        } else {
            inlineChain[depthCount] = i;
        }
        ++depthCount;
    }

    size_t total = 0;
    for (size_t k = 0; k < depthCount; ++k) total += static_cast<size_t>(nodes_[chain[k]].name_len) + 1;
    out.reserve(out.size() + total);

    for (size_t k = depthCount; k-- > 0;) {
        const std::string_view component = name(chain[k]);
        if (k + 1 != depthCount && (out.empty() || out.back() != '/')) out.push_back('/');
        out.append(component);
    }
}

std::string FlatArena::path(uint32_t idx) const {
    std::string result;
    appendPath(idx, result);
    return result;
}

std::string FlatArena::relativePath(uint32_t idx, uint32_t base) const {
    if (idx == kInvalidNode || idx >= size()) return {};
    if (idx == base || !isAncestorOrSelf(base, idx)) return std::string(name(idx));

    std::vector<uint32_t> chain;
    for (uint32_t i = idx; i != base && i != kInvalidNode; i = nodes_[i].parent_idx) chain.push_back(i);
    std::string result;
    for (size_t k = chain.size(); k-- > 0;) {
        if (!result.empty()) result.push_back('/');
        result.append(name(chain[k]));
    }
    return result;
}

uint32_t FlatArena::depth(uint32_t idx) const noexcept {
    uint32_t d = 0;
    if (idx == kInvalidNode || idx >= size()) return 0;
    for (uint32_t p = nodes_[idx].parent_idx; p != kInvalidNode; p = nodes_[p].parent_idx) ++d;
    return d;
}

bool FlatArena::isAncestorOrSelf(uint32_t ancestor, uint32_t idx) const noexcept {
    if (ancestor == kInvalidNode || idx == kInvalidNode || idx >= size()) return false;
    // Parent indices are strictly smaller than child indices, so the climb can stop as
    // soon as it passes below `ancestor`.
    uint32_t i = idx;
    while (i != kInvalidNode && i > ancestor) i = nodes_[i].parent_idx;
    return i == ancestor;
}

uint32_t FlatArena::childToward(uint32_t ancestor, uint32_t idx) const noexcept {
    if (ancestor == kInvalidNode || idx == kInvalidNode || idx >= size() || idx == ancestor) return kInvalidNode;
    uint32_t i = idx;
    while (i != kInvalidNode && i > ancestor) {
        const uint32_t parent = nodes_[i].parent_idx;
        if (parent == ancestor) return i;
        i = parent;
    }
    return kInvalidNode;
}

uint32_t FlatArena::representativeFile(uint32_t idx) const noexcept {
    if (idx == kInvalidNode || idx >= size()) return kInvalidNode;
    uint32_t current = idx;
    while (nodes_[current].is_dir()) {
        // Child lists are sorted largest-first, so the head is the dominant child; step
        // past empty directories to find one that actually leads to a file.
        uint32_t child = firstChild(current);
        while (child != kInvalidNode && nodes_[child].is_dir() && nodes_[child].file_count == 0) {
            child = nextSibling(child);
        }
        if (child == kInvalidNode) return kInvalidNode;
        current = child;
    }
    return current;
}

void FlatArena::rebuildExtensionStats() {
    extensions_.resetStats();
    const uint32_t count = size();
    for (uint32_t i = 0; i < count; ++i) {
        const FsNode& n = nodes_[i];
        if (n.is_dir() || n.is_deleted()) continue;
        extensions_.addFile(n.ext_id, n.allocated_bytes, n.logical_bytes);
    }
}

uint64_t FlatArena::memoryBytes() const noexcept {
    return static_cast<uint64_t>(size()) * sizeof(FsNode) + strings_.size();
}

}  // namespace diskprism
