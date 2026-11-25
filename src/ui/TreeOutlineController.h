// DiskPrism UI — the directory tree pane (NSOutlineView over the flat arena).
#import <AppKit/AppKit.h>

#import "NodeActions.h"
#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
class NodeMatchSet;
}  // namespace diskprism

NS_ASSUME_NONNULL_BEGIN

/// Shows the scanned tree with columns Name, Usage (share of the parent), Size, Items
/// and Modified.
///
/// Outline items are `NSNumber`s holding arena node indices. On 64-bit Darwin such small
/// integers are tagged pointers, so the outline view "owns" millions of items without a
/// single heap-allocated wrapper object: the arena stays the only copy of the tree. Only
/// directories the user has actually expanded get a small index vector (their children
/// in the current sort order).
@interface TreeOutlineController : NSViewController <NSOutlineViewDataSource, NSOutlineViewDelegate>

@property (nonatomic, weak, nullable) id<DPNodeActionHandler> actionHandler;
@property (nonatomic, readonly) NSOutlineView *outlineView;

/// Installs the data to show (borrowed pointers; pass nullptr before the arena dies).
- (void)setArena:(nullable const diskprism::FlatArena *)arena
         matches:(nullable const diskprism::NodeMatchSet *)matches
          metric:(diskprism::SizeMetric)metric;

/// Sizes, filter or metric changed: redisplay, keeping expansion and selection.
- (void)reloadKeepingState;

/// Switches the Size / Usage columns (and the default sort) to another metric.
- (void)setMetric:(diskprism::SizeMetric)metric;

/// Expands the ancestors of `node`, selects its row and scrolls it into view.
- (void)revealNode:(uint32_t)node;

/// Highlights the row of `node` (or of its nearest visible ancestor) without scrolling;
/// used while the pointer moves over the canvas.
- (void)setExternalHoverNode:(uint32_t)node;

@property (nonatomic, readonly) uint32_t selectedNode;

/// Screen rectangle of a node's row icon (the Quick Look zoom origin), or NSZeroRect.
- (NSRect)screenRectForNode:(uint32_t)node;

@end

NS_ASSUME_NONNULL_END
