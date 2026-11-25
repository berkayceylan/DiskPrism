#import "DiskMapMetalView.h"

#import <QuartzCore/QuartzCore.h>

#import "DiskMapRenderer.h"
#include "FlatArena.hpp"
#include "GpuInstances.hpp"
#include "NodeFilter.hpp"
#include "SunburstLayout.hpp"
#include "SunburstTransition.hpp"
#include "TreemapLayout.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace diskprism;
using namespace diskprism::gpu;

namespace {

// Virtual key codes (kVK_* from Carbon's Events.h, which is not worth importing for five constants).
constexpr unsigned short kKeyReturn = 0x24;
constexpr unsigned short kKeySpace = 0x31;
constexpr unsigned short kKeyDelete = 0x33;
constexpr unsigned short kKeyEscape = 0x35;
constexpr unsigned short kKeyEnter = 0x4C;

constexpr double kTreemapTransitionSeconds = 0.28;
constexpr double kSunburstTransitionSeconds = 0.25;
constexpr double kBloomSeconds = 0.42;

constexpr float kTreemapMinExtentPoints = 1.5f;  // sub-pixel culling threshold
constexpr float kSunburstMarginPoints = 16.0f;
constexpr int kSunburstRings = 6;
constexpr float kSunburstMinAngleDegrees = 0.4f;

enum class Transition {
    None,
    TreemapZoomIn,   // a directory of the old layout grows to fill the viewport
    TreemapZoomOut,  // the old layout shrinks into its directory in the new layout
    TreemapFade,     // plain cross-fade (data changed, mode switched)
    Sunburst,        // per-arc tween evaluated on the GPU
};

enum class SceneChange {
    Fresh,   // new data or another view mode
    Focus,   // drill-down / zoom-out within the same data
    Reload,  // same focus, different sizes / filter / metric
};

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
};

