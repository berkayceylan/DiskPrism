#import "MainWindowController.h"

#import <QuickLookUI/QuickLookUI.h>

#import "CanvasViewController.h"
#import "ExtensionListController.h"
#import "Formatting.h"
#import "ScanCoordinator.h"
#import "ToolbarViews.h"
#import "TreeOutlineController.h"
#include "BulkScanner.hpp"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"
#include "VolumeInfo.hpp"

#include <memory>
#include <optional>
#include <string>

using namespace diskprism;

namespace {

NSToolbarItemIdentifier const kToolbarTarget = @"com.berkayceylan.diskprism.toolbar.target";
NSToolbarItemIdentifier const kToolbarChoose = @"com.berkayceylan.diskprism.toolbar.choose";
NSToolbarItemIdentifier const kToolbarViewMode = @"com.berkayceylan.diskprism.toolbar.viewmode";
NSToolbarItemIdentifier const kToolbarMetric = @"com.berkayceylan.diskprism.toolbar.metric";
NSToolbarItemIdentifier const kToolbarScan = @"com.berkayceylan.diskprism.toolbar.scan";
NSToolbarItemIdentifier const kToolbarTelemetry = @"com.berkayceylan.diskprism.toolbar.telemetry";
NSToolbarItemIdentifier const kToolbarSearch = @"com.berkayceylan.diskprism.toolbar.search";

NSString *const kTerminalBundleIdentifier = @"com.apple.Terminal";

/// "1,234,567 items · 345.6 GB".
NSString *summaryOf(uint64_t items, uint64_t bytes) {
    return [NSString stringWithFormat:@"%@ %@  ·  %@", DPFormatCount(items), items == 1 ? @"item" : @"items", DPFormatBytes(bytes)];
}

}  // namespace

@interface MainWindowController () <NSWindowDelegate,
                                    NSToolbarDelegate,
                                    NSMenuDelegate,
                                    NSMenuItemValidation,
                                    NSSearchFieldDelegate,
                                    DPNodeActionHandler,
                                    ExtensionListDelegate,
                                    QLPreviewPanelDataSource,
                                    QLPreviewPanelDelegate>
@end

@implementation MainWindowController {
    // Model. The arena is owned here; the panes only borrow pointers to it.
    std::unique_ptr<FlatArena> _arena;
    NodeMatchSet _matches;
    FilterQuery _query;
    SizeMetric _metric;
    DPViewMode _viewMode;
    uint32_t _focus;
    uint32_t _selection;
    ScanResult _lastResult;
    BOOL _hasResult;

    NSString *_targetPath;
    NSString *_targetName;
    DPScanCoordinator *_coordinator;
    NSURL *_lastTrashedItemURL;

    // Panes.
    NSSplitViewController *_split;
    TreeOutlineController *_outline;
    CanvasViewController *_canvas;
    ExtensionListController *_extensions;

    // Toolbar.
    NSMenuToolbarItem *_targetItem;
    NSMenu *_targetMenu;
    NSToolbarItemGroup *_viewModeGroup;
    NSToolbarItemGroup *_metricGroup;
    NSToolbarItem *_scanItem;
    DPTelemetryPillView *_telemetry;
    NSSearchToolbarItem *_searchItem;
    NSString *_restingTelemetry;  // what the pill shows when no filter is active
    NSString *_scanDetail;        // one-line account of the last scan, for the pill's tooltip
}

// ---------------------------------------------------------------------------------------
#pragma mark - Construction
// ---------------------------------------------------------------------------------------

- (instancetype)initWithShaderPath:(nullable NSString *)shaderPath {
    const NSRect screen = NSScreen.mainScreen != nil ? NSScreen.mainScreen.visibleFrame : NSMakeRect(0, 0, 1440, 900);
    const CGFloat width = MIN(MAX(screen.size.width * 0.94, 1100), 1760);
    const CGFloat height = MIN(MAX(screen.size.height * 0.88, 640), 1050);
    const NSRect frame = NSMakeRect(NSMidX(screen) - width / 2, NSMidY(screen) - height / 2, width, height);

    NSWindow *window = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable |
                                                             NSWindowStyleMaskFullSizeContentView
                                                     backing:NSBackingStoreBuffered
                                                       defer:YES];
    self = [super initWithWindow:window];
    if (self == nil) return nil;

    _metric = SizeMetric::Physical;
    _viewMode = DPViewModeTreemap;
    _focus = kInvalidNode;
    _selection = kInvalidNode;
    _coordinator = [[DPScanCoordinator alloc] init];
    _restingTelemetry = @"Ready";
    _scanDetail = @"";

    _outline = [[TreeOutlineController alloc] initWithNibName:nil bundle:nil];
    _outline.actionHandler = self;
    _canvas = [[CanvasViewController alloc] initWithShaderPath:shaderPath];
    _canvas.actionHandler = self;
    _extensions = [[ExtensionListController alloc] initWithNibName:nil bundle:nil];
    _extensions.delegate = self;

    _split = [[NSSplitViewController alloc] initWithNibName:nil bundle:nil];
    NSSplitViewItem *treeItem = [NSSplitViewItem splitViewItemWithViewController:_outline];
    treeItem.minimumThickness = 300;
    treeItem.canCollapse = YES;
    treeItem.holdingPriority = 260;
    NSSplitViewItem *canvasItem = [NSSplitViewItem splitViewItemWithViewController:_canvas];
    canvasItem.minimumThickness = 380;
    canvasItem.holdingPriority = 240;  // the canvas absorbs window resizing
    NSSplitViewItem *typesItem = [NSSplitViewItem splitViewItemWithViewController:_extensions];
    typesItem.minimumThickness = 220;
    typesItem.maximumThickness = 340;
    typesItem.canCollapse = YES;
    typesItem.holdingPriority = 270;
    [_split addSplitViewItem:treeItem];
    [_split addSplitViewItem:canvasItem];
    [_split addSplitViewItem:typesItem];
    _split.splitView.dividerStyle = NSSplitViewDividerStyleThin;
    _split.splitView.autosaveName = @"DiskPrismSplit";

    window.title = @"DiskPrism";
    // The toolbar carries seven controls; the title text would take a quarter of it, and
    // the breadcrumb bar already shows where you are. The title still names the window
    // in Mission Control and the Window menu.
    window.titleVisibility = NSWindowTitleHidden;
    window.minSize = NSMakeSize(980, 600);
    window.toolbarStyle = NSWindowToolbarStyleUnified;
    window.restorable = NO;
    window.delegate = self;
    window.contentViewController = _split;
    [window setFrame:frame display:NO];
    // Dropping a folder or volume anywhere in the window starts a scan of it.
    [window registerForDraggedTypes:@[NSPasteboardTypeFileURL]];

    NSToolbar *toolbar = [[NSToolbar alloc] initWithIdentifier:@"com.berkayceylan.diskprism.toolbar.main"];
    toolbar.delegate = self;
    toolbar.displayMode = NSToolbarDisplayModeIconOnly;
    toolbar.allowsUserCustomization = NO;
    window.toolbar = toolbar;

    // Initial pane widths: tree ~41 % (all five columns visible), file types 284 pt,
    // canvas the rest.
    [_split.splitView layoutSubtreeIfNeeded];
    const CGFloat treeWidth = MIN(MAX(round(width * 0.41), 500), 660);
    [_split.splitView setPosition:treeWidth ofDividerAtIndex:0];
    [_split.splitView setPosition:width - 284 ofDividerAtIndex:1];

    const VolumeInfo boot = [self bootVolume];
    _targetPath = DPStringFromFileSystemBytes(boot.mount_point);
    _targetName = DPStringFromFileSystemBytes(boot.name);
    [self updateTargetTitle];
    [_canvas showEmptyStateForVolumeNamed:_targetName];
    [self updateControls];
    return self;
}

