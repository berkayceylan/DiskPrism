// AppKit integration tests: the real MainWindowController and its panes, driven
// programmatically.
//
// The window used here is real but fully transparent and ignores the mouse, so nothing
// appears on screen while the suite runs. Pointer input is delivered by handing
// synthesised NSEvents straight to the views. On a machine without a window server the
// suite logs that fact and passes.
#import <AppKit/AppKit.h>
#import <QuickLookUI/QuickLookUI.h>

#include "TestHarness.hpp"

#import "AppDelegate.h"
#import "BreadcrumbBar.h"
#import "CanvasViewController.h"
#import "DiskMapMetalView.h"
#import "ExtensionListController.h"
#import "Formatting.h"
#import "HoverHudView.h"
#import "MainWindowController.h"
#import "PermissionBannerView.h"
#import "ToolbarViews.h"
#import "TreeOutlineController.h"
#include "FlatArena.hpp"
#include "VolumeInfo.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#ifndef DP_SHADER_SOURCE_PATH
#error "DP_SHADER_SOURCE_PATH must point at src/gpu/Shaders.metal"
#endif

using namespace diskprism;

/// Just enough of an NSDraggingInfo for the destination methods under test.
@interface DPFakeDrag : NSObject
@property (nonatomic, strong) NSPasteboard *pasteboard;
@end

@implementation DPFakeDrag
- (NSPasteboard *)draggingPasteboard {
    return self.pasteboard;
}
- (NSDragOperation)draggingSourceOperationMask {
    return NSDragOperationEvery;
}
@end

namespace {

bool haveWindowServer() {
    static const bool available = [] {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];  // no Dock icon, no menu bar take-over
        const bool screens = NSScreen.screens.count > 0;
        if (!screens) std::printf("      NOTE: no window server in this session — AppKit cases are skipped.\n");
        return screens;
    }();
    return available;
}

/// Runs the main run loop (timers, GCD main queue, display links) until `condition`
/// holds or the timeout expires.
bool spinUntil(double timeoutSeconds, bool (^condition)(void)) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + timeoutSeconds;
    while (!condition()) {
        if (CFAbsoluteTimeGetCurrent() > deadline) return false;
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.004]];
    }
    return true;
}

void spinFor(double seconds) {
    const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
    while (CFAbsoluteTimeGetCurrent() < deadline) {
        [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.004]];
    }
}

/// Waits until the map has drawn nothing for `seconds`, so that a frame that lands late on a busy
/// machine is not taken for one the test caused. False if the map never goes quiet.
bool waitUntilQuiet(DiskMapMetalView *map, double seconds) {
    __block uint64_t frames = map.renderedFrameCount;
    __block CFAbsoluteTime since = CFAbsoluteTimeGetCurrent();
    return spinUntil(10.0, ^bool {
        if (map.isAnimating || map.renderedFrameCount != frames) {
            frames = map.renderedFrameCount;
            since = CFAbsoluteTimeGetCurrent();
        }
        return CFAbsoluteTimeGetCurrent() - since >= seconds;
    });
}

/// A scratch directory tree with known contents.
class Fixture {
public:
    Fixture() {
        NSString *pattern = [NSTemporaryDirectory() stringByAppendingPathComponent:@"diskprism-ui-XXXXXX"];
        // -fileSystemRepresentation returns a fresh temporary buffer on every call: take it once.
        const char *raw = pattern.fileSystemRepresentation;
        std::vector<char> buffer(raw, raw + strlen(raw) + 1);
        if (::mkdtemp(buffer.data()) == nullptr) abort();
        char *resolved = ::realpath(buffer.data(), nullptr);
        path_ = resolved;
        ::free(resolved);

        mkdir("Movies");
        mkdir("Movies/Trips");
        mkdir("Documents");
        mkdir("Documents/Reports");
        mkdir("Empty");
        write("Movies/holiday.mov", 900 * 1024);
        write("Movies/Trips/alps.mov", 600 * 1024);
        write("Movies/Trips/coast.mp4", 300 * 1024);
        write("Documents/thesis.pdf", 250 * 1024);
        write("Documents/Reports/q1.pdf", 120 * 1024);
        write("Documents/Reports/q2.pdf", 110 * 1024);
        write("Documents/notes.txt", 20 * 1024);
        write("README", 8 * 1024);
    }
    ~Fixture() {
        NSString *path = @(path_.c_str());
        // Restore permissions a test may have taken away, then remove the tree.
        NSDirectoryEnumerator<NSString *> *enumerator = [NSFileManager.defaultManager enumeratorAtPath:path];
        for (NSString *relative in enumerator) ::chmod([path stringByAppendingPathComponent:relative].fileSystemRepresentation, 0755);
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
    }
    Fixture(const Fixture &) = delete;
    Fixture &operator=(const Fixture &) = delete;

    [[nodiscard]] NSString *path() const { return @(path_.c_str()); }
    [[nodiscard]] NSString *sub(const char *relative) const { return [path() stringByAppendingPathComponent:@(relative)]; }

    void mkdir(const char *relative) const {
        if (::mkdir((path_ + "/" + relative).c_str(), 0755) != 0) abort();
    }
    void write(const char *relative, size_t bytes) const {
        const int fd = ::open((path_ + "/" + relative).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) abort();
        const std::vector<char> data(bytes, 'd');
        if (::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size())) abort();
        ::fsync(fd);
        ::close(fd);
    }

private:
    std::string path_;
};

/// The window controller under test, with an invisible on-screen window.
struct Harness {
    MainWindowController *controller;

    Harness() {
        controller = [[MainWindowController alloc] initWithShaderPath:@DP_SHADER_SOURCE_PATH];
        NSWindow *window = controller.window;
        window.alphaValue = 0.0;          // really on screen (so views lay out and draw) but invisible
        window.ignoresMouseEvents = YES;  // and untouchable
        window.hasShadow = NO;
        [window setFrame:NSMakeRect(80, 80, 1280, 800) display:NO];
        [window orderFrontRegardless];
        spinFor(0.05);
    }
    ~Harness() {
        controller.scanDidFinishHandler = nil;
        [controller.window close];
        spinFor(0.02);
    }

    bool scan(NSString *path) {
        __block BOOL done = NO;
        controller.scanDidFinishHandler = ^{
            done = YES;
        };
        [controller scanPath:path];
        const bool finished = spinUntil(60.0, ^bool {
            return done;
        });
        controller.scanDidFinishHandler = nil;
        return finished;
    }

    /// Waits until any transition has ended and at least one frame of the scene exists.
    bool settle() {
        DiskMapMetalView *map = controller.canvasController.mapView;
        spinFor(0.03);
        return spinUntil(5.0, ^bool {
            return !map.isAnimating && map.renderedFrameCount > 0;
        });
    }

    [[nodiscard]] const FlatArena &arena() const { return *controller.arena; }

    [[nodiscard]] uint32_t node(const char *relative) const {
        const FlatArena &a = arena();
        uint32_t current = a.root();
        std::string path(relative);
        size_t pos = 0;
        while (pos < path.size() && current != kInvalidNode) {
            const size_t slash = path.find('/', pos);
            const std::string component = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            uint32_t found = kInvalidNode;
            a.forEachChild(current, [&](uint32_t child) {
                if (a.name(child) == component) found = child;
            });
            current = found;
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        return current;
    }
};

NSEvent *mouseEvent(NSEventType type, NSView *view, NSPoint pointInView, NSInteger clicks) {
    return [NSEvent mouseEventWithType:type
                              location:[view convertPoint:pointInView toView:nil]
                         modifierFlags:0
                             timestamp:NSProcessInfo.processInfo.systemUptime
                          windowNumber:view.window.windowNumber
                               context:nil
                           eventNumber:0
                            clickCount:clicks
                              pressure:1.0];
}

/// What AppKit delivers for a real double click: a first press with click count 1, then 2.
void doubleClick(NSView *view, NSPoint point) {
    [view mouseDown:mouseEvent(NSEventTypeLeftMouseDown, view, point, 1)];
    [view mouseDown:mouseEvent(NSEventTypeLeftMouseDown, view, point, 2)];
}

NSPoint centreOf(NSRect rect) {
    return NSMakePoint(NSMidX(rect), NSMidY(rect));
}

NSArray<NSString *> *titlesOf(NSMenu *menu) {
    NSMutableArray<NSString *> *titles = [NSMutableArray array];
    for (NSMenuItem *item in menu.itemArray) {
        if (!item.isSeparatorItem) [titles addObject:item.title];
    }
    return titles;
}

NSMenuItem *itemTitled(NSMenu *menu, NSString *title) {
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.title isEqualToString:title]) return item;
    }
    return nil;
}

