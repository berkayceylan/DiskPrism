// DiskPrism core — squarified cushion treemap layout (Bruls, Huizing, van Wijk).
#pragma once

#include "CoreTypes.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace diskprism {

class FlatArena;
class NodeMatchSet;

enum TreemapRectFlags : uint32_t {
    kTreemapFile = 1u << 0,       ///< a single file
    kTreemapDirectory = 1u << 1,  ///< a directory too small to subdivide, drawn as one block
    kTreemapAggregate = 1u << 2,  ///< the remaining small children of `node`, merged into one block
    kTreemapDimmed = 1u << 3,     ///< does not match the active filter
    kTreemapDataless = 1u << 4,   ///< cloud-only item
};

/// One block the renderer draws. Coordinates are in viewport units with the origin in
/// the top-left corner and y pointing down.
struct TreemapRect {
    float x, y, w, h;
    /// van Wijk cushion surface z(u, v) = ax·u² + bx·u + ay·v² + by·v, where (u, v) is the
    /// position *relative to (x, y)*. Storing the coefficients rect-relative keeps the
    /// shader's normal computation free of catastrophic cancellation in 32-bit floats.
    float ax, bx, ay, by;
    uint32_t node;   ///< arena node this block stands for
    uint32_t flags;  ///< TreemapRectFlags
    Color color;     ///< base color (by file type), sRGB
    uint16_t depth;  ///< nesting depth below the focused directory (its children are 1)
    uint16_t ext_id;
};

/// Rectangle of a directory that was subdivided (it has no block of its own).
struct TreemapFrame {
    uint32_t node;
    float x, y, w, h;
    uint16_t depth;
};

struct TreemapParams {
    float width = 0;
    float height = 0;
    SizeMetric metric = SizeMetric::Physical;
    /// Screen-space culling threshold: a rectangle narrower or lower than this is drawn
    /// as a single block instead of being subdivided, and children whose area falls
    /// below `min_extent²` are merged into one aggregate block per directory.
    float min_extent = 3.0f;
    /// Round rectangle edges to whole units (device pixels) so borders stay crisp.
    bool snap_to_pixels = true;
    float ridge_height = 0.34f;  ///< van Wijk h: cushion height of the first nesting level
    float ridge_decay = 0.82f;   ///< van Wijk f: height factor per additional level
    const NodeMatchSet* matches = nullptr;  ///< active filter, if any
};

/// Computes the treemap for one focused directory.
///
/// Children are taken largest-first and packed row by row along the shorter side of the
/// remaining rectangle, growing a row while that improves its worst aspect ratio — the
/// squarified algorithm. Each nesting level adds a parabolic ridge to the surface
/// coefficients handed down to its children (cushion shading).
///
/// The arena's child lists must be sorted by `params.metric` (see
/// `FlatArena::sortChildren`); that is what lets a directory with a million tiny children
/// cost O(visible blocks) instead of O(children). If they are not, the layout sorts a
/// private copy per directory and stays correct, just slower.
class TreemapLayout {
public:
    void build(const FlatArena& arena, uint32_t focus, const TreemapParams& params);
    void clear();

    [[nodiscard]] const std::vector<TreemapRect>& rects() const noexcept { return rects_; }
    [[nodiscard]] const std::vector<TreemapFrame>& frames() const noexcept { return frames_; }
    [[nodiscard]] uint32_t focus() const noexcept { return focus_; }
    [[nodiscard]] float width() const noexcept { return width_; }
    [[nodiscard]] float height() const noexcept { return height_; }

    /// Index into `rects()` of the block containing (x, y), or -1. Uses a uniform grid,
    /// so the cost is independent of the number of blocks.
    [[nodiscard]] int32_t hitTest(float x, float y) const noexcept;

    /// Index of the block that represents exactly `node` (not an aggregate), or -1.
    [[nodiscard]] int32_t rectIndexForNode(uint32_t node) const noexcept;

    /// Area covered by `node`: its block, or its frame if it was subdivided.
    /// Returns false if the node has no geometry of its own in this layout.
    [[nodiscard]] bool rectForNode(uint32_t node, float outXYWH[4]) const noexcept;

private:
    void buildIndexes();

    std::vector<TreemapRect> rects_;
    std::vector<TreemapFrame> frames_;
    uint32_t focus_ = kInvalidNode;
    float width_ = 0;
    float height_ = 0;

    // node -> index lookups (sorted by node)
    std::vector<std::pair<uint32_t, int32_t>> rect_by_node_;
    std::vector<std::pair<uint32_t, int32_t>> frame_by_node_;

    // Uniform grid in CSR form: items of cell c are grid_items_[grid_start_[c] .. grid_start_[c + 1]).
    float cell_size_ = 32.0f;
    int grid_cols_ = 0;
    int grid_rows_ = 0;
    std::vector<uint32_t> grid_start_;
    std::vector<uint32_t> grid_items_;
};

}  // namespace diskprism
