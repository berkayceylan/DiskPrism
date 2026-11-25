#include "NodeFilter.hpp"

#include "FlatArena.hpp"

namespace diskprism {

namespace {

constexpr char lowerAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr uint32_t kCancelCheckInterval = 1u << 16;

bool cancelled(const std::atomic<bool>* cancel, uint32_t i) noexcept {
    return cancel != nullptr && (i % kCancelCheckInterval) == 0 && cancel->load(std::memory_order_relaxed);
}

}  // namespace

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    for (char& c : result) c = lowerAscii(c);
    return result;
}

bool containsCaseInsensitive(std::string_view haystack, std::string_view lowercasedNeedle) noexcept {
    if (lowercasedNeedle.empty()) return true;
    if (lowercasedNeedle.size() > haystack.size()) return false;
    const char first = lowercasedNeedle.front();
    const size_t lastStart = haystack.size() - lowercasedNeedle.size();
    for (size_t i = 0; i <= lastStart; ++i) {
        if (lowerAscii(haystack[i]) != first) continue;
        size_t j = 1;
        while (j < lowercasedNeedle.size() && lowerAscii(haystack[i + j]) == lowercasedNeedle[j]) ++j;
        if (j == lowercasedNeedle.size()) return true;
    }
    return false;
}

void NodeMatchSet::clear() noexcept {
    bits_.clear();
    query_ = FilterQuery{};
    active_ = false;
    match_count_ = 0;
    matched_files_ = 0;
    matched_allocated_ = 0;
    matched_logical_ = 0;
}

bool NodeMatchSet::compute(const FlatArena& arena, const FilterQuery& query, const std::atomic<bool>* cancel) {
    clear();
    if (query.empty()) return true;

    const uint32_t count = arena.size();
    const std::string needle = toLowerAscii(query.name_substring);
    const bool byName = !needle.empty();
    const bool byExtension = query.extension_id >= 0;
    std::vector<uint8_t> bits(count, 0);

    uint64_t matchCount = 0;
    uint64_t matchedFiles = 0;
    uint64_t matchedAllocated = 0;
    uint64_t matchedLogical = 0;

    // Sweep 1 (forward): does the node itself match, and does an ancestor? A parent's
    // index is always smaller than its children's, so its bits are final by now.
    for (uint32_t i = 0; i < count; ++i) {
        if (cancelled(cancel, i)) return false;
        const FsNode& n = arena.node(i);
        if (n.is_deleted()) continue;

        bool self = true;
        if (byExtension) self = !n.is_dir() && n.ext_id == static_cast<uint32_t>(query.extension_id);
        if (self && byName) self = containsCaseInsensitive(arena.name(i), needle);

        uint8_t b = self ? uint8_t{kSelf} : uint8_t{0};
        if (n.parent_idx != kInvalidNode && (bits[n.parent_idx] & (kSelf | kInherited)) != 0) b |= kInherited;
        bits[i] = b;

        if (self) ++matchCount;
        if (b != 0 && !n.is_dir()) {
            ++matchedFiles;
            matchedAllocated += n.allocated_bytes;
            matchedLogical += n.logical_bytes;
        }
    }

    // Sweep 2 (reverse): tell every ancestor that something below it matches.
    for (uint32_t i = count; i-- > 1;) {
        if (cancelled(cancel, i)) return false;
        if ((bits[i] & (kSelf | kContains)) == 0) continue;
        const uint32_t parent = arena.node(i).parent_idx;
        if (parent != kInvalidNode) bits[parent] |= kContains;
    }

    bits_ = std::move(bits);
    query_ = query;
    query_.name_substring = needle;
    active_ = true;
    match_count_ = matchCount;
    matched_files_ = matchedFiles;
    matched_allocated_ = matchedAllocated;
    matched_logical_ = matchedLogical;
    return true;
}

}  // namespace diskprism