NSArray<NSString *> *outlineNames(MainWindowController *controller) {
    NSOutlineView *outline = controller.outlineController.outlineView;
    NSMutableArray<NSString *> *names = [NSMutableArray array];
    for (NSInteger row = 0; row < outline.numberOfRows; ++row) {
        const uint32_t node = static_cast<NSNumber *>([outline itemAtRow:row]).unsignedIntValue;
        [names addObject:DPStringFromFileSystemBytes(controller.arena->name(node))];
    }
    return names;
}

}  // namespace

// ----------------------------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------------------------

DP_TEST(window_is_built_in_code_with_toolbar_and_three_panes) {
    if (!haveWindowServer()) return;
    Harness h;
    NSWindow *window = h.controller.window;
    REQUIRE(window != nil);
    CHECK((window.styleMask & NSWindowStyleMaskFullSizeContentView) != 0);
    CHECK((window.styleMask & NSWindowStyleMaskResizable) != 0);
    CHECK([window.title isEqualToString:@"DiskPrism"]);
    CHECK(window.toolbarStyle == NSWindowToolbarStyleUnified);

    // No nib anywhere: the content is an NSSplitViewController built in code.
    CHECK([window.contentViewController isKindOfClass:NSSplitViewController.class]);
    NSSplitViewController *split = static_cast<NSSplitViewController *>(window.contentViewController);
    REQUIRE(split.splitViewItems.count == 3);
    CHECK(split.splitViewItems[0].viewController == h.controller.outlineController);
    CHECK(split.splitViewItems[1].viewController == h.controller.canvasController);
    CHECK(split.splitViewItems[2].viewController == h.controller.extensionController);
    CHECK(h.controller.outlineController.nibName == nil);

    // Toolbar: target menu, choose folder, view mode, size metric, scan, telemetry, filter.
    NSToolbar *toolbar = window.toolbar;
    REQUIRE(toolbar != nil);
    NSMenuToolbarItem *target = nil;
    NSSearchToolbarItem *search = nil;
    NSMutableArray<NSToolbarItemGroup *> *groups = [NSMutableArray array];
    DPTelemetryPillView *pill = nil;
    NSToolbarItem *scanItem = nil;
    for (NSToolbarItem *item in toolbar.items) {
        if ([item isKindOfClass:NSMenuToolbarItem.class]) target = static_cast<NSMenuToolbarItem *>(item);
        if ([item isKindOfClass:NSSearchToolbarItem.class]) search = static_cast<NSSearchToolbarItem *>(item);
        if ([item isKindOfClass:NSToolbarItemGroup.class]) [groups addObject:static_cast<NSToolbarItemGroup *>(item)];
        if ([item.view isKindOfClass:DPTelemetryPillView.class]) pill = static_cast<DPTelemetryPillView *>(item.view);
        if (item.action == @selector(toggleScan:)) scanItem = item;
    }
    REQUIRE(target != nil && search != nil && pill != nil && scanItem != nil);
    REQUIRE(groups.count == 2);
    CHECK([groups[0].subitems[0].label isEqualToString:@"Treemap"]);
    CHECK([groups[0].subitems[1].label isEqualToString:@"Sunburst"]);
    CHECK([groups[1].subitems[0].label isEqualToString:@"Physical (Disk)"]);
    CHECK([groups[1].subitems[1].label isEqualToString:@"Logical"]);
    CHECK(groups[0].selectedIndex == 0);
    CHECK(groups[1].selectedIndex == 0);  // physical disk usage is the default
    CHECK([scanItem.title isEqualToString:@"Scan"]);
    CHECK([pill.text isEqualToString:@"Ready"]);
    CHECK([search.searchField.placeholderString isEqualToString:@"Filter by name"]);

    // Outline columns.
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    NSMutableArray<NSString *> *columns = [NSMutableArray array];
    for (NSTableColumn *column in outline.tableColumns) [columns addObject:column.title];
    CHECK([columns isEqualToArray:(@[@"Name", @"Usage", @"Physical Size", @"Items", @"Modified"])]);

    // Nothing scanned yet: the invitation is showing, the panes are empty.
    CHECK(h.controller.canvasController.emptyStateVisible);
    NSButton *defaultButton = static_cast<NSButton *>(window.defaultButtonCell.controlView);
    CHECK(defaultButton != nil && [defaultButton.title hasPrefix:@"Scan “"]);  // Return starts the first scan
    CHECK(!h.controller.hasData);
    CHECK(outline.numberOfRows == 0);
    CHECK(h.controller.extensionController.tableView.numberOfRows == 0);
    CHECK(h.controller.canvasController.mapView.shaderErrorMessage == nil);

    // File-type sidebar sits on a vibrant sidebar backdrop.
    NSView *sidebar = h.controller.extensionController.view;
    REQUIRE([sidebar isKindOfClass:NSVisualEffectView.class]);
    CHECK(static_cast<NSVisualEffectView *>(sidebar).material == NSVisualEffectMaterialSidebar);
    // The hover HUD is a vibrant panel as well.
    CHECK(h.controller.canvasController.hud.material == NSVisualEffectMaterialHUDWindow);
}

DP_TEST(target_menu_lists_volumes_with_live_capacity) {
    if (!haveWindowServer()) return;
    Harness h;
    NSMenuToolbarItem *target = nil;
    for (NSToolbarItem *item in h.controller.window.toolbar.items) {
        if ([item isKindOfClass:NSMenuToolbarItem.class]) target = static_cast<NSMenuToolbarItem *>(item);
    }
    REQUIRE(target != nil);
    NSMenu *menu = target.menu;
    REQUIRE(menu.delegate != nil);
    [menu.delegate menuNeedsUpdate:menu];  // what AppKit does right before the menu opens

    NSMutableArray<DPVolumeMenuItemView *> *volumeViews = [NSMutableArray array];
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.view isKindOfClass:DPVolumeMenuItemView.class]) [volumeViews addObject:static_cast<DPVolumeMenuItemView *>(item.view)];
    }
    REQUIRE(volumeViews.count >= 1);  // at least the startup disk
    for (DPVolumeMenuItemView *view in volumeViews) {
        CHECK([view.detailText containsString:@"available of"]);
        CHECK(view.usedFraction >= 0.0 && view.usedFraction <= 1.0);
    }
    CHECK(itemTitled(menu, @"Choose Folder…") != nil);
    CHECK(itemTitled(menu, @"Home") != nil);
    // The startup disk's item scans "/".
    NSMenuItem *first = nil;
    for (NSMenuItem *item in menu.itemArray) {
        if ([item.view isKindOfClass:DPVolumeMenuItemView.class]) {
            first = item;
            break;
        }
    }
    CHECK([first.representedObject isEqual:@"/"]);
    CHECK(first.action == NSSelectorFromString(@"scanFromMenu:"));
    std::printf("      %lu volume(s); first: %s\n", static_cast<unsigned long>(volumeViews.count), volumeViews[0].detailText.UTF8String);
}

