// DiskPrism UI — translucent floating read-out for the item under the pointer.
#import <AppKit/AppKit.h>

#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
}

NS_ASSUME_NONNULL_BEGIN

/// A small vibrant panel that follows the pointer over the canvas and shows the hovered
/// item's path, physical and logical size, share of its parent, contents and status
/// (cloud-only, hard link, unreadable…). It never intercepts mouse events.
@interface HoverHudView : NSVisualEffectView

/// Fills the panel for `node`. `aggregate` means the pointer is over the merged
/// "smaller items" block of the directory `node`.
- (void)showNode:(uint32_t)node
       aggregate:(BOOL)aggregate
             hub:(BOOL)hub
           arena:(const diskprism::FlatArena &)arena
          metric:(diskprism::SizeMetric)metric;

/// Places the panel next to `point` (in the superview's coordinates), flipping to the
/// other side of the pointer where it would leave `bounds`.
- (void)positionNearPoint:(NSPoint)point inBounds:(NSRect)bounds;

// Read-outs (exposed so tests can check what the user is told).
@property (nonatomic, readonly) NSString *titleText;
@property (nonatomic, readonly) NSString *pathText;
@property (nonatomic, readonly) NSString *physicalText;
@property (nonatomic, readonly) NSString *logicalText;
@property (nonatomic, readonly) NSString *shareText;
@property (nonatomic, readonly) NSString *contentsText;
@property (nonatomic, readonly) NSString *statusText;

@end

NS_ASSUME_NONNULL_END
