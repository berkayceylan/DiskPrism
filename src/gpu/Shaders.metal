// DiskPrism — Metal shaders for the cushion treemap and the sunburst.
//
// This file is shipped as text in Contents/Resources and compiled at run time with
// -[MTLDevice newLibraryWithSource:options:error:]; nothing here depends on the offline
// `metal` compiler. The structures below mirror src/gpu/MetalTypes.h field for field.

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------------------
// Shared definitions
// ---------------------------------------------------------------------------------------

constant float kPi = 3.14159265358979323846;
constant float kTwoPi = 6.28318530717958647692;

constant uint kInstanceDimmed = 1u << 0;     // outside the active filter
constant uint kInstanceAggregate = 1u << 1;  // many small items merged into one
constant uint kInstanceDirectory = 1u << 2;  // stands for a directory
constant uint kInstanceDataless = 1u << 3;   // cloud-only, not stored on this disk
constant uint kInstanceHub = 1u << 4;        // sunburst centre disc

struct FrameUniforms {
    float4 viewport;       // width, height (drawable pixels), backing scale, layer opacity
    float4 transform;      // scale.xy, offset.xy: layout coordinates -> screen pixels
    float4 background;     // canvas color, sRGB
    float4 lightDir;       // xyz: unit vector toward the light, w: ambient term
    float4 lightParams;    // diffuse strength, specular strength, shininess, dim strength
    float4 borderColor;    // rgb + opacity of the 1 px block borders
    float4 hoverRect;      // x, y, w, h in layout coordinates (w <= 0: none)
    float4 selectionRect;  // same, for the selected node
    float4 accentColor;    // selection frame color
    float4 sunburst;       // centre.xy (screen pixels), animation progress, divider width (pixels)
    int4 ids;              // hovered instance, selected instance, dark mode (0/1), unused
};

struct TreemapInstance {
    float4 rect;           // x, y, width, height in drawable pixels, origin top-left
    float4 cushionCoeffs;  // ax, bx, ay, by of the cushion surface, relative to the rect origin
    float4 color;          // base color, sRGB
    uint nodeIdx;
    uint flags;
};

struct SunburstInstance {
    float4 arcFrom;        // start angle, end angle, inner radius, outer radius at progress 0
    float4 arcTo;          // ... at progress 1
    float4 colorFrom;      // rgb + opacity at progress 0
    float4 colorTo;        // rgb + opacity at progress 1
    uint nodeIdx;
    uint flags;
};

static inline float luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

static inline float2 toClipSpace(float2 screenPixels, float2 viewportSize) {
    return float2(screenPixels.x / viewportSize.x * 2.0 - 1.0, 1.0 - screenPixels.y / viewportSize.y * 2.0);
}

/// Pushes a color toward a muted version of itself that sits just above the canvas color.
static inline float3 dimColor(float3 rgb, float3 background, float strength) {
    float3 muted = mix(float3(luminance(rgb)), background, 0.62);
    return mix(rgb, muted, strength);
}

// ---------------------------------------------------------------------------------------
// Squarified cushion treemap
// ---------------------------------------------------------------------------------------

struct TreemapVaryings {
    float4 position [[position]];
    float2 local;                 // position inside the block, in layout pixels
    float2 size [[flat]];         // block size, in layout pixels
    float2 origin [[flat]];       // block origin, in layout pixels
    float4 cushion [[flat]];      // ax, bx, ay, by
    float4 color [[flat]];
    uint flags [[flat]];
    uint instance [[flat]];
};

vertex TreemapVaryings treemap_vertex(uint vertexId [[vertex_id]],
                                      uint instanceId [[instance_id]],
                                      const device TreemapInstance* instances [[buffer(0)]],
                                      constant FrameUniforms& u [[buffer(1)]]) {
    TreemapInstance inst = instances[instanceId];

    // Four vertices of a triangle strip: (0,0) (1,0) (0,1) (1,1).
    float2 corner = float2(float(vertexId & 1u), float(vertexId >> 1u));
    float2 local = corner * inst.rect.zw;
    float2 screen = (inst.rect.xy + local) * u.transform.xy + u.transform.zw;

    TreemapVaryings out;
    out.position = float4(toClipSpace(screen, u.viewport.xy), 0.0, 1.0);
    out.local = local;
    out.size = inst.rect.zw;
    out.origin = inst.rect.xy;
    out.cushion = inst.cushionCoeffs;
    out.color = inst.color;
    out.flags = inst.flags;
    out.instance = instanceId;
    return out;
}

/// Distance in screen pixels from `pos` to the nearest edge of `rect`; negative outside.
static inline float insetDistance(float2 pos, float4 rect, float2 scale) {
    float2 lo = (pos - rect.xy) * scale;
    float2 hi = (rect.xy + rect.zw - pos) * scale;
    return min(min(lo.x, lo.y), min(hi.x, hi.y));
}

