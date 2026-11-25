// Metal shader compilation, GPU struct layout and off-screen rendering tests.
//
// With a Metal device the shaders are compiled exactly as the app does it (from source,
// at run time), both pipelines are built and real frames are rendered into off-screen
// textures whose pixels are then inspected. On a machine without a Metal device (some
// headless CI VMs) the GPU cases log that fact and fall back to validating that the
// shader source is present and structurally sound.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "TestHarness.hpp"

#import "DiskMapRenderer.h"
#include "FlatArena.hpp"
#include "GpuInstances.hpp"
#include "MetalTypes.h"
#include "NodeFilter.hpp"
#include "SunburstLayout.hpp"
#include "SunburstTransition.hpp"
#include "TreemapLayout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#ifndef DP_SHADER_SOURCE_PATH
#error "DP_SHADER_SOURCE_PATH must point at src/gpu/Shaders.metal"
#endif

using namespace diskprism;
using namespace diskprism::gpu;

namespace {

NSString *shaderPath() {
    return @DP_SHADER_SOURCE_PATH;
}

NSString *loadShaderSource() {
    NSError *error = nil;
    NSString *source = [DPMetalRenderer shaderSourceAtPath:shaderPath() error:&error];
    if (source == nil) std::fprintf(stderr, "    %s\n", error.localizedDescription.UTF8String);
    return source;
}

id<MTLDevice> deviceOrLog() {
    static id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    static bool logged = false;
    if (device == nil && !logged) {
        std::printf("      NOTE: no Metal device is available in this environment (headless VM?).\n"
                    "            GPU cases validate shader presence and structure only.\n");
        logged = true;
    }
    return device;
}

DPMetalRenderer *makeRenderer() {
    id<MTLDevice> device = deviceOrLog();
    if (device == nil) return nil;
    NSString *source = loadShaderSource();
    if (source == nil) return nil;
    NSError *error = nil;
    DPMetalRenderer *renderer = [[DPMetalRenderer alloc] initWithDevice:device
                                                           shaderSource:source
                                                            pixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  error:&error];
    if (renderer == nil) std::fprintf(stderr, "    %s\n", error.localizedDescription.UTF8String);
    return renderer;
}

struct Pixel {
    double r, g, b, a;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;
    double gpuMilliseconds = 0;

    [[nodiscard]] Pixel at(int x, int y) const {
        const uint8_t* p = &bgra[(static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4];
        return Pixel{p[2] / 255.0, p[1] / 255.0, p[0] / 255.0, p[3] / 255.0};
    }
};

double distance(const Pixel& a, const Pixel& b) {
    return std::sqrt((a.r - b.r) * (a.r - b.r) + (a.g - b.g) * (a.g - b.g) + (a.b - b.b) * (a.b - b.b));
}
double distance(const Pixel& a, simd_float4 b) {
    return distance(a, Pixel{b.x, b.y, b.z, 1.0});
}
double brightness(const Pixel& p) {
    return 0.2126 * p.r + 0.7152 * p.g + 0.0722 * p.b;
}
double saturation(const Pixel& p) {
    const double hi = std::max({p.r, p.g, p.b});
    const double lo = std::min({p.r, p.g, p.b});
    return hi <= 0 ? 0 : (hi - lo) / hi;
}
/// Cosine of the angle between two colors seen as vectors: 1 means "same hue".
double hueSimilarity(const Pixel& p, simd_float4 base) {
    const double dot = p.r * base.x + p.g * base.y + p.b * base.z;
    const double lp = std::sqrt(p.r * p.r + p.g * p.g + p.b * p.b);
    const double lb = std::sqrt(static_cast<double>(base.x) * base.x + static_cast<double>(base.y) * base.y +
                                static_cast<double>(base.z) * base.z);
    return (lp <= 0 || lb <= 0) ? 0 : dot / (lp * lb);
}

/// Renders one frame into an off-screen texture and reads it back.
Image renderOffscreen(DPMetalRenderer *renderer, int width, int height, simd_float4 clear,
                      void (^draw)(id<MTLRenderCommandEncoder> encoder)) {
    Image image;
    image.width = width;
    image.height = height;
    image.bgra.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);

    id<MTLTexture> target = [renderer newReadableRenderTargetWithWidth:static_cast<NSUInteger>(width)
                                                               height:static_cast<NSUInteger>(height)];
    if (target == nil) return image;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(clear.x, clear.y, clear.z, 1.0);

