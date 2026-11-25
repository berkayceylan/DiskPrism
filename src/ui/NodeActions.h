// DiskPrism UI — how the panes report user intent to the window controller.
#import <AppKit/AppKit.h>

#include <cstdint>

NS_ASSUME_NONNULL_BEGIN

/// Implemented by the window controller. Every pane (outline, canvas, breadcrumb) reports
/// what the user did through this protocol and never talks to another pane directly, so
/// selection, hover and focus stay consistent everywhere.
@protocol DPNodeActionHandler <NSObject>

/// The user selected `node` in `source`.
- (void)nodeSource:(id)source didSelectNode:(uint32_t)node;

/// The pointer is over `node` in `source` (diskprism::kInvalidNode: over nothing).
- (void)nodeSource:(id)source didHoverNode:(uint32_t)node;

/// The user wants `node` to become the focused directory (drill down / zoom out).
- (void)nodeSource:(id)source didRequestFocusNode:(uint32_t)node;

/// Context menu for `node`.
- (nullable NSMenu *)nodeSource:(id)source menuForNode:(uint32_t)node;

/// Space bar: toggle Quick Look for the current selection.
- (void)nodeSourceDidRequestQuickLook:(id)source;

/// Command-Delete: move `node` to the Trash.
- (void)nodeSource:(id)source didRequestTrashNode:(uint32_t)node;

@end

NS_ASSUME_NONNULL_END