fragment float4 treemap_fragment(TreemapVaryings in [[stage_in]], constant FrameUniforms& u [[buffer(1)]]) {
    // --- van Wijk cushion shading -------------------------------------------------------
    // The block carries the coefficients of the surface z = ax*x^2 + bx*x + ay*y^2 + by*y
    // accumulated over all nesting levels. Its normal is analytic: (-dz/dx, -dz/dy, 1).
    float nx = -(2.0 * in.cushion.x * in.local.x + in.cushion.y);
    float ny = -(2.0 * in.cushion.z * in.local.y + in.cushion.w);
    float3 normal = normalize(float3(nx, ny, 1.0));

    float3 light = u.lightDir.xyz;
    float diffuse = max(dot(normal, light), 0.0);
    // Blinn-Phong highlight with the viewer looking straight down at the map.
    float3 halfVector = normalize(light + float3(0.0, 0.0, 1.0));
    float specular = pow(max(dot(normal, halfVector), 0.0), u.lightParams.z) * u.lightParams.y;

    float3 rgb = in.color.rgb * (u.lightDir.w + u.lightParams.x * diffuse) + specular;

    float2 layoutPos = in.origin + in.local;
    float2 screenPos = layoutPos * u.transform.xy + u.transform.zw;
    float backingScale = u.viewport.z;

    // --- Cloud-only files: diagonal hatch (they take no room on this disk) ---------------
    if ((in.flags & kInstanceDataless) != 0u) {
        float stripe = step(0.5, fract((screenPos.x + screenPos.y) / (7.0 * backingScale)));
        rgb = mix(rgb, mix(rgb, u.background.rgb, 0.5), stripe);
    }

    // --- Filter: dim everything that does not match ---------------------------------------
    if ((in.flags & kInstanceDimmed) != 0u) {
        rgb = dimColor(rgb, u.background.rgb, u.lightParams.w);
    }

    // --- Crisp 1 px borders ------------------------------------------------------------------
    // Blocks are snapped to whole pixels, so the last pixel row / column of a block is
    // exactly the set of fragments less than one pixel from its far edges. Very small
    // blocks fade the border out so dense regions do not turn into a dark mesh.
    float2 sizePx = in.size * u.transform.xy;
    float2 farPx = (in.size - in.local) * u.transform.xy;
    float onBorder = step(min(farPx.x, farPx.y), 1.0);
    float borderFade = clamp((min(sizePx.x, sizePx.y) - 2.0) / 4.0, 0.0, 1.0);
    rgb = mix(rgb, u.borderColor.rgb, u.borderColor.a * onBorder * borderFade);

    // --- Hover highlight ---------------------------------------------------------------------
    if (int(in.instance) == u.ids.x) {
        rgb = mix(rgb, float3(1.0), 0.20) + 0.03;
    }
    if (u.hoverRect.z > 0.0) {
        float d = insetDistance(layoutPos, u.hoverRect, u.transform.xy);
        if (d >= 0.0) {
            float frame = 1.0 - smoothstep(backingScale, backingScale + 1.0, d);
            rgb = mix(rgb, float3(1.0), 0.05 + 0.70 * frame);
        }
    }

    // --- Selection frame: accent line with a dark hairline inside for contrast ---------------
    if (u.selectionRect.z > 0.0) {
        float d = insetDistance(layoutPos, u.selectionRect, u.transform.xy);
        if (d >= 0.0) {
            float lineWidth = 1.5 * backingScale;
            float frame = 1.0 - smoothstep(lineWidth, lineWidth + 1.0, d);
            float hairline = (1.0 - smoothstep(lineWidth + 1.0, lineWidth + 2.0, d)) * (1.0 - frame);
            rgb = mix(rgb, float3(0.0), 0.45 * hairline);
            rgb = mix(rgb, u.accentColor.rgb, u.accentColor.a * frame);
        }
    }

    return float4(clamp(rgb, 0.0, 1.0), u.viewport.w);
}

// ---------------------------------------------------------------------------------------
// Sunburst: instanced quads, analytic anti-aliased annular sectors
// ---------------------------------------------------------------------------------------

struct SunburstVaryings {
    float4 position [[position]];
    float2 p;                    // fragment position relative to the centre, screen pixels (y down)
    float4 arc [[flat]];         // start angle, end angle, inner radius, outer radius
    float4 color [[flat]];
    uint flags [[flat]];
    uint instance [[flat]];
};

