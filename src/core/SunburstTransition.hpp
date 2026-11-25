// DiskPrism core — geometry of an animated sunburst drill-down / zoom-out.
#pragma once

#include "CoreTypes.hpp"
#include "SunburstLayout.hpp"

#include <cstdint>
#include <vector>

namespace diskprism {

class FlatArena;

enum class SunburstTransitionKind : uint8_t {
    Bloom,    ///< no geometric relation (new scan, unrelated focus): arcs unfurl from 12 o'clock
    ZoomIn,   ///< the new focus is a directory shown in the old layout: it expands to the full disc
    ZoomOut,  ///< the new focus is an ancestor: the old disc shrinks into its slice
    Morph,    ///< same focus, different sizes (item removed, metric switched): arcs slide to their new place
};

/// One arc with its geometry at both ends of the transition. The renderer uploads these
/// once and animates by interpolating (start, end, inner, outer) on the GPU.
struct SunburstTweenArc {
    float from[4];        ///< start angle, end angle, inner radius, outer radius at progress 0
    float to[4];          ///< … at progress 1
    Color color_from;
    Color color_to;
    float alpha_from;
    float alpha_to;
    uint32_t node;
    uint32_t flags;       ///< SunburstArcFlags
    int32_t layout_index; ///< index into the new layout's arcs; -1 for an arc that is leaving
    bool is_hub;
};

struct SunburstTransition {
    SunburstTransitionKind kind = SunburstTransitionKind::Bloom;
    /// Draw order: leaving arcs, then the new layout's arcs in layout order, then the hub.
    std::vector<SunburstTweenArc> arcs;
    size_t first_layout_arc = 0;  ///< index in `arcs` of the new layout's arc 0
    size_t hub_index = 0;         ///< index in `arcs` of the hub
};

/// Builds the tween between two layouts of the same viewport.
///
/// Zooming into a directory whose slice spans [a0, a1] on ring d is the map
///     angle' = clamp((angle − a0) / (a1 − a0), 0, 1) · 2π,     ring' = ring − d,
/// applied to everything on screen: the slice opens up to the full circle, its descendants
/// move d rings inward, its former siblings are squeezed to nothing at 12 o'clock and the
/// rings inside it collapse into the hub. Because the map is monotonic in the angle, arcs
/// can be interpolated independently without ever overlapping. Zooming out runs the same
/// map backwards.
[[nodiscard]] SunburstTransition buildSunburstTransition(const FlatArena& arena, const SunburstLayout& oldLayout,
                                                         const SunburstLayout& newLayout, Color hubColor);

/// A steady (non-animating) arc list for a layout: `from == to`.
[[nodiscard]] SunburstTransition buildSunburstSteadyState(const SunburstLayout& layout, Color hubColor);

}  // namespace diskprism
