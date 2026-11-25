// DiskPrism GPU — conversion of core layout results into GPU instance data.
// Plain C++ (no Objective-C), shared by the view and the headless render tests.
#pragma once

#include "CoreTypes.hpp"
#include "MetalTypes.h"
#include "SunburstTransition.hpp"
#include "TreemapLayout.hpp"

#include <vector>

namespace diskprism::gpu {

/// Colors and lighting that depend on the system appearance.
struct Theme {
    bool dark = true;
    simd_float4 background{0.110f, 0.110f, 0.118f, 1.0f};  ///< canvas color, sRGB
    simd_float4 accent{0.04f, 0.52f, 1.0f, 1.0f};          ///< selection color, sRGB

    /// Defaults for dark or light appearance.
    [[nodiscard]] static Theme standard(bool dark) noexcept;

    /// Color of the sunburst's centre disc.
    [[nodiscard]] Color hubColor() const noexcept;
};

[[nodiscard]] simd_float4 toFloat4(Color color, float alpha = 1.0f) noexcept;

/// Fills `out` with one instance per treemap block.
void buildTreemapInstances(const TreemapLayout& layout, std::vector<TreemapInstance>& out);

/// Fills `out` with one instance per tween arc, in the transition's draw order.
void buildSunburstInstances(const SunburstTransition& transition, std::vector<SunburstInstance>& out);

/// Uniforms for an untransformed, fully opaque frame with nothing hovered or selected.
[[nodiscard]] FrameUniforms makeFrameUniforms(float widthPixels, float heightPixels, float backingScale,
                                              const Theme& theme) noexcept;

}  // namespace diskprism::gpu