RectF mix(const RectF& a, const RectF& b, float t) noexcept {
    return RectF{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

/// (scale.xy, offset.xy) of the affine map that carries rectangle `from` onto `to`.
simd_float4 mapRect(const RectF& from, const RectF& to) noexcept {
    const float sx = to.w / from.w;
    const float sy = to.h / from.h;
    return simd_float4{sx, sy, to.x - from.x * sx, to.y - from.y * sy};
}

float cubicEaseOut(double t) noexcept {
    const double inverse = 1.0 - std::clamp(t, 0.0, 1.0);
    return static_cast<float>(1.0 - inverse * inverse * inverse);
}

DPMapHit noHit() noexcept {
    DPMapHit hit;
    hit.node = kInvalidNode;
    hit.isAggregate = NO;
    hit.isHub = NO;
    return hit;
}

}  // namespace

@implementation DiskMapMetalView {
    DPMetalRenderer *_renderer;
    NSString *_shaderError;
    NSTextField *_errorLabel;
    BOOL _configured;

    // Borrowed model.
    const FlatArena *_arena;
    const NodeMatchSet *_matches;
    DPViewMode _viewMode;
    SizeMetric _sizeMetric;
    uint32_t _focus;
    uint32_t _selected;
    uint32_t _externalHover;

    // Pointer.
    DPMapHit _hover;
    int32_t _hoverIndex;  // block / arc index, SunburstLayout::kHitHub, or -1
    NSPoint _lastMouse;
    BOOL _mouseInside;
    NSTrackingArea *_trackingArea;
    CGFloat _magnification;
    BOOL _magnifyConsumed;
    BOOL _clickDrilled;  // the first click of the current click sequence already changed the focus
    BOOL _clickOnHub;    // …and it was a click on the hub

    // Layout of the scene that is (or is becoming) visible; hit-testing always uses it.
    TreemapLayout _treemap;
    SunburstLayout _sunburst;
    BOOL _layoutValid;
    CGSize _layoutPixels;
    CGFloat _layoutScale;
    double _lastLayoutMs;
    Theme _theme;

    // GPU buffers.
    id<MTLBuffer> _treemapBuffer;
    NSUInteger _treemapCount;
    id<MTLBuffer> _previousTreemapBuffer;  // only during a treemap transition
    NSUInteger _previousTreemapCount;
    RectF _pivot;                          // zoom rectangle of a treemap transition
    id<MTLBuffer> _sunburstBuffer;
    NSUInteger _sunburstCount;
    NSUInteger _sunburstFirstArc;          // instance index of the layout's arc 0
    NSUInteger _sunburstHubIndex;

    // Animation.
    Transition _transition;
    CADisplayLink *_displayLink;
    CFTimeInterval _transitionStart;
    double _transitionDuration;
    float _progress;  // eased, 0…1
    uint64_t _frameCount;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Setup
// ---------------------------------------------------------------------------------------

- (instancetype)initWithFrame:(NSRect)frame shaderPath:(nullable NSString *)shaderPath {
    self = [self initWithFrame:frame device:MTLCreateSystemDefaultDevice()];
    if (self != nil) [self setUpWithShaderPath:shaderPath];
    return self;
}

- (void)setUpWithShaderPath:(nullable NSString *)shaderPath {
    _focus = kInvalidNode;
    _selected = kInvalidNode;
    _externalHover = kInvalidNode;
    _hover = noHit();
    _hoverIndex = -1;
    _viewMode = DPViewModeTreemap;
    _sizeMetric = SizeMetric::Physical;
    _transition = Transition::None;
    _progress = 1.0f;

    self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    self.framebufferOnly = YES;
    self.autoResizeDrawable = YES;
    // Event-driven rendering: the view never runs its own timer. It draws when it is
    // invalidated, and during transitions when the on-demand display link calls -draw.
    self.paused = YES;
    self.enableSetNeedsDisplay = YES;
    // Colors are authored in sRGB; tag the layer so wide-gamut displays match them.
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    self.colorspace = srgb;
    CGColorSpaceRelease(srgb);

    [self updateTheme];

    NSError *error = nil;
    if (self.device == nil) {
        _shaderError = @"Metal is not available on this Mac, so the disk map cannot be drawn.";
    } else {
        NSString *source = [DPMetalRenderer shaderSourceAtPath:shaderPath error:&error];
        if (source != nil) {
            _renderer = [[DPMetalRenderer alloc] initWithDevice:self.device
                                                   shaderSource:source
                                                    pixelFormat:self.colorPixelFormat
                                                          error:&error];
        }
        if (_renderer == nil) _shaderError = error.localizedDescription;
    }
    if (_shaderError != nil) {
        NSLog(@"DiskPrism: %@", _shaderError);
        [self showErrorOverlay];
    }
    _configured = YES;
}

- (void)dealloc {
    [_displayLink invalidate];
}

/// Strict diagnostics: if the shaders do not build, say exactly why, in the canvas.
- (void)showErrorOverlay {
    _errorLabel = [NSTextField wrappingLabelWithString:[@"The disk map could not be initialised.\n\n" stringByAppendingString:_shaderError]];
    _errorLabel.font = [NSFont monospacedSystemFontOfSize:11 weight:NSFontWeightRegular];
    _errorLabel.textColor = NSColor.systemRedColor;
    _errorLabel.selectable = YES;
    _errorLabel.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_errorLabel];
    [NSLayoutConstraint activateConstraints:@[
        [_errorLabel.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:24],
        [_errorLabel.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-24],
        [_errorLabel.topAnchor constraintEqualToAnchor:self.topAnchor constant:24],
    ]];
}

- (void)updateTheme {
    NSAppearance *appearance = self.effectiveAppearance;
    NSAppearanceName match = [appearance bestMatchFromAppearancesWithNames:@[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    Theme theme = Theme::standard([match isEqualToString:NSAppearanceNameDarkAqua]);
    // A white frame (with the shader's dark hairline inside it) stays visible on every
    // block color, which the system accent color would not.
    theme.accent = simd_float4{1.0f, 1.0f, 1.0f, 0.96f};
    _theme = theme;
    self.clearColor = MTLClearColorMake(theme.background.x, theme.background.y, theme.background.z, 1.0);
}

// ---------------------------------------------------------------------------------------
#pragma mark - NSView
// ---------------------------------------------------------------------------------------

- (BOOL)isFlipped {
    return YES;  // y grows downward, like the framebuffer and the layout engines
}

- (BOOL)isOpaque {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)acceptsFirstMouse:(nullable NSEvent *)__unused event {
    return YES;
}

- (void)viewDidChangeEffectiveAppearance {
    [super viewDidChangeEffectiveAppearance];
    if (!_configured) return;
    [self updateTheme];
    _layoutValid = NO;  // the hub color is part of the instance data
    self.needsDisplay = YES;
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    _layoutValid = NO;
    self.needsDisplay = YES;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    if (self.window == nil) {
        [self finishTransition];
    } else {
        self.needsDisplay = YES;
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    self.needsDisplay = YES;  // -drawRect: notices the new drawable size and relays out
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea != nil) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                         NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (BOOL)isAccessibilityElement {
    return YES;
}

- (NSAccessibilityRole)accessibilityRole {
    return NSAccessibilityImageRole;
}

- (NSString *)accessibilityLabel {
    return _viewMode == DPViewModeTreemap ? @"Disk usage treemap" : @"Disk usage sunburst";
}

// ---------------------------------------------------------------------------------------
#pragma mark - Model
// ---------------------------------------------------------------------------------------

- (DPViewMode)viewMode {
    return _viewMode;
}

- (void)setViewMode:(DPViewMode)viewMode {
    [self setViewMode:viewMode animated:NO];
}

- (void)setViewMode:(DPViewMode)viewMode animated:(BOOL)animated {
    if (viewMode == _viewMode) return;
    [self applySceneChange:SceneChange::Fresh
                  animated:animated
                    mutate:^{
                        self->_viewMode = viewMode;
                    }];
}

- (SizeMetric)sizeMetric {
    return _sizeMetric;
}

- (void)setSizeMetric:(SizeMetric)sizeMetric {
    if (sizeMetric == _sizeMetric) return;
    _sizeMetric = sizeMetric;
    _layoutValid = NO;
    self.needsDisplay = YES;
}

- (uint32_t)focusNode {
    return _focus;
}

- (uint32_t)selectedNode {
    return _selected;
}

- (nullable NSString *)shaderErrorMessage {
    return _shaderError;
}

- (NSUInteger)instanceCount {
    return _viewMode == DPViewModeTreemap ? _treemapCount : _sunburstCount;
}

- (uint64_t)renderedFrameCount {
    return _frameCount;
}

- (BOOL)isAnimating {
    return _displayLink != nil;
}

- (double)lastLayoutMilliseconds {
    return _lastLayoutMs;
}

- (void)setArena:(nullable const FlatArena *)arena
         matches:(nullable const NodeMatchSet *)matches
           focus:(uint32_t)focus
        animated:(BOOL)animated {
    [self applySceneChange:SceneChange::Fresh
                  animated:animated && arena != nullptr
                    mutate:^{
                        self->_arena = arena;
                        self->_matches = matches;
                        self->_selected = kInvalidNode;
                        self->_externalHover = kInvalidNode;
                        self->_focus = [self validFocus:focus];
                    }];
}

- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated {
    const uint32_t target = [self validFocus:node];
    if (target == _focus) return;
    [self applySceneChange:SceneChange::Focus
                  animated:animated
                    mutate:^{
                        self->_focus = target;
                    }];
}

- (void)reloadDataAnimated:(BOOL)animated {
    [self applySceneChange:SceneChange::Reload
                  animated:animated
                    mutate:^{
                        // The focused directory may just have been removed.
                        self->_focus = [self validFocus:self->_focus];
                        if (self->_arena == nullptr || !self->_arena->isValid(self->_selected)) self->_selected = kInvalidNode;
                        if (self->_arena == nullptr || !self->_arena->isValid(self->_externalHover)) self->_externalHover = kInvalidNode;
                    }];
}

- (void)setSelectedNode:(uint32_t)node {
    if (node == _selected) return;
    _selected = node;
    self.needsDisplay = YES;
}

- (void)setExternalHoverNode:(uint32_t)node {
    if (node == _externalHover) return;
    _externalHover = node;
    self.needsDisplay = YES;
}

/// A focus must be a live directory; anything else falls back to its nearest live
/// ancestor directory, and finally to the root.
- (uint32_t)validFocus:(uint32_t)node {
    if (_arena == nullptr || _arena->size() == 0) return kInvalidNode;
    uint32_t current = node < _arena->size() ? node : _arena->root();
    while (current != kInvalidNode && (!_arena->isValid(current) || !_arena->node(current).is_dir())) {
        current = _arena->node(current).parent_idx;
    }
    return current != kInvalidNode ? current : _arena->root();
}

// ---------------------------------------------------------------------------------------
#pragma mark - Layout and buffers
// ---------------------------------------------------------------------------------------

- (CGFloat)backingScale {
    if (self.window != nil) return self.window.backingScaleFactor;
    return _layoutScale > 0 ? _layoutScale : 2.0;
}

- (void)buildLayoutForPixelSize:(CGSize)pixels scale:(CGFloat)scale {
    const auto start = std::chrono::steady_clock::now();
    _layoutPixels = pixels;
    _layoutScale = scale;
    _treemap.clear();
    _sunburst.clear();

    if (_arena != nullptr && _focus != kInvalidNode && pixels.width >= 1 && pixels.height >= 1) {
        const NodeMatchSet *matches = (_matches != nullptr && _matches->active()) ? _matches : nullptr;
        if (_viewMode == DPViewModeTreemap) {
            TreemapParams params;
            params.width = static_cast<float>(pixels.width);
            params.height = static_cast<float>(pixels.height);
            params.metric = _sizeMetric;
            params.min_extent = kTreemapMinExtentPoints * static_cast<float>(scale);
            params.snap_to_pixels = true;
            params.matches = matches;
            _treemap.build(*_arena, _focus, params);
        } else {
            SunburstParams params;
            params.width = static_cast<float>(pixels.width);
            params.height = static_cast<float>(pixels.height);
            params.metric = _sizeMetric;
            params.max_depth = kSunburstRings;
            params.min_angle_degrees = kSunburstMinAngleDegrees;
            params.margin = kSunburstMarginPoints * static_cast<float>(scale);
            params.matches = matches;
            _sunburst.build(*_arena, _focus, params);
        }
    }
    _layoutValid = YES;
    _lastLayoutMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

- (void)uploadSunburst:(const SunburstTransition &)transition {
    std::vector<SunburstInstance> instances;
    buildSunburstInstances(transition, instances);
    _sunburstCount = instances.size();
    _sunburstFirstArc = transition.first_layout_arc;
    _sunburstHubIndex = transition.hub_index;
    _sunburstBuffer = [_renderer newBufferWithBytes:instances.data()
                                             length:instances.size() * sizeof(SunburstInstance)
                                              label:@"Sunburst instances"];
}

/// Uploads the instance data of the current layout in its resting state.
- (void)uploadSteadyBuffers {
    if (_renderer == nil) return;
    const auto start = std::chrono::steady_clock::now();
    if (_viewMode == DPViewModeTreemap) {
        std::vector<TreemapInstance> instances;
        buildTreemapInstances(_treemap, instances);
        _treemapCount = instances.size();
        _treemapBuffer = [_renderer newBufferWithBytes:instances.data()
                                                length:instances.size() * sizeof(TreemapInstance)
                                                 label:@"Treemap instances"];
        _sunburstBuffer = nil;
        _sunburstCount = 0;
    } else {
        if (_arena != nullptr && _focus != kInvalidNode && _sunburst.outerRadius() > 0) {
            [self uploadSunburst:buildSunburstSteadyState(_sunburst, _theme.hubColor())];
        } else {
            _sunburstBuffer = nil;
            _sunburstCount = 0;
        }
        _treemapBuffer = nil;
        _treemapCount = 0;
    }
    _lastLayoutMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

/// Applies a change to the scene and, if asked, animates from the old scene to the new.
- (void)applySceneChange:(SceneChange)change animated:(BOOL)animated mutate:(void (NS_NOESCAPE ^)(void))mutate {
    [self finishTransition];

    const BOOL hadScene = _layoutValid && _arena != nullptr && _focus != kInvalidNode;
    const BOOL canAnimate = animated && _renderer != nil && self.window != nil && _layoutPixels.width >= 1;
    const DPViewMode oldMode = _viewMode;
    const uint32_t oldFocus = _focus;
    id<MTLBuffer> oldTreemapBuffer = _treemapBuffer;
    const NSUInteger oldTreemapCount = _treemapCount;
    SunburstLayout oldSunburst;
    if (canAnimate && hadScene && change != SceneChange::Fresh && oldMode == DPViewModeSunburst) oldSunburst = _sunburst;

    mutate();

    // Treemap zoom-in: where is the new focus inside the layout that is still on screen?
    Transition kind = Transition::None;
    RectF pivot;
    const BOOL treemapFocusChange = canAnimate && hadScene && change == SceneChange::Focus && oldMode == DPViewModeTreemap &&
                                    _viewMode == DPViewModeTreemap && _arena != nullptr && _focus != oldFocus;
    if (treemapFocusChange && _arena->isAncestorOrSelf(oldFocus, _focus)) {
        float box[4];
        if (_treemap.rectForNode(_focus, box) && box[2] >= 1.0f && box[3] >= 1.0f) {
            pivot = RectF{box[0], box[1], box[2], box[3]};
            kind = Transition::TreemapZoomIn;
        }
    }

    if (_layoutPixels.width < 1 || _layoutPixels.height < 1) {
        // No size yet: the first -drawRect: builds the layout.
        _layoutValid = NO;
        self.needsDisplay = YES;
        return;
    }
    [self buildLayoutForPixelSize:_layoutPixels scale:_layoutScale];
    [self uploadSteadyBuffers];

    if (canAnimate && _arena != nullptr && _focus != kInvalidNode) {
        if (_viewMode == DPViewModeTreemap) {
            if (kind == Transition::None && treemapFocusChange && _arena->isAncestorOrSelf(_focus, oldFocus)) {
                // Zoom-out: the old focus is a rectangle somewhere in the new layout.
                float box[4];
                if (_treemap.rectForNode(oldFocus, box) && box[2] >= 1.0f && box[3] >= 1.0f) {
                    pivot = RectF{box[0], box[1], box[2], box[3]};
                    kind = Transition::TreemapZoomOut;
                }
            }
            if (kind == Transition::None) kind = Transition::TreemapFade;
            const BOOL keepPrevious = hadScene && oldMode == DPViewModeTreemap && oldTreemapBuffer != nil &&
                                      change != SceneChange::Fresh;
            _previousTreemapBuffer = keepPrevious ? oldTreemapBuffer : nil;
            _previousTreemapCount = keepPrevious ? oldTreemapCount : 0;
            if (!keepPrevious) kind = Transition::TreemapFade;
            _pivot = pivot;
            [self beginTransition:kind duration:kTreemapTransitionSeconds];
        } else if (_sunburst.outerRadius() > 0) {
            const SunburstTransition transition = buildSunburstTransition(*_arena, oldSunburst, _sunburst, _theme.hubColor());
            [self uploadSunburst:transition];
            [self beginTransition:Transition::Sunburst
                         duration:transition.kind == SunburstTransitionKind::Bloom ? kBloomSeconds : kSunburstTransitionSeconds];
        }
    }

    [self refreshHoverNotifyingDelegate:YES];
    self.needsDisplay = YES;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Animation (on-demand display link)
// ---------------------------------------------------------------------------------------

- (void)beginTransition:(Transition)transition duration:(double)duration {
    _transition = transition;
    _transitionDuration = duration;
    _transitionStart = CACurrentMediaTime();
    _progress = 0.0f;
    if (_displayLink == nil) {
        // The link only exists while something is moving. It is created here and torn
        // down in -finishTransition, so an idle view has no timer of any kind.
        _displayLink = [self displayLinkWithTarget:self selector:@selector(displayLinkDidFire:)];
        [_displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
}

- (void)displayLinkDidFire:(CADisplayLink *)link {
    if (_transition == Transition::None) {
        [self finishTransition];
        return;
    }
    const double elapsed = std::max(link.targetTimestamp, CACurrentMediaTime()) - _transitionStart;
    const double t = _transitionDuration > 0 ? elapsed / _transitionDuration : 1.0;
    if (t >= 1.0) {
        // Settled: stop the link in this very callback, then draw the final frame once.
        [self finishTransition];
        [self draw];
        return;
    }
    _progress = cubicEaseOut(t);
    [self draw];
}

- (void)finishTransition {
    if (_displayLink != nil) {
        [_displayLink invalidate];
        _displayLink = nil;
    }
    if (_transition == Transition::None) return;
    const BOOL wasSunburst = _transition == Transition::Sunburst;
    _transition = Transition::None;
    _progress = 1.0f;
    _previousTreemapBuffer = nil;
    _previousTreemapCount = 0;
    // Replace the tween buffer (which still holds the arcs that left) by the resting one.
    if (wasSunburst && _viewMode == DPViewModeSunburst && _layoutValid) [self uploadSteadyBuffers];
    self.needsDisplay = YES;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Drawing
// ---------------------------------------------------------------------------------------

/// Rectangle (layout pixels) of the nearest drawn ancestor-or-self of `node` that lies
/// strictly below the focus.
- (BOOL)treemapRectForNode:(uint32_t)node into:(float *)box {
    if (_arena == nullptr || node == kInvalidNode || !_arena->isValid(node) || !_arena->isAncestorOrSelf(_focus, node)) return NO;
    for (uint32_t n = node; n != kInvalidNode && n != _focus; n = _arena->node(n).parent_idx) {
        if (_treemap.rectForNode(n, box)) return YES;
    }
    return NO;
}

/// Index of the arc of the nearest drawn ancestor-or-self of `node` below the focus, or -1.
- (int32_t)sunburstArcForNode:(uint32_t)node {
    if (_arena == nullptr || node == kInvalidNode || !_arena->isValid(node) || !_arena->isAncestorOrSelf(_focus, node)) return -1;
    for (uint32_t n = node; n != kInvalidNode && n != _focus; n = _arena->node(n).parent_idx) {
        const int32_t index = _sunburst.arcIndexForNode(n);
        if (index >= 0) return index;
    }
    return -1;
}

- (void)encodeSceneWithEncoder:(id<MTLRenderCommandEncoder>)encoder pixelSize:(CGSize)pixels scale:(CGFloat)scale {
    FrameUniforms uniforms = makeFrameUniforms(static_cast<float>(pixels.width), static_cast<float>(pixels.height),
                                               static_cast<float>(scale), _theme);

    if (_viewMode == DPViewModeTreemap) {
        if (_treemapCount == 0 || _treemapBuffer == nil) return;
        const BOOL inTransition = _transition == Transition::TreemapZoomIn || _transition == Transition::TreemapZoomOut ||
                                  _transition == Transition::TreemapFade;
        if (inTransition) {
            const RectF viewport{0.0f, 0.0f, static_cast<float>(pixels.width), static_cast<float>(pixels.height)};
            FrameUniforms previous = uniforms;
            FrameUniforms current = uniforms;
            if (_transition == Transition::TreemapZoomIn) {
                // Both layers show the new focus in the same, growing rectangle: the old
                // one magnified so its directory fills it, the new one squeezed into it.
                const RectF target = mix(_pivot, viewport, _progress);
                previous.transform = mapRect(_pivot, target);
                current.transform = mapRect(viewport, target);
            } else if (_transition == Transition::TreemapZoomOut) {
                const RectF target = mix(viewport, _pivot, _progress);
                previous.transform = mapRect(viewport, target);
                current.transform = mapRect(_pivot, target);
            }
            current.viewport.w = _progress;  // the new layer fades in on top
            if (_previousTreemapBuffer != nil) {
                [_renderer drawTreemapWithEncoder:encoder instances:_previousTreemapBuffer count:_previousTreemapCount uniforms:previous];
            }
            [_renderer drawTreemapWithEncoder:encoder instances:_treemapBuffer count:_treemapCount uniforms:current];
            return;
        }

        float box[4];
        if (_externalHover != kInvalidNode) {
            // Hover coming from the outline: light up that node's whole area.
            if ([self treemapRectForNode:_externalHover into:box]) uniforms.hoverRect = simd_float4{box[0], box[1], box[2], box[3]};
            uniforms.ids.x = _treemap.rectIndexForNode(_externalHover);
        } else if (_hoverIndex >= 0) {
            uniforms.ids.x = _hoverIndex;
            // Also outline the top-level folder the pointer is in — the one a double
            // click would drill into.
            const uint32_t top = _arena != nullptr ? _arena->childToward(_focus, _hover.node) : kInvalidNode;
            const BOOL framed = top != kInvalidNode && top != _hover.node && _arena->node(top).is_dir() && _treemap.rectForNode(top, box);
            // (A folder that fills the whole canvas would only put a frame around everything.)
            if (framed && (box[2] < _treemap.width() || box[3] < _treemap.height())) {
                uniforms.hoverRect = simd_float4{box[0], box[1], box[2], box[3]};
            }
        }
        if (_selected != kInvalidNode && _selected != _focus && [self treemapRectForNode:_selected into:box]) {
            uniforms.selectionRect = simd_float4{box[0], box[1], box[2], box[3]};
        }
        [_renderer drawTreemapWithEncoder:encoder instances:_treemapBuffer count:_treemapCount uniforms:uniforms];
        return;
    }

    if (_sunburstCount == 0 || _sunburstBuffer == nil) return;
    const BOOL tweening = _transition == Transition::Sunburst;
    uniforms.sunburst = simd_float4{_sunburst.centerX(), _sunburst.centerY(), tweening ? _progress : 1.0f,
                                    std::max(1.0f, static_cast<float>(scale))};
    if (!tweening) {
        const auto instanceOfArc = [self](int32_t arc) { return static_cast<int>(self->_sunburstFirstArc) + arc; };
        if (_externalHover != kInvalidNode) {
            const int32_t arc = [self sunburstArcForNode:_externalHover];
            if (arc >= 0) uniforms.ids.x = instanceOfArc(arc);
        } else if (_hoverIndex == SunburstLayout::kHitHub) {
            uniforms.ids.x = static_cast<int>(_sunburstHubIndex);
        } else if (_hoverIndex >= 0) {
            uniforms.ids.x = instanceOfArc(_hoverIndex);
        }
        if (_selected != kInvalidNode && _selected != _focus) {
            const int32_t arc = [self sunburstArcForNode:_selected];
            if (arc >= 0) uniforms.ids.y = instanceOfArc(arc);
        }
    }
    [_renderer drawSunburstWithEncoder:encoder instances:_sunburstBuffer count:_sunburstCount uniforms:uniforms];
}

- (void)drawRect:(NSRect)__unused dirtyRect {
    if (_renderer == nil) return;
    const CGSize pixels = self.drawableSize;
    if (pixels.width < 1 || pixels.height < 1) return;
    const CGFloat scale = [self backingScale];

    if (!_layoutValid || !CGSizeEqualToSize(pixels, _layoutPixels) || scale != _layoutScale) {
        [self finishTransition];  // a resize ends any running transition
        [self buildLayoutForPixelSize:pixels scale:scale];
        [self uploadSteadyBuffers];
        [self refreshHoverNotifyingDelegate:NO];
    }

    MTLRenderPassDescriptor *pass = self.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = self.currentDrawable;
    if (pass == nil || drawable == nil) return;

    id<MTLCommandBuffer> commandBuffer = [_renderer.commandQueue commandBuffer];
    commandBuffer.label = @"DiskPrism frame";
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [self encodeSceneWithEncoder:encoder pixelSize:pixels scale:scale];
    [encoder endEncoding];
    [commandBuffer presentDrawable:drawable];
    [commandBuffer commit];
    ++_frameCount;
}

- (nullable CGImageRef)newSnapshotWithPixelWidth:(NSUInteger)width height:(NSUInteger)height scale:(CGFloat)scale {
    if (_renderer == nil || width == 0 || height == 0) return nil;
    [self finishTransition];
    const CGSize pixels = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
    [self buildLayoutForPixelSize:pixels scale:scale];
    [self uploadSteadyBuffers];

    id<MTLTexture> target = [_renderer newReadableRenderTargetWithWidth:width height:height];
    if (target == nil) return nil;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = self.clearColor;

    id<MTLCommandBuffer> commandBuffer = [_renderer.commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [self encodeSceneWithEncoder:encoder pixelSize:pixels scale:scale];
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];

    NSMutableData *bytes = [NSMutableData dataWithLength:width * height * 4];
    [_renderer readPixelsFromTexture:target intoBytes:bytes.mutableBytes];

    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGDataProviderRef provider = CGDataProviderCreateWithCFData((__bridge CFDataRef)bytes);
    const CGBitmapInfo bitmapInfo =
        static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Little) | static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst);
    CGImageRef image = CGImageCreate(width, height, 8, 32, width * 4, srgb, bitmapInfo, provider, nullptr, false,
                                     kCGRenderingIntentDefault);
    CGDataProviderRelease(provider);
    CGColorSpaceRelease(srgb);

    // The on-screen size is in general different: rebuild before the next real frame.
    _layoutValid = NO;
    self.needsDisplay = YES;
    return image;
}

- (NSRect)hubRect {
    if (_viewMode != DPViewModeSunburst) return NSZeroRect;
    // Same geometry as SunburstLayout, in points: independent of whether a frame has
    // been drawn yet.
    const NSRect bounds = self.bounds;
    const CGFloat outer = MIN(bounds.size.width, bounds.size.height) / 2 - kSunburstMarginPoints;
    if (outer <= 0) return NSZeroRect;
    const CGFloat hub = outer * SunburstParams{}.hub_fraction;
    return NSMakeRect(NSMidX(bounds) - hub, NSMidY(bounds) - hub, 2 * hub, 2 * hub);
}

- (NSRect)rectForNode:(uint32_t)node {
    if (!_layoutValid || _layoutScale <= 0) return NSZeroRect;
    const CGFloat scale = _layoutScale;
    if (_viewMode == DPViewModeTreemap) {
        float box[4];
        if (![self treemapRectForNode:node into:box]) return NSZeroRect;
        return NSMakeRect(box[0] / scale, box[1] / scale, box[2] / scale, box[3] / scale);
    }
    const int32_t index = [self sunburstArcForNode:node];
    if (index < 0) return NSZeroRect;
    const SunburstArc &arc = _sunburst.arcs()[static_cast<size_t>(index)];
    const double angle = (static_cast<double>(arc.start_angle) + arc.end_angle) * 0.5;
    const double radius = (static_cast<double>(arc.inner_radius) + arc.outer_radius) * 0.5;
    const double halfSide = std::max((static_cast<double>(arc.outer_radius) - arc.inner_radius) * 0.5, 4.0);
    const double x = _sunburst.centerX() + radius * std::sin(angle);
    const double y = _sunburst.centerY() - radius * std::cos(angle);
    return NSMakeRect((x - halfSide) / scale, (y - halfSide) / scale, 2 * halfSide / scale, 2 * halfSide / scale);
}

// ---------------------------------------------------------------------------------------
#pragma mark - Pointer
// ---------------------------------------------------------------------------------------

/// Maps a point in view coordinates (points, y down) to what is drawn there. The layout
/// lives in device pixels, so the point is scaled by the backing factor first — on a
/// Retina display that is what makes a hit land on the exact pixel under the cursor.
- (DPMapHit)hitAtPoint:(NSPoint)point index:(int32_t *)outIndex {
    DPMapHit hit = noHit();
    int32_t index = -1;
    if (_arena != nullptr && _layoutValid && _layoutScale > 0 && NSPointInRect(point, self.bounds)) {
        const auto x = static_cast<float>(point.x * _layoutScale);
        const auto y = static_cast<float>(point.y * _layoutScale);
        if (_viewMode == DPViewModeTreemap) {
            index = _treemap.hitTest(x, y);
            if (index >= 0) {
                const TreemapRect &rect = _treemap.rects()[static_cast<size_t>(index)];
                hit.node = rect.node;
                hit.isAggregate = (rect.flags & kTreemapAggregate) != 0;
            }
        } else {
            index = _sunburst.hitTest(x, y);
            if (index == SunburstLayout::kHitHub) {
                hit.node = _focus;
                hit.isHub = YES;
            } else if (index >= 0) {
                const SunburstArc &arc = _sunburst.arcs()[static_cast<size_t>(index)];
                hit.node = arc.node;
                hit.isAggregate = (arc.flags & kArcAggregate) != 0;
            } else {
                index = -1;
            }
        }
    }
    if (outIndex != nullptr) *outIndex = index;
    return hit;
}

- (void)refreshHoverNotifyingDelegate:(BOOL)notify {
    int32_t index = -1;
    DPMapHit hit = noHit();
    if (_mouseInside) hit = [self hitAtPoint:_lastMouse index:&index];
    if (index != _hoverIndex || hit.node != _hover.node || hit.isAggregate != _hover.isAggregate) {
        _hoverIndex = index;
        _hover = hit;
        self.needsDisplay = YES;  // one frame with the new highlight; nothing keeps running
    }
    if (notify) [self.mapDelegate diskMapView:self didHover:hit atPoint:_lastMouse];
}

- (void)mouseMoved:(NSEvent *)event {
    _mouseInside = YES;
    _lastMouse = [self convertPoint:event.locationInWindow fromView:nil];
    [self refreshHoverNotifyingDelegate:YES];
}

- (void)mouseEntered:(NSEvent *)event {
    [self mouseMoved:event];
}

- (void)mouseExited:(NSEvent *)__unused event {
    _mouseInside = NO;
    [self refreshHoverNotifyingDelegate:YES];
}

- (void)requestFocus:(uint32_t)node {
    if (_arena == nullptr || node == kInvalidNode || node == _focus) return;
    [self.mapDelegate diskMapView:self didRequestFocusNode:node];
}

- (void)requestZoomOut {
    if (_arena == nullptr || _focus == kInvalidNode) return;
    [self requestFocus:_arena->node(_focus).parent_idx];
}

/// The directory a "drill in" gesture at the pointer should open: one level at a time
/// in the treemap, the clicked folder itself in the sunburst.
- (uint32_t)drillTargetForHit:(DPMapHit)hit {
    if (_arena == nullptr || hit.node == kInvalidNode || hit.isHub) return kInvalidNode;
    uint32_t target = kInvalidNode;
    if (_viewMode == DPViewModeTreemap) {
        target = hit.node == _focus ? kInvalidNode : _arena->childToward(_focus, hit.node);
    } else if (!hit.isAggregate) {
        target = hit.node;
    }
    if (target == kInvalidNode || !_arena->isValid(target)) return kInvalidNode;
    const FsNode &n = _arena->node(target);
    return (n.is_dir() && n.bytes(_sizeMetric) > 0) ? target : kInvalidNode;
}

- (void)mouseDown:(NSEvent *)event {
    [self.window makeFirstResponder:self];
    _mouseInside = YES;
    _lastMouse = [self convertPoint:event.locationInWindow fromView:nil];
    [self refreshHoverNotifyingDelegate:NO];
    const DPMapHit hit = _hover;
    if (event.clickCount <= 1) {
        _clickDrilled = NO;
        _clickOnHub = hit.isHub;
    } else if (_clickDrilled && !(_clickOnHub && hit.isHub)) {
        // The second half of a double click: its first half already drilled in, and what
        // is under the pointer now is a different item the user never aimed at.
        // (Clicking the hub repeatedly to climb several levels is fine.)
        return;
    }
    if (hit.node == kInvalidNode || _arena == nullptr) return;

    const uint32_t focusBefore = _focus;
    if (hit.isHub) {
        [self requestZoomOut];  // the centre of the sunburst leads back to the parent
    } else {
        [self.mapDelegate diskMapView:self didSelectNode:hit.node];
        // Sunburst: one click on a folder drills in (so does a double click). Treemap: a
        // double click does, so that a single click can simply select the block.
        if (_viewMode == DPViewModeSunburst || event.clickCount == 2) {
            const uint32_t target = [self drillTargetForHit:hit];
            if (target != kInvalidNode) [self requestFocus:target];
        }
    }
    if (_focus != focusBefore) _clickDrilled = YES;
}

- (nullable NSMenu *)menuForEvent:(NSEvent *)event {
    _mouseInside = YES;
    _lastMouse = [self convertPoint:event.locationInWindow fromView:nil];
    [self refreshHoverNotifyingDelegate:NO];
    const DPMapHit hit = _hover;
    if (hit.node == kInvalidNode) return nil;
    [self.mapDelegate diskMapView:self didSelectNode:hit.node];
    return [self.mapDelegate diskMapView:self menuForNode:hit.node];
}

- (void)keyDown:(NSEvent *)event {
    switch (event.keyCode) {
        case kKeyEscape:
        case kKeyDelete:
            [self requestZoomOut];
            return;
        case kKeyReturn:
        case kKeyEnter:
            if (_arena != nullptr && _arena->isValid(_selected)) {
                const uint32_t target = _arena->node(_selected).is_dir() ? _selected : _arena->node(_selected).parent_idx;
                [self requestFocus:target];
            }
            return;
        case kKeySpace: {
            id<DiskMapMetalViewDelegate> delegate = self.mapDelegate;
            if ([delegate respondsToSelector:@selector(diskMapViewDidRequestQuickLook:)]) {
                [delegate diskMapViewDidRequestQuickLook:self];
            }
            return;
        }
        default:
            [super keyDown:event];
    }
}

/// Pinch: spread to drill into what is under the pointer, pinch to zoom back out.
- (void)magnifyWithEvent:(NSEvent *)event {
    if (event.phase == NSEventPhaseBegan) {
        _magnification = 0;
        _magnifyConsumed = NO;
    }
    _magnification += event.magnification;
    if (!_magnifyConsumed) {
        if (_magnification > 0.25) {
            _magnifyConsumed = YES;
            const uint32_t target = [self drillTargetForHit:_hover];
            if (target != kInvalidNode) [self requestFocus:target];
        } else if (_magnification < -0.25) {
            _magnifyConsumed = YES;
            [self requestZoomOut];
        }
    }
    if (event.phase == NSEventPhaseEnded || event.phase == NSEventPhaseCancelled) {
        _magnification = 0;
        _magnifyConsumed = NO;
    }
}

@end
