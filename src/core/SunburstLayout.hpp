// DiskPrism core — sunburst (radial partition) layout.
#pragma once

#include "CoreTypes.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace diskprism {

class FlatArena;
class NodeMatchSet;

enum SunburstArcFlags : uint32_t {
    kArcFile = 1u << 0,
    kArcDirectory = 1u << 1,
    kArcAggregate = 1u << 2,  ///< "other smaller items" of `node`, merged into one slice
    kArcDimmed = 1u << 3,     ///< does not match the active filter
    kArcDataless = 1u << 4,   ///< cloud-only item
};

/// One annular sector. Angles are radians measured clockwise from 12 o'clock, so a point
/// at angle θ and radius r is (cx + r·sin θ, cy − r·cos θ) in y-down viewport coordinates.
struct SunburstArc {
    float start_angle;
    float end_angle;
    float inner_radius;
    float outer_radius;
    uint32_t node;   ///< arena node (for an aggregate: the directory whose leftovers it shows)
    uint32_t flags;  ///< SunburstArcFlags
    Color color;
    uint16_t depth;  ///< ring number; 1 = children of the focused directory
    uint16_t ext_id;
    int32_t parent;  ///< index of the parent arc, or -1 on ring 1
};

struct SunburstParams {
    float width = 0;
    float height = 0;
    SizeMetric metric = SizeMetric::Physical;
    int max_depth = 6;                 ///< number of rings
    float min_angle_degrees = 0.4f;    ///< thinner slices are merged per parent
    float margin = 14.0f;              ///< gap between the outermost ring and the viewport edge
    float hub_fraction = 0.25f;        ///< hub radius as a fraction of the outer radius
    float ring_decay = 0.86f;          ///< each ring is this much thinner than the one inside it
    const NodeMatchSet* matches = nullptr;
};

/// Computes concentric rings around a focused directory: ring 1 holds its children with
/// angular spans proportional to their size, ring 2 their children inside the parent's
/// span, and so on. Within a parent, children are placed largest-first; everything
/// thinner than `min_angle_degrees` is merged into one trailing "other" slice.
///
/// Arcs are generated ring by ring in angular order, so each ring is a sorted run of the
/// output vector and hit-testing is a ring lookup plus a binary search.
class SunburstLayout {
public:
    static constexpr int32_t kHitNone = -1;
    static constexpr int32_t kHitHub = -2;

    void build(const FlatArena& arena, uint32_t focus, const SunburstParams& params);
    void clear();

    [[nodiscard]] const std::vector<SunburstArc>& arcs() const noexcept { return arcs_; }
    [[nodiscard]] uint32_t focus() const noexcept { return focus_; }
    [[nodiscard]] float centerX() const noexcept { return center_x_; }
    [[nodiscard]] float centerY() const noexcept { return center_y_; }
    [[nodiscard]] float hubRadius() const noexcept { return hub_radius_; }
    [[nodiscard]] float outerRadius() const noexcept { return outer_radius_; }
    [[nodiscard]] int ringCount() const noexcept { return max_depth_; }

    /// Radii of a ring. Ring 0 is the hub disc [0, hubRadius]; rings beyond `ringCount()`
    /// continue the same geometric progression (used to animate arcs in from outside the
    /// visible disc) and negative rings collapse to the centre.
    void ringBounds(int ring, float& inner, float& outer) const noexcept;

    /// Arc index under (x, y), `kHitHub` for the centre disc, or `kHitNone`. O(log n).
    [[nodiscard]] int32_t hitTest(float x, float y) const noexcept;

    /// Index of the arc that represents exactly `node` (not an aggregate), or -1.
    [[nodiscard]] int32_t arcIndexForNode(uint32_t node) const noexcept;

    /// Half-open index range [first, last) of the arcs on `ring` (1-based).
    [[nodiscard]] std::pair<size_t, size_t> ringRange(int ring) const noexcept;

private:
    [[nodiscard]] float ringOuterRadius(int ring) const noexcept;

    std::vector<SunburstArc> arcs_;
    std::vector<size_t> ring_begin_;  // ring_begin_[k] = first arc of ring k + 1; back() = arcs_.size()
    std::vector<std::pair<uint32_t, int32_t>> arc_by_node_;
    uint32_t focus_ = kInvalidNode;
    float center_x_ = 0;
    float center_y_ = 0;
    float hub_radius_ = 0;
    float outer_radius_ = 0;
    float ring_unit_ = 0;   // width of ring 1
    float ring_decay_ = 0.86f;
    int max_depth_ = 0;
};

}  // namespace diskprism