    id<MTLCommandBuffer> commandBuffer = [renderer.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    draw(encoder);
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    image.gpuMilliseconds = (commandBuffer.GPUEndTime - commandBuffer.GPUStartTime) * 1000.0;
    [renderer readPixelsFromTexture:target intoBytes:image.bgra.data()];
    return image;
}

NodeInit dirInit() {
    NodeInit init;
    init.flags = kNodeDir;
    return init;
}
NodeInit fileInit(uint64_t bytes) {
    NodeInit init;
    init.allocated_bytes = bytes;
    init.logical_bytes = bytes;
    return init;
}

/// Member offset inside a reflected struct, or -1 if the member does not exist.
long memberOffset(MTLStructType *type, NSString *name) {
    MTLStructMember *member = [type memberByName:name];
    return member == nil ? -1 : static_cast<long>(member.offset);
}

id<MTLBufferBinding> bufferBinding(NSArray<id<MTLBinding>> *bindings, NSUInteger index) {
    for (id<MTLBinding> binding in bindings) {
        if (binding.type == MTLBindingTypeBuffer && binding.index == index) return (id<MTLBufferBinding>)binding;
    }
    return nil;
}

MTLStructType *structTypeOf(id<MTLBufferBinding> binding) {
    if (binding == nil) return nil;
    if (binding.bufferStructType != nil) return binding.bufferStructType;
    return binding.bufferPointerType.elementStructType;
}

}  // namespace

// ----------------------------------------------------------------------------------------
// Shader source
// ----------------------------------------------------------------------------------------

DP_TEST(shader_source_is_present_and_self_contained) {
    NSString *source = loadShaderSource();
    REQUIRE(source != nil);
    CHECK(source.length > 2000);
    // All four entry points exist, with the right stage qualifiers.
    for (NSString *declaration in @[
             @"vertex TreemapVaryings treemap_vertex(", @"fragment float4 treemap_fragment(",
             @"vertex SunburstVaryings sunburst_vertex(", @"fragment float4 sunburst_fragment("
         ]) {
        CHECK([source containsString:declaration]);
    }
    // Compiled from a string at run time: it may only include the Metal standard library.
    NSRegularExpression *include = [NSRegularExpression regularExpressionWithPattern:@"^\\s*#\\s*include\\s+(.*)$"
                                                                             options:NSRegularExpressionAnchorsMatchLines
                                                                               error:nil];
    __block int includes = 0;
    __block bool onlyStdlib = true;
    [include enumerateMatchesInString:source
                              options:0
                                range:NSMakeRange(0, source.length)
                           usingBlock:^(NSTextCheckingResult *match, NSMatchingFlags, BOOL *) {
                               ++includes;
                               NSString *what = [source substringWithRange:[match rangeAtIndex:1]];
                               if (![what containsString:@"<metal_stdlib>"]) onlyStdlib = false;
                           }];
    CHECK_EQ(includes, 1);
    CHECK(onlyStdlib);
    // Structurally sound even without a compiler: balanced braces and parentheses
    // (comments are prose and are ignored).
    NSRegularExpression *comment = [NSRegularExpression regularExpressionWithPattern:@"//.*$"
                                                                             options:NSRegularExpressionAnchorsMatchLines
                                                                               error:nil];
    NSString *code = [comment stringByReplacingMatchesInString:source options:0 range:NSMakeRange(0, source.length) withTemplate:@""];
    long braces = 0, parens = 0;
    bool neverNegative = true;
    for (NSUInteger i = 0; i < code.length; ++i) {
        const unichar c = [code characterAtIndex:i];
        if (c == '{') ++braces;
        if (c == '}') --braces;
        if (c == '(') ++parens;
        if (c == ')') --parens;
        if (braces < 0 || parens < 0) neverNegative = false;
    }
    CHECK_EQ(braces, 0l);
    CHECK_EQ(parens, 0l);
    CHECK(neverNegative);

    // A missing file is reported with the paths that were tried.
    NSError *error = nil;
    CHECK([DPMetalRenderer shaderSourceAtPath:@"/nonexistent/Shaders.metal" error:&error] == nil);
    REQUIRE(error != nil);
    CHECK(error.code == DPRendererErrorShaderSourceMissing);
    CHECK([error.localizedDescription containsString:@"/nonexistent/Shaders.metal"]);
}

DP_TEST(shaders_compile_at_runtime_with_zero_errors) {
    id<MTLDevice> device = deviceOrLog();
    if (device == nil) return;
    NSString *source = loadShaderSource();
    REQUIRE(source != nil);

    // Compile exactly as the app does: from source text, no offline toolchain.
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:[[MTLCompileOptions alloc] init] error:&error];
    if (library == nil) std::fprintf(stderr, "    %s\n", error.localizedDescription.UTF8String);
    REQUIRE(library != nil);
    // On success Metal reports warnings (if any) through `error`: there must be none.
    if (error != nil) std::fprintf(stderr, "    compiler diagnostics: %s\n", error.localizedDescription.UTF8String);
    CHECK(error == nil);

    NSArray<NSString *> *names = library.functionNames;
    CHECK_EQ(names.count, NSUInteger{4});
    struct Expected {
        const char* name;
        MTLFunctionType type;
    };
    for (const Expected& expected : {Expected{kTreemapVertexFunction, MTLFunctionTypeVertex},
                                     Expected{kTreemapFragmentFunction, MTLFunctionTypeFragment},
                                     Expected{kSunburstVertexFunction, MTLFunctionTypeVertex},
                                     Expected{kSunburstFragmentFunction, MTLFunctionTypeFragment}}) {
        id<MTLFunction> function = [library newFunctionWithName:@(expected.name)];
        REQUIRE(function != nil);
        CHECK(function.functionType == expected.type);
    }

    // And both render pipelines link (vertex outputs match fragment inputs).
    DPMetalRenderer *renderer = makeRenderer();
    REQUIRE(renderer != nil);
    CHECK(renderer.treemapPipeline != nil);
    CHECK(renderer.sunburstPipeline != nil);
    CHECK(renderer.setupMilliseconds > 0);
    std::printf("      device: %s — library + 2 pipelines built in %.1f ms\n", device.name.UTF8String,
                renderer.setupMilliseconds);
}