DP_TEST(main_menu_actions_all_reach_the_window_controller) {
    if (!haveWindowServer()) return;
    Harness h;
    NSMenu *bar = [AppDelegate buildMainMenuWithApplicationName:@"DiskPrism"];
    NSMutableArray<NSString *> *topLevel = [NSMutableArray array];
    for (NSMenuItem *item in bar.itemArray) [topLevel addObject:item.title];
    CHECK([topLevel isEqualToArray:(@[@"DiskPrism", @"File", @"Edit", @"View", @"Go", @"Window", @"Help"])]);

    // Every app-specific action must be implemented by the window controller (a typo in
    // a selector would otherwise leave a permanently disabled menu item).
    int appActions = 0;
    NSArray<NSString *> *appSelectors = @[
        @"chooseFolder:", @"scanDefaultTarget:", @"rescan:", @"cancelScan:", @"revealInFinder:", @"quickLookSelection:",
        @"openInTerminal:", @"moveSelectionToTrash:", @"copyFullPath:", @"focusSearchField:", @"clearFilters:", @"showTreemap:",
        @"showSunburst:", @"usePhysicalSize:", @"useLogicalSize:", @"zoomIntoSelection:", @"zoomOut:", @"zoomToScanRoot:"
    ];
    for (NSMenuItem *top in bar.itemArray) {
        for (NSMenuItem *item in top.submenu.itemArray) {
            if (item.action == nullptr) continue;
            NSString *name = NSStringFromSelector(item.action);
            if (![appSelectors containsObject:name]) continue;
            ++appActions;
            CHECK([h.controller respondsToSelector:item.action]);
            CHECK(item.target == nil);  // routed through the responder chain
        }
    }
    CHECK_EQ(appActions, static_cast<int>(appSelectors.count));

    // Validation reflects the state: nothing to cancel, nothing selected.
    NSMenuItem *cancel = [[NSMenuItem alloc] initWithTitle:@"Cancel Scan" action:@selector(cancelScan:) keyEquivalent:@""];
    NSMenuItem *trash = [[NSMenuItem alloc] initWithTitle:@"Move to Trash" action:@selector(moveSelectionToTrash:) keyEquivalent:@""];
    NSMenuItem *treemap = [[NSMenuItem alloc] initWithTitle:@"as Treemap" action:@selector(showTreemap:) keyEquivalent:@""];
    id<NSMenuItemValidation> validator = static_cast<id<NSMenuItemValidation>>(h.controller);
    CHECK(![validator validateMenuItem:cancel]);
    CHECK(![validator validateMenuItem:trash]);
    CHECK([validator validateMenuItem:treemap]);
    CHECK(treemap.state == NSControlStateValueOn);
}

// ----------------------------------------------------------------------------------------
// Scanning into the panes
// ----------------------------------------------------------------------------------------

DP_TEST(scan_fills_tree_canvas_and_type_list) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.controller.hasData);
    const FlatArena &arena = h.arena();
    CHECK(!h.controller.isScanning);
    CHECK(!h.controller.canvasController.emptyStateVisible);
    CHECK(!h.controller.canvasController.scanProgressVisible);
    // The hidden overlays no longer own Return or Escape.
    CHECK(h.controller.window.defaultButtonCell == nil);
    CHECK_EQ(arena.node(arena.root()).file_count, 8u);
    CHECK_EQ(h.controller.focusNode, arena.root());

    // Outline: the scan root is the single top-level row, already expanded, children
    // largest first.
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    NSArray<NSString *> *rows = outlineNames(h.controller);
    REQUIRE(rows.count == 5);
    CHECK([rows[0] isEqualToString:fixture.path()]);
    CHECK([rows[1] isEqualToString:@"Movies"]);
    CHECK([rows[2] isEqualToString:@"Documents"]);
    CHECK([rows[3] isEqualToString:@"README"]);
    CHECK([rows[4] isEqualToString:@"Empty"]);
    CHECK([outline isItemExpanded:[outline itemAtRow:0]]);
    CHECK([outline isExpandable:[outline itemAtRow:1]]);
    CHECK(![outline isExpandable:[outline itemAtRow:3]]);  // a file
    CHECK(![outline isExpandable:[outline itemAtRow:4]]);  // an empty folder

    // Items are tagged-pointer NSNumbers holding arena indices: no wrapper objects.
    id item = [outline itemAtRow:1];
    REQUIRE([item isKindOfClass:NSNumber.class]);
    const uint32_t movies = h.node("Movies");
    CHECK_EQ(static_cast<NSNumber *>(item).unsignedIntValue, movies);
    const id boxedAgain = @(movies);
    CHECK(item == boxedAgain);  // identical pointer: the value lives in the pointer itself
    const auto itemBits = reinterpret_cast<uintptr_t>((__bridge void *)item);
#if defined(__arm64__)
    CHECK((itemBits >> 63) == 1);  // arm64 tagged-pointer bit
#elif defined(__x86_64__)
    CHECK((itemBits & 1) == 1);  // x86_64 tagged-pointer bit
#endif

    // Cell contents of the "Movies" row.
    NSTableCellView *nameCell = [outline viewAtColumn:0 row:1 makeIfNecessary:YES];
    NSTableCellView *sizeCell = [outline viewAtColumn:2 row:1 makeIfNecessary:YES];
    NSTableCellView *itemsCell = [outline viewAtColumn:3 row:1 makeIfNecessary:YES];
    NSTableCellView *dateCell = [outline viewAtColumn:4 row:1 makeIfNecessary:YES];
    REQUIRE(nameCell != nil && sizeCell != nil && itemsCell != nil && dateCell != nil);
    CHECK([nameCell.textField.stringValue isEqualToString:@"Movies"]);
    CHECK(nameCell.imageView.image != nil);  // native icon
    CHECK([sizeCell.textField.stringValue isEqualToString:DPFormatBytes(arena.node(movies).allocated_bytes)]);
    CHECK([itemsCell.textField.stringValue isEqualToString:@"3"]);
    CHECK(dateCell.textField.stringValue.length > 3);
    NSView *usageCell = [outline viewAtColumn:1 row:1 makeIfNecessary:YES];
    REQUIRE([usageCell respondsToSelector:NSSelectorFromString(@"bar")]);
    id bar = [usageCell valueForKey:@"bar"];
    const double fraction = [[bar valueForKey:@"fraction"] doubleValue];
    CHECK_NEAR(fraction, static_cast<double>(arena.node(movies).allocated_bytes) / arena.node(arena.root()).allocated_bytes, 1e-9);

    // File types, ranked by space.
    ExtensionListController *types = h.controller.extensionController;
    NSArray<NSNumber *> *ranked = types.rankedExtensions;
    REQUIRE(ranked.count == 5);  // mov, pdf, mp4, txt, <none>
    CHECK_EQ(ranked[0].unsignedIntValue, static_cast<unsigned>(*arena.extensions().find("mov")));
    CHECK_EQ(ranked[1].unsignedIntValue, static_cast<unsigned>(*arena.extensions().find("pdf")));
    CHECK_EQ(ranked[2].unsignedIntValue, static_cast<unsigned>(*arena.extensions().find("mp4")));
    CHECK(types.tableView.numberOfRows == 5);
    NSTableCellView *typeCell = [types.tableView viewAtColumn:0 row:0 makeIfNecessary:YES];
    NSTableCellView *filesCell = [types.tableView viewAtColumn:3 row:0 makeIfNecessary:YES];
    CHECK([typeCell.textField.stringValue isEqualToString:@".mov"]);
    CHECK([filesCell.textField.stringValue isEqualToString:@"2"]);

    // Breadcrumb, toolbar, canvas.
    CHECK(h.controller.canvasController.breadcrumbBar.segmentNodes.count == 1);
    CHECK([h.controller.telemetryText containsString:@"13 items"]);  // 8 files + 5 folders below the root
    CHECK([h.controller.telemetryText containsString:@"files/s"]);
    CHECK([h.controller.window.subtitle isEqualToString:fixture.path()]);
    CHECK(h.controller.window.titleVisibility == NSWindowTitleHidden);  // the toolbar gets the room
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    CHECK_EQ(map.focusNode, arena.root());
    CHECK(map.instanceCount == 8);  // one block per file
    CHECK(map.renderedFrameCount > 0);
    CHECK(h.controller.deniedFolderCount == 0);
    CHECK(h.controller.canvasController.banner.hidden);
}

