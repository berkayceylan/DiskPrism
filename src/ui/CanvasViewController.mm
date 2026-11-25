#import "CanvasViewController.h"

#import "Formatting.h"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"

using namespace diskprism;

/// Container whose coordinate system matches the Metal view's (origin top-left).
@interface DPFlippedView : NSView
@end

@implementation DPFlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

/// A view that shows information but lets every mouse event through to the canvas.
@interface DPPassthroughView : NSView
@end

@implementation DPPassthroughView
- (nullable NSView *)hitTest:(NSPoint)__unused point {
    return nil;
}
@end

@implementation CanvasViewController {
    NSString *_shaderPath;
    DiskMapMetalView *_mapView;
    BreadcrumbBar *_breadcrumb;
    PermissionBannerView *_banner;
    HoverHudView *_hud;
    DPFlippedView *_mapContainer;

    // Sunburst hub read-out.
    DPPassthroughView *_hubView;
    NSTextField *_hubTitle;
    NSTextField *_hubValue;
    NSTextField *_hubDetail;
    NSLayoutConstraint *_hubWidth;

    // Empty state.
    NSView *_emptyView;
    NSButton *_emptyScanButton;

    // Scan progress.
    NSVisualEffectView *_progressView;
    NSProgressIndicator *_spinner;
    NSTextField *_progressTitle;
    NSTextField *_progressPrimary;
    NSTextField *_progressSecondary;
    NSButton *_progressCancelButton;

    const FlatArena *_arena;
    SizeMetric _metric;
    DPViewMode _mode;
    DPMapHit _hudHit;  // what the HUD currently describes (meaningful while it is visible)
}

- (instancetype)initWithShaderPath:(nullable NSString *)shaderPath {
    self = [super initWithNibName:nil bundle:nil];
    if (self != nil) {
        _shaderPath = [shaderPath copy];
        _metric = SizeMetric::Physical;
        _mode = DPViewModeTreemap;
    }
    return self;
}

- (DiskMapMetalView *)mapView {
    [self loadViewIfNeeded];
    return _mapView;
}

- (BreadcrumbBar *)breadcrumbBar {
    [self loadViewIfNeeded];
    return _breadcrumb;
}

- (HoverHudView *)hud {
    [self loadViewIfNeeded];
    return _hud;
}

- (PermissionBannerView *)banner {
    [self loadViewIfNeeded];
    return _banner;
}

- (NSString *)hubTitle {
    return _hubTitle.stringValue;
}

- (NSString *)hubValue {
    return _hubValue.stringValue;
}

- (BOOL)emptyStateVisible {
    return _emptyView != nil && !_emptyView.hidden;
}

- (BOOL)scanProgressVisible {
    return _progressView != nil && !_progressView.hidden;
}

// ---------------------------------------------------------------------------------------
#pragma mark - View construction
// ---------------------------------------------------------------------------------------