DP_TEST(shader_errors_are_reported_with_compiler_diagnostics) {
    id<MTLDevice> device = deviceOrLog();
    if (device == nil) return;
    NSString *source = loadShaderSource();
    REQUIRE(source != nil);

    // A syntax error: the compiler's message (with its line number) must reach the caller.
    NSString *broken = [source stringByReplacingOccurrencesOfString:@"float3 light = u.lightDir.xyz;"
                                                         withString:@"float3 light = u.lightDir.xyz"];
    REQUIRE(![broken isEqualToString:source]);
    NSError *error = nil;
    DPMetalRenderer *renderer = [[DPMetalRenderer alloc] initWithDevice:device
                                                           shaderSource:broken
                                                            pixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  error:&error];
    CHECK(renderer == nil);
    REQUIRE(error != nil);
    CHECK([error.domain isEqualToString:DPRendererErrorDomain]);
    CHECK(error.code == DPRendererErrorShaderCompileFailed);
    CHECK([error.localizedDescription containsString:@"error"]);
    CHECK([error.localizedDescription rangeOfString:@":[0-9]+:[0-9]+:" options:NSRegularExpressionSearch].location != NSNotFound);
    CHECK(error.userInfo[NSUnderlyingErrorKey] != nil);

    // A missing entry point is named explicitly.
    NSString *renamed = [source stringByReplacingOccurrencesOfString:@"sunburst_fragment(" withString:@"sunburst_frag("];
    error = nil;
    renderer = [[DPMetalRenderer alloc] initWithDevice:device shaderSource:renamed pixelFormat:MTLPixelFormatBGRA8Unorm error:&error];
    CHECK(renderer == nil);
    REQUIRE(error != nil);
    CHECK(error.code == DPRendererErrorEntryPointMissing);
    CHECK([error.localizedDescription containsString:@"sunburst_fragment"]);

    // Garbage is rejected, not crashed on.
    error = nil;
    renderer = [[DPMetalRenderer alloc] initWithDevice:device shaderSource:@"this is not a shader" pixelFormat:MTLPixelFormatBGRA8Unorm error:&error];
    CHECK(renderer == nil);
    CHECK(error != nil);
}

DP_TEST(cpu_struct_layout_matches_shader_reflection) {
    DPMetalRenderer *renderer = makeRenderer();
    if (renderer == nil) {
        CHECK(deviceOrLog() == nil);  // only acceptable without a device
        return;
    }

    // Treemap instances (buffer 0 of the vertex stage).
    id<MTLBufferBinding> treemapInstances = bufferBinding(renderer.treemapReflection.vertexBindings, kBufferIndexInstances);
    REQUIRE(treemapInstances != nil);
    MTLStructType *treemapType = structTypeOf(treemapInstances);
    REQUIRE(treemapType != nil);
    CHECK_EQ(treemapInstances.bufferDataSize, NSUInteger{sizeof(TreemapInstance)});
    CHECK_EQ(memberOffset(treemapType, @"rect"), static_cast<long>(offsetof(TreemapInstance, rect)));
    CHECK_EQ(memberOffset(treemapType, @"cushionCoeffs"), static_cast<long>(offsetof(TreemapInstance, cushionCoeffs)));
    CHECK_EQ(memberOffset(treemapType, @"color"), static_cast<long>(offsetof(TreemapInstance, color)));
    CHECK_EQ(memberOffset(treemapType, @"nodeIdx"), static_cast<long>(offsetof(TreemapInstance, nodeIdx)));
    CHECK_EQ(memberOffset(treemapType, @"flags"), static_cast<long>(offsetof(TreemapInstance, flags)));

    // Sunburst instances.
    id<MTLBufferBinding> sunburstInstances = bufferBinding(renderer.sunburstReflection.vertexBindings, kBufferIndexInstances);
    REQUIRE(sunburstInstances != nil);
    MTLStructType *sunburstType = structTypeOf(sunburstInstances);
    REQUIRE(sunburstType != nil);
    CHECK_EQ(sunburstInstances.bufferDataSize, NSUInteger{sizeof(SunburstInstance)});
    CHECK_EQ(memberOffset(sunburstType, @"arcFrom"), static_cast<long>(offsetof(SunburstInstance, arcFrom)));
    CHECK_EQ(memberOffset(sunburstType, @"arcTo"), static_cast<long>(offsetof(SunburstInstance, arcTo)));
    CHECK_EQ(memberOffset(sunburstType, @"colorFrom"), static_cast<long>(offsetof(SunburstInstance, colorFrom)));
    CHECK_EQ(memberOffset(sunburstType, @"colorTo"), static_cast<long>(offsetof(SunburstInstance, colorTo)));
    CHECK_EQ(memberOffset(sunburstType, @"nodeIdx"), static_cast<long>(offsetof(SunburstInstance, nodeIdx)));
    CHECK_EQ(memberOffset(sunburstType, @"flags"), static_cast<long>(offsetof(SunburstInstance, flags)));

    // Frame uniforms, as seen by all four functions.
    for (NSArray<id<MTLBinding>> *bindings in @[
             renderer.treemapReflection.vertexBindings, renderer.treemapReflection.fragmentBindings,
             renderer.sunburstReflection.vertexBindings, renderer.sunburstReflection.fragmentBindings
         ]) {
        id<MTLBufferBinding> uniforms = bufferBinding(bindings, kBufferIndexUniforms);
        REQUIRE(uniforms != nil);
        MTLStructType *type = structTypeOf(uniforms);
        REQUIRE(type != nil);
        CHECK_EQ(uniforms.bufferDataSize, NSUInteger{sizeof(FrameUniforms)});
        CHECK_EQ(memberOffset(type, @"viewport"), static_cast<long>(offsetof(FrameUniforms, viewport)));
        CHECK_EQ(memberOffset(type, @"transform"), static_cast<long>(offsetof(FrameUniforms, transform)));
        CHECK_EQ(memberOffset(type, @"background"), static_cast<long>(offsetof(FrameUniforms, background)));
        CHECK_EQ(memberOffset(type, @"lightDir"), static_cast<long>(offsetof(FrameUniforms, lightDir)));
        CHECK_EQ(memberOffset(type, @"lightParams"), static_cast<long>(offsetof(FrameUniforms, lightParams)));
        CHECK_EQ(memberOffset(type, @"borderColor"), static_cast<long>(offsetof(FrameUniforms, borderColor)));
        CHECK_EQ(memberOffset(type, @"hoverRect"), static_cast<long>(offsetof(FrameUniforms, hoverRect)));
        CHECK_EQ(memberOffset(type, @"selectionRect"), static_cast<long>(offsetof(FrameUniforms, selectionRect)));
        CHECK_EQ(memberOffset(type, @"accentColor"), static_cast<long>(offsetof(FrameUniforms, accentColor)));
        CHECK_EQ(memberOffset(type, @"sunburst"), static_cast<long>(offsetof(FrameUniforms, sunburst)));
        CHECK_EQ(memberOffset(type, @"ids"), static_cast<long>(offsetof(FrameUniforms, ids)));
    }
}