- (void)dealloc {
    [self detachPanes];
}

/// The panes hold raw pointers into the arena: clear them before it can go away.
- (void)detachPanes {
    [_outline setArena:nullptr matches:nullptr metric:_metric];
    [_extensions setArena:nullptr metric:_metric];
    [_canvas setArena:nullptr matches:nullptr focus:kInvalidNode metric:_metric animated:NO];
}

- (VolumeInfo)bootVolume {
    for (const VolumeInfo &volume : listUserVolumes()) {
        if (volume.is_root) return volume;
    }
    VolumeInfo fallback;
    fallback.mount_point = "/";
    fallback.name = "Macintosh HD";
    return fallback;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Properties
// ---------------------------------------------------------------------------------------

- (nullable const FlatArena *)arena {
    return _arena.get();
}

- (BOOL)isScanning {
    return _coordinator.isScanning;
}

- (BOOL)hasData {
    return _arena != nullptr;
}

- (uint32_t)focusNode {
    return _focus;
}

- (uint32_t)selectedNode {
    return _selection;
}

- (NSString *)telemetryText {
    return _telemetry != nil ? _telemetry.text : _restingTelemetry;
}

- (uint64_t)deniedFolderCount {
    return _hasResult ? _lastResult.totals.denied : 0;
}

- (nullable NSURL *)lastTrashedItemURL {
    return _lastTrashedItemURL;
}

- (TreeOutlineController *)outlineController {
    return _outline;
}

- (CanvasViewController *)canvasController {
    return _canvas;
}

- (ExtensionListController *)extensionController {
    return _extensions;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Toolbar
// ---------------------------------------------------------------------------------------

- (NSArray<NSToolbarItemIdentifier> *)toolbarDefaultItemIdentifiers:(NSToolbar *)__unused toolbar {
    return @[
        kToolbarTarget, kToolbarChoose, NSToolbarSpaceItemIdentifier, kToolbarViewMode, kToolbarMetric,
        NSToolbarSpaceItemIdentifier, kToolbarScan, kToolbarTelemetry, NSToolbarFlexibleSpaceItemIdentifier, kToolbarSearch
    ];
}

- (NSArray<NSToolbarItemIdentifier> *)toolbarAllowedItemIdentifiers:(NSToolbar *)toolbar {
    return [self toolbarDefaultItemIdentifiers:toolbar];
}

- (nullable NSToolbarItem *)toolbar:(NSToolbar *)__unused toolbar
              itemForItemIdentifier:(NSToolbarItemIdentifier)identifier
          willBeInsertedIntoToolbar:(BOOL)__unused inserted {
    if ([identifier isEqualToString:kToolbarTarget]) {
        // Quick drop-down of the mounted volumes; rebuilt whenever it opens (see
        // -menuNeedsUpdate:) so capacity bars and free space are always current.
        _targetMenu = [[NSMenu alloc] initWithTitle:@"Scan Target"];
        _targetMenu.delegate = self;
        _targetMenu.autoenablesItems = NO;
        _targetItem = [[NSMenuToolbarItem alloc] initWithItemIdentifier:identifier];
        _targetItem.menu = _targetMenu;
        _targetItem.showsIndicator = YES;
        _targetItem.label = @"Target";
        _targetItem.toolTip = @"Choose the volume or folder to scan";
        [self updateTargetTitle];
        return _targetItem;
    }
    if ([identifier isEqualToString:kToolbarChoose]) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
        item.label = @"Choose Folder…";
        item.toolTip = @"Choose a folder to scan";
        item.image = DPSymbol(@"folder.badge.plus", @"Choose Folder");
        if (item.image == nil) item.title = @"Choose Folder…";
        item.bordered = YES;
        item.target = self;
        item.action = @selector(chooseFolder:);
        return item;
    }
    if ([identifier isEqualToString:kToolbarViewMode]) {
        _viewModeGroup = [NSToolbarItemGroup groupWithItemIdentifier:identifier
                                                              titles:@[@"Treemap", @"Sunburst"]
                                                       selectionMode:NSToolbarItemGroupSelectionModeSelectOne
                                                              labels:@[@"Treemap", @"Sunburst"]
                                                              target:self
                                                              action:@selector(viewModeGroupChanged:)];
        _viewModeGroup.label = @"View";
        _viewModeGroup.toolTip = @"Show the tree as a cushion treemap or as a sunburst";
        _viewModeGroup.selectedIndex = _viewMode == DPViewModeTreemap ? 0 : 1;
        return _viewModeGroup;
    }
    if ([identifier isEqualToString:kToolbarMetric]) {
        _metricGroup = [NSToolbarItemGroup groupWithItemIdentifier:identifier
                                                            titles:@[@"Physical (Disk)", @"Logical"]
                                                     selectionMode:NSToolbarItemGroupSelectionModeSelectOne
                                                            labels:@[@"Physical (Disk)", @"Logical"]
                                                            target:self
                                                            action:@selector(metricGroupChanged:)];
        _metricGroup.label = @"Size";
        _metricGroup.toolTip = @"Physical: space actually occupied on this disk. Logical: the files' apparent sizes.";
        _metricGroup.selectedIndex = _metric == SizeMetric::Physical ? 0 : 1;
        return _metricGroup;
    }
    if ([identifier isEqualToString:kToolbarScan]) {
        _scanItem = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
        _scanItem.bordered = YES;
        _scanItem.target = self;
        _scanItem.action = @selector(toggleScan:);
        [self updateControls];
        return _scanItem;
    }
    if ([identifier isEqualToString:kToolbarTelemetry]) {
        NSToolbarItem *item = [[NSToolbarItem alloc] initWithItemIdentifier:identifier];
        _telemetry = [[DPTelemetryPillView alloc] initWithFrame:NSMakeRect(0, 0, 280, 24)];
        _telemetry.text = _restingTelemetry;
        [_telemetry.widthAnchor constraintEqualToConstant:300].active = YES;  // fixed: no toolbar reflow while numbers tick
        item.view = _telemetry;
        item.label = @"Status";
        return item;
    }
    if ([identifier isEqualToString:kToolbarSearch]) {
        _searchItem = [[NSSearchToolbarItem alloc] initWithItemIdentifier:identifier];
        _searchItem.label = @"Filter";
        _searchItem.preferredWidthForSearchField = 200;
        _searchItem.searchField = [[DPSearchField alloc] initWithFrame:NSZeroRect];
        NSSearchField *field = _searchItem.searchField;
        field.placeholderString = @"Filter by name";
        field.delegate = self;
        field.target = self;
        field.action = @selector(searchFieldChanged:);
        field.sendsSearchStringImmediately = YES;  // filter as you type
        field.sendsWholeSearchString = NO;
        [field setAccessibilityLabel:@"Filter by name"];
        return _searchItem;
    }
    return nil;
}

- (void)updateTargetTitle {
    if (_targetItem != nil) _targetItem.title = _targetName.length > 0 ? _targetName : @"Choose Target";
}

/// Reflects the scan state in the Scan / Cancel button and the two mode switches.
- (void)updateControls {
    const BOOL scanning = self.isScanning;
    if (_scanItem != nil) {
        _scanItem.title = scanning ? @"Cancel" : (self.hasData ? @"Rescan" : @"Scan");
        _scanItem.label = _scanItem.title;
        _scanItem.toolTip = scanning ? @"Stop the scan and show what has been found so far"
                                     : [NSString stringWithFormat:@"Scan “%@”", _targetName];
    }
    if (_telemetry != nil) _telemetry.active = scanning;
    if (_viewModeGroup != nil) _viewModeGroup.selectedIndex = _viewMode == DPViewModeTreemap ? 0 : 1;
    if (_metricGroup != nil) _metricGroup.selectedIndex = _metric == SizeMetric::Physical ? 0 : 1;
}

- (void)viewModeGroupChanged:(NSToolbarItemGroup *)sender {
    self.viewMode = sender.selectedIndex == 0 ? DPViewModeTreemap : DPViewModeSunburst;
}

- (void)metricGroupChanged:(NSToolbarItemGroup *)sender {
    self.sizeMetric = sender.selectedIndex == 0 ? SizeMetric::Physical : SizeMetric::Logical;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Target menu
// ---------------------------------------------------------------------------------------

- (void)menuNeedsUpdate:(NSMenu *)menu {
    if (menu != _targetMenu) return;
    [menu removeAllItems];

    // An NSMenuToolbarItem uses the first item as the button's own title.
    NSMenuItem *header = [[NSMenuItem alloc] initWithTitle:_targetName.length > 0 ? _targetName : @"Choose Target" action:nil keyEquivalent:@""];
    header.hidden = YES;
    [menu addItem:header];

    NSMenuItem *volumesHeader = [[NSMenuItem alloc] initWithTitle:@"Volumes" action:nil keyEquivalent:@""];
    volumesHeader.enabled = NO;
    [menu addItem:volumesHeader];
    for (const VolumeInfo &volume : listUserVolumes()) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:DPStringFromFileSystemBytes(volume.name)
                                                      action:@selector(scanFromMenu:)
                                                keyEquivalent:@""];
        item.target = self;
        item.representedObject = DPStringFromFileSystemBytes(volume.mount_point);
        item.view = [[DPVolumeMenuItemView alloc] initWithVolume:volume];
        [menu addItem:item];
    }

    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem *foldersHeader = [[NSMenuItem alloc] initWithTitle:@"Folders" action:nil keyEquivalent:@""];
    foldersHeader.enabled = NO;
    [menu addItem:foldersHeader];
    struct Shortcut {
        NSString *title;
        NSString *path;
        NSString *symbol;
    };
    const Shortcut shortcuts[] = {
        {@"Home", NSHomeDirectory(), @"house"},
        {@"Applications", @"/Applications", @"square.grid.2x2"},
    };
    for (const Shortcut &shortcut : shortcuts) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:shortcut.title action:@selector(scanFromMenu:) keyEquivalent:@""];
        item.target = self;
        item.representedObject = shortcut.path;
        item.image = DPSymbol(shortcut.symbol, shortcut.title);
        [menu addItem:item];
    }
    NSMenuItem *choose = [[NSMenuItem alloc] initWithTitle:@"Choose Folder…" action:@selector(chooseFolder:) keyEquivalent:@""];
    choose.target = self;
    choose.image = DPSymbol(@"folder.badge.plus", @"Choose Folder");
    [menu addItem:choose];
}

