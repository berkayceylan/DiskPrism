// DiskPrism UI — the file-type sidebar (extensions ranked by the space they occupy).
#import <AppKit/AppKit.h>

#include "CoreTypes.hpp"

namespace diskprism {
class FlatArena;
}

NS_ASSUME_NONNULL_BEGIN

@class ExtensionListController;

@protocol ExtensionListDelegate <NSObject>
/// The user picked a file type to highlight, or cleared the filter (`extensionId` < 0).
- (void)extensionList:(ExtensionListController *)list didSelectExtension:(int32_t)extensionId;
@end

/// Lists every extension with its color, share, bytes and file count on a translucent
/// sidebar backdrop. Clicking a row filters the canvas to that type; clicking it again,
/// or the Clear button in the header, removes the filter.
@interface ExtensionListController : NSViewController <NSTableViewDataSource, NSTableViewDelegate>

@property (nonatomic, weak, nullable) id<ExtensionListDelegate> delegate;
@property (nonatomic, readonly) NSTableView *tableView;

/// Rebuilds the ranking. `arena` is borrowed (pass nullptr before it is destroyed).
- (void)setArena:(nullable const diskprism::FlatArena *)arena metric:(diskprism::SizeMetric)metric;

/// The histogram changed (an item was removed): re-rank, keeping the selection.
- (void)reloadKeepingSelection;

/// Extension currently used as the filter, or -1.
@property (nonatomic) int32_t selectedExtension;

/// Extension IDs in display order (largest first).
@property (nonatomic, readonly) NSArray<NSNumber *> *rankedExtensions;

@end

NS_ASSUME_NONNULL_END