// ----------------------------------------------------------------------------------------
// Rendering
// ----------------------------------------------------------------------------------------

DP_TEST(treemap_renders_cushions_borders_hover_and_dimming) {
    DPMetalRenderer *renderer = makeRenderer();
    if (renderer == nil) {
        CHECK(deviceOrLog() == nil);
        return;
    }
    // Two equally large files of different types: two 120 x 120 blocks side by side.
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/gpu");
    const uint32_t movie = arena.addChild(root, "movie.mov", fileInit(1'000'000));
    const uint32_t photo = arena.addChild(root, "photo.jpg", fileInit(1'000'000));
    arena.finalize();

    constexpr int kWidth = 240, kHeight = 120;
    TreemapParams params;
    params.width = kWidth;
    params.height = kHeight;
    TreemapLayout layout;
    layout.build(arena, root, params);
    REQUIRE(layout.rects().size() == 2);
    const int32_t movieIndex = layout.rectIndexForNode(movie);
    const int32_t photoIndex = layout.rectIndexForNode(photo);
    REQUIRE(movieIndex >= 0 && photoIndex >= 0);
    const TreemapRect movieRect = layout.rects()[static_cast<size_t>(movieIndex)];
    const TreemapRect photoRect = layout.rects()[static_cast<size_t>(photoIndex)];

    std::vector<TreemapInstance> instances;
    buildTreemapInstances(layout, instances);
    id<MTLBuffer> buffer = [renderer newBufferWithBytes:instances.data()
                                                 length:instances.size() * sizeof(TreemapInstance)
                                                  label:@"test"];
    REQUIRE(buffer != nil);

    const Theme theme = Theme::standard(true);
    const FrameUniforms base = makeFrameUniforms(kWidth, kHeight, 1.0f, theme);
    const auto render = [&](const FrameUniforms& uniforms) {
        return renderOffscreen(renderer, kWidth, kHeight, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
            [renderer drawTreemapWithEncoder:encoder instances:buffer count:instances.size() uniforms:uniforms];
        });
    };
    const Image image = render(base);

    const auto centre = [](const TreemapRect& r) { return std::make_pair(static_cast<int>(r.x + r.w / 2), static_cast<int>(r.y + r.h / 2)); };
    const auto [mx, my] = centre(movieRect);
    const auto [px, py] = centre(photoRect);
    const simd_float4 movieColor = toFloat4(movieRect.color);
    const simd_float4 photoColor = toFloat4(photoRect.color);

    // Each block shows its own file-type hue; nothing of the canvas shows through.
    CHECK(hueSimilarity(image.at(mx, my), movieColor) > 0.985);
    CHECK(hueSimilarity(image.at(px, py), photoColor) > 0.985);
    CHECK(distance(image.at(mx, my), image.at(px, py)) > 0.15);
    int backgroundPixels = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (distance(image.at(x, y), theme.background) < 0.02) ++backgroundPixels;
        }
    }
    CHECK_EQ(backgroundPixels, 0);

    // Cushion shading: lit from the upper left, so within one block the upper-left region
    // is brighter than the lower-right, and the block is brightest away from its edges.
    const int x0 = static_cast<int>(movieRect.x), y0 = static_cast<int>(movieRect.y);
    const int w = static_cast<int>(movieRect.w), h = static_cast<int>(movieRect.h);
    const double upperLeft = brightness(image.at(x0 + w / 4, y0 + h / 4));
    const double lowerRight = brightness(image.at(x0 + 3 * w / 4, y0 + 3 * h / 4));
    const double corner = brightness(image.at(x0 + w - 6, y0 + h - 6));
    CHECK(upperLeft > lowerRight + 0.03);
    CHECK(lowerRight > corner + 0.02);
    // The gradient is smooth (a continuous surface, not flat color).
    int distinctLevels = 0;
    double previous = -1;
    for (int x = x0 + 4; x < x0 + w - 4; ++x) {
        const double value = brightness(image.at(x, y0 + h / 2));
        if (std::fabs(value - previous) > 1e-4) ++distinctLevels;
        previous = value;
    }
    CHECK(distinctLevels > 20);

    // Crisp 1 px border: the last pixel column and row of a block are darkened, the
    // pixels just inside them are not.
    const double lastColumn = brightness(image.at(x0 + w - 1, y0 + h / 2));
    const double beforeLast = brightness(image.at(x0 + w - 2, y0 + h / 2));
    const double lastRow = brightness(image.at(x0 + w / 2, y0 + h - 1));
    const double beforeLastRow = brightness(image.at(x0 + w / 2, y0 + h - 2));
    CHECK(lastColumn < beforeLast * 0.8);
    CHECK(lastRow < beforeLastRow * 0.8);
    CHECK(std::fabs(brightness(image.at(x0 + w - 3, y0 + h / 2)) - beforeLast) < 0.03);

    // Hover: the hovered instance is brighter, the other block is untouched.
    FrameUniforms hover = base;
    hover.ids.x = movieIndex;
    const Image hovered = render(hover);
    CHECK(brightness(hovered.at(mx, my)) > brightness(image.at(mx, my)) + 0.05);
    CHECK(distance(hovered.at(px, py), image.at(px, py)) < 1e-3);

    // Selection frame in the accent color just inside the selected rectangle.
    FrameUniforms selected = base;
    selected.selectionRect = simd_float4{photoRect.x, photoRect.y, photoRect.w, photoRect.h};
    const Image selection = render(selected);
    const int sx = static_cast<int>(photoRect.x);
    const int sy = static_cast<int>(photoRect.y + photoRect.h / 2);
    CHECK(distance(selection.at(sx, sy), theme.accent) < 0.08);
    CHECK(distance(selection.at(px, py), image.at(px, py)) < 1e-3);          // interior unchanged
    CHECK(distance(selection.at(mx, my), image.at(mx, my)) < 1e-3);          // other block unchanged

    // Filter: a dimmed block loses its saturation and brightness, a matching one keeps both.
    NodeMatchSet matches;
    FilterQuery query;
    query.extension_id = *arena.extensions().find("mov");
    REQUIRE(matches.compute(arena, query));
    params.matches = &matches;
    layout.build(arena, root, params);
    buildTreemapInstances(layout, instances);
    buffer = [renderer newBufferWithBytes:instances.data() length:instances.size() * sizeof(TreemapInstance) label:@"test"];
    const Image filtered = render(base);
    CHECK(distance(filtered.at(mx, my), image.at(mx, my)) < 1e-3);
    CHECK(saturation(filtered.at(px, py)) < saturation(image.at(px, py)) * 0.5);
    CHECK(brightness(filtered.at(px, py)) < brightness(image.at(px, py)) * 0.7);

    // Light appearance uses a different canvas and lighting but the same geometry.
    const Theme light = Theme::standard(false);
    const FrameUniforms lightUniforms = makeFrameUniforms(kWidth, kHeight, 1.0f, light);
    params.matches = nullptr;
    layout.build(arena, root, params);
    buildTreemapInstances(layout, instances);
    buffer = [renderer newBufferWithBytes:instances.data() length:instances.size() * sizeof(TreemapInstance) label:@"test"];
    const Image lightImage = render(lightUniforms);
    CHECK(hueSimilarity(lightImage.at(mx, my), movieColor) > 0.985);
    CHECK(brightness(lightImage.at(mx, my)) > brightness(image.at(mx, my)));  // more ambient light
}