- (void)scanFromMenu:(NSMenuItem *)sender {
    NSString *path = sender.representedObject;
    if ([path isKindOfClass:NSString.class]) [self scanPath:path];
}

// ---------------------------------------------------------------------------------------
#pragma mark - Scanning
// ---------------------------------------------------------------------------------------

- (IBAction)chooseFolder:(nullable id)__unused sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES;
    panel.canChooseFiles = NO;
    panel.allowsMultipleSelection = NO;
    panel.canCreateDirectories = NO;
    panel.showsHiddenFiles = YES;
    panel.prompt = @"Scan";
    panel.message = @"Choose a folder or volume to analyze";
    __weak MainWindowController *weakSelf = self;
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse response) {
                      if (response != NSModalResponseOK || panel.URL == nil) return;
                      [weakSelf scanPath:panel.URL.path];
                  }];
}

- (IBAction)scanDefaultTarget:(nullable id)__unused sender {
    const VolumeInfo boot = [self bootVolume];
    [self scanPath:DPStringFromFileSystemBytes(boot.mount_point)];
}

- (IBAction)rescan:(nullable id)__unused sender {
    if (_targetPath.length > 0) [self scanPath:_targetPath];
}

- (IBAction)cancelScan:(nullable id)__unused sender {
    if (self.isScanning) [_coordinator cancel];
}