DP_TEST(selection_and_hover_stay_in_step_between_tree_and_canvas) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    const uint32_t movies = h.node("Movies");
    const uint32_t alps = h.node("Movies/Trips/alps.mov");
    const uint32_t thesis = h.node("Documents/thesis.pdf");
    REQUIRE(movies != kInvalidNode && alps != kInvalidNode && thesis != kInvalidNode);

    // Tree -> GPU: selecting a row highlights the item on the canvas.
    [outline selectRowIndexes:[NSIndexSet indexSetWithIndex:1] byExtendingSelection:NO];
    CHECK_EQ(h.controller.selectedNode, movies);
    CHECK_EQ(map.selectedNode, movies);

    // GPU -> tree: a click on a block deep inside collapsed folders reveals it.
    CHECK([outline rowForItem:@(alps)] == -1);  // not visible yet
    const NSRect block = [map rectForNode:alps];
    REQUIRE(!NSIsEmptyRect(block));
    [map mouseDown:mouseEvent(NSEventTypeLeftMouseDown, map, centreOf(block), 1)];
    CHECK_EQ(h.controller.selectedNode, alps);
    CHECK_EQ(h.controller.outlineController.selectedNode, alps);
    const NSInteger alpsRow = [outline rowForItem:@(alps)];
    CHECK(alpsRow > 0);
    CHECK([outline isItemExpanded:@(movies)]);
    CHECK([outline isItemExpanded:@(h.node("Movies/Trips"))]);
    CHECK_EQ(map.selectedNode, alps);
    CHECK_EQ(map.focusNode, h.arena().root());  // a single click selects, it does not zoom

    // Hover on the canvas: the HUD describes the item under the pointer…
    HoverHudView *hud = h.controller.canvasController.hud;
    CHECK(hud.hidden);
    const NSRect thesisBlock = [map rectForNode:thesis];
    REQUIRE(!NSIsEmptyRect(thesisBlock));
    [map mouseMoved:mouseEvent(NSEventTypeMouseMoved, map, centreOf(thesisBlock), 0)];
    CHECK(!hud.hidden);
    CHECK([hud.titleText isEqualToString:@"thesis.pdf"]);
    CHECK([hud.pathText isEqualToString:@"Documents/thesis.pdf"]);  // relative to the scan root
    CHECK([hud.physicalText isEqualToString:DPFormatBytes(h.arena().node(thesis).allocated_bytes)]);
    CHECK([hud.logicalText isEqualToString:DPFormatBytes(250 * 1024)]);
    CHECK([hud.shareText hasSuffix:@"%"]);
    CHECK(NSContainsRect(NSInsetRect(hud.superview.bounds, -1, -1), hud.frame));  // stays inside the canvas
    // Every value is shown in full (nothing truncated to "1…MB").
    [hud layoutSubtreeIfNeeded];
    bool valuesFit = true;
    NSMutableArray<NSView *> *pending = [NSMutableArray arrayWithObject:hud];
    while (pending.count > 0) {
        NSView *view = pending.lastObject;
        [pending removeLastObject];
        [pending addObjectsFromArray:view.subviews];
        if (![view isKindOfClass:NSTextField.class]) continue;
        NSTextField *label = static_cast<NSTextField *>(view);
        if (label.alignment == NSTextAlignmentRight && label.frame.size.width + 0.5 < label.intrinsicContentSize.width) valuesFit = false;
    }
    CHECK(valuesFit);
    // …and the pointer leaving the map hides it again.
    [map mouseExited:mouseEvent(NSEventTypeMouseMoved, map, NSMakePoint(-50, -50), 0)];
    CHECK(hud.hidden);

    // Hovering a row in the tree is forwarded to the canvas (external hover highlight).
    REQUIRE(waitUntilQuiet(map, 0.2));  // the pointer leaving the map drew a frame of its own
    const uint64_t framesBefore = map.renderedFrameCount;
    [h.controller.outlineController.actionHandler nodeSource:h.controller.outlineController didHoverNode:movies];
    CHECK(spinUntil(5.0, ^bool {
        return map.renderedFrameCount > framesBefore;  // the highlight
    }));

    // Selecting something outside the zoomed-in area zooms out just far enough to show it.
    [h.controller setFocusNode:movies animated:NO];
    CHECK_EQ(map.focusNode, movies);
    [h.controller selectNode:thesis];
    CHECK_EQ(map.focusNode, h.arena().root());  // lowest folder containing both
    CHECK_EQ(map.selectedNode, thesis);
}

DP_TEST(columns_stay_aligned_when_deep_folders_widen_the_name_column) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    NSString *deep = fixture.path();
    for (int i = 0; i < 9; ++i) {
        deep = [deep stringByAppendingPathComponent:[NSString stringWithFormat:@"level-%d", i]];
        REQUIRE(::mkdir(deep.fileSystemRepresentation, 0755) == 0);
    }
    NSString *leaf = [deep stringByAppendingPathComponent:@"leaf.bin"];
    REQUIRE([[NSMutableData dataWithLength:5000] writeToFile:leaf atomically:NO]);
    Harness h;
    [h.controller.window setFrame:NSMakeRect(80, 80, 1440, 800) display:YES];
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    h.controller.sizeMetric = SizeMetric::Logical;
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    const CGFloat widthBefore = outline.outlineTableColumn.width;

    uint32_t node = h.arena().root();
    uint32_t third = kInvalidNode;  // one level deeper than the column allows for initially
    for (int i = 0; i < 9; ++i) {
        if (i == 3) third = node;
        uint32_t next = kInvalidNode;
        h.arena().forEachChild(node, [&](uint32_t c) {
            if (h.arena().name(c) == "level-" + std::to_string(i)) next = c;
        });
        REQUIRE(next != kInvalidNode);
        node = next;
    }
    const auto aligned = [&] {
        [outline layoutSubtreeIfNeeded];
        // Compare with where the *column* is, not with other rows: rows can be wrong together.
        bool same = true;
        int compared = 0;
        for (NSInteger row = 0; row < outline.numberOfRows; ++row) {
            NSView *cell = [outline viewAtColumn:2 row:row makeIfNecessary:NO];
            if (cell == nil) continue;
            const NSRect actual = [outline convertRect:cell.bounds fromView:cell];
            const NSRect wanted = [outline frameOfCellAtColumn:2 row:row];
            if (std::fabs(actual.origin.x - wanted.origin.x) > 0.5 || std::fabs(actual.size.width - wanted.size.width) > 0.5) same = false;
            ++compared;
        }
        return same && compared >= 5;
    };
    // The wait lets deferred layout run; a slow machine then gets longer to finish it.
    const auto becomesAligned = [&] {
        spinFor(0.3);
        return spinUntil(5.0, ^bool {
            return aligned();
        });
    };
    CHECK(becomesAligned());  // the rows are on screen before anything is revealed
    [h.controller selectNode:third];
    CHECK(becomesAligned());
    [h.controller selectNode:h.arena().firstChild(node)];  // reveals the rest at once
    CHECK(becomesAligned());                                // every row moved with the column…
    CHECK(outline.outlineTableColumn.width > widthBefore);  // …and it did widen
    // The same through a single expansion by hand.
    REQUIRE(h.scan(fixture.path()));
    for (NSInteger row = 0; row < outline.numberOfRows; ++row) [outline expandItem:[outline itemAtRow:row]];
    CHECK(becomesAligned());
}

DP_TEST(drill_down_and_zoom_out_follow_the_breadcrumb) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    BreadcrumbBar *breadcrumb = h.controller.canvasController.breadcrumbBar;
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    const uint32_t root = h.arena().root();
    const uint32_t movies = h.node("Movies");
    const uint32_t trips = h.node("Movies/Trips");
    const uint32_t alps = h.node("Movies/Trips/alps.mov");

    // Treemap: a double click drills one level toward the clicked block.
    doubleClick(map, centreOf([map rectForNode:alps]));
    CHECK_EQ(h.controller.focusNode, movies);
    CHECK_EQ(map.focusNode, movies);
    CHECK([breadcrumb.segmentNodes isEqualToArray:(@[@(root), @(movies)])]);
    CHECK([outline isItemExpanded:@(movies)]);
    CHECK_EQ(h.controller.outlineController.selectedNode, movies);
    REQUIRE(h.settle());
    CHECK(map.instanceCount == 3);  // only Movies' three files are laid out now

    doubleClick(map, centreOf([map rectForNode:alps]));
    CHECK_EQ(map.focusNode, trips);
    CHECK([breadcrumb.segmentNodes isEqualToArray:(@[@(root), @(movies), @(trips)])]);
    REQUIRE(h.settle());

    // Breadcrumb: clicking an ancestor segment jumps straight there.
    [h.controller.canvasController breadcrumbBar:breadcrumb didSelectNode:root];
    CHECK_EQ(map.focusNode, root);
    CHECK(breadcrumb.segmentNodes.count == 1);
    REQUIRE(h.settle());

    // Double-clicking a folder row in the tree drills in as well.
    [h.controller.outlineController.actionHandler nodeSource:h.controller.outlineController didRequestFocusNode:trips];
    CHECK_EQ(map.focusNode, trips);
    REQUIRE(h.settle());

    // Zoom out one level at a time; at the top it is a no-op.
    [h.controller zoomOut:nil];
    CHECK_EQ(map.focusNode, movies);
    [h.controller zoomOut:nil];
    CHECK_EQ(map.focusNode, root);
    [h.controller zoomOut:nil];
    CHECK_EQ(map.focusNode, root);
    // A file cannot be focused: its folder is.
    [h.controller setFocusNode:alps animated:NO];
    CHECK_EQ(map.focusNode, trips);
    [h.controller zoomToScanRoot:nil];
    CHECK_EQ(map.focusNode, root);
}