DP_TEST(treemap_layers_can_be_transformed_and_faded) {
    DPMetalRenderer *renderer = makeRenderer();
    if (renderer == nil) {
        CHECK(deviceOrLog() == nil);
        return;
    }
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/layers");
    arena.addChild(root, "only.mov", fileInit(1000));
    arena.finalize();
    constexpr int kSize = 100;
    TreemapParams params;
    params.width = kSize;
    params.height = kSize;
    TreemapLayout layout;
    layout.build(arena, root, params);
    std::vector<TreemapInstance> instances;
    buildTreemapInstances(layout, instances);
    REQUIRE(instances.size() == 1);
    id<MTLBuffer> buffer = [renderer newBufferWithBytes:instances.data() length:sizeof(TreemapInstance) label:@"test"];
    const Theme theme = Theme::standard(true);

    // Scale the layer to the lower-right quadrant: the other quadrants show the canvas.
    FrameUniforms uniforms = makeFrameUniforms(kSize, kSize, 1.0f, theme);
    uniforms.transform = simd_float4{0.5f, 0.5f, 50.0f, 50.0f};
    const Image scaled = renderOffscreen(renderer, kSize, kSize, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
        [renderer drawTreemapWithEncoder:encoder instances:buffer count:1 uniforms:uniforms];
    });
    CHECK(distance(scaled.at(25, 25), theme.background) < 0.01);
    CHECK(distance(scaled.at(75, 25), theme.background) < 0.01);
    CHECK(distance(scaled.at(25, 75), theme.background) < 0.01);
    CHECK(distance(scaled.at(75, 75), theme.background) > 0.2);
    CHECK(distance(scaled.at(49, 49), theme.background) < 0.01);  // pixel-exact edge
    CHECK(distance(scaled.at(51, 51), theme.background) > 0.1);

    // Half-transparent layer: exactly halfway between the canvas and the opaque result.
    FrameUniforms opaque = makeFrameUniforms(kSize, kSize, 1.0f, theme);
    FrameUniforms faded = opaque;
    faded.viewport.w = 0.5f;
    const Image full = renderOffscreen(renderer, kSize, kSize, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
        [renderer drawTreemapWithEncoder:encoder instances:buffer count:1 uniforms:opaque];
    });
    const Image half = renderOffscreen(renderer, kSize, kSize, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
        [renderer drawTreemapWithEncoder:encoder instances:buffer count:1 uniforms:faded];
    });
    const Pixel a = full.at(50, 50);
    const Pixel b = half.at(50, 50);
    CHECK_NEAR(b.r, (a.r + theme.background.x) / 2, 0.012);
    CHECK_NEAR(b.g, (a.g + theme.background.y) / 2, 0.012);
    CHECK_NEAR(b.b, (a.b + theme.background.z) / 2, 0.012);
}