- (IBAction)toggleScan:(nullable id)sender {
    if (self.isScanning) {
        [self cancelScan:sender];
    } else {
        [self rescan:sender];
    }
}

- (NSString *)displayNameForPath:(NSString *)path {
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    NSNumber *isVolume = nil;
    NSString *volumeName = nil;
    if ([url getResourceValue:&isVolume forKey:NSURLIsVolumeKey error:nil] && isVolume.boolValue &&
        [url getResourceValue:&volumeName forKey:NSURLVolumeNameKey error:nil] && volumeName.length > 0) {
        return volumeName;
    }
    return path.lastPathComponent.length > 0 ? path.lastPathComponent : path;
}

- (void)scanPath:(NSString *)path {
    NSString *standardized = path.stringByStandardizingPath;
    BOOL isDirectory = NO;
    if (![NSFileManager.defaultManager fileExistsAtPath:standardized isDirectory:&isDirectory]) {
        [self presentMessage:@"That folder could not be found." detail:standardized];
        return;
    }
    if (!isDirectory) standardized = standardized.stringByDeletingLastPathComponent;  // a file: scan its folder

    _targetPath = standardized;
    _targetName = [self displayNameForPath:standardized];
    [self updateTargetTitle];
    [self closeQuickLook];

    [_canvas hideEmptyState];
    [_canvas.banner showForDeniedCount:0];
    [_canvas showScanProgressWithTitle:[NSString stringWithFormat:@"Scanning “%@”…", _targetName]];
    _restingTelemetry = @"Scanning…";
    _telemetry.text = _restingTelemetry;

    __weak MainWindowController *weakSelf = self;
    [_coordinator scanPath:standardized
                sortMetric:_metric
                  progress:^(ScanProgress progress) {
                      [weakSelf scanDidProgress:progress];
                  }
                completion:^(FlatArena *_Nullable arena, ScanResult result) {
                    MainWindowController *strongSelf = weakSelf;
                    if (strongSelf == nil) {
                        delete arena;
                        return;
                    }
                    [strongSelf scanDidFinishWithArena:arena result:result];
                }];
    [self updateControls];
}

/// Live telemetry, ~18 times a second while a scan runs.
- (void)scanDidProgress:(const ScanProgress &)progress {
    // Everything found so far below the scan root (which is a directory itself).
    const uint64_t items = progress.files + (progress.directories > 0 ? progress.directories - 1 : 0);
    NSString *counts = summaryOf(items, progress.bytes(_metric));
    NSString *rate = [NSString stringWithFormat:@"%@ files/s", DPFormatCompactCount(progress.files_per_second)];
    _restingTelemetry = [NSString stringWithFormat:@"%@  ·  %@", counts, rate];
    _telemetry.text = _restingTelemetry;
    NSString *secondary = [NSString stringWithFormat:@"%@  ·  %@", rate, DPFormatDuration(progress.elapsed_seconds)];
    if (progress.denied > 0) {
        secondary = [secondary stringByAppendingFormat:@"  ·  %@ protected skipped", DPFormatCount(progress.denied)];
    }
    [_canvas updateScanProgress:counts secondary:secondary];
}

