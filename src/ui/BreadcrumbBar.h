// DiskPrism UI — clickable path of the focused directory, shown above the canvas.
#import <AppKit/AppKit.h>

#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
}

NS_ASSUME_NONNULL_BEGIN

@class BreadcrumbBar;

@protocol BreadcrumbBarDelegate <NSObject>
/// The user clicked a path segment (or the "enclosing folder" button).
- (void)breadcrumbBar:(BreadcrumbBar *)bar didSelectNode:(uint32_t)node;
@end

/// One segment per directory from the scan root down to the focused directory. Clicking
/// a segment jumps straight to that ancestor.
@interface BreadcrumbBar : NSView

@property (nonatomic, weak, nullable) id<BreadcrumbBarDelegate> delegate;

/// Rebuilds the segments. `arena` is only read during this call.
- (void)setArena:(nullable const diskprism::FlatArena *)arena
           focus:(uint32_t)focus
          metric:(diskprism::SizeMetric)metric;

/// Nodes currently shown, root first (exposed for tests and accessibility).
@property (nonatomic, readonly) NSArray<NSNumber *> *segmentNodes;

@end

NS_ASSUME_NONNULL_END