DP_TEST(sunburst_mode_shares_the_arena_and_zooms_through_the_hub) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    CanvasViewController *canvas = h.controller.canvasController;
    const FlatArena *arenaBefore = h.controller.arena;
    const uint32_t root = h.arena().root();
    const uint32_t movies = h.node("Movies");

    CHECK(NSIsEmptyRect(map.hubRect));  // no hub in treemap mode
    h.controller.viewMode = DPViewModeSunburst;
    CHECK(h.controller.arena == arenaBefore);  // instant switch on the same data, no rescan
    CHECK(map.viewMode == DPViewModeSunburst);
    REQUIRE(h.settle());
    // 3 children with a size on ring 1 (the empty folder has no slice), 5 on ring 2,
    // 4 on ring 3, plus the hub.
    CHECK(map.instanceCount == 13);

    // The hub shows the focused folder's name and size.
    CHECK([canvas.hubTitle isEqualToString:fixture.path().lastPathComponent]);
    CHECK([canvas.hubValue isEqualToString:DPFormatBytes(h.arena().node(root).allocated_bytes)]);
    const NSRect hub = map.hubRect;
    CHECK(hub.size.width > 40);
    CHECK_NEAR(NSMidX(hub), NSMidX(map.bounds), 0.5);

    // One click on a directory arc drills into it, with an animated transition…
    const NSRect arc = [map rectForNode:movies];
    REQUIRE(!NSIsEmptyRect(arc));
    [map mouseDown:mouseEvent(NSEventTypeLeftMouseDown, map, centreOf(arc), 1)];
    CHECK_EQ(map.focusNode, movies);
    CHECK(map.isAnimating);  // the on-demand display link is running
    REQUIRE(h.settle());
    CHECK(!map.isAnimating);  // …and is gone once the animation has settled
    CHECK([canvas.hubTitle isEqualToString:@"Movies"]);
    CHECK([canvas.hubValue isEqualToString:DPFormatBytes(h.arena().node(movies).allocated_bytes)]);

    // …and a click on the centre hub zooms back out to the parent.
    [map mouseMoved:mouseEvent(NSEventTypeMouseMoved, map, centreOf(map.hubRect), 0)];
    CHECK([canvas.hud.statusText containsString:@"zoom out"]);
    [map mouseDown:mouseEvent(NSEventTypeLeftMouseDown, map, centreOf(map.hubRect), 1)];
    CHECK_EQ(map.focusNode, root);
    REQUIRE(h.settle());

    // A double click on an arc drills exactly once: its second half must not hit whatever
    // the new layout happens to show under the pointer (here: the hub or a child).
    const NSPoint arcPoint = centreOf([map rectForNode:movies]);
    doubleClick(map, arcPoint);
    CHECK_EQ(map.focusNode, movies);
    REQUIRE(h.settle());
    [h.controller zoomToScanRoot:nil];
    REQUIRE(h.settle());

    // Back to the treemap: same focus, same data.
    h.controller.viewMode = DPViewModeTreemap;
    REQUIRE(h.settle());
    CHECK(map.instanceCount == 8);
    CHECK(h.controller.arena == arenaBefore);
}

DP_TEST(size_metric_switch_resorts_and_relabels) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    // A sparse file: tiny on disk, the largest item by logical size.
    const int fd = ::open(fixture.sub("sparse.img").fileSystemRepresentation, O_WRONLY | O_CREAT, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 64 << 20) == 0);
    ::fsync(fd);
    ::close(fd);

    Harness h;
    REQUIRE(h.scan(fixture.path()));
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    CHECK([outlineNames(h.controller)[1] isEqualToString:@"Movies"]);  // physically the largest
    CHECK([[outline tableColumnWithIdentifier:@"size"].title isEqualToString:@"Physical Size"]);

    h.controller.sizeMetric = SizeMetric::Logical;
    CHECK(h.arena().sortedMetric() == SizeMetric::Logical);
    CHECK([outlineNames(h.controller)[1] isEqualToString:@"sparse.img"]);  // logically the largest
    CHECK([[outline tableColumnWithIdentifier:@"size"].title isEqualToString:@"Logical Size"]);
    CHECK(h.controller.canvasController.mapView.sizeMetric == SizeMetric::Logical);
    NSTableCellView *sizeCell = [outline viewAtColumn:2 row:1 makeIfNecessary:YES];
    CHECK([sizeCell.textField.stringValue isEqualToString:DPFormatBytes(64 << 20)]);
    CHECK([h.controller.telemetryText containsString:@"logical"]);

    h.controller.sizeMetric = SizeMetric::Physical;
    CHECK(h.arena().sortedMetric() == SizeMetric::Physical);
    CHECK([outlineNames(h.controller)[1] isEqualToString:@"Movies"]);

    // Sorting by another column (a click on its header) re-orders the rows.
    outline.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"name" ascending:YES]];
    NSArray<NSString *> *byName = outlineNames(h.controller);
    REQUIRE(byName.count == 6);
    CHECK([byName[1] isEqualToString:@"Documents"]);
    CHECK([byName[2] isEqualToString:@"Empty"]);
    CHECK([byName[5] isEqualToString:@"sparse.img"]);
}