- (void)scanDidFinishWithArena:(nullable FlatArena *)arenaOwned result:(const ScanResult &)result {
    std::unique_ptr<FlatArena> arena(arenaOwned);
    [_canvas hideScanProgress];

    if (arena == nullptr) {
        // The root could not be scanned at all; whatever was on screen stays.
        _restingTelemetry = self.hasData ? _restingTelemetry : @"Ready";
        _telemetry.text = _restingTelemetry;
        if (!self.hasData) [_canvas showEmptyStateForVolumeNamed:DPStringFromFileSystemBytes([self bootVolume].name)];
        [self updateControls];
        [self presentMessage:@"The scan could not be started." detail:DPStringFromFileSystemBytes(result.error_message)];
        if (self.scanDidFinishHandler != nil) self.scanDidFinishHandler();
        return;
    }

    // Swap the trees: every pane is pointed at the new arena before the old one is freed.
    std::unique_ptr<FlatArena> previous = std::move(_arena);
    _arena = std::move(arena);
    _lastResult = result;
    _hasResult = YES;
    _focus = _arena->root();
    _selection = kInvalidNode;

    // Extension IDs belong to the old table; the name filter carries over.
    _query.extension_id = -1;
    _matches.compute(*_arena, _query);

    [_outline setArena:_arena.get() matches:&_matches metric:_metric];
    [_extensions setArena:_arena.get() metric:_metric];
    _extensions.selectedExtension = -1;
    [_canvas setArena:_arena.get() matches:&_matches focus:_focus metric:_metric animated:YES];
    previous.reset();

    // Slim banner if macOS (TCC) or Unix permissions kept folders out of the scan.
    [_canvas.banner showForDeniedCount:result.totals.denied];

    NSString *counts = [self arenaSummary];
    switch (result.status) {
        case ScanStatus::Cancelled:
            _restingTelemetry = [NSString stringWithFormat:@"Cancelled — partial: %@", counts];
            break;
        case ScanStatus::ArenaFull:
            _restingTelemetry = [NSString stringWithFormat:@"Truncated — %@", counts];
            break;
        default:
            _restingTelemetry = [NSString stringWithFormat:@"%@  ·  %@ files/s", counts,
                                                           DPFormatCompactCount(result.totals.files_per_second)];
            break;
    }
    _scanDetail = [NSString stringWithFormat:@"Scanned %@ files and %@ folders in %@ on %u threads", DPFormatCount(result.totals.files),
                                             DPFormatCount(result.totals.directories), DPFormatDuration(result.scan_seconds),
                                             result.threads];
    [self refreshTelemetry];
    self.window.subtitle = DPNodePath(*_arena, _arena->root());
    [self updateControls];
    if (self.scanDidFinishHandler != nil) self.scanDidFinishHandler();
}

/// "N items · size" for everything below the scan root, in the active metric.
- (NSString *)arenaSummary {
    const uint32_t live = _arena->liveNodeCount();
    return summaryOf(live > 0 ? live - 1 : 0, _arena->node(_arena->root()).bytes(_metric));
}

/// The pill shows the scan summary, or what the active filter matched.
- (void)refreshTelemetry {
    if (_arena != nullptr && _matches.active()) {
        const uint64_t bytes = _metric == SizeMetric::Physical ? _matches.matchedAllocatedBytes() : _matches.matchedLogicalBytes();
        _telemetry.text = [NSString stringWithFormat:@"Filter: %@ %@  ·  %@", DPFormatCount(_matches.matchedFiles()),
                                                     _matches.matchedFiles() == 1 ? @"file" : @"files", DPFormatBytes(bytes)];
    } else {
        _telemetry.text = _restingTelemetry;
    }
    if (_arena != nullptr && _hasResult) {
        // Tooltip: how the scanned total relates to the volume it lives on.
        const std::optional<VolumeInfo> volume = volumeForPath(_arena->path(_arena->root()));
        if (volume.has_value()) {
            _telemetry.toolTip = [NSString stringWithFormat:@"%@\n%@\nVolume “%@”: %@ total, %@ available, %@ purgeable", _telemetry.text,
                                                            _scanDetail, DPStringFromFileSystemBytes(volume->name),
                                                            DPFormatBytes(volume->total_bytes),
                                                            DPFormatBytes(volume->important_available_bytes),
                                                            DPFormatBytes(volume->purgeable_bytes())];
        }
    }
}

// ---------------------------------------------------------------------------------------
#pragma mark - View mode, metric, filters
// ---------------------------------------------------------------------------------------

- (DPViewMode)viewMode {
    return _viewMode;
}

- (void)setViewMode:(DPViewMode)viewMode {
    if (viewMode == _viewMode) return;
    _viewMode = viewMode;
    // Same arena, same focus: only the picture changes.
    [_canvas setViewMode:viewMode animated:YES];
    [self updateControls];
}

- (SizeMetric)sizeMetric {
    return _metric;
}

- (void)setSizeMetric:(SizeMetric)sizeMetric {
    if (sizeMetric == _metric) return;
    _metric = sizeMetric;
    // Layouts and the outline rely on child lists sorted by the active metric.
    if (_arena != nullptr) _arena->sortChildren(sizeMetric);
    [_outline setMetric:sizeMetric];
    [_extensions setArena:_arena.get() metric:sizeMetric];
    [_canvas setMetric:sizeMetric];
    if (_arena != nullptr && _hasResult && !self.isScanning) {
        _restingTelemetry = [NSString stringWithFormat:@"%@  ·  %@", [self arenaSummary],
                                                       sizeMetric == SizeMetric::Physical ? @"on disk" : @"logical"];
    }
    [self refreshTelemetry];
    [self updateControls];
}

- (IBAction)showTreemap:(nullable id)__unused sender {
    self.viewMode = DPViewModeTreemap;
}

- (IBAction)showSunburst:(nullable id)__unused sender {
    self.viewMode = DPViewModeSunburst;
}

- (IBAction)usePhysicalSize:(nullable id)__unused sender {
    self.sizeMetric = SizeMetric::Physical;
}

- (IBAction)useLogicalSize:(nullable id)__unused sender {
    self.sizeMetric = SizeMetric::Logical;
}

- (void)applyFilter {
    if (_arena == nullptr) {
        _matches.clear();
        return;
    }
    _matches.compute(*_arena, _query);
    [_outline reloadKeepingState];
    [_canvas reloadDataAnimated:NO];
    [self refreshTelemetry];
}