DP_TEST(sunburst_renders_antialiased_arcs_hub_and_animation) {
    DPMetalRenderer *renderer = makeRenderer();
    if (renderer == nil) {
        CHECK(deviceOrLog() == nil);
        return;
    }
    // 75 % / 25 %: the big slice covers 12 o'clock -> 9 o'clock clockwise.
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/sun");
    const uint32_t big = arena.addChild(root, "big.mov", fileInit(750));
    const uint32_t small = arena.addChild(root, "small.jpg", fileInit(250));
    arena.finalize();

    constexpr int kSize = 400;
    SunburstParams params;
    params.width = kSize;
    params.height = kSize;
    params.max_depth = 3;
    SunburstLayout layout;
    layout.build(arena, root, params);
    REQUIRE(layout.arcs().size() == 2);

    const Theme theme = Theme::standard(true);
    const SunburstTransition steady = buildSunburstSteadyState(layout, theme.hubColor());
    std::vector<SunburstInstance> instances;
    buildSunburstInstances(steady, instances);
    REQUIRE(instances.size() == 3);
    id<MTLBuffer> buffer = [renderer newBufferWithBytes:instances.data() length:instances.size() * sizeof(SunburstInstance) label:@"test"];
    FrameUniforms uniforms = makeFrameUniforms(kSize, kSize, 1.0f, theme);
    const auto render = [&](const FrameUniforms& u, id<MTLBuffer> instanceBuffer, NSUInteger count) {
        return renderOffscreen(renderer, kSize, kSize, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
            [renderer drawSunburstWithEncoder:encoder instances:instanceBuffer count:count uniforms:u];
        });
    };
    const Image image = render(uniforms, buffer, instances.size());

    const float cx = layout.centerX(), cy = layout.centerY();
    const SunburstArc bigArc = layout.arcs()[static_cast<size_t>(layout.arcIndexForNode(big))];
    const SunburstArc smallArc = layout.arcs()[static_cast<size_t>(layout.arcIndexForNode(small))];
    const auto pointOn = [&](const SunburstArc& arc, double angleFraction, double radiusFraction) {
        const double theta = arc.start_angle + (arc.end_angle - arc.start_angle) * angleFraction;
        const double radius = arc.inner_radius + (arc.outer_radius - arc.inner_radius) * radiusFraction;
        return std::make_pair(static_cast<int>(std::lround(cx + radius * std::sin(theta))),
                              static_cast<int>(std::lround(cy - radius * std::cos(theta))));
    };

    // Hub in the middle, arcs in their file-type colors, canvas in the corners and
    // outside the first ring (only one of three rings is populated).
    CHECK(distance(image.at(kSize / 2, kSize / 2), toFloat4(theme.hubColor())) < 0.03);
    const auto [bx, by] = pointOn(bigArc, 0.5, 0.5);
    const auto [sx, sy] = pointOn(smallArc, 0.5, 0.5);
    CHECK(hueSimilarity(image.at(bx, by), toFloat4(bigArc.color)) > 0.985);
    CHECK(hueSimilarity(image.at(sx, sy), toFloat4(smallArc.color)) > 0.985);
    CHECK(distance(image.at(bx, by), image.at(sx, sy)) > 0.15);
    CHECK(distance(image.at(3, 3), theme.background) < 0.01);
    CHECK(distance(image.at(kSize - 4, kSize - 4), theme.background) < 0.01);
    const auto [ox, oy] = pointOn(bigArc, 0.5, 2.2);  // well outside ring 1
    CHECK(distance(image.at(ox, oy), theme.background) < 0.01);

    // Geometry: 2 o'clock belongs to the big slice, 10:30 to the small one.
    const int probe = static_cast<int>((bigArc.inner_radius + bigArc.outer_radius) / 2);
    const Pixel rightSide = image.at(static_cast<int>(cx) + probe, static_cast<int>(cy));          // 3 o'clock
    const Pixel upperLeft = image.at(static_cast<int>(cx - probe * 0.7071), static_cast<int>(cy - probe * 0.7071));
    CHECK(hueSimilarity(rightSide, toFloat4(bigArc.color)) > 0.985);
    CHECK(hueSimilarity(upperLeft, toFloat4(smallArc.color)) > 0.985);

    // Analytic anti-aliasing: along the outer rim there are pixels partially blended
    // between the arc color and the canvas (a hard-edged rasteriser would produce none).
    int blended = 0, solid = 0;
    const Pixel arcPixel = image.at(static_cast<int>(cx) + probe, static_cast<int>(cy));
    for (int step = 0; step < 720; ++step) {
        const double theta = step * 3.14159265358979 / 720.0 + 0.05;  // inside the big slice
        for (double offset = -1.5; offset <= 1.5; offset += 0.5) {
            const double radius = bigArc.outer_radius + offset;
            const Pixel p = image.at(static_cast<int>(std::floor(cx + radius * std::sin(theta))),
                                     static_cast<int>(std::floor(cy - radius * std::cos(theta))));
            const double toArc = distance(p, arcPixel);
            const double toCanvas = distance(p, theme.background);
            if (toCanvas > 0.06 && toArc > 0.10) ++blended;
            if (toCanvas <= 0.06 || toArc <= 0.10) ++solid;
        }
    }
    CHECK(blended > 150);
    CHECK(solid > 1000);
    // A thin divider separates the two slices: on the boundary the color dips toward the canvas.
    const double boundary = bigArc.end_angle;
    const auto [dx, dy] = std::make_pair(static_cast<int>(std::lround(cx + probe * std::sin(boundary))),
                                         static_cast<int>(std::lround(cy - probe * std::cos(boundary))));
    double darkest = 1e9;
    for (int ox2 = -2; ox2 <= 2; ++ox2) {
        for (int oy2 = -2; oy2 <= 2; ++oy2) darkest = std::min(darkest, brightness(image.at(dx + ox2, dy + oy2)));
    }
    CHECK(darkest < std::min(brightness(image.at(bx, by)), brightness(image.at(sx, sy))) - 0.08);

    // Hover brightens exactly one instance; selection draws an accent rim.
    FrameUniforms hover = uniforms;
    hover.ids.x = static_cast<int>(steady.first_layout_arc) + layout.arcIndexForNode(small);
    const Image hovered = render(hover, buffer, instances.size());
    CHECK(brightness(hovered.at(sx, sy)) > brightness(image.at(sx, sy)) + 0.05);
    CHECK(distance(hovered.at(bx, by), image.at(bx, by)) < 1e-3);
    FrameUniforms hubHover = uniforms;
    hubHover.ids.x = static_cast<int>(steady.hub_index);
    CHECK(distance(render(hubHover, buffer, instances.size()).at(kSize / 2, kSize / 2), image.at(kSize / 2, kSize / 2)) > 0.02);

    // Animation: a "bloom" transition starts with every arc collapsed at 12 o'clock…
    SunburstLayout empty;
    const SunburstTransition bloom = buildSunburstTransition(arena, empty, layout, theme.hubColor());
    REQUIRE(bloom.kind == SunburstTransitionKind::Bloom);
    std::vector<SunburstInstance> bloomInstances;
    buildSunburstInstances(bloom, bloomInstances);
    id<MTLBuffer> bloomBuffer = [renderer newBufferWithBytes:bloomInstances.data()
                                                      length:bloomInstances.size() * sizeof(SunburstInstance)
                                                       label:@"test"];
    FrameUniforms animated = uniforms;
    animated.sunburst.z = 0.0f;
    const Image startFrame = render(animated, bloomBuffer, bloomInstances.size());
    CHECK(distance(startFrame.at(bx, by), theme.background) < 0.01);
    CHECK(distance(startFrame.at(sx, sy), theme.background) < 0.01);
    // …half way the big slice has swept half of its final angle (and is half faded in)…
    animated.sunburst.z = 0.5f;
    const Image midFrame = render(animated, bloomBuffer, bloomInstances.size());
    const auto [qx, qy] = pointOn(bigArc, 0.25, 0.5);
    const auto [farX, farY] = pointOn(bigArc, 0.75, 0.5);
    CHECK(distance(midFrame.at(qx, qy), theme.background) > 0.05);
    CHECK(distance(midFrame.at(farX, farY), theme.background) < 0.01);
    // …and at progress 1 the frame is identical to the steady state.
    animated.sunburst.z = 1.0f;
    const Image endFrame = render(animated, bloomBuffer, bloomInstances.size());
    double worst = 0;
    for (int y = 0; y < kSize; y += 3) {
        for (int x = 0; x < kSize; x += 3) worst = std::max(worst, distance(endFrame.at(x, y), image.at(x, y)));
    }
    CHECK(worst < 0.01);
}