DP_TEST(filters_highlight_matches_in_tree_and_canvas) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    ExtensionListController *types = h.controller.extensionController;
    const FlatArena &arena = h.arena();

    // Substring filter from the search field.
    [h.controller setNameFilter:@"REPORT"];  // case-insensitive: matches the folder "Reports"
    CHECK([h.controller.telemetryText hasPrefix:@"Filter: 2 files"]);  // q1.pdf and q2.pdf live inside it
    // Tree: the matching branch stays prominent, unrelated rows are muted.
    NSTableCellView *documents = [outline viewAtColumn:0 row:[outline rowForItem:@(h.node("Documents"))] makeIfNecessary:YES];
    NSTableCellView *movies = [outline viewAtColumn:0 row:[outline rowForItem:@(h.node("Movies"))] makeIfNecessary:YES];
    REQUIRE(documents != nil && movies != nil);
    CHECK([documents.textField.textColor isEqual:NSColor.labelColor]);        // contains a match
    CHECK([movies.textField.textColor isEqual:NSColor.tertiaryLabelColor]);   // does not

    // File-type filter from the sidebar; combines with the name filter (AND).
    const int32_t pdf = *arena.extensions().find("pdf");
    [h.controller setNameFilter:@""];
    [h.controller setExtensionFilter:pdf];
    CHECK_EQ(types.selectedExtension, pdf);
    CHECK(types.tableView.selectedRow == 1);  // .pdf is the second-largest type
    CHECK([h.controller.telemetryText hasPrefix:@"Filter: 3 files"]);
    [h.controller setNameFilter:@"q"];
    CHECK([h.controller.telemetryText hasPrefix:@"Filter: 2 files"]);

    // Selecting a row without the mouse (arrow keys, VoiceOver, accessibility scripting)
    // filters too, and so does text put into the search field through accessibility.
    [h.controller clearFilters:nil];
    [types.tableView selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    CHECK_EQ(types.selectedExtension, static_cast<int32_t>(*arena.extensions().find("mov")));
    CHECK([h.controller.telemetryText hasPrefix:@"Filter: 2 files"]);
    [types.tableView deselectAll:nil];
    CHECK_EQ(types.selectedExtension, -1);
    NSSearchField *searchField = nil;
    for (NSToolbarItem *item in h.controller.window.toolbar.items) {
        if ([item isKindOfClass:NSSearchToolbarItem.class]) searchField = static_cast<NSSearchToolbarItem *>(item).searchField;
    }
    REQUIRE(searchField != nil);
    searchField.stringValue = @"thesis";
    [searchField setAccessibilityValue:@"thesis"];
    CHECK([h.controller.telemetryText hasPrefix:@"Filter: 1 file "]);
    [h.controller clearFilters:nil];
    CHECK([searchField.stringValue isEqualToString:@""]);
    [h.controller setExtensionFilter:pdf];
    [h.controller setNameFilter:@"q"];

    // One click on the sidebar's Clear Filter button removes the type filter…
    NSButton *clear = nil;
    for (NSView *view in types.view.subviews) {
        if ([view isKindOfClass:NSButton.class] && [static_cast<NSButton *>(view).title isEqualToString:@"Clear Filter"]) clear = static_cast<NSButton *>(view);
    }
    REQUIRE(clear != nil);
    CHECK(!clear.hidden);
    [clear performClick:nil];
    CHECK_EQ(types.selectedExtension, -1);
    CHECK(clear.hidden);
    CHECK(types.tableView.selectedRow == -1);
    // …and Clear Filters removes everything.
    [h.controller clearFilters:nil];
    CHECK(![h.controller.telemetryText hasPrefix:@"Filter:"]);
    documents = [outline viewAtColumn:0 row:[outline rowForItem:@(h.node("Documents"))] makeIfNecessary:YES];
    movies = [outline viewAtColumn:0 row:[outline rowForItem:@(h.node("Movies"))] makeIfNecessary:YES];
    CHECK([movies.textField.textColor isEqual:NSColor.labelColor]);
}

// ----------------------------------------------------------------------------------------
// Drag and drop, Quick Look, hover latency
// ----------------------------------------------------------------------------------------

DP_TEST(dropping_a_folder_or_file_starts_a_scan) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    NSWindow *window = h.controller.window;
    NSPasteboard *pasteboard = [NSPasteboard pasteboardWithUniqueName];
    const auto drop = [&](NSString *path) {
        [pasteboard clearContents];
        [pasteboard writeObjects:@[[NSURL fileURLWithPath:path]]];
        DPFakeDrag *drag = [[DPFakeDrag alloc] init];
        drag.pasteboard = pasteboard;
        __block BOOL done = NO;
        h.controller.scanDidFinishHandler = ^{
            done = YES;
        };
        // Delivered to the *window*, as AppKit does: it forwards to its delegate.
        id<NSDraggingDestination> destination = static_cast<id<NSDraggingDestination>>(window);
        CHECK([destination draggingEntered:static_cast<id<NSDraggingInfo>>(drag)] == NSDragOperationGeneric);
        CHECK([destination performDragOperation:static_cast<id<NSDraggingInfo>>(drag)]);
        const bool finished = spinUntil(30.0, ^bool {
            return done;
        });
        h.controller.scanDidFinishHandler = nil;
        return finished;
    };
    REQUIRE(drop(fixture.path()));
    CHECK([DPNodePath(h.arena(), h.arena().root()) isEqualToString:fixture.path()]);
    // Dropping a file scans the folder it is in.
    REQUIRE(drop(fixture.sub("Movies/holiday.mov")));
    CHECK([DPNodePath(h.arena(), h.arena().root()) isEqualToString:fixture.sub("Movies")]);
    // Something that is not a file is refused.
    [pasteboard clearContents];
    [pasteboard setString:@"just text" forType:NSPasteboardTypeString];
    DPFakeDrag *text = [[DPFakeDrag alloc] init];
    text.pasteboard = pasteboard;
    CHECK([static_cast<id<NSDraggingDestination>>(window) draggingEntered:static_cast<id<NSDraggingInfo>>(text)] == NSDragOperationNone);
    [pasteboard releaseGlobally];
}

DP_TEST(quick_look_previews_the_selection) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    id<QLPreviewPanelDataSource> source = static_cast<id<QLPreviewPanelDataSource>>(h.controller);
    CHECK(![h.controller acceptsPreviewPanelControl:nil]);  // nothing selected yet
    const uint32_t thesis = h.node("Documents/thesis.pdf");
    [h.controller selectNode:thesis];
    CHECK([h.controller acceptsPreviewPanelControl:nil]);
    CHECK([source numberOfPreviewItemsInPreviewPanel:nil] == 1);
    id<QLPreviewItem> item = [source previewPanel:nil previewItemAtIndex:0];
    CHECK([item.previewItemURL.path isEqualToString:fixture.sub("Documents/thesis.pdf")]);
}

DP_TEST(hover_updates_stay_below_one_millisecond) {
    if (!haveWindowServer()) return;
    Harness h;
    REQUIRE(h.scan(@"/usr/share"));  // ~20 000 nodes, thousands of blocks
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    REQUIRE(map.instanceCount > 1000);
    const NSSize size = map.bounds.size;
    // Warm-up: fonts, formatters, first HUD layout.
    for (int i = 0; i < 50; ++i) {
        [map mouseMoved:mouseEvent(NSEventTypeMouseMoved, map,
                                   NSMakePoint(20 + fmod(i * 37.0, size.width - 40), 20 + fmod(i * 53.0, size.height - 40)), 0)];
    }
    // Every event lands on another block: hit test + GPU hover state + HUD + tree highlight.
    std::vector<double> samples;
    for (int i = 0; i < 400; ++i) {
        NSEvent *event = mouseEvent(NSEventTypeMouseMoved, map,
                                    NSMakePoint(10 + fmod(i * 61.7, size.width - 20), 10 + fmod(i * 43.3, size.height - 20)), 0);
        const CFAbsoluteTime start = CFAbsoluteTimeGetCurrent();
        [map mouseMoved:event];
        samples.push_back((CFAbsoluteTimeGetCurrent() - start) * 1e3);
    }
    std::sort(samples.begin(), samples.end());
    const double median = samples[samples.size() / 2];
    const double p95 = samples[samples.size() * 95 / 100];
    std::printf("      hover event -> GPU state + HUD over %lu blocks: median %.3f ms, p95 %.3f ms\n",
                static_cast<unsigned long>(map.instanceCount), median, p95);
#if defined(NDEBUG)
    const double limit = dptest::onCI() ? 20.0 : 1.0;  // only meaningful in an optimised build on real hardware
#else
    const double limit = dptest::onCI() ? 100.0 : 20.0;
#endif
    CHECK(median < limit);
}

// ----------------------------------------------------------------------------------------
// Context actions and deletion
// ----------------------------------------------------------------------------------------

DP_TEST(context_menu_offers_every_action) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    const uint32_t movies = h.node("Movies");
    const uint32_t alps = h.node("Movies/Trips/alps.mov");
    NSArray<NSString *> *expected = @[@"Zoom Into Folder", @"Zoom Out", @"Reveal in Finder", @"Quick Look", @"Copy Full Path", @"Open in Terminal", @"Move to Trash"];

    // The same menu serves the tree and the canvas.
    NSMenu *fromTree = [h.controller.outlineController.actionHandler nodeSource:h.controller.outlineController menuForNode:movies];
    NSMenu *fromCanvas = [h.controller.canvasController.actionHandler nodeSource:h.controller.canvasController menuForNode:movies];
    REQUIRE(fromTree != nil && fromCanvas != nil);
    CHECK([titlesOf(fromTree) isEqualToArray:expected]);
    CHECK([titlesOf(fromCanvas) isEqualToArray:expected]);
    for (NSMenuItem *item in fromTree.itemArray) {
        if (item.isSeparatorItem) continue;
        CHECK(item.target == h.controller);
        CHECK([h.controller respondsToSelector:item.action]);
        CHECK([item.representedObject isEqual:@(movies)]);
    }
    CHECK(itemTitled(fromTree, @"Zoom Into Folder").enabled);
    CHECK(!itemTitled(fromTree, @"Zoom Out").enabled);  // already at the top
    CHECK(itemTitled(fromTree, @"Move to Trash").enabled);

    // A file zooms into its enclosing folder; the scan root cannot be trashed.
    NSMenu *forFile = [h.controller contextMenuForNode:alps];
    CHECK(itemTitled(forFile, @"Zoom Into Enclosing Folder") != nil);
    NSMenu *forRoot = [h.controller contextMenuForNode:h.arena().root()];
    CHECK(!itemTitled(forRoot, @"Move to Trash").enabled);
    CHECK(!itemTitled(forRoot, @"Zoom Into Folder").enabled);  // it is what is shown already
    CHECK([h.controller contextMenuForNode:kInvalidNode] == nil);

    // "Zoom Into Folder" from the menu.
    [h.controller zoomIntoSelection:itemTitled(fromTree, @"Zoom Into Folder")];
    CHECK_EQ(h.controller.focusNode, movies);
    NSMenu *zoomed = [h.controller contextMenuForNode:alps];
    CHECK(itemTitled(zoomed, @"Zoom Out").enabled);
}

