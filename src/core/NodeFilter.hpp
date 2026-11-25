// DiskPrism core — name / extension filtering over the whole arena.
#pragma once

#include "CoreTypes.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace diskprism {

class FlatArena;

/// What the user is filtering by. Both criteria may be active at once (logical AND).
struct FilterQuery {
    std::string name_substring;  ///< case-insensitive substring of the entry name; empty = any
    int32_t extension_id = -1;   ///< ExtensionTable ID of the file type; -1 = any

    [[nodiscard]] bool empty() const noexcept { return name_substring.empty() && extension_id < 0; }
    bool operator==(const FilterQuery&) const = default;
};

/// Result of evaluating a `FilterQuery` over an arena: one small bit set per node.
///
/// The evaluation is two linear sweeps over the node array (no recursion, sequential
/// memory access), relying on the arena invariant that a parent's index is smaller than
/// its children's: a forward sweep pushes "an ancestor matches" downward and a reverse
/// sweep pulls "a descendant matches" upward.
class NodeMatchSet {
public:
    enum : uint8_t {
        kSelf = 1 << 0,       ///< the node itself satisfies the query
        kInherited = 1 << 1,  ///< an ancestor satisfies it (everything inside a matching folder)
        kContains = 1 << 2,   ///< at least one descendant satisfies it
    };

    /// Evaluates `query`. Returns false (leaving the set cleared) if `cancel` became true.
    bool compute(const FlatArena& arena, const FilterQuery& query, const std::atomic<bool>* cancel = nullptr);

    void clear() noexcept;

    /// True when a non-empty query has been evaluated.
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const FilterQuery& query() const noexcept { return query_; }

    [[nodiscard]] uint8_t bits(uint32_t idx) const noexcept { return idx < bits_.size() ? bits_[idx] : uint8_t{0}; }

    /// The node matches or lies inside something that matches: show it at full strength.
    [[nodiscard]] bool highlighted(uint32_t idx) const noexcept { return (bits(idx) & (kSelf | kInherited)) != 0; }

    /// The node matches, lies inside a match, or contains one: relevant for a block that
    /// stands in for a whole directory.
    [[nodiscard]] bool relevant(uint32_t idx) const noexcept { return bits(idx) != 0; }

    [[nodiscard]] uint64_t matchCount() const noexcept { return match_count_; }
    [[nodiscard]] uint64_t matchedFiles() const noexcept { return matched_files_; }
    [[nodiscard]] uint64_t matchedAllocatedBytes() const noexcept { return matched_allocated_; }
    [[nodiscard]] uint64_t matchedLogicalBytes() const noexcept { return matched_logical_; }

private:
    std::vector<uint8_t> bits_;
    FilterQuery query_;
    bool active_ = false;
    uint64_t match_count_ = 0;        ///< nodes whose own name / type matches
    uint64_t matched_files_ = 0;      ///< files that are highlighted
    uint64_t matched_allocated_ = 0;  ///< their physical bytes
    uint64_t matched_logical_ = 0;    ///< their logical bytes
};

/// ASCII-lowercases `text` (bytes >= 0x80 are left untouched).
[[nodiscard]] std::string toLowerAscii(std::string_view text);

/// Case-insensitive (ASCII) substring test; `lowercasedNeedle` must already be lowercase.
[[nodiscard]] bool containsCaseInsensitive(std::string_view haystack, std::string_view lowercasedNeedle) noexcept;

}  // namespace diskprism