- (void)setNameFilter:(NSString *)text {
    const std::string needle = text.UTF8String != nullptr ? std::string(text.UTF8String) : std::string();
    if (needle == _query.name_substring) return;
    _query.name_substring = needle;
    if (_searchItem != nil && ![_searchItem.searchField.stringValue isEqualToString:text]) _searchItem.searchField.stringValue = text;
    [self applyFilter];
}

- (void)setExtensionFilter:(int32_t)extensionId {
    const int32_t normalized = extensionId < 0 ? -1 : extensionId;
    if (normalized == _query.extension_id) return;
    _query.extension_id = normalized;
    _extensions.selectedExtension = normalized;
    [self applyFilter];
}

- (void)searchFieldChanged:(NSSearchField *)sender {
    [self setNameFilter:sender.stringValue];
}

/// Typing, pasting, dictation: filter on every change of the text.
- (void)controlTextDidChange:(NSNotification *)notification {
    if (_searchItem != nil && notification.object == _searchItem.searchField) [self setNameFilter:_searchItem.searchField.stringValue];
}

/// The field's own clear (x) button.
- (void)searchFieldDidEndSearching:(NSSearchField *)__unused sender {
    [self setNameFilter:@""];
}

- (void)extensionList:(ExtensionListController *)__unused list didSelectExtension:(int32_t)extensionId {
    [self setExtensionFilter:extensionId];
}

- (IBAction)focusSearchField:(nullable id)__unused sender {
    if (_searchItem != nil) [_searchItem beginSearchInteraction];
}

- (IBAction)clearFilters:(nullable id)__unused sender {
    [self setNameFilter:@""];
    [self setExtensionFilter:-1];
}

// ---------------------------------------------------------------------------------------
#pragma mark - Selection, hover, focus (DPNodeActionHandler)
// ---------------------------------------------------------------------------------------

- (void)selectNode:(uint32_t)node {
    [self nodeSource:self didSelectNode:node];
}

- (void)nodeSource:(id)source didSelectNode:(uint32_t)node {
    if (_arena == nullptr || !_arena->isValid(node)) return;
    _selection = node;

    // Tree → GPU: if the selection lies outside what the canvas shows, zoom out to the
    // closest folder that contains both, so the selection frame is actually visible.
    if (!_arena->isAncestorOrSelf(_focus, node)) {
        uint32_t common = _arena->node(node).parent_idx;
        while (common != kInvalidNode && !_arena->isAncestorOrSelf(common, _focus)) common = _arena->node(common).parent_idx;
        if (common != kInvalidNode) [self setFocusNode:common animated:YES];
    }
    // GPU → tree: a click on the canvas expands the path to the item and selects its row.
    if (source != _outline) [_outline revealNode:node];
    [_canvas setSelectedNode:node];
    [self reloadQuickLook];
}

- (void)nodeSource:(id)source didHoverNode:(uint32_t)node {
    if (source == _outline) {
        [_canvas setExternalHoverNode:node];
    } else {
        [_outline setExternalHoverNode:node];
    }
}

- (void)nodeSource:(id)__unused source didRequestFocusNode:(uint32_t)node {
    [self setFocusNode:node animated:YES];
}

- (nullable NSMenu *)nodeSource:(id)__unused source menuForNode:(uint32_t)node {
    return [self contextMenuForNode:node];
}

- (void)nodeSourceDidRequestQuickLook:(id)__unused source {
    [self quickLookSelection:nil];
}

- (void)nodeSource:(id)__unused source didRequestTrashNode:(uint32_t)node {
    [self trashNodeReportingErrors:node];
}

- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated {
    if (_arena == nullptr || !_arena->isValid(node)) return;
    // Only a directory can be focused; a file focuses its folder.
    const uint32_t target = _arena->node(node).is_dir() ? node : _arena->node(node).parent_idx;
    if (target == kInvalidNode || target == _focus) return;
    _focus = target;
    [_canvas setFocusNode:target animated:animated];
    _focus = _canvas.mapView.focusNode;
    // Keep the tree in step: the focused folder is revealed and opened.
    [_outline revealNode:_focus];
    [_outline.outlineView expandItem:@(_focus)];
    _selection = _focus;
    [_canvas setSelectedNode:_focus];
}

- (IBAction)zoomIntoSelection:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node != kInvalidNode) [self setFocusNode:node animated:YES];
}

- (IBAction)zoomOut:(nullable id)__unused sender {
    if (_arena == nullptr || _focus == kInvalidNode) return;
    const uint32_t parent = _arena->node(_focus).parent_idx;
    if (parent != kInvalidNode) [self setFocusNode:parent animated:YES];
}

- (IBAction)zoomToScanRoot:(nullable id)__unused sender {
    if (_arena != nullptr) [self setFocusNode:_arena->root() animated:YES];
}

// ---------------------------------------------------------------------------------------
#pragma mark - Context menu and item actions
// ---------------------------------------------------------------------------------------

/// The node an action applies to: the menu item's, or else the current selection.
- (uint32_t)nodeForSender:(nullable id)sender {
    uint32_t node = _selection;
    if ([sender isKindOfClass:NSMenuItem.class]) {
        id represented = static_cast<NSMenuItem *>(sender).representedObject;
        if ([represented isKindOfClass:NSNumber.class]) node = static_cast<NSNumber *>(represented).unsignedIntValue;
    }
    return (_arena != nullptr && _arena->isValid(node)) ? node : kInvalidNode;
}

