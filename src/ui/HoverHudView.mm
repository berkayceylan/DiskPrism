#import "HoverHudView.h"

#import "Formatting.h"
#include "FlatArena.hpp"

using namespace diskprism;

namespace {

constexpr CGFloat kHudWidth = 304;
constexpr CGFloat kHudPadding = 12;
constexpr CGFloat kPointerOffset = 18;

NSTextField *captionLabel(NSString *text) {
    NSTextField *label = DPMakeLabel(text, [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    [label setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [label setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

NSTextField *valueLabel() {
    NSTextField *label = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightMedium], NSColor.labelColor);
    label.alignment = NSTextAlignmentRight;
    // The numbers are the point of the panel: the column grows with them instead of the
    // text being truncated.
    [label setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    return label;
}

}  // namespace

@implementation HoverHudView {
    NSTextField *_title;
    NSTextField *_path;
    NSTextField *_physical;
    NSTextField *_logical;
    NSTextField *_share;
    NSTextField *_contentsCaption;
    NSTextField *_contents;
    NSTextField *_status;
    NSImageView *_statusIcon;
    NSStackView *_statusRow;
    NSSize _contentSize;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;

    self.material = NSVisualEffectMaterialHUDWindow;
    self.blendingMode = NSVisualEffectBlendingModeWithinWindow;
    self.state = NSVisualEffectStateActive;
    self.wantsLayer = YES;
    self.layer.cornerRadius = 10;
    self.layer.masksToBounds = YES;
    self.layer.borderWidth = 0.5;
    self.layer.borderColor = [NSColor.separatorColor colorWithAlphaComponent:0.6].CGColor;
    self.hidden = YES;
    [self setAccessibilityElement:NO];

    _title = DPMakeLabel(@"", [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold], NSColor.labelColor);
    _path = DPMakeLabel(@"", [NSFont systemFontOfSize:11], NSColor.secondaryLabelColor);
    _path.lineBreakMode = NSLineBreakByTruncatingHead;  // the tail of a path is the informative part

    _physical = valueLabel();
    _logical = valueLabel();
    _share = valueLabel();
    _contents = valueLabel();
    _contentsCaption = captionLabel(@"Contains");

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[captionLabel(@"On disk"), _physical],
        @[captionLabel(@"Logical size"), _logical],
        @[captionLabel(@"Share of parent"), _share],
        @[_contentsCaption, _contents],
    ]];
    grid.rowSpacing = 3;
    grid.columnSpacing = 12;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementLeading;
    [grid columnAtIndex:1].xPlacement = NSGridCellPlacementFill;
    grid.translatesAutoresizingMaskIntoConstraints = NO;

    _statusIcon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _statusIcon.contentTintColor = NSColor.secondaryLabelColor;
    _statusIcon.translatesAutoresizingMaskIntoConstraints = NO;
    [_statusIcon setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    _status = [NSTextField wrappingLabelWithString:@""];
    _status.font = [NSFont systemFontOfSize:11];
    _status.textColor = NSColor.secondaryLabelColor;
    _status.selectable = NO;
    _status.translatesAutoresizingMaskIntoConstraints = NO;
    _status.preferredMaxLayoutWidth = kHudWidth - 2 * kHudPadding - 22;
    _statusRow = [NSStackView stackViewWithViews:@[_statusIcon, _status]];
    _statusRow.orientation = NSUserInterfaceLayoutOrientationHorizontal;
    _statusRow.alignment = NSLayoutAttributeFirstBaseline;
    _statusRow.spacing = 6;

    NSBox *separator = [[NSBox alloc] initWithFrame:NSZeroRect];
    separator.boxType = NSBoxSeparator;

    NSStackView *stack = [NSStackView stackViewWithViews:@[_title, _path, separator, grid, _statusRow]];
    stack.orientation = NSUserInterfaceLayoutOrientationVertical;
    stack.alignment = NSLayoutAttributeLeading;
    stack.spacing = 5;
    stack.edgeInsets = NSEdgeInsetsMake(kHudPadding, kHudPadding, kHudPadding, kHudPadding);
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [stack setCustomSpacing:8 afterView:_path];
    [stack setCustomSpacing:8 afterView:separator];
    [stack setCustomSpacing:8 afterView:grid];
    [self addSubview:stack];

    const CGFloat inner = kHudWidth - 2 * kHudPadding;
    [NSLayoutConstraint activateConstraints:@[
        [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
        [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [stack.widthAnchor constraintEqualToConstant:kHudWidth],
        [_title.widthAnchor constraintLessThanOrEqualToConstant:inner],
        [_path.widthAnchor constraintLessThanOrEqualToConstant:inner],
        [separator.widthAnchor constraintEqualToConstant:inner],
        [grid.widthAnchor constraintEqualToConstant:inner],
    ]];
    return self;
}

/// The panel is purely informational: clicks and mouse-moved events go to the canvas.
- (nullable NSView *)hitTest:(NSPoint)__unused point {
    return nil;
}

- (NSString *)titleText {
    return _title.stringValue;
}
- (NSString *)pathText {
    return _path.stringValue;
}
- (NSString *)physicalText {
    return _physical.stringValue;
}
- (NSString *)logicalText {
    return _logical.stringValue;
}
- (NSString *)shareText {
    return _share.stringValue;
}
- (NSString *)contentsText {
    return _contents.stringValue;
}
- (NSString *)statusText {
    return _status.stringValue;
}

- (void)showNode:(uint32_t)node aggregate:(BOOL)aggregate hub:(BOOL)hub arena:(const FlatArena &)arena metric:(SizeMetric)metric {
    if (node == kInvalidNode || !arena.isValid(node)) {
        self.hidden = YES;
        return;
    }
    const FsNode &n = arena.node(node);
    NSString *name = DPNodeDisplayName(arena, node);

    if (aggregate) {
        _title.stringValue = [NSString stringWithFormat:@"Smaller items in “%@”", name];
    } else {
        _title.stringValue = name;
    }

    // Path relative to what was scanned; the root shows its full path.
    const uint32_t root = arena.root();
    _path.stringValue = node == root ? DPNodePath(arena, node) : DPStringFromFileSystemBytes(arena.relativePath(node, root));

    _physical.stringValue = DPFormatBytes(n.allocated_bytes);
    _logical.stringValue = DPFormatBytes(n.logical_bytes);

    if (n.parent_idx != kInvalidNode) {
        const uint64_t parentBytes = arena.node(n.parent_idx).bytes(metric);
        const double fraction = parentBytes > 0 ? static_cast<double>(n.bytes(metric)) / static_cast<double>(parentBytes) : 0.0;
        _share.stringValue = DPFormatPercent(fraction);
    } else {
        _share.stringValue = @"—";
    }

    if (n.is_dir()) {
        _contentsCaption.stringValue = @"Contains";
        _contents.stringValue = [NSString stringWithFormat:@"%@ %@, %@ %@", DPFormatCount(n.file_count),
                                                           n.file_count == 1 ? @"file" : @"files", DPFormatCount(n.child_count),
                                                           n.child_count == 1 ? @"item" : @"items"];
    } else {
        _contentsCaption.stringValue = @"Modified";
        _contents.stringValue = DPFormatDate(n.mtime);
    }

    // One status line, most important condition first.
    NSString *symbol = nil;
    NSString *status = nil;
    if (aggregate) {
        symbol = @"square.grid.3x3";
        status = @"Too small to draw individually. Zoom into this folder to see them.";
    } else if (hub) {
        symbol = n.parent_idx != kInvalidNode ? @"arrow.up.left.circle" : @"internaldrive";
        status = n.parent_idx != kInvalidNode ? @"Click to zoom out to the enclosing folder." : @"This is the top of the scanned tree.";
    } else if (n.access_denied()) {
        symbol = @"lock.fill";
        status = @"Access denied — its contents could not be read and are not counted.";
    } else if (n.is_mount_point()) {
        symbol = @"externaldrive";
        status = @"Another volume is mounted here. It is not part of this scan.";
    } else if (n.is_dataless()) {
        symbol = @"icloud";
        status = @"In iCloud only — not downloaded, so it takes no space on this disk.";
    } else if (n.is_hardlink_dup()) {
        symbol = @"link";
        status = @"Hard link — its data is counted once, at another name of the same file.";
    } else if ((n.flags & kNodeHardlink) != 0) {
        symbol = @"link";
        status = @"Hard-linked file — this name carries the on-disk size.";
    } else if (n.is_symlink()) {
        symbol = @"arrowshape.turn.up.right";
        status = @"Symbolic link — not followed.";
    } else if ((n.flags & kNodeIOError) != 0) {
        symbol = @"exclamationmark.triangle";
        status = @"Could not be read (I/O error).";
    }
    if (status != nil) {
        _status.stringValue = status;
        _statusIcon.image = DPSymbol(symbol, status);
        _statusRow.hidden = NO;
    } else {
        _status.stringValue = @"";
        _statusRow.hidden = YES;
    }

    self.hidden = NO;
    _contentSize = self.fittingSize;  // solved once per item; following the pointer is free
}

- (void)positionNearPoint:(NSPoint)point inBounds:(NSRect)bounds {
    const NSSize size = _contentSize;
    // Default: below and to the right of the pointer (the superview is flipped).
    CGFloat x = point.x + kPointerOffset;
    CGFloat y = point.y + kPointerOffset;
    if (x + size.width > NSMaxX(bounds) - 8) x = point.x - kPointerOffset - size.width;
    if (y + size.height > NSMaxY(bounds) - 8) y = point.y - kPointerOffset - size.height;
    x = MAX(NSMinX(bounds) + 8, MIN(x, NSMaxX(bounds) - size.width - 8));
    y = MAX(NSMinY(bounds) + 8, MIN(y, NSMaxY(bounds) - size.height - 8));
    self.frame = NSMakeRect(round(x), round(y), size.width, size.height);
}

@end