- (void)loadView {
    // Window-background material: the strip under the (full-size-content) toolbar matches
    // the window chrome in both appearances.
    NSVisualEffectView *root = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 760, 620)];
    root.material = NSVisualEffectMaterialWindowBackground;
    root.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    root.state = NSVisualEffectStateFollowsWindowActiveState;

    _breadcrumb = [[BreadcrumbBar alloc] initWithFrame:NSZeroRect];
    _breadcrumb.delegate = self;
    _breadcrumb.translatesAutoresizingMaskIntoConstraints = NO;

    _banner = [[PermissionBannerView alloc] initWithFrame:NSZeroRect];
    _banner.translatesAutoresizingMaskIntoConstraints = NO;

    _mapContainer = [[DPFlippedView alloc] initWithFrame:NSZeroRect];
    _mapContainer.translatesAutoresizingMaskIntoConstraints = NO;

    _mapView = [[DiskMapMetalView alloc] initWithFrame:NSMakeRect(0, 0, 760, 560) shaderPath:_shaderPath];
    _mapView.mapDelegate = self;
    _mapView.translatesAutoresizingMaskIntoConstraints = NO;
    [_mapContainer addSubview:_mapView];

    [self buildHubView];
    [self buildEmptyView];
    [self buildProgressView];

    _hud = [[HoverHudView alloc] initWithFrame:NSMakeRect(0, 0, 304, 120)];
    [_mapContainer addSubview:_hud];  // positioned by frame, next to the pointer

    NSStackView *column = [NSStackView stackViewWithViews:@[_breadcrumb, _banner, _mapContainer]];
    column.orientation = NSUserInterfaceLayoutOrientationVertical;
    column.alignment = NSLayoutAttributeLeading;
    column.spacing = 0;
    column.detachesHiddenViews = YES;  // a hidden banner takes no room
    column.translatesAutoresizingMaskIntoConstraints = NO;
    [root addSubview:column];

    [NSLayoutConstraint activateConstraints:@[
        [column.topAnchor constraintEqualToAnchor:root.safeAreaLayoutGuide.topAnchor],
        [column.leadingAnchor constraintEqualToAnchor:root.leadingAnchor],
        [column.trailingAnchor constraintEqualToAnchor:root.trailingAnchor],
        [column.bottomAnchor constraintEqualToAnchor:root.bottomAnchor],
        [_breadcrumb.widthAnchor constraintEqualToAnchor:column.widthAnchor],
        [_banner.widthAnchor constraintEqualToAnchor:column.widthAnchor],
        [_mapContainer.widthAnchor constraintEqualToAnchor:column.widthAnchor],
        [_mapView.leadingAnchor constraintEqualToAnchor:_mapContainer.leadingAnchor],
        [_mapView.trailingAnchor constraintEqualToAnchor:_mapContainer.trailingAnchor],
        [_mapView.topAnchor constraintEqualToAnchor:_mapContainer.topAnchor],
        [_mapView.bottomAnchor constraintEqualToAnchor:_mapContainer.bottomAnchor],
    ]];
    [_mapContainer setContentHuggingPriority:NSLayoutPriorityDefaultLow - 20 forOrientation:NSLayoutConstraintOrientationVertical];
    self.view = root;
}

/// Name and size of the focused folder, centred on the sunburst's hub. Text is drawn by
/// AppKit above the Metal layer; the disc itself is part of the Metal scene.
- (void)buildHubView {
    _hubView = [[DPPassthroughView alloc] initWithFrame:NSZeroRect];
    _hubView.translatesAutoresizingMaskIntoConstraints = NO;
    _hubView.hidden = YES;

    _hubTitle = DPMakeLabel(@"", [NSFont systemFontOfSize:12 weight:NSFontWeightMedium], NSColor.secondaryLabelColor);
    _hubTitle.alignment = NSTextAlignmentCenter;
    NSFont *valueFont = [NSFont monospacedDigitSystemFontOfSize:21 weight:NSFontWeightSemibold];
    _hubValue = DPMakeLabel(@"", valueFont, NSColor.labelColor);
    _hubValue.alignment = NSTextAlignmentCenter;
    _hubValue.lineBreakMode = NSLineBreakByClipping;
    _hubDetail = DPMakeLabel(@"", [NSFont systemFontOfSize:10.5], NSColor.tertiaryLabelColor);
    _hubDetail.alignment = NSTextAlignmentCenter;

    NSStackView *stack = [NSStackView stackViewWithViews:@[_hubTitle, _hubValue, _hubDetail]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 1;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_hubView addSubview:stack];
    [_mapContainer addSubview:_hubView];

    _hubWidth = [_hubView.widthAnchor constraintEqualToConstant:120];
    [NSLayoutConstraint activateConstraints:@[
        [_hubView.centerXAnchor constraintEqualToAnchor:_mapContainer.centerXAnchor],
        [_hubView.centerYAnchor constraintEqualToAnchor:_mapContainer.centerYAnchor],
        _hubWidth,
        [stack.leadingAnchor constraintEqualToAnchor:_hubView.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:_hubView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:_hubView.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:_hubView.bottomAnchor],
        [_hubTitle.widthAnchor constraintLessThanOrEqualToAnchor:_hubView.widthAnchor],
        [_hubValue.widthAnchor constraintLessThanOrEqualToAnchor:_hubView.widthAnchor],
        [_hubDetail.widthAnchor constraintLessThanOrEqualToAnchor:_hubView.widthAnchor],
    ]];
}

