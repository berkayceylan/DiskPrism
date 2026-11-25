// DiskPrism GPU — CPU-side mirror of the structures declared in Shaders.metal.
//
// The shader source is compiled at run time (no offline `metal` compiler is involved), so
// the two definitions cannot share a header. They are kept identical by hand and the
// Metal test verifies size and field offsets against the compiled pipeline's reflection
// data, so any drift fails the test suite instead of producing garbage on screen.
#pragma once

#include <simd/simd.h>

#include <cstddef>
#include <cstdint>

namespace diskprism::gpu {

/// Per-instance flag bits (TreemapInstance::flags / SunburstInstance::flags).
enum InstanceFlags : uint32_t {
    kInstanceDimmed = 1u << 0,     ///< outside the active filter
    kInstanceAggregate = 1u << 1,  ///< many small items merged into one block / slice
    kInstanceDirectory = 1u << 2,  ///< stands for a directory rather than a file
    kInstanceDataless = 1u << 3,   ///< cloud-only item (not stored on this disk)
    kInstanceHub = 1u << 4,        ///< the sunburst's centre disc
};

/// One treemap block. 64 bytes; all rectangles are drawn with a single instanced call
/// (4-vertex triangle strip per instance).
struct TreemapInstance {
    simd_float4 rect;           ///< x, y, width, height in drawable pixels, origin top-left
    simd_float4 cushionCoeffs;  ///< ax, bx, ay, by of the cushion surface, relative to the rect origin
    simd_float4 color;          ///< base color, sRGB
    uint32_t nodeIdx;           ///< arena node the block stands for
    uint32_t flags;             ///< InstanceFlags
    uint32_t reserved[2];
};
static_assert(sizeof(TreemapInstance) == 64);
static_assert(offsetof(TreemapInstance, cushionCoeffs) == 16);
static_assert(offsetof(TreemapInstance, color) == 32);
static_assert(offsetof(TreemapInstance, nodeIdx) == 48);
static_assert(offsetof(TreemapInstance, flags) == 52);

/// One sunburst sector. Geometry and color are stored for both ends of a transition and
/// interpolated on the GPU, so an animation frame only has to update one uniform.
struct SunburstInstance {
    simd_float4 arcFrom;    ///< start angle, end angle, inner radius, outer radius at progress 0
    simd_float4 arcTo;      ///< … at progress 1
    simd_float4 colorFrom;  ///< rgb + opacity at progress 0
    simd_float4 colorTo;    ///< rgb + opacity at progress 1
    uint32_t nodeIdx;
    uint32_t flags;         ///< InstanceFlags
    uint32_t reserved[2];
};
static_assert(sizeof(SunburstInstance) == 80);
static_assert(offsetof(SunburstInstance, arcTo) == 16);
static_assert(offsetof(SunburstInstance, colorFrom) == 32);
static_assert(offsetof(SunburstInstance, colorTo) == 48);
static_assert(offsetof(SunburstInstance, nodeIdx) == 64);
static_assert(offsetof(SunburstInstance, flags) == 68);

/// Per-draw constants shared by both pipelines.
struct FrameUniforms {
    simd_float4 viewport;       ///< width, height (drawable pixels), backing scale, layer opacity
    simd_float4 transform;      ///< scale.xy, offset.xy: layout coordinates -> screen pixels
    simd_float4 background;     ///< canvas color, sRGB
    simd_float4 lightDir;       ///< xyz: unit vector toward the light, w: ambient term
    simd_float4 lightParams;    ///< diffuse strength, specular strength, shininess, dim strength
    simd_float4 borderColor;    ///< rgb + opacity of the 1 px block borders
    simd_float4 hoverRect;      ///< x, y, w, h in layout coordinates (w <= 0: none)
    simd_float4 selectionRect;  ///< same, for the selected node
    simd_float4 accentColor;    ///< selection frame color
    simd_float4 sunburst;       ///< centre.xy (screen pixels), animation progress, divider width (pixels)
    simd_int4 ids;              ///< hovered instance, selected instance, dark mode (0/1), unused
};
static_assert(sizeof(FrameUniforms) == 176);
static_assert(offsetof(FrameUniforms, sunburst) == 144);
static_assert(offsetof(FrameUniforms, ids) == 160);

/// Names of the shader entry points in Shaders.metal.
inline constexpr const char* kTreemapVertexFunction = "treemap_vertex";
inline constexpr const char* kTreemapFragmentFunction = "treemap_fragment";
inline constexpr const char* kSunburstVertexFunction = "sunburst_vertex";
inline constexpr const char* kSunburstFragmentFunction = "sunburst_fragment";

/// Buffer binding indices used by both vertex and fragment stages.
inline constexpr unsigned kBufferIndexInstances = 0;
inline constexpr unsigned kBufferIndexUniforms = 1;

}  // namespace diskprism::gpu