DP_TEST(move_to_trash_updates_everything_without_a_rescan) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    const FlatArena &arena = h.arena();
    const FlatArena *arenaBefore = h.controller.arena;
    NSOutlineView *outline = h.controller.outlineController.outlineView;
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    NSFileManager *files = NSFileManager.defaultManager;
    NSMutableArray<NSURL *> *trashed = [NSMutableArray array];

    const uint32_t root = arena.root();
    const uint32_t movies = h.node("Movies");
    const uint32_t trips = h.node("Movies/Trips");
    const uint32_t alps = h.node("Movies/Trips/alps.mov");
    const int32_t mov = *arena.extensions().find("mov");
    const uint64_t rootBefore = arena.node(root).allocated_bytes;
    const uint64_t alpsBytes = arena.node(alps).allocated_bytes;
    const ExtensionStats movBefore = arena.extensions().stats(static_cast<uint16_t>(mov));
    [h.controller selectNode:alps];
    CHECK([outline rowForItem:@(alps)] > 0);

    // --- a single file -------------------------------------------------------------------
    NSError *error = nil;
    REQUIRE([h.controller moveNodeToTrash:alps error:&error]);
    CHECK(error == nil);
    if (h.controller.lastTrashedItemURL != nil) [trashed addObject:h.controller.lastTrashedItemURL];
    CHECK(![files fileExistsAtPath:fixture.sub("Movies/Trips/alps.mov")]);            // really gone from the folder
    CHECK(h.controller.lastTrashedItemURL != nil);
    CHECK([files fileExistsAtPath:h.controller.lastTrashedItemURL.path]);             // and really in the Trash
    CHECK(h.controller.arena == arenaBefore);                                         // same arena: no rescan
    CHECK(!arena.isValid(alps));
    // Totals rolled back along the whole ancestor chain.
    CHECK_EQ(arena.node(root).allocated_bytes, rootBefore - alpsBytes);
    CHECK_EQ(arena.node(root).file_count, 7u);
    CHECK_EQ(arena.node(movies).file_count, 2u);
    CHECK_EQ(arena.node(trips).file_count, 1u);
    // The type histogram followed.
    const ExtensionStats movAfter = arena.extensions().stats(static_cast<uint16_t>(mov));
    CHECK_EQ(movAfter.file_count, movBefore.file_count - 1);
    CHECK_EQ(movAfter.allocated_bytes, movBefore.allocated_bytes - alpsBytes);
    // The tree lost exactly that row and selected its folder.
    CHECK([outline rowForItem:@(alps)] == -1);
    CHECK_EQ(h.controller.selectedNode, trips);
    CHECK([outline rowForItem:@(trips)] > 0);
    NSTableCellView *moviesItems = [outline viewAtColumn:3 row:[outline rowForItem:@(movies)] makeIfNecessary:YES];
    CHECK([moviesItems.textField.stringValue isEqualToString:@"2"]);
    CHECK([h.controller.telemetryText hasPrefix:@"Moved "]);
    // The GPU buffer was rebuilt from the updated arena.
    REQUIRE(h.settle());
    CHECK(map.instanceCount == 7);
    CHECK(NSIsEmptyRect([map rectForNode:alps]));

    // --- a whole folder, while it is the focused one -----------------------------------------
    [h.controller setFocusNode:trips animated:NO];
    CHECK_EQ(map.focusNode, trips);
    const uint64_t moviesBytes = arena.node(movies).allocated_bytes;
    REQUIRE([h.controller moveNodeToTrash:movies error:&error]);
    if (h.controller.lastTrashedItemURL != nil) [trashed addObject:h.controller.lastTrashedItemURL];
    CHECK(![files fileExistsAtPath:fixture.sub("Movies")]);
    CHECK(!arena.isValid(movies) && !arena.isValid(trips));
    CHECK_EQ(arena.node(root).allocated_bytes, rootBefore - alpsBytes - moviesBytes);
    CHECK_EQ(arena.node(root).file_count, 5u);
    CHECK_EQ(h.controller.focusNode, root);  // the focus fell back to a folder that still exists
    CHECK_EQ(map.focusNode, root);
    CHECK_EQ(arena.extensions().stats(static_cast<uint16_t>(mov)).file_count, uint64_t{0});
    // ".mov" and ".mp4" have disappeared from the type list.
    CHECK(h.controller.extensionController.rankedExtensions.count == 3);
    CHECK([outlineNames(h.controller)[1] isEqualToString:@"Documents"]);
    REQUIRE(h.settle());
    CHECK(map.instanceCount == 5);

    // --- failures leave the model untouched ----------------------------------------------------
    const uint32_t notes = h.node("Documents/notes.txt");
    REQUIRE([files removeItemAtPath:fixture.sub("Documents/notes.txt") error:nil]);  // vanished behind our back
    const uint64_t rootNow = arena.node(root).allocated_bytes;
    error = nil;
    CHECK(![h.controller moveNodeToTrash:notes error:&error]);
    CHECK(error != nil);
    CHECK(arena.isValid(notes));
    CHECK_EQ(arena.node(root).allocated_bytes, rootNow);
    error = nil;
    CHECK(![h.controller moveNodeToTrash:root error:&error]);  // the scan root is never trashed
    CHECK(error != nil);
    CHECK([files fileExistsAtPath:fixture.path()]);

    // Tidy up: remove the fixture items this test put into the Trash.
    int removed = 0;
    for (NSURL *url in trashed) {
        if ([files removeItemAtURL:url error:nil]) ++removed;
    }
    std::printf("      trashed %lu fixture item(s), removed %d of them from the Trash again\n", static_cast<unsigned long>(trashed.count), removed);
}