vertex SunburstVaryings sunburst_vertex(uint vertexId [[vertex_id]],
                                        uint instanceId [[instance_id]],
                                        const device SunburstInstance* instances [[buffer(0)]],
                                        constant FrameUniforms& u [[buffer(1)]]) {
    SunburstInstance inst = instances[instanceId];
    float progress = u.sunburst.z;
    float4 arc = mix(inst.arcFrom, inst.arcTo, progress);
    float4 color = mix(inst.colorFrom, inst.colorTo, progress);

    float midAngle = 0.5 * (arc.x + arc.y);
    float halfSweep = clamp(0.5 * (arc.y - arc.x), 0.0, kPi);
    float inner = max(arc.z, 0.0);
    float outer = max(arc.w, inner);

    // Angles run clockwise from 12 o'clock; with y pointing down that is (sin a, -cos a).
    // The sector is covered by a quad aligned with its bisector, which stays tight for
    // thin slices at any orientation (an axis-aligned box would not).
    float2 axisU = float2(sin(midAngle), -cos(midAngle));
    float2 axisV = float2(cos(midAngle), sin(midAngle));
    float pad = 1.5;  // room for the anti-aliased edge
    float uMin, uMax, vMax;
    if (halfSweep < 0.5 * kPi) {
        uMin = inner * cos(halfSweep) - pad;
        vMax = outer * sin(halfSweep) + pad;
    } else {
        uMin = outer * cos(halfSweep) - pad;
        vMax = outer + pad;
    }
    uMax = outer + pad;

    float2 corner = float2(float(vertexId & 1u), float(vertexId >> 1u));
    float2 p = axisU * mix(uMin, uMax, corner.x) + axisV * mix(-vMax, vMax, corner.y);

    // Nothing to draw: collapse the quad.
    bool empty = (outer - inner) <= 0.0 || halfSweep <= 0.0 || color.a <= 0.0;
    float2 screen = u.sunburst.xy + (empty ? float2(0.0) : p);

    SunburstVaryings out;
    out.position = float4(toClipSpace(screen, u.viewport.xy), 0.0, 1.0);
    out.p = empty ? float2(0.0) : p;
    out.arc = float4(arc.x, arc.y, inner, outer);
    out.color = color;
    out.flags = inst.flags;
    out.instance = instanceId;
    return out;
}

fragment float4 sunburst_fragment(SunburstVaryings in [[stage_in]], constant FrameUniforms& u [[buffer(1)]]) {
    float inner = in.arc.z;
    float outer = in.arc.w;
    float midAngle = 0.5 * (in.arc.x + in.arc.y);
    float halfSweep = 0.5 * (in.arc.y - in.arc.x);
    bool isHub = (in.flags & kInstanceHub) != 0u;

    // Exact polar coordinates of this fragment.
    float radius = length(in.p);
    float theta = atan2(in.p.x, -in.p.y);  // clockwise from 12 o'clock, (-pi, pi]
    float delta = theta - midAngle;        // signed angle from the bisector...
    delta -= kTwoPi * round(delta / kTwoPi);  // ...wrapped to (-pi, pi]

    // Signed distances to the four edges, in pixels, positive inside. The angular one is
    // the perpendicular distance to the nearer radial edge.
    float gap = isHub ? 0.0 : 0.5 * u.sunburst.w;
    float dOuter = outer - radius - gap;
    float dInner = inner > 0.0 ? radius - inner - gap : 1.0e6;
    float dAngle = 1.0e6;
    if (halfSweep < kPi - 1.0e-4) {
        dAngle = radius * sin(clamp(halfSweep - abs(delta), -0.5 * kPi, 0.5 * kPi)) - gap;
    }
    float sd = min(min(dOuter, dInner), dAngle);

    // Analytic anti-aliasing: turn the signed distance into coverage over one pixel.
    float aa = max(fwidth(sd), 1.0e-4);
    float coverage = clamp(sd / aa + 0.5, 0.0, 1.0);
    if (coverage <= 0.0) discard_fragment();

    float3 rgb = in.color.rgb;
    float backingScale = u.viewport.z;
    bool hovered = int(in.instance) == u.ids.x;
    bool selected = int(in.instance) == u.ids.y;

    if (isHub) {
        // Centre disc: flat, with a soft rim; lifts slightly under the cursor.
        float rim = 1.0 - smoothstep(0.0, 2.5 * backingScale, outer - radius);
        rgb = mix(rgb, u.borderColor.rgb, 0.35 * rim);
        if (hovered) rgb = mix(rgb, u.accentColor.rgb, 0.16);
    } else {
        // Subtle radial bevel so each ring reads as a raised band.
        float across = clamp((radius - inner) / max(outer - inner, 1.0e-3), 0.0, 1.0);
        rgb *= mix(1.07, 0.90, across * across);

        if ((in.flags & kInstanceAggregate) != 0u) {
            // "Other smaller items": concentric hatch marks it as a stand-in.
            float stripe = step(0.5, fract(radius / (5.0 * backingScale)));
            rgb = mix(rgb, mix(rgb, u.background.rgb, 0.35), stripe);
        }
        if ((in.flags & kInstanceDataless) != 0u) {
            float stripe = step(0.5, fract((in.p.x + in.p.y) / (7.0 * backingScale)));
            rgb = mix(rgb, mix(rgb, u.background.rgb, 0.5), stripe);
        }
        if ((in.flags & kInstanceDimmed) != 0u) {
            rgb = dimColor(rgb, u.background.rgb, u.lightParams.w);
        }
        if (hovered) {
            rgb = mix(rgb, float3(1.0), 0.24) + 0.03;
        }
        if (selected) {
            float lineWidth = 1.5 * backingScale;
            float frame = 1.0 - smoothstep(lineWidth, lineWidth + 1.0, sd);
            rgb = mix(rgb, u.accentColor.rgb, u.accentColor.a * frame);
        }
    }

    return float4(clamp(rgb, 0.0, 1.0), coverage * in.color.a * u.viewport.w);
}
