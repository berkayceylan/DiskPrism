// DiskPrism UI — the main window: toolbar, three-pane split view and the coordination
// between the panes, the scanner and the arena.
#import <AppKit/AppKit.h>

#import "DiskMapMetalView.h"
#import "NodeActions.h"
#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
}

@class CanvasViewController;
@class ExtensionListController;
@class TreeOutlineController;

NS_ASSUME_NONNULL_BEGIN

/// Owns the scanned tree and keeps every pane in step with it.
///
/// All panes report user intent through `DPNodeActionHandler`; this controller is the
/// single place that changes selection, hover, focus, filter and metric and then pushes
/// the new state to the outline, the canvas and the file-type list.
@interface MainWindowController : NSWindowController

/// `shaderPath` overrides the bundled Shaders.metal (tests and tools outside the bundle).
- (instancetype)initWithShaderPath:(nullable NSString *)shaderPath;

// --- Scanning ---------------------------------------------------------------------------

/// Scans a directory (a file's enclosing folder if a file is given). Also the entry point
/// for drag and drop and for "open with DiskPrism".
- (void)scanPath:(NSString *)path;

@property (nonatomic, readonly, getter=isScanning) BOOL scanning;
@property (nonatomic, readonly) BOOL hasData;
/// Invoked on the main thread once a scan has ended and its results are on screen.
@property (nonatomic, copy, nullable) void (^scanDidFinishHandler)(void);

// --- State --------------------------------------------------------------------------------

@property (nonatomic, readonly, nullable) const diskprism::FlatArena *arena;
@property (nonatomic, readonly) uint32_t focusNode;
@property (nonatomic, readonly) uint32_t selectedNode;
@property (nonatomic) DPViewMode viewMode;
@property (nonatomic) diskprism::SizeMetric sizeMetric;
@property (nonatomic, readonly) NSString *telemetryText;
/// Number of folders the last scan could not read (drives the Full Disk Access banner).
@property (nonatomic, readonly) uint64_t deniedFolderCount;

- (void)setFocusNode:(uint32_t)node animated:(BOOL)animated;
- (void)selectNode:(uint32_t)node;

/// Substring filter over file and folder names (empty string: none).
- (void)setNameFilter:(NSString *)text;
/// File-type filter by extension ID (negative: none).
- (void)setExtensionFilter:(int32_t)extensionId;

// --- Item actions -------------------------------------------------------------------------

- (nullable NSMenu *)contextMenuForNode:(uint32_t)node;

/// Moves the item to the Trash with NSFileManager and, on success, removes it from the
/// arena incrementally and refreshes every pane — no rescan.
- (BOOL)moveNodeToTrash:(uint32_t)node error:(NSError **)error;
/// Where the last successfully trashed item ended up.
@property (nonatomic, readonly, nullable) NSURL *lastTrashedItemURL;

// --- Panes (exposed for tests) ---------------------------------------------------------------

@property (nonatomic, readonly) TreeOutlineController *outlineController;
@property (nonatomic, readonly) CanvasViewController *canvasController;
@property (nonatomic, readonly) ExtensionListController *extensionController;

// --- Menu and toolbar actions --------------------------------------------------------------

- (IBAction)chooseFolder:(nullable id)sender;
- (IBAction)scanDefaultTarget:(nullable id)sender;
- (IBAction)rescan:(nullable id)sender;
- (IBAction)cancelScan:(nullable id)sender;
- (IBAction)toggleScan:(nullable id)sender;
- (IBAction)showTreemap:(nullable id)sender;
- (IBAction)showSunburst:(nullable id)sender;
- (IBAction)usePhysicalSize:(nullable id)sender;
- (IBAction)useLogicalSize:(nullable id)sender;
- (IBAction)zoomIntoSelection:(nullable id)sender;
- (IBAction)zoomOut:(nullable id)sender;
- (IBAction)zoomToScanRoot:(nullable id)sender;
- (IBAction)revealInFinder:(nullable id)sender;
- (IBAction)quickLookSelection:(nullable id)sender;
- (IBAction)copyFullPath:(nullable id)sender;
- (IBAction)openInTerminal:(nullable id)sender;
- (IBAction)moveSelectionToTrash:(nullable id)sender;
- (IBAction)focusSearchField:(nullable id)sender;
- (IBAction)clearFilters:(nullable id)sender;

@end

NS_ASSUME_NONNULL_END
