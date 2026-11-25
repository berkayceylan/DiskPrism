// DiskPrism GPU — the Metal canvas that shows the scanned tree as a treemap or sunburst.
#import <AppKit/AppKit.h>
#import <MetalKit/MetalKit.h>

#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
class NodeMatchSet;
}  // namespace diskprism

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, DPViewMode) {
    DPViewModeTreemap = 0,   ///< squarified cushion treemap
    DPViewModeSunburst = 1,  ///< radial sunburst
};

/// What lies under a point of the canvas.
typedef struct DPMapHit {
    uint32_t node;     ///< arena node, or diskprism::kInvalidNode for "nothing"
    BOOL isAggregate;  ///< the block / slice stands for the small leftovers of `node`
    BOOL isHub;        ///< the sunburst's centre disc (node is the focused directory)
} DPMapHit;

@class DiskMapMetalView;

@protocol DiskMapMetalViewDelegate <NSObject>
/// The pointer moved; `point` is in the view's coordinates. Called with an invalid node
/// when the pointer leaves the map.
- (void)diskMapView:(DiskMapMetalView *)view didHover:(DPMapHit)hit atPoint:(NSPoint)point;
/// The user clicked an item.
- (void)diskMapView:(DiskMapMetalView *)view didSelectNode:(uint32_t)node;
/// The user asked to drill into (or zoom out to) `node`.
- (void)diskMapView:(DiskMapMetalView *)view didRequestFocusNode:(uint32_t)node;
/// Context menu for `node` (already selected when this is called).
- (nullable NSMenu *)diskMapView:(DiskMapMetalView *)view menuForNode:(uint32_t)node;
@optional
/// Space bar: toggle Quick Look for the selection.
- (void)diskMapViewDidRequestQuickLook:(DiskMapMetalView *)view;
@end

/// Renders the arena with Metal and translates pointer input into nodes.
///
/// Rendering is strictly event driven: the view is an `MTKView` that is permanently
/// paused and only draws when something invalidates it (new data, resize, hover,
/// selection, appearance). While a transition is running a `CADisplayLink` obtained
/// from the view drives the frames; it is invalidated in the very callback in which the
/// animation settles, so an idle window costs no CPU and no GPU time at all.
@interface DiskMapMetalView : MTKView

/// Creates the view and compiles the shaders. `shaderPath` overrides the bundled
/// Shaders.metal (used by tests and tools that run outside the app bundle).
- (instancetype)initWithFrame:(NSRect)frame shaderPath:(nullable NSString *)shaderPath;

@property (nonatomic, weak, nullable) id<DiskMapMetalViewDelegate> mapDelegate;

@property (nonatomic) DPViewMode viewMode;
- (void)setViewMode:(DPViewMode)viewMode animated:(BOOL)animated;

/// Which byte count sizes the blocks. The arena's child lists must be sorted by the same
/// metric (the owner re-sorts, then calls `-reloadDataAnimated:`).
@property (nonatomic) diskprism::SizeMetric sizeMetric;

@property (nonatomic, readonly) uint32_t focusNode;
@property (nonatomic, readonly) uint32_t selectedNode;

/// Installs the data to show. Both pointers are borrowed: they must stay valid until
/// they are replaced (pass nullptr before destroying the arena).
- (void)setArena:(nullable const diskprism::FlatArena *)arena
         matches:(nullable const diskprism::NodeMatchSet *)matches
           focus:(uint32_t)focus
        animated:(BOOL)animated;

/// Drills into (or zooms out to) a directory.
- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated;

- (void)setSelectedNode:(uint32_t)node;

/// Highlights a node hovered elsewhere (the outline view); kInvalidNode clears it.
- (void)setExternalHoverNode:(uint32_t)node;

/// Rebuilds layout and GPU buffers after the arena's contents, the filter or the metric
/// changed.
- (void)reloadDataAnimated:(BOOL)animated;

/// On-screen rectangle (view coordinates) of a node, or NSZeroRect if it is not drawn.
- (NSRect)rectForNode:(uint32_t)node;

/// Bounding square (view coordinates) of the sunburst's centre disc; NSZeroRect in
/// treemap mode. The canvas panel centres the focused folder's name and size on it.
@property (nonatomic, readonly) NSRect hubRect;

/// Text of the shader compiler's diagnostics if the shaders could not be built.
@property (nonatomic, readonly, nullable) NSString *shaderErrorMessage;

// --- Diagnostics ---------------------------------------------------------------------

@property (nonatomic, readonly) NSUInteger instanceCount;        ///< blocks / arcs in the current scene
@property (nonatomic, readonly) uint64_t renderedFrameCount;     ///< frames drawn since creation
@property (nonatomic, readonly) BOOL isAnimating;                ///< a display link is currently running
@property (nonatomic, readonly) double lastLayoutMilliseconds;   ///< CPU time of the last layout + upload

/// Renders the current scene (without transitions) into an image of the given pixel size.
/// Works without a window, which is what the headless render checks rely on.
- (nullable CGImageRef)newSnapshotWithPixelWidth:(NSUInteger)width
                                          height:(NSUInteger)height
                                           scale:(CGFloat)scale CF_RETURNS_RETAINED;

@end

NS_ASSUME_NONNULL_END