DP_TEST(large_scenes_render_in_a_single_instanced_draw) {
    DPMetalRenderer *renderer = makeRenderer();
    if (renderer == nil) {
        CHECK(deviceOrLog() == nil);
        return;
    }
    // A synthetic disk with 400 000 files in 40 000 directories.
    constexpr uint32_t kNodes = 440000;
    FlatArena arena(ArenaConfig{kNodes + 16, 64u << 20});
    uint64_t state = 0x853c49e6748fea9bull;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    static const char* const kExtensions[] = {".mov", ".jpg", ".txt", ".dylib", "", ".zip", ".log", ".o", ".png", ".db", ".h", ".car"};
    std::vector<uint32_t> dirs{arena.addRoot("/big")};
    for (uint32_t i = 0; i < kNodes - 1; ++i) {
        const uint32_t parent = dirs[next() % dirs.size()];
        if (i % 11 == 0) {
            dirs.push_back(arena.addChild(parent, "d", dirInit()));
        } else {
            const double u = static_cast<double>(next() >> 11) / static_cast<double>(1ull << 53);
            arena.addChild(parent, std::string("f") + kExtensions[next() % 12], fileInit(static_cast<uint64_t>(std::pow(2.0, 4.0 + 24.0 * u * u * u))));
        }
    }
    arena.finalize();

    constexpr int kWidth = 2880, kHeight = 1800;  // a 15" Retina display
    TreemapParams params;
    params.width = kWidth;
    params.height = kHeight;
    params.min_extent = 3.0f;
    TreemapLayout layout;
    const auto layoutStart = std::chrono::steady_clock::now();
    layout.build(arena, arena.root(), params);
    const double layoutMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - layoutStart).count();

    std::vector<TreemapInstance> instances;
    buildTreemapInstances(layout, instances);
    REQUIRE(instances.size() > 20000);
    id<MTLBuffer> buffer = [renderer newBufferWithBytes:instances.data() length:instances.size() * sizeof(TreemapInstance) label:@"big"];
    const Theme theme = Theme::standard(true);
    const FrameUniforms uniforms = makeFrameUniforms(kWidth, kHeight, 2.0f, theme);
    Image image;
    double bestGpu = 1e9;
    for (int i = 0; i < 5; ++i) {
        image = renderOffscreen(renderer, kWidth, kHeight, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
            [renderer drawTreemapWithEncoder:encoder instances:buffer count:instances.size() uniforms:uniforms];
        });
        if (image.gpuMilliseconds > 0) bestGpu = std::min(bestGpu, image.gpuMilliseconds);
    }
    // The tiling is complete on the GPU too: no canvas pixel is left anywhere.
    long uncovered = 0;
    for (int y = 0; y < kHeight; y += 2) {
        for (int x = 0; x < kWidth; x += 2) {
            if (distance(image.at(x, y), theme.background) < 0.004) ++uncovered;
        }
    }
    CHECK(uncovered < (kWidth / 2) * (kHeight / 2) / 1000);  // < 0.1 % (dark blocks may resemble the canvas)
    // One 60 Hz frame, with a wide margin on any Apple GPU. Paravirtualised GPUs report no
    // timestamps (or meaningless ones), so there is nothing to check on a CI runner.
    if (!dptest::onCI()) CHECK(bestGpu < 16.6);
    std::printf("      %u nodes -> %zu blocks; layout %.2f ms, one draw call, GPU %.2f ms at %dx%d\n", kNodes,
                instances.size(), layoutMs, bestGpu, kWidth, kHeight);

    // Same data as a sunburst.
    SunburstParams sunburstParams;
    sunburstParams.width = kHeight;
    sunburstParams.height = kHeight;
    SunburstLayout sunburst;
    sunburst.build(arena, arena.root(), sunburstParams);
    std::vector<SunburstInstance> arcs;
    buildSunburstInstances(buildSunburstSteadyState(sunburst, theme.hubColor()), arcs);
    REQUIRE(arcs.size() > 100);
    id<MTLBuffer> arcBuffer = [renderer newBufferWithBytes:arcs.data() length:arcs.size() * sizeof(SunburstInstance) label:@"big"];
    const FrameUniforms sunburstUniforms = makeFrameUniforms(kHeight, kHeight, 2.0f, theme);
    double bestArcGpu = 1e9;
    for (int i = 0; i < 5; ++i) {
        const Image arcImage = renderOffscreen(renderer, kHeight, kHeight, theme.background, ^(id<MTLRenderCommandEncoder> encoder) {
            [renderer drawSunburstWithEncoder:encoder instances:arcBuffer count:arcs.size() uniforms:sunburstUniforms];
        });
        if (arcImage.gpuMilliseconds > 0) bestArcGpu = std::min(bestArcGpu, arcImage.gpuMilliseconds);
    }
    if (!dptest::onCI()) CHECK(bestArcGpu < 16.6);
    std::printf("      sunburst: %zu arcs, one draw call, GPU %.2f ms at %dx%d\n", arcs.size(), bestArcGpu, kHeight, kHeight);
}

DP_TEST_MAIN()