- (nullable NSMenu *)contextMenuForNode:(uint32_t)node {
    if (_arena == nullptr || !_arena->isValid(node)) return nil;
    const FsNode &n = _arena->node(node);
    const BOOL isRoot = node == _arena->root();
    NSString *name = DPNodeDisplayName(*_arena, node);

    NSMenu *menu = [[NSMenu alloc] initWithTitle:name];
    menu.autoenablesItems = NO;
    const auto add = [&](NSString *title, SEL action, NSString *symbol, BOOL enabled) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:@""];
        item.target = self;
        item.representedObject = @(node);
        item.enabled = enabled;
        item.image = DPSymbol(symbol, title);
        [menu addItem:item];
        return item;
    };

    const uint32_t zoomTarget = n.is_dir() ? node : n.parent_idx;
    const BOOL canZoom = zoomTarget != kInvalidNode && zoomTarget != _focus && _arena->node(zoomTarget).file_count > 0;
    add(n.is_dir() ? @"Zoom Into Folder" : @"Zoom Into Enclosing Folder", @selector(zoomIntoSelection:), @"plus.magnifyingglass", canZoom);
    add(@"Zoom Out", @selector(zoomOut:), @"minus.magnifyingglass", _focus != kInvalidNode && _arena->node(_focus).parent_idx != kInvalidNode);
    [menu addItem:NSMenuItem.separatorItem];
    add(@"Reveal in Finder", @selector(revealInFinder:), @"folder", YES);
    add(@"Quick Look", @selector(quickLookSelection:), @"eye", YES);
    add(@"Copy Full Path", @selector(copyFullPath:), @"doc.on.doc", YES);
    add(@"Open in Terminal", @selector(openInTerminal:), @"terminal", YES);
    [menu addItem:NSMenuItem.separatorItem];
    // The scan root and other volumes' mount points are not things to throw away here.
    add(@"Move to Trash", @selector(moveSelectionToTrash:), @"trash", !isRoot && !n.is_mount_point());
    return menu;
}

- (IBAction)revealInFinder:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node == kInvalidNode) return;
    [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[DPNodeURL(*_arena, node)]];
}

- (IBAction)copyFullPath:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node == kInvalidNode) return;
    NSPasteboard *pasteboard = NSPasteboard.generalPasteboard;
    [pasteboard clearContents];
    [pasteboard setString:DPNodePath(*_arena, node) forType:NSPasteboardTypeString];
}

- (IBAction)openInTerminal:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node == kInvalidNode) return;
    // A file opens its folder. Terminal is handed the folder URL through Launch Services;
    // no shell, script or subprocess of ours is involved.
    const uint32_t directory = _arena->node(node).is_dir() ? node : _arena->node(node).parent_idx;
    if (directory == kInvalidNode) return;
    NSURL *terminal = [NSWorkspace.sharedWorkspace URLForApplicationWithBundleIdentifier:kTerminalBundleIdentifier];
    if (terminal == nil) {
        [self presentMessage:@"Terminal could not be found." detail:@"The Terminal application is not installed on this Mac."];
        return;
    }
    __weak MainWindowController *weakSelf = self;
    [NSWorkspace.sharedWorkspace openURLs:@[DPNodeURL(*_arena, directory)]
                     withApplicationAtURL:terminal
                            configuration:[NSWorkspaceOpenConfiguration configuration]
                        completionHandler:^(NSRunningApplication *_Nullable __unused app, NSError *_Nullable error) {
                            if (error == nil) return;
                            dispatch_async(dispatch_get_main_queue(), ^{
                                [weakSelf presentMessage:@"The folder could not be opened in Terminal." detail:error.localizedDescription];
                            });
                        }];
}

- (IBAction)moveSelectionToTrash:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node != kInvalidNode) [self trashNodeReportingErrors:node];
}

- (void)trashNodeReportingErrors:(uint32_t)node {
    NSError *error = nil;
    if (![self moveNodeToTrash:node error:&error] && error != nil) {
        [self presentMessage:@"The item could not be moved to the Trash." detail:error.localizedDescription];
    }
}

- (BOOL)moveNodeToTrash:(uint32_t)node error:(NSError **)error {
    if (_arena == nullptr || !_arena->isValid(node) || node == _arena->root() || self.isScanning) {
        if (error != nil) {
            *error = [NSError errorWithDomain:NSCocoaErrorDomain
                                         code:NSFeatureUnsupportedError
                                     userInfo:@{NSLocalizedDescriptionKey : @"This item cannot be moved to the Trash from here."}];
        }
        return NO;
    }

    NSURL *resulting = nil;
    if (![NSFileManager.defaultManager trashItemAtURL:DPNodeURL(*_arena, node) resultingItemURL:&resulting error:error]) return NO;
    _lastTrashedItemURL = resulting;

    // The file system has changed; bring the model in line without rescanning anything:
    // O(depth) roll-back of the totals, histogram update, order restored along the chain.
    [self closeQuickLook];
    const uint32_t parent = _arena->node(node).parent_idx;
    const RemovalResult removed = _arena->removeNode(node);
    _arena->resortAncestorChain(parent);

    if (!_arena->isValid(_focus)) _focus = parent;  // the focused folder itself (or an ancestor of it) went away
    _selection = parent;
    if (_matches.active()) _matches.compute(*_arena, _query);

    [_outline reloadKeepingState];
    [_outline revealNode:parent];
    [_extensions reloadKeepingSelection];
    if (_extensions.selectedExtension != _query.extension_id) {
        // The last file of the filtered type is gone: the filter went with it.
        _query.extension_id = _extensions.selectedExtension;
        _matches.compute(*_arena, _query);
        [_outline reloadKeepingState];
    }
    // The GPU buffers are rebuilt from the updated arena and morph to the new layout.
    [_canvas reloadDataAnimated:YES];
    _focus = _canvas.mapView.focusNode;
    [_canvas setSelectedNode:_selection];

    const FsNode &root = _arena->node(_arena->root());
    _restingTelemetry = [NSString stringWithFormat:@"Moved %@ to Trash  ·  %@ left", DPFormatBytes(removed.bytes(_metric)),
                                                   DPFormatBytes(root.bytes(_metric))];
    [self refreshTelemetry];
    return YES;
}

- (void)presentMessage:(NSString *)message detail:(NSString *)detail {
    NSAlert *alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = message;
    alert.informativeText = detail;
    if (self.window.isVisible) {
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    } else {
        NSLog(@"DiskPrism: %@ %@", message, detail);
    }
}