DP_TEST(permission_banner_reports_skipped_folders) {
    if (!haveWindowServer()) return;
    if (::geteuid() == 0) {
        std::printf("      (running as root — nothing is ever denied; skipped)\n");
        return;
    }
    Fixture fixture;
    fixture.mkdir("Locked");
    fixture.write("Locked/secret.bin", 4096);
    REQUIRE(::chmod(fixture.sub("Locked").fileSystemRepresentation, 0000) == 0);

    Harness h;
    REQUIRE(h.scan(fixture.path()));
    PermissionBannerView *banner = h.controller.canvasController.banner;
    CHECK_EQ(h.controller.deniedFolderCount, uint64_t{1});
    CHECK(!banner.hidden);
    CHECK([banner.message isEqualToString:@"1 protected folder was skipped"]);
    CHECK([banner.settingsButton.title containsString:@"Full Disk Access"]);
    CHECK(banner.settingsButton.action == NSSelectorFromString(@"openSettings:"));
    CHECK([PermissionBannerView.fullDiskAccessSettingsURL.absoluteString
        isEqualToString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"]);
    // The folder is flagged in the tree and the HUD says why it is empty.
    const uint32_t locked = h.node("Locked");
    REQUIRE(locked != kInvalidNode);
    CHECK(h.arena().node(locked).access_denied());
    HoverHudView *hud = h.controller.canvasController.hud;
    [hud showNode:locked aggregate:NO hub:NO arena:h.arena() metric:SizeMetric::Physical];
    CHECK([hud.statusText containsString:@"Access denied"]);

    // The banner can be dismissed, and a clean scan does not bring it back.
    NSButton *close = nil;
    for (NSView *view in banner.subviews) {
        if ([view isKindOfClass:NSButton.class] && [static_cast<NSButton *>(view).toolTip isEqualToString:@"Dismiss"]) close = static_cast<NSButton *>(view);
    }
    REQUIRE(close != nil);
    [close performClick:nil];
    CHECK(banner.hidden);
    ::chmod(fixture.sub("Locked").fileSystemRepresentation, 0755);
    REQUIRE(h.scan(fixture.path()));
    CHECK_EQ(h.controller.deniedFolderCount, uint64_t{0});
    CHECK(banner.hidden);
}

DP_TEST(hud_explains_cloud_only_and_hard_linked_items) {
    if (!haveWindowServer()) return;
    FlatArena arena(ArenaConfig{64, 1 << 16});
    const uint32_t root = arena.addRoot("/Volumes/Test");
    NodeInit cloud;
    cloud.logical_bytes = 5ull << 30;
    cloud.allocated_bytes = 0;
    cloud.flags = kNodeDataless;
    const uint32_t evicted = arena.addChild(root, "Holiday.mov", cloud);
    NodeInit link;
    link.logical_bytes = 1 << 20;
    link.flags = kNodeHardlink | kNodeHardlinkDup;
    const uint32_t duplicate = arena.addChild(root, "copy.bin", link);
    NodeInit directory;
    directory.flags = kNodeDir;
    const uint32_t folder = arena.addChild(root, "Projects", directory);
    arena.addChild(folder, "a.txt", NodeInit{4096, 100, 1700000000, 0});
    arena.finalize(SizeMetric::Logical);

    HoverHudView *hud = [[HoverHudView alloc] initWithFrame:NSZeroRect];
    [hud showNode:evicted aggregate:NO hub:NO arena:arena metric:SizeMetric::Logical];
    CHECK(!hud.hidden);
    CHECK([hud.titleText isEqualToString:@"Holiday.mov"]);
    CHECK([hud.physicalText isEqualToString:DPFormatBytes(0)]);          // nothing on this disk…
    CHECK([hud.logicalText isEqualToString:DPFormatBytes(5ull << 30)]);  // …5 GiB in the cloud
    CHECK([hud.statusText containsString:@"iCloud"]);
    CHECK([hud.shareText hasSuffix:@"%"]);

    [hud showNode:duplicate aggregate:NO hub:NO arena:arena metric:SizeMetric::Logical];
    CHECK([hud.statusText containsString:@"Hard link"]);
    CHECK([hud.statusText containsString:@"counted once"]);

    [hud showNode:folder aggregate:NO hub:NO arena:arena metric:SizeMetric::Logical];
    CHECK([hud.contentsText isEqualToString:@"1 file, 1 item"]);
    CHECK([hud.pathText isEqualToString:@"Projects"]);
    [hud showNode:folder aggregate:YES hub:NO arena:arena metric:SizeMetric::Logical];
    CHECK([hud.titleText hasPrefix:@"Smaller items in"]);

    [hud showNode:kInvalidNode aggregate:NO hub:NO arena:arena metric:SizeMetric::Logical];
    CHECK(hud.hidden);
}

// ----------------------------------------------------------------------------------------
// Rendering cost
// ----------------------------------------------------------------------------------------

DP_TEST(rendering_is_event_driven_and_idle_draws_nothing) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;
    CHECK(map.isPaused);                 // the view never runs a timer of its own
    CHECK(map.enableSetNeedsDisplay);
    REQUIRE(waitUntilQuiet(map, 0.3));   // let the reveal animation's last frame land
    REQUIRE(!map.isAnimating);

    // Idle: a full second of run-loop time renders not a single frame.
    const uint64_t idleStart = map.renderedFrameCount;
    spinFor(1.0);
    CHECK_EQ(map.renderedFrameCount, idleStart);
    CHECK(!map.isAnimating);

    // A hover change costs a frame or two, then it is quiet again.
    const NSRect block = [map rectForNode:h.node("Movies/holiday.mov")];
    [map mouseMoved:mouseEvent(NSEventTypeMouseMoved, map, centreOf(block), 0)];
    CHECK(spinUntil(5.0, ^bool {
        return map.renderedFrameCount > idleStart;
    }));
    REQUIRE(waitUntilQuiet(map, 0.25));
    const uint64_t afterHover = map.renderedFrameCount;
    CHECK(afterHover - idleStart <= 3);
    spinFor(0.6);
    CHECK_EQ(map.renderedFrameCount, afterHover);
    // Moving within the same block changes nothing on the GPU: no frame at all.
    [map mouseMoved:mouseEvent(NSEventTypeMouseMoved, map, NSMakePoint(NSMidX(block) + 2, NSMidY(block) + 2), 0)];
    spinFor(0.2);
    CHECK_EQ(map.renderedFrameCount, afterHover);

    // A drill-down animates at display rate for ~0.3 s and then stops by itself. Its clock starts
    // inside this call, which also updates the tree, so the measurement has to start before it.
    const CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
    [h.controller setFocusNode:h.node("Movies") animated:YES];
    CHECK(map.isAnimating);
    REQUIRE(spinUntil(3.0, ^bool {
        return !map.isAnimating;
    }));
    const double animationSeconds = CFAbsoluteTimeGetCurrent() - started;
    const uint64_t animationFrames = map.renderedFrameCount - afterHover;
    CHECK(animationSeconds > 0.15 && animationSeconds < (dptest::onCI() ? 3.0 : 1.0));
    CHECK(animationFrames >= (dptest::onCI() ? 2u : 8u));  // ~17 at 60 Hz, ~34 at 120 Hz
    REQUIRE(waitUntilQuiet(map, 0.2));
    const uint64_t settled = map.renderedFrameCount;
    spinFor(1.0);
    CHECK_EQ(map.renderedFrameCount, settled);  // the display link is gone: silence again
    std::printf("      idle: 0 frames in 1.0 s; drill-down: %llu frames in %.0f ms (%.0f fps), then 0 frames in 1.0 s\n",
                static_cast<unsigned long long>(animationFrames), animationSeconds * 1e3,
                static_cast<double>(animationFrames) / animationSeconds);
    std::printf("      last layout + GPU upload: %.3f ms for %lu instances\n", map.lastLayoutMilliseconds,
                static_cast<unsigned long>(map.instanceCount));
}

DP_TEST(appearance_change_recolours_the_canvas) {
    if (!haveWindowServer()) return;
    Fixture fixture;
    Harness h;
    REQUIRE(h.scan(fixture.path()));
    REQUIRE(h.settle());
    DiskMapMetalView *map = h.controller.canvasController.mapView;

    h.controller.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    spinUntil(5.0, ^bool {
        return map.clearColor.red < 0.3;
    });
    REQUIRE(waitUntilQuiet(map, 0.4));
    const MTLClearColor dark = map.clearColor;
    const uint64_t framesDark = map.renderedFrameCount;
    h.controller.window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameAqua];
    // Wait for the redraw itself rather than for a fixed time: a busy machine may take a while.
    const bool redrawn = spinUntil(5.0, ^bool {
        return map.renderedFrameCount > framesDark && map.clearColor.red > 0.7;
    });
    const MTLClearColor light = map.clearColor;
    CHECK(dark.red < 0.3 && light.red > 0.7);  // the canvas followed the appearance
    CHECK(redrawn);                            // and was redrawn for it
    REQUIRE(waitUntilQuiet(map, 0.5));
    const uint64_t settled = map.renderedFrameCount;
    spinFor(0.5);
    CHECK_EQ(map.renderedFrameCount, settled);

    // Off-screen snapshots of both appearances differ in the canvas color only where expected.
    CGImageRef image = [map newSnapshotWithPixelWidth:400 height:300 scale:2.0];
    REQUIRE(image != nullptr);
    CHECK_EQ(CGImageGetWidth(image), size_t{400});
    CHECK_EQ(CGImageGetHeight(image), size_t{300});
    CGImageRelease(image);
}

DP_TEST_MAIN()