- (void)buildEmptyView {
    NSImageView *icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    NSImage *symbol = DPSymbol(@"chart.pie.fill", @"Disk usage");
    if (symbol != nil) {
        NSImageSymbolConfiguration *configuration = [NSImageSymbolConfiguration configurationWithPointSize:46 weight:NSFontWeightRegular];
        icon.image = [symbol imageWithSymbolConfiguration:configuration];
    }
    icon.contentTintColor = NSColor.tertiaryLabelColor;

    NSTextField *title = DPMakeLabel(@"See what is using your disk", [NSFont systemFontOfSize:20 weight:NSFontWeightSemibold], NSColor.labelColor);
    title.alignment = NSTextAlignmentCenter;
    NSTextField *subtitle = [NSTextField wrappingLabelWithString:@"Scan a volume or choose any folder — or drop one anywhere in this window."];
    subtitle.font = [NSFont systemFontOfSize:13];
    subtitle.textColor = NSColor.secondaryLabelColor;
    subtitle.alignment = NSTextAlignmentCenter;
    subtitle.selectable = NO;
    subtitle.translatesAutoresizingMaskIntoConstraints = NO;

    // nil target: the actions travel up the responder chain to the window controller.
    _emptyScanButton = [NSButton buttonWithTitle:@"Scan Macintosh HD" target:nil action:NSSelectorFromString(@"scanDefaultTarget:")];
    _emptyScanButton.bezelStyle = NSBezelStylePush;
    _emptyScanButton.controlSize = NSControlSizeLarge;
    NSButton *choose = [NSButton buttonWithTitle:@"Choose Folder…" target:nil action:NSSelectorFromString(@"chooseFolder:")];
    choose.bezelStyle = NSBezelStylePush;
    choose.controlSize = NSControlSizeLarge;
    NSStackView *buttons = [NSStackView stackViewWithViews:@[_emptyScanButton, choose]];
    buttons.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    buttons.spacing = 10;

    NSStackView *stack = [NSStackView stackViewWithViews:@[icon, title, subtitle, buttons]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 8;
    [stack setCustomSpacing:14 afterView:icon];
    [stack setCustomSpacing:18 afterView:subtitle];
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    stack.hidden = YES;
    _emptyView = stack;
    [_mapContainer addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerXAnchor constraintEqualToAnchor:_mapContainer.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:_mapContainer.centerYAnchor constant:-10],
        [subtitle.widthAnchor constraintLessThanOrEqualToConstant:380],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:_mapContainer.widthAnchor constant:-40],
    ]];
}

- (void)buildProgressView {
    _progressView = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    _progressView.material = NSVisualEffectMaterialPopover;
    _progressView.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    _progressView.state = NSVisualEffectStateActive;
    _progressView.wantsLayer = YES;
    _progressView.layer.cornerRadius = 14;
    _progressView.layer.masksToBounds = YES;
    _progressView.layer.borderWidth = 0.5;
    _progressView.layer.borderColor = [NSColor.separatorColor colorWithAlphaComponent:0.6].CGColor;
    _progressView.translatesAutoresizingMaskIntoConstraints = NO;
    _progressView.hidden = YES;

    _spinner = [[NSProgressIndicator alloc] initWithFrame:NSZeroRect];
    _spinner.style = NSProgressIndicatorStyleSpinning;
    _spinner.indeterminate = YES;
    _spinner.controlSize = NSControlSizeRegular;
    _spinner.displayedWhenStopped = NO;

    _progressTitle = DPMakeLabel(@"Scanning…", [NSFont systemFontOfSize:14 weight:NSFontWeightSemibold], NSColor.labelColor);
    _progressTitle.alignment = NSTextAlignmentCenter;
    _progressPrimary = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:13 weight:NSFontWeightRegular], NSColor.labelColor);
    _progressPrimary.alignment = NSTextAlignmentCenter;
    _progressSecondary = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular], NSColor.secondaryLabelColor);
    _progressSecondary.alignment = NSTextAlignmentCenter;

    NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:nil action:NSSelectorFromString(@"cancelScan:")];
    cancel.bezelStyle = NSBezelStylePush;
    _progressCancelButton = cancel;

    NSStackView *stack = [NSStackView stackViewWithViews:@[_spinner, _progressTitle, _progressPrimary, _progressSecondary, cancel]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeCenterX;
    stack.spacing = 6;
    [stack setCustomSpacing:12 afterView:_spinner];
    [stack setCustomSpacing:14 afterView:_progressSecondary];
    stack.edgeInsets = NSEdgeInsetsMake(22, 28, 20, 28);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [_progressView addSubview:stack];
    [_mapContainer addSubview:_progressView];
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:_progressView.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:_progressView.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:_progressView.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:_progressView.bottomAnchor],
        [_progressView.centerXAnchor constraintEqualToAnchor:_mapContainer.centerXAnchor],
        [_progressView.centerYAnchor constraintEqualToAnchor:_mapContainer.centerYAnchor],
        [_progressView.widthAnchor constraintGreaterThanOrEqualToConstant:320],
        [_progressTitle.widthAnchor constraintLessThanOrEqualToConstant:420],
    ]];
}

