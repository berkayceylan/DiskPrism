#include "GpuInstances.hpp"

#include "SunburstLayout.hpp"

#include <cmath>

namespace diskprism::gpu {

Theme Theme::standard(bool dark) noexcept {
    Theme theme;
    theme.dark = dark;
    theme.background = dark ? simd_float4{0.110f, 0.110f, 0.118f, 1.0f} : simd_float4{0.925f, 0.925f, 0.937f, 1.0f};
    theme.accent = simd_float4{0.04f, 0.52f, 1.0f, 1.0f};
    return theme;
}

Color Theme::hubColor() const noexcept {
    return dark ? Color{46, 46, 50, 255} : Color{252, 252, 253, 255};
}

simd_float4 toFloat4(Color color, float alpha) noexcept {
    return simd_float4{static_cast<float>(color.r) / 255.0f, static_cast<float>(color.g) / 255.0f,
                       static_cast<float>(color.b) / 255.0f, alpha};
}

void buildTreemapInstances(const TreemapLayout& layout, std::vector<TreemapInstance>& out) {
    const auto& rects = layout.rects();
    out.clear();
    out.reserve(rects.size());
    for (const TreemapRect& r : rects) {
        TreemapInstance instance{};
        instance.rect = simd_float4{r.x, r.y, r.w, r.h};
        instance.cushionCoeffs = simd_float4{r.ax, r.bx, r.ay, r.by};
        instance.color = toFloat4(r.color);
        instance.nodeIdx = r.node;
        uint32_t flags = 0;
        if ((r.flags & kTreemapDimmed) != 0) flags |= kInstanceDimmed;
        if ((r.flags & kTreemapAggregate) != 0) flags |= kInstanceAggregate;
        if ((r.flags & kTreemapDirectory) != 0) flags |= kInstanceDirectory;
        if ((r.flags & kTreemapDataless) != 0) flags |= kInstanceDataless;
        instance.flags = flags;
        out.push_back(instance);
    }
}

void buildSunburstInstances(const SunburstTransition& transition, std::vector<SunburstInstance>& out) {
    out.clear();
    out.reserve(transition.arcs.size());
    for (const SunburstTweenArc& arc : transition.arcs) {
        SunburstInstance instance{};
        instance.arcFrom = simd_float4{arc.from[0], arc.from[1], arc.from[2], arc.from[3]};
        instance.arcTo = simd_float4{arc.to[0], arc.to[1], arc.to[2], arc.to[3]};
        instance.colorFrom = toFloat4(arc.color_from, arc.alpha_from);
        instance.colorTo = toFloat4(arc.color_to, arc.alpha_to);
        instance.nodeIdx = arc.node;
        uint32_t flags = 0;
        if (arc.is_hub) flags |= kInstanceHub;
        if ((arc.flags & kArcDimmed) != 0) flags |= kInstanceDimmed;
        if ((arc.flags & kArcAggregate) != 0) flags |= kInstanceAggregate;
        if ((arc.flags & kArcDirectory) != 0) flags |= kInstanceDirectory;
        if ((arc.flags & kArcDataless) != 0) flags |= kInstanceDataless;
        instance.flags = flags;
        out.push_back(instance);
    }
}

FrameUniforms makeFrameUniforms(float widthPixels, float heightPixels, float backingScale, const Theme& theme) noexcept {
    FrameUniforms u{};
    u.viewport = simd_float4{widthPixels, heightPixels, backingScale, 1.0f};
    u.transform = simd_float4{1.0f, 1.0f, 0.0f, 0.0f};
    u.background = theme.background;

    // Light from the upper left, fairly high above the map (van Wijk's classic setup).
    // A flat surface receives ambient + diffuse * light.z, which is tuned to ~1 so that a
    // block's base color is what you see on the top of its cushion.
    const simd_float3 toLight = simd_normalize(simd_float3{-0.42f, -0.58f, 1.0f});
    if (theme.dark) {
        u.lightDir = simd_float4{toLight.x, toLight.y, toLight.z, 0.38f};  // w: ambient
        u.lightParams = simd_float4{0.76f, 0.05f, 14.0f, 0.86f};          // diffuse, specular, shininess, dim
        u.borderColor = simd_float4{0.0f, 0.0f, 0.0f, 0.34f};
    } else {
        // A bright canvas needs more ambient light and less contrast to keep the same
        // perceived depth without the shadows turning muddy.
        u.lightDir = simd_float4{toLight.x, toLight.y, toLight.z, 0.56f};
        u.lightParams = simd_float4{0.58f, 0.04f, 14.0f, 0.84f};
        u.borderColor = simd_float4{0.12f, 0.12f, 0.16f, 0.26f};
    }
    u.hoverRect = simd_float4{0.0f, 0.0f, 0.0f, 0.0f};
    u.selectionRect = simd_float4{0.0f, 0.0f, 0.0f, 0.0f};
    u.accentColor = theme.accent;
    u.sunburst = simd_float4{widthPixels * 0.5f, heightPixels * 0.5f, 1.0f, std::fmax(1.0f, backingScale)};
    u.ids = simd_int4{-1, -1, theme.dark ? 1 : 0, 0};
    return u;
}

}  // namespace diskprism::gpu
