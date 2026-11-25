// DiskPrism UI — the centre pane: breadcrumb bar, permission banner, Metal canvas and
// the views floating above it (hover HUD, sunburst hub read-out, empty and scanning states).
#import <AppKit/AppKit.h>

#import "BreadcrumbBar.h"
#import "DiskMapMetalView.h"
#import "HoverHudView.h"
#import "NodeActions.h"
#import "PermissionBannerView.h"
#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
class NodeMatchSet;
}  // namespace diskprism

NS_ASSUME_NONNULL_BEGIN

@interface CanvasViewController : NSViewController <DiskMapMetalViewDelegate, BreadcrumbBarDelegate>

/// `shaderPath` overrides the bundled Shaders.metal (tests and tools outside the bundle).
- (instancetype)initWithShaderPath:(nullable NSString *)shaderPath;

@property (nonatomic, weak, nullable) id<DPNodeActionHandler> actionHandler;

@property (nonatomic, readonly) DiskMapMetalView *mapView;
@property (nonatomic, readonly) BreadcrumbBar *breadcrumbBar;
@property (nonatomic, readonly) HoverHudView *hud;
@property (nonatomic, readonly) PermissionBannerView *banner;

/// Installs the data to show (borrowed pointers; pass nullptr before the arena dies).
- (void)setArena:(nullable const diskprism::FlatArena *)arena
         matches:(nullable const diskprism::NodeMatchSet *)matches
           focus:(uint32_t)focus
          metric:(diskprism::SizeMetric)metric
        animated:(BOOL)animated;

- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated;
- (void)setSelectedNode:(uint32_t)node;
- (void)setExternalHoverNode:(uint32_t)node;
- (void)setViewMode:(DPViewMode)mode animated:(BOOL)animated;

/// The arena was re-sorted for another metric.
- (void)setMetric:(diskprism::SizeMetric)metric;

/// Sizes or the filter changed.
- (void)reloadDataAnimated:(BOOL)animated;

// --- Overlays ---------------------------------------------------------------------------

/// "Nothing scanned yet" invitation. Its buttons send `scanDefaultTarget:` and
/// `chooseFolder:` up the responder chain.
- (void)showEmptyStateForVolumeNamed:(NSString *)volumeName;
- (void)hideEmptyState;

/// Progress panel shown while a scan runs. Its Cancel button sends `cancelScan:`.
- (void)showScanProgressWithTitle:(NSString *)title;
- (void)updateScanProgress:(NSString *)primary secondary:(NSString *)secondary;
- (void)hideScanProgress;

// Read-outs of the sunburst hub (exposed for tests).
@property (nonatomic, readonly) NSString *hubTitle;
@property (nonatomic, readonly) NSString *hubValue;
@property (nonatomic, readonly) BOOL emptyStateVisible;
@property (nonatomic, readonly) BOOL scanProgressVisible;

@end

NS_ASSUME_NONNULL_END