// ---------------------------------------------------------------------------------------
#pragma mark - Quick Look
// ---------------------------------------------------------------------------------------

- (IBAction)quickLookSelection:(nullable id)sender {
    const uint32_t node = [self nodeForSender:sender];
    if (node == kInvalidNode) return;
    if (node != _selection) [self selectNode:node];
    QLPreviewPanel *panel = QLPreviewPanel.sharedPreviewPanel;
    if (panel.isVisible) {
        [panel orderOut:nil];
    } else {
        [self.window makeKeyWindow];
        [panel makeKeyAndOrderFront:nil];
    }
}

- (void)reloadQuickLook {
    if (QLPreviewPanel.sharedPreviewPanelExists && QLPreviewPanel.sharedPreviewPanel.isVisible) {
        [QLPreviewPanel.sharedPreviewPanel reloadData];
    }
}

- (void)closeQuickLook {
    if (QLPreviewPanel.sharedPreviewPanelExists && QLPreviewPanel.sharedPreviewPanel.isVisible) {
        [QLPreviewPanel.sharedPreviewPanel orderOut:nil];
    }
}

// The panel walks the responder chain looking for a controller; the window controller is on it.
- (BOOL)acceptsPreviewPanelControl:(QLPreviewPanel *)__unused panel {
    return _arena != nullptr && _arena->isValid(_selection);
}

- (void)beginPreviewPanelControl:(QLPreviewPanel *)panel {
    panel.dataSource = self;
    panel.delegate = self;
}

- (void)endPreviewPanelControl:(QLPreviewPanel *)panel {
    panel.dataSource = nil;
    panel.delegate = nil;
}

- (NSInteger)numberOfPreviewItemsInPreviewPanel:(QLPreviewPanel *)__unused panel {
    return (_arena != nullptr && _arena->isValid(_selection)) ? 1 : 0;
}

- (id<QLPreviewItem>)previewPanel:(QLPreviewPanel *)__unused panel previewItemAtIndex:(NSInteger)__unused index {
    return DPNodeURL(*_arena, _selection);
}

- (NSRect)previewPanel:(QLPreviewPanel *)__unused panel sourceFrameOnScreenForPreviewItem:(id<QLPreviewItem>)__unused item {
    // Zoom out of the item's row icon when it is visible, otherwise out of its block on the canvas.
    const NSRect rowRect = [_outline screenRectForNode:_selection];
    if (!NSIsEmptyRect(rowRect)) return rowRect;
    DiskMapMetalView *map = _canvas.mapView;
    const NSRect inMap = [map rectForNode:_selection];
    if (NSIsEmptyRect(inMap) || map.window == nil) return NSZeroRect;
    return [map.window convertRectToScreen:[map convertRect:inMap toView:nil]];
}

- (BOOL)previewPanel:(QLPreviewPanel *)__unused panel handleEvent:(NSEvent *)event {
    // Arrow keys keep navigating the tree while the preview is open, as in Finder.
    if (event.type == NSEventTypeKeyDown) {
        [_outline.outlineView keyDown:event];
        return YES;
    }
    return NO;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Menu validation
// ---------------------------------------------------------------------------------------

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    const SEL action = menuItem.action;
    const BOOL hasSelection = _arena != nullptr && _arena->isValid(_selection);
    if (action == @selector(showTreemap:)) {
        menuItem.state = _viewMode == DPViewModeTreemap ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(showSunburst:)) {
        menuItem.state = _viewMode == DPViewModeSunburst ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(usePhysicalSize:)) {
        menuItem.state = _metric == SizeMetric::Physical ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(useLogicalSize:)) {
        menuItem.state = _metric == SizeMetric::Logical ? NSControlStateValueOn : NSControlStateValueOff;
        return YES;
    }
    if (action == @selector(cancelScan:)) return self.isScanning;
    if (action == @selector(rescan:)) return !self.isScanning && _targetPath.length > 0;
    if (action == @selector(chooseFolder:) || action == @selector(scanDefaultTarget:)) return YES;
    if (action == @selector(zoomOut:)) return _arena != nullptr && _focus != kInvalidNode && _arena->node(_focus).parent_idx != kInvalidNode;
    if (action == @selector(zoomToScanRoot:)) return _arena != nullptr && _focus != _arena->root();
    if (action == @selector(clearFilters:)) return !_query.empty();
    if (action == @selector(focusSearchField:)) return YES;
    if (action == @selector(moveSelectionToTrash:)) {
        return hasSelection && _selection != _arena->root() && !_arena->node(_selection).is_mount_point() && !self.isScanning;
    }
    if (action == @selector(zoomIntoSelection:) || action == @selector(revealInFinder:) || action == @selector(quickLookSelection:) ||
        action == @selector(copyFullPath:) || action == @selector(openInTerminal:)) {
        return hasSelection;
    }
    return YES;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Window delegate, drag and drop
// ---------------------------------------------------------------------------------------

- (void)windowWillClose:(NSNotification *)__unused notification {
    [_coordinator cancel];
    [self closeQuickLook];
    [self detachPanes];
}

/// First directory (or file) URL on the pasteboard, if any.
- (nullable NSURL *)droppedURL:(id<NSDraggingInfo>)info {
    NSArray<NSURL *> *urls = [info.draggingPasteboard readObjectsForClasses:@[NSURL.class]
                                                                    options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    return urls.firstObject;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [self droppedURL:sender] != nil ? NSDragOperationGeneric : NSDragOperationNone;
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    return [self droppedURL:sender] != nil ? NSDragOperationGeneric : NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSURL *url = [self droppedURL:sender];
    if (url == nil) return NO;
    // Start right away, but after the drag session has ended.
    NSString *path = url.path;
    __weak MainWindowController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf scanPath:path];
    });
    return YES;
}

@end