- (void)viewDidLayout {
    [super viewDidLayout];
    [self updateHubGeometry];
    _hud.hidden = YES;  // its anchor moved; it reappears with the next pointer movement
}

// ---------------------------------------------------------------------------------------
#pragma mark - Model
// ---------------------------------------------------------------------------------------

- (void)setArena:(nullable const FlatArena *)arena
         matches:(nullable const NodeMatchSet *)matches
           focus:(uint32_t)focus
          metric:(SizeMetric)metric
        animated:(BOOL)animated {
    [self loadViewIfNeeded];
    _arena = arena;
    _metric = metric;
    _hud.hidden = YES;
    _mapView.sizeMetric = metric;
    [_mapView setArena:arena matches:matches focus:focus animated:animated];
    [self refreshFocusDependents];
}

- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated {
    [self loadViewIfNeeded];
    _hud.hidden = YES;
    [_mapView setFocusNode:node animated:animated];
    [self refreshFocusDependents];
}

- (void)setSelectedNode:(uint32_t)node {
    [self.mapView setSelectedNode:node];
}

- (void)setExternalHoverNode:(uint32_t)node {
    [self.mapView setExternalHoverNode:node];
}

- (void)setViewMode:(DPViewMode)mode animated:(BOOL)animated {
    [self loadViewIfNeeded];
    _mode = mode;
    _hud.hidden = YES;
    [_mapView setViewMode:mode animated:animated];
    [self refreshFocusDependents];
}

- (void)setMetric:(SizeMetric)metric {
    [self loadViewIfNeeded];
    _metric = metric;
    _hud.hidden = YES;
    _mapView.sizeMetric = metric;
    [_mapView reloadDataAnimated:YES];
    [self refreshFocusDependents];
}

- (void)reloadDataAnimated:(BOOL)animated {
    [self loadViewIfNeeded];
    _hud.hidden = YES;
    [_mapView reloadDataAnimated:animated];
    [self refreshFocusDependents];
}

/// Breadcrumb and hub both describe the focused directory.
- (void)refreshFocusDependents {
    const uint32_t focus = _mapView.focusNode;
    [_breadcrumb setArena:_arena focus:focus metric:_metric];

    const BOOL showHub = _mode == DPViewModeSunburst && _arena != nullptr && focus != kInvalidNode && _arena->isValid(focus) &&
                         !self.emptyStateVisible;
    if (showHub) {
        const FsNode &n = _arena->node(focus);
        _hubTitle.stringValue = DPNodeDisplayName(*_arena, focus);
        _hubValue.stringValue = DPFormatBytes(n.bytes(_metric));
        NSString *files = [NSString stringWithFormat:@"%@ %@", DPFormatCount(n.file_count), n.file_count == 1 ? @"file" : @"files"];
        _hubDetail.stringValue = n.parent_idx != kInvalidNode ? [files stringByAppendingString:@"  ·  click to zoom out"] : files;
    }
    _hubView.hidden = !showHub;
    [self updateHubGeometry];
}

- (void)updateHubGeometry {
    if (_mapView == nil) return;
    const NSRect hub = _mapView.hubRect;
    // Keep the text inside the disc (a square inscribed with a little slack).
    _hubWidth.constant = MAX(hub.size.width * 0.80, 60);
    const BOOL roomy = hub.size.width >= 150;
    _hubValue.font = [NSFont monospacedDigitSystemFontOfSize:roomy ? 21 : 15 weight:NSFontWeightSemibold];
    _hubDetail.hidden = hub.size.width < 190;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Overlays
// ---------------------------------------------------------------------------------------

- (void)showEmptyStateForVolumeNamed:(NSString *)volumeName {
    [self loadViewIfNeeded];
    _emptyScanButton.title = [NSString stringWithFormat:@"Scan “%@”", volumeName];
    // Return is this button's shortcut only while the invitation is showing; a hidden
    // default button must not be reachable from the keyboard or through accessibility.
    _emptyScanButton.keyEquivalent = @"\r";
    _emptyView.hidden = NO;
    _hubView.hidden = YES;
}

/// Takes Return away from the (now hidden) invitation button. Clearing the key equivalent
/// alone is not enough: the window remembers its default button cell.
- (void)disarmEmptyStateButton {
    _emptyScanButton.keyEquivalent = @"";
    NSWindow *window = self.view.window;
    if (window != nil && window.defaultButtonCell == _emptyScanButton.cell) window.defaultButtonCell = nil;
}

- (void)hideEmptyState {
    [self disarmEmptyStateButton];
    _emptyView.hidden = YES;
    [self refreshFocusDependents];
}

- (void)showScanProgressWithTitle:(NSString *)title {
    [self loadViewIfNeeded];
    _progressTitle.stringValue = title;
    _progressPrimary.stringValue = @"Starting…";
    _progressSecondary.stringValue = @" ";
    _progressCancelButton.keyEquivalent = @"\033";  // Escape cancels, while a scan is running
    _progressView.hidden = NO;
    [self disarmEmptyStateButton];
    _emptyView.hidden = YES;
    _hud.hidden = YES;
    [_spinner startAnimation:nil];
}

- (void)updateScanProgress:(NSString *)primary secondary:(NSString *)secondary {
    _progressPrimary.stringValue = primary;
    _progressSecondary.stringValue = secondary;
}

- (void)hideScanProgress {
    [_spinner stopAnimation:nil];  // nothing keeps animating once the scan is over
    _progressCancelButton.keyEquivalent = @"";  // Escape goes back to "zoom out" on the canvas
    _progressView.hidden = YES;
}

// ---------------------------------------------------------------------------------------
#pragma mark - DiskMapMetalViewDelegate
// ---------------------------------------------------------------------------------------

- (void)diskMapView:(DiskMapMetalView *)__unused view didHover:(DPMapHit)hit atPoint:(NSPoint)point {
    if (_arena == nullptr || hit.node == kInvalidNode || self.scanProgressVisible) {
        _hud.hidden = YES;
    } else {
        const BOOL sameItem = !_hud.hidden && _hudHit.node == hit.node && _hudHit.isAggregate == hit.isAggregate && _hudHit.isHub == hit.isHub;
        if (!sameItem) {
            [_hud showNode:hit.node aggregate:hit.isAggregate hub:hit.isHub arena:*_arena metric:_metric];
            _hudHit = hit;
        }
        [_hud positionNearPoint:point inBounds:_mapContainer.bounds];
    }
    [self.actionHandler nodeSource:self didHoverNode:hit.node];
}

- (void)diskMapView:(DiskMapMetalView *)__unused view didSelectNode:(uint32_t)node {
    [self.actionHandler nodeSource:self didSelectNode:node];
}

- (void)diskMapView:(DiskMapMetalView *)__unused view didRequestFocusNode:(uint32_t)node {
    [self.actionHandler nodeSource:self didRequestFocusNode:node];
}

- (nullable NSMenu *)diskMapView:(DiskMapMetalView *)__unused view menuForNode:(uint32_t)node {
    _hud.hidden = YES;
    return [self.actionHandler nodeSource:self menuForNode:node];
}

- (void)diskMapViewDidRequestQuickLook:(DiskMapMetalView *)__unused view {
    [self.actionHandler nodeSourceDidRequestQuickLook:self];
}

// ---------------------------------------------------------------------------------------
#pragma mark - BreadcrumbBarDelegate
// ---------------------------------------------------------------------------------------

- (void)breadcrumbBar:(BreadcrumbBar *)__unused bar didSelectNode:(uint32_t)node {
    [self.actionHandler nodeSource:self didRequestFocusNode:node];
}

@end
