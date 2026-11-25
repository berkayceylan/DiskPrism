#import "TreeOutlineController.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "Formatting.h"
#include "FlatArena.hpp"
#include "NodeFilter.hpp"

#include <algorithm>
#include <strings.h>
#include <unordered_map>
#include <vector>

using namespace diskprism;

namespace {

NSUserInterfaceItemIdentifier const kColumnName = @"name";
NSUserInterfaceItemIdentifier const kColumnUsage = @"usage";
NSUserInterfaceItemIdentifier const kColumnSize = @"size";
NSUserInterfaceItemIdentifier const kColumnItems = @"items";
NSUserInterfaceItemIdentifier const kColumnModified = @"modified";
NSUserInterfaceItemIdentifier const kRowIdentifier = @"row";

constexpr CGFloat kNameColumnBaseWidth = 120;   // minimum when nothing deep is expanded
constexpr CGFloat kNameTextWidth = 150;         // room a name should keep at any depth
constexpr CGFloat kIndentationPerLevel = 12;

constexpr unsigned short kKeyReturn = 0x24;
constexpr unsigned short kKeySpace = 0x31;
constexpr unsigned short kKeyDelete = 0x33;
constexpr unsigned short kKeyEnter = 0x4C;

enum class SortKey { Size, Name, Items, Modified };

uint32_t nodeOfItem(id _Nullable item) {
    return item == nil ? kInvalidNode : static_cast<NSNumber *>(item).unsignedIntValue;
}

/// Case-insensitive byte-wise comparison (fast; names are mostly ASCII).
int compareNames(std::string_view a, std::string_view b) noexcept {
    const size_t common = std::min(a.size(), b.size());
    const int order = common == 0 ? 0 : ::strncasecmp(a.data(), b.data(), common);
    if (order != 0) return order;
    return a.size() < b.size() ? -1 : (a.size() > b.size() ? 1 : 0);
}

}  // namespace

// ---------------------------------------------------------------------------------------
#pragma mark - Views
// ---------------------------------------------------------------------------------------

/// Lightweight percentage bar: a rounded track, a filled part and the number.
@interface DPUsageBarView : NSView
@property (nonatomic) double fraction;
@property (nonatomic) BOOL emphasized;  ///< the row is selected in a key window
@property (nonatomic) BOOL muted;       ///< outside the active filter
@end

@implementation DPUsageBarView

- (void)setFraction:(double)fraction {
    if (fraction == _fraction) return;
    _fraction = fraction;
    self.needsDisplay = YES;
}

- (void)setEmphasized:(BOOL)emphasized {
    if (emphasized == _emphasized) return;
    _emphasized = emphasized;
    self.needsDisplay = YES;
}

- (void)setMuted:(BOOL)muted {
    if (muted == _muted) return;
    _muted = muted;
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)__unused dirtyRect {
    const NSRect bounds = self.bounds;
    constexpr CGFloat kTextWidth = 44;
    constexpr CGFloat kBarHeight = 6;
    const CGFloat barWidth = MAX(bounds.size.width - kTextWidth - 4, 8);
    const NSRect track = NSMakeRect(0, round(NSMidY(bounds) - kBarHeight / 2), barWidth, kBarHeight);

    NSColor *trackColor = _emphasized ? [NSColor.alternateSelectedControlTextColor colorWithAlphaComponent:0.28]
                                      : [NSColor.labelColor colorWithAlphaComponent:0.10];
    [trackColor setFill];
    [[NSBezierPath bezierPathWithRoundedRect:track xRadius:kBarHeight / 2 yRadius:kBarHeight / 2] fill];

    const double clamped = std::clamp(_fraction, 0.0, 1.0);
    if (clamped > 0) {
        NSRect fill = track;
        fill.size.width = MAX(round(barWidth * clamped), kBarHeight);
        NSColor *fillColor = _emphasized ? NSColor.alternateSelectedControlTextColor : NSColor.controlAccentColor;
        if (_muted && !_emphasized) fillColor = [NSColor.labelColor colorWithAlphaComponent:0.25];
        [fillColor setFill];
        [[NSBezierPath bezierPathWithRoundedRect:fill xRadius:kBarHeight / 2 yRadius:kBarHeight / 2] fill];
    }

    NSColor *textColor = _emphasized ? NSColor.alternateSelectedControlTextColor
                                     : (_muted ? NSColor.tertiaryLabelColor : NSColor.secondaryLabelColor);
    NSMutableParagraphStyle *style = [[NSMutableParagraphStyle alloc] init];
    style.alignment = NSTextAlignmentRight;
    NSDictionary<NSAttributedStringKey, id> *attributes = @{
        NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName : textColor,
        NSParagraphStyleAttributeName : style,
    };
    NSString *text = DPFormatPercent(_fraction);
    const NSSize textSize = [text sizeWithAttributes:attributes];
    [text drawInRect:NSMakeRect(NSMaxX(bounds) - kTextWidth, round(NSMidY(bounds) - textSize.height / 2), kTextWidth, textSize.height)
      withAttributes:attributes];
}

- (BOOL)isFlipped {
    return YES;
}

@end

@interface DPUsageCellView : NSTableCellView
@property (nonatomic, readonly) DPUsageBarView *bar;
@end

@implementation DPUsageCellView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    _bar = [[DPUsageBarView alloc] initWithFrame:NSZeroRect];
    _bar.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:_bar];
    [NSLayoutConstraint activateConstraints:@[
        [_bar.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2],
        [_bar.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2],
        [_bar.topAnchor constraintEqualToAnchor:self.topAnchor],
        [_bar.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

- (void)setBackgroundStyle:(NSBackgroundStyle)backgroundStyle {
    [super setBackgroundStyle:backgroundStyle];
    _bar.emphasized = backgroundStyle == NSBackgroundStyleEmphasized;
}

@end

/// Icon, name and a trailing status glyph (cloud-only, hard link, locked, other volume).
@interface DPNameCellView : NSTableCellView
@property (nonatomic, readonly) NSImageView *badge;
@end

@implementation DPNameCellView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;

    NSImageView *icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *name = DPMakeLabel(@"", [NSFont systemFontOfSize:NSFont.systemFontSize], NSColor.labelColor);
    _badge = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _badge.translatesAutoresizingMaskIntoConstraints = NO;
    _badge.contentTintColor = NSColor.secondaryLabelColor;
    _badge.imageScaling = NSImageScaleProportionallyDown;
    [_badge setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    [self addSubview:icon];
    [self addSubview:name];
    [self addSubview:_badge];
    self.imageView = icon;
    self.textField = name;
    [NSLayoutConstraint activateConstraints:@[
        [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2],
        [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [icon.widthAnchor constraintEqualToConstant:16],
        [icon.heightAnchor constraintEqualToConstant:16],
        [name.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:5],
        [name.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_badge.leadingAnchor constraintEqualToAnchor:name.trailingAnchor constant:4],
        [_badge.trailingAnchor constraintLessThanOrEqualToAnchor:self.trailingAnchor constant:-4],
        [_badge.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_badge.heightAnchor constraintEqualToConstant:13],
    ]];
    return self;
}

- (void)setBackgroundStyle:(NSBackgroundStyle)backgroundStyle {
    [super setBackgroundStyle:backgroundStyle];
    _badge.contentTintColor = backgroundStyle == NSBackgroundStyleEmphasized ? NSColor.alternateSelectedControlTextColor
                                                                              : NSColor.secondaryLabelColor;
}

@end

/// A right-aligned number.
@interface DPValueCellView : NSTableCellView
@end

@implementation DPValueCellView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    NSTextField *label = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize + 1 weight:NSFontWeightRegular],
                                     NSColor.labelColor);
    label.alignment = NSTextAlignmentRight;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:label];
    self.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2],
        [label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-4],
        [label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

@end

/// Row view with a subtle highlight for "the pointer is over this item in the map".
@interface DPRowView : NSTableRowView
@property (nonatomic) BOOL hoverHighlighted;
@end

@implementation DPRowView

- (void)setHoverHighlighted:(BOOL)hoverHighlighted {
    if (hoverHighlighted == _hoverHighlighted) return;
    _hoverHighlighted = hoverHighlighted;
    self.needsDisplay = YES;
}

- (void)drawBackgroundInRect:(NSRect)dirtyRect {
    [super drawBackgroundInRect:dirtyRect];
    if (!_hoverHighlighted || self.selected) return;
    [[NSColor.labelColor colorWithAlphaComponent:0.07] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds, 5, 1) xRadius:5 yRadius:5] fill];
}

@end

@class DPOutlineView;

@protocol DPOutlineViewInteraction <NSObject>
- (void)outlineView:(DPOutlineView *)outlineView pointerMovedToRow:(NSInteger)row;
- (nullable NSMenu *)outlineView:(DPOutlineView *)outlineView menuForRow:(NSInteger)row;
- (BOOL)outlineView:(DPOutlineView *)outlineView handleKeyDown:(NSEvent *)event;
@end

/// NSOutlineView that reports pointer movement, context-menu requests and a few keys.
@interface DPOutlineView : NSOutlineView
@property (nonatomic, weak, nullable) id<DPOutlineViewInteraction> interaction;
@end

@implementation DPOutlineView {
    NSTrackingArea *_trackingArea;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea != nil) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                                                         NSTrackingActiveInActiveApp | NSTrackingInVisibleRect
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseMoved:(NSEvent *)event {
    [super mouseMoved:event];
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    [self.interaction outlineView:self pointerMovedToRow:[self rowAtPoint:point]];
}

- (void)mouseExited:(NSEvent *)event {
    [super mouseExited:event];
    [self.interaction outlineView:self pointerMovedToRow:-1];
}

- (nullable NSMenu *)menuForEvent:(NSEvent *)event {
    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const NSInteger row = [self rowAtPoint:point];
    if (row < 0) return nil;
    if (![self isRowSelected:row]) [self selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
    return [self.interaction outlineView:self menuForRow:row];
}

- (void)keyDown:(NSEvent *)event {
    if ([self.interaction outlineView:self handleKeyDown:event]) return;
    [super keyDown:event];
}

@end

// ---------------------------------------------------------------------------------------
#pragma mark - Controller
// ---------------------------------------------------------------------------------------

@interface TreeOutlineController () <DPOutlineViewInteraction>
@end

@implementation TreeOutlineController {
    DPOutlineView *_outline;
    NSScrollView *_scrollView;

    const FlatArena *_arena;
    const NodeMatchSet *_matches;
    SizeMetric _metric;
    SortKey _sortKey;
    BOOL _sortAscending;

    /// Children (in display order) of the directories the outline view has asked about.
    std::unordered_map<uint32_t, std::vector<uint32_t>> _children;

    NSInteger _hoverRow;
    NSInteger _deepestLevel;  // deepest level any expansion has shown; decides the Name column's width
    BOOL _widthUpdateScheduled;
    BOOL _suppressSelectionCallback;
    NSMutableDictionary<NSNumber *, NSImage *> *_typeIcons;  // by extension ID
    NSCache<NSNumber *, NSImage *> *_bundleIcons;            // by node, for .app and friends
    NSImage *_folderIcon;
}

- (NSOutlineView *)outlineView {
    return _outline;
}

- (void)loadView {
    _metric = SizeMetric::Physical;
    _sortKey = SortKey::Size;
    _sortAscending = NO;
    _hoverRow = -1;
    _typeIcons = [NSMutableDictionary dictionary];
    _bundleIcons = [[NSCache alloc] init];
    _bundleIcons.countLimit = 2000;
    _folderIcon = [NSWorkspace.sharedWorkspace iconForContentType:UTTypeFolder];

    _outline = [[DPOutlineView alloc] initWithFrame:NSZeroRect];
    _outline.interaction = self;
    _outline.dataSource = self;
    _outline.delegate = self;
    _outline.style = NSTableViewStyleInset;
    _outline.rowHeight = 22;
    _outline.intercellSpacing = NSMakeSize(6, 2);
    _outline.indentationPerLevel = kIndentationPerLevel;
    _outline.autoresizesOutlineColumn = NO;
    _outline.allowsMultipleSelection = NO;
    _outline.allowsEmptySelection = YES;
    _outline.allowsColumnReordering = NO;
    _outline.usesAlternatingRowBackgroundColors = NO;
    _outline.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    _outline.target = self;
    _outline.doubleAction = @selector(rowDoubleClicked:);
    [_outline setAccessibilityLabel:@"Directory tree"];

    struct ColumnSpec {
        NSUserInterfaceItemIdentifier identifier;
        NSString *title;
        CGFloat width;
        CGFloat minWidth;
        NSString *sortKey;
        BOOL ascendingFirst;
        NSTextAlignment alignment;
    };
    const ColumnSpec specs[] = {
        {kColumnName, @"Name", 220, 120, @"name", YES, NSTextAlignmentLeft},
        {kColumnUsage, @"Usage", 84, 76, @"size", NO, NSTextAlignmentLeft},
        {kColumnSize, @"Physical Size", 96, 70, @"size", NO, NSTextAlignmentRight},
        {kColumnItems, @"Items", 62, 44, @"items", NO, NSTextAlignmentRight},
        {kColumnModified, @"Modified", 72, 64, @"modified", NO, NSTextAlignmentRight},
    };
    for (const ColumnSpec &spec : specs) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:spec.identifier];
        column.title = spec.title;
        column.width = spec.width;
        column.minWidth = spec.minWidth;
        column.headerCell.alignment = spec.alignment;
        column.sortDescriptorPrototype = [NSSortDescriptor sortDescriptorWithKey:spec.sortKey ascending:spec.ascendingFirst];
        column.resizingMask = spec.identifier == kColumnName ? (NSTableColumnAutoresizingMask | NSTableColumnUserResizingMask)
                                                            : NSTableColumnUserResizingMask;
        [_outline addTableColumn:column];
        if (spec.identifier == kColumnName) _outline.outlineTableColumn = column;
    }
    _outline.sortDescriptors = @[[NSSortDescriptor sortDescriptorWithKey:@"size" ascending:NO]];

    _scrollView = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 620, 600)];
    _scrollView.documentView = _outline;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.hasHorizontalScroller = YES;
    _scrollView.autohidesScrollers = YES;
    _scrollView.borderType = NSNoBorder;
    self.view = _scrollView;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Model
// ---------------------------------------------------------------------------------------

- (void)viewDidLayout {
    [super viewDidLayout];
    // Let the Name column take whatever the fixed columns leave of the pane's width.
    [_outline sizeToFit];
}

/// Deeply nested rows lose their name to indentation. Whenever a deeper level becomes
/// visible the Name column's minimum width grows with it (the table then scrolls
/// sideways instead of truncating names down to an ellipsis).
///
/// Never call this from an expand / collapse callback: the outline view is in the middle of
/// updating its rows then, and rows that already exist would keep their old column positions.
- (void)applyNameColumnWidth {
    _widthUpdateScheduled = NO;
    NSTableColumn *column = _outline.outlineTableColumn;
    const NSInteger level = _deepestLevel;
    const CGFloat needed = kNameColumnBaseWidth + MAX(level, 0) * kIndentationPerLevel + (level > 2 ? kNameTextWidth - 90 : 0);
    if (needed <= column.minWidth) return;
    // Width first: raising minWidth above the current width changes the width silently, and
    // rows that are already on screen would keep their cells at the old column positions.
    if (column.width < needed) column.width = needed;
    column.minWidth = needed;
    [_outline sizeToFit];
}

/// Coalesces any number of expansions into one column update on the next turn of the run loop.
- (void)scheduleNameColumnWidthForLevel:(NSInteger)level {
    _deepestLevel = MAX(_deepestLevel, level);
    if (_widthUpdateScheduled) return;
    _widthUpdateScheduled = YES;
    __weak TreeOutlineController *weakSelf = self;
    dispatch_async(dispatch_get_main_queue(), ^{
        [weakSelf applyNameColumnWidth];
    });
}

- (void)resetNameColumnWidth {
    _deepestLevel = 0;
    _outline.outlineTableColumn.minWidth = kNameColumnBaseWidth;
    [_outline sizeToFit];
}

/// Scrolls sideways so that the name of `row` (not just its indentation) is in view.
- (void)scrollNameOfRowToVisible:(NSInteger)row {
    if (row < 0) return;
    const NSRect cell = [_outline frameOfCellAtColumn:0 row:row];
    const NSRect visible = _outline.visibleRect;
    const CGFloat textStart = NSMinX(cell);
    const CGFloat wanted = MIN(kNameTextWidth + 40, visible.size.width);
    if (textStart >= NSMinX(visible) && textStart + wanted <= NSMaxX(visible)) return;  // already readable
    const CGFloat x = MAX(0, MIN(textStart - 24, NSMaxX(_outline.bounds) - visible.size.width));
    [_outline scrollPoint:NSMakePoint(x, NSMinY(visible))];
}

- (void)setArena:(nullable const FlatArena *)arena matches:(nullable const NodeMatchSet *)matches metric:(SizeMetric)metric {
    [self loadViewIfNeeded];
    _arena = arena;
    _matches = matches;
    _metric = metric;
    _children.clear();
    [_bundleIcons removeAllObjects];
    _hoverRow = -1;
    [self updateSizeColumnTitle];
    [self resetNameColumnWidth];

    _suppressSelectionCallback = YES;
    [_outline deselectAll:nil];
    [_outline reloadData];
    if (_arena != nullptr && _arena->size() > 0) {
        // The scan root is the single top-level row; show its contents right away.
        [_outline expandItem:@(_arena->root())];
    }
    _suppressSelectionCallback = NO;
}

- (void)setMetric:(SizeMetric)metric {
    if (metric == _metric) return;
    _metric = metric;
    [self updateSizeColumnTitle];
    [self reloadKeepingState];
}

- (void)updateSizeColumnTitle {
    [_outline tableColumnWithIdentifier:kColumnSize].title = _metric == SizeMetric::Physical ? @"Physical Size" : @"Logical Size";
    [_outline.headerView setNeedsDisplay:YES];
}

- (void)reloadKeepingState {
    if (_outline == nil) return;
    // Remember what is expanded and selected (as node indices, top-down)…
    std::vector<uint32_t> expanded;
    const NSInteger rows = _outline.numberOfRows;
    for (NSInteger row = 0; row < rows; ++row) {
        id item = [_outline itemAtRow:row];
        if ([_outline isItemExpanded:item]) expanded.push_back(nodeOfItem(item));
    }
    const uint32_t selected = self.selectedNode;
    const NSPoint scrollOrigin = _scrollView.contentView.bounds.origin;

    _children.clear();
    _hoverRow = -1;
    _suppressSelectionCallback = YES;
    [NSAnimationContext beginGrouping];
    NSAnimationContext.currentContext.duration = 0;
    [_outline reloadData];
    // …and restore it for every node that still exists.
    if (_arena != nullptr) {
        for (const uint32_t node : expanded) {
            if (_arena->isValid(node)) [_outline expandItem:@(node)];
        }
        if (selected != kInvalidNode && _arena->isValid(selected)) {
            const NSInteger row = [_outline rowForItem:@(selected)];
            if (row >= 0) [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
        }
    }
    [NSAnimationContext endGrouping];
    [_scrollView.contentView scrollToPoint:scrollOrigin];
    [_scrollView reflectScrolledClipView:_scrollView.contentView];
    _suppressSelectionCallback = NO;
}

- (uint32_t)selectedNode {
    const NSInteger row = _outline.selectedRow;
    return row < 0 ? kInvalidNode : nodeOfItem([_outline itemAtRow:row]);
}

/// Children of a directory in the current sort order. Built on first use (i.e. when the
/// user expands the directory) and kept until the data or the sort order changes.
- (const std::vector<uint32_t> &)childrenOf:(uint32_t)node {
    const auto found = _children.find(node);
    if (found != _children.end()) return found->second;

    std::vector<uint32_t> list;
    list.reserve(_arena->node(node).child_count);
    _arena->forEachChild(node, [&list](uint32_t child) { list.push_back(child); });

    const FlatArena &arena = *_arena;
    const SizeMetric metric = _metric;
    const bool ascending = _sortAscending;
    // The arena already keeps children largest-first for its sorted metric, which is the
    // default view; only other orders need work.
    const bool alreadySorted = _sortKey == SortKey::Size && !ascending && arena.isSorted() && arena.sortedMetric() == metric;
    if (!alreadySorted) {
        const SortKey key = _sortKey;
        std::sort(list.begin(), list.end(), [&arena, metric, ascending, key](uint32_t a, uint32_t b) {
            const FsNode &x = arena.node(a);
            const FsNode &y = arena.node(b);
            int order = 0;
            switch (key) {
                case SortKey::Size:
                    order = x.bytes(metric) < y.bytes(metric) ? -1 : (x.bytes(metric) > y.bytes(metric) ? 1 : 0);
                    break;
                case SortKey::Items:
                    order = x.file_count < y.file_count ? -1 : (x.file_count > y.file_count ? 1 : 0);
                    break;
                case SortKey::Modified:
                    order = x.mtime < y.mtime ? -1 : (x.mtime > y.mtime ? 1 : 0);
                    break;
                case SortKey::Name:
                    order = compareNames(arena.name(a), arena.name(b));
                    break;
            }
            if (order == 0) return key == SortKey::Name ? a < b : compareNames(arena.name(a), arena.name(b)) < 0;
            return ascending ? order < 0 : order > 0;
        });
    }
    return _children.emplace(node, std::move(list)).first->second;
}

// ---------------------------------------------------------------------------------------
#pragma mark - NSOutlineViewDataSource
// ---------------------------------------------------------------------------------------

- (NSInteger)outlineView:(NSOutlineView *)__unused outlineView numberOfChildrenOfItem:(nullable id)item {
    if (_arena == nullptr || _arena->size() == 0) return 0;
    if (item == nil) return 1;
    const uint32_t node = nodeOfItem(item);
    if (!_arena->isValid(node)) return 0;
    return static_cast<NSInteger>([self childrenOf:node].size());
}

- (id)outlineView:(NSOutlineView *)__unused outlineView child:(NSInteger)index ofItem:(nullable id)item {
    // @(index) is a tagged pointer: no object is allocated for the item.
    if (item == nil) return @(_arena->root());
    return @([self childrenOf:nodeOfItem(item)][static_cast<size_t>(index)]);
}

- (BOOL)outlineView:(NSOutlineView *)__unused outlineView isItemExpandable:(id)item {
    const uint32_t node = nodeOfItem(item);
    if (_arena == nullptr || !_arena->isValid(node)) return NO;
    const FsNode &n = _arena->node(node);
    return n.is_dir() && n.child_count > 0;
}

- (void)outlineView:(NSOutlineView *)outlineView sortDescriptorsDidChange:(NSArray<NSSortDescriptor *> *)__unused oldDescriptors {
    NSSortDescriptor *descriptor = outlineView.sortDescriptors.firstObject;
    NSString *key = descriptor.key;
    if ([key isEqualToString:@"name"]) {
        _sortKey = SortKey::Name;
    } else if ([key isEqualToString:@"items"]) {
        _sortKey = SortKey::Items;
    } else if ([key isEqualToString:@"modified"]) {
        _sortKey = SortKey::Modified;
    } else {
        _sortKey = SortKey::Size;
    }
    _sortAscending = descriptor != nil && descriptor.ascending;
    [self reloadKeepingState];
}

// ---------------------------------------------------------------------------------------
#pragma mark - NSOutlineViewDelegate
// ---------------------------------------------------------------------------------------

- (NSImage *)iconForNode:(uint32_t)node {
    const FsNode &n = _arena->node(node);
    if (node == _arena->root()) {
        return [NSWorkspace.sharedWorkspace iconForFile:DPNodePath(*_arena, node)];
    }
    if (n.is_dir()) {
        // Bundles (.app, .framework, .photoslibrary…) have their own icons; plain folders
        // share one image.
        const std::string_view name = _arena->name(node);
        if (name.find('.') == std::string_view::npos || name.front() == '.') return _folderIcon;
        NSImage *cached = [_bundleIcons objectForKey:@(node)];
        if (cached != nil) return cached;
        NSImage *icon = [NSWorkspace.sharedWorkspace iconForFile:DPNodePath(*_arena, node)];
        if (icon == nil) icon = _folderIcon;
        [_bundleIcons setObject:icon forKey:@(node)];
        return icon;
    }
    // Files: one icon per type, resolved once.
    NSNumber *key = @(n.ext_id);
    NSImage *icon = _typeIcons[key];
    if (icon == nil) {
        UTType *type = nil;
        if (n.ext_id != ExtensionTable::kNone) {
            type = [UTType typeWithFilenameExtension:DPStringFromFileSystemBytes(_arena->extensions().name(n.ext_id))];
        }
        if (type == nil) type = n.is_symlink() ? UTTypeSymbolicLink : UTTypeData;
        icon = [NSWorkspace.sharedWorkspace iconForContentType:type];
        _typeIcons[key] = icon;
    }
    return icon;
}

- (nullable NSImage *)badgeForNode:(const FsNode &)n toolTip:(NSString *_Nullable *_Nonnull)toolTip {
    if (n.access_denied()) {
        *toolTip = @"Access denied — contents not counted";
        return DPSymbol(@"lock.fill", *toolTip);
    }
    if (n.is_mount_point()) {
        *toolTip = @"Another volume — not part of this scan";
        return DPSymbol(@"externaldrive", *toolTip);
    }
    if (n.is_dataless()) {
        *toolTip = @"In iCloud only — takes no space on this disk";
        return DPSymbol(@"icloud", *toolTip);
    }
    if (n.is_hardlink_dup()) {
        *toolTip = @"Hard link — counted at another name of the same file";
        return DPSymbol(@"link", *toolTip);
    }
    if ((n.flags & kNodeHardlink) != 0) {
        *toolTip = @"Hard-linked file — this name carries the on-disk size";
        return DPSymbol(@"link", *toolTip);
    }
    if (n.is_symlink()) {
        *toolTip = @"Symbolic link — not followed";
        return DPSymbol(@"arrowshape.turn.up.right", *toolTip);
    }
    if ((n.flags & kNodeIOError) != 0) {
        *toolTip = @"Could not be read";
        return DPSymbol(@"exclamationmark.triangle", *toolTip);
    }
    *toolTip = nil;
    return nil;
}

- (nullable NSView *)outlineView:(NSOutlineView *)outlineView viewForTableColumn:(nullable NSTableColumn *)tableColumn item:(id)item {
    if (tableColumn == nil || _arena == nullptr) return nil;
    const uint32_t node = nodeOfItem(item);
    if (!_arena->isValid(node)) return nil;
    const FsNode &n = _arena->node(node);
    NSUserInterfaceItemIdentifier identifier = tableColumn.identifier;

    // With a filter active, items outside it are muted and direct hits are emphasised.
    const bool filtering = _matches != nullptr && _matches->active();
    const uint8_t bits = filtering ? _matches->bits(node) : uint8_t{0};
    const bool muted = filtering && bits == 0;
    const bool directHit = filtering && (bits & NodeMatchSet::kSelf) != 0;
    NSColor *primary = muted ? NSColor.tertiaryLabelColor : NSColor.labelColor;
    NSColor *secondary = muted ? NSColor.tertiaryLabelColor : NSColor.secondaryLabelColor;

    if ([identifier isEqualToString:kColumnName]) {
        DPNameCellView *cell = [outlineView makeViewWithIdentifier:identifier owner:self];
        if (cell == nil) {
            cell = [[DPNameCellView alloc] initWithFrame:NSZeroRect];
            cell.identifier = identifier;
        }
        NSString *name = DPNodeDisplayName(*_arena, node);
        cell.textField.stringValue = name;
        cell.textField.textColor = primary;
        cell.textField.font = [NSFont systemFontOfSize:NSFont.systemFontSize weight:directHit ? NSFontWeightSemibold : NSFontWeightRegular];
        cell.imageView.image = [self iconForNode:node];
        cell.imageView.alphaValue = muted ? 0.45 : 1.0;
        NSString *toolTip = nil;
        cell.badge.image = [self badgeForNode:n toolTip:&toolTip];
        cell.badge.toolTip = toolTip;
        cell.toolTip = toolTip != nil ? [NSString stringWithFormat:@"%@\n%@", name, toolTip] : name;
        return cell;
    }

    if ([identifier isEqualToString:kColumnUsage]) {
        DPUsageCellView *cell = [outlineView makeViewWithIdentifier:identifier owner:self];
        if (cell == nil) {
            cell = [[DPUsageCellView alloc] initWithFrame:NSZeroRect];
            cell.identifier = identifier;
        }
        double fraction = 1.0;
        if (n.parent_idx != kInvalidNode) {
            const uint64_t parentBytes = _arena->node(n.parent_idx).bytes(_metric);
            fraction = parentBytes > 0 ? static_cast<double>(n.bytes(_metric)) / static_cast<double>(parentBytes) : 0.0;
        }
        cell.bar.fraction = fraction;
        cell.bar.muted = muted;
        return cell;
    }

    DPValueCellView *cell = [outlineView makeViewWithIdentifier:identifier owner:self];
    if (cell == nil) {
        cell = [[DPValueCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = identifier;
    }
    if ([identifier isEqualToString:kColumnSize]) {
        cell.textField.stringValue = DPFormatBytes(n.bytes(_metric));
        cell.textField.textColor = primary;
        if (_metric == SizeMetric::Physical && n.logical_bytes != n.allocated_bytes) {
            cell.toolTip = [NSString stringWithFormat:@"%@ on disk, %@ logical", DPFormatBytes(n.allocated_bytes), DPFormatBytes(n.logical_bytes)];
        } else {
            cell.toolTip = nil;
        }
    } else if ([identifier isEqualToString:kColumnItems]) {
        cell.textField.stringValue = n.is_dir() ? DPFormatCount(n.file_count) : @"";
        cell.textField.textColor = secondary;
        cell.toolTip = n.is_dir() ? [NSString stringWithFormat:@"%@ files in this folder and its subfolders", DPFormatCount(n.file_count)] : nil;
    } else {
        cell.textField.stringValue = DPFormatDate(n.mtime);
        cell.textField.textColor = secondary;
        cell.toolTip = nil;
    }
    return cell;
}

- (nullable NSTableRowView *)outlineView:(NSOutlineView *)outlineView rowViewForItem:(id)__unused item {
    DPRowView *rowView = [outlineView makeViewWithIdentifier:kRowIdentifier owner:self];
    if (rowView == nil) {
        rowView = [[DPRowView alloc] initWithFrame:NSZeroRect];
        rowView.identifier = kRowIdentifier;
    }
    return rowView;
}

- (void)outlineView:(NSOutlineView *)__unused outlineView didAddRowView:(NSTableRowView *)rowView forRow:(NSInteger)row {
    if ([rowView isKindOfClass:DPRowView.class]) static_cast<DPRowView *>(rowView).hoverHighlighted = row == _hoverRow;
}

- (void)outlineViewSelectionDidChange:(NSNotification *)__unused notification {
    if (_suppressSelectionCallback) return;
    const uint32_t node = self.selectedNode;
    if (node != kInvalidNode) [self.actionHandler nodeSource:self didSelectNode:node];
}

- (void)outlineViewItemDidCollapse:(NSNotification *)notification {
    // Give the memory of a collapsed directory's child list back.
    id item = notification.userInfo[@"NSObject"];
    if (item != nil) _children.erase(nodeOfItem(item));
    _hoverRow = -1;
}

- (void)outlineViewItemDidExpand:(NSNotification *)notification {
    _hoverRow = -1;  // row numbers below the expanded item shifted
    id item = notification.userInfo[@"NSObject"];
    if (item != nil) [self scheduleNameColumnWidthForLevel:[_outline levelForItem:item] + 1];
}

// ---------------------------------------------------------------------------------------
#pragma mark - Interaction
// ---------------------------------------------------------------------------------------

- (void)rowDoubleClicked:(id)__unused sender {
    const NSInteger row = _outline.clickedRow;
    if (row < 0 || _arena == nullptr) return;
    const uint32_t node = nodeOfItem([_outline itemAtRow:row]);
    if (!_arena->isValid(node)) return;
    // Double-clicking a folder drills into it; a file drills into its folder.
    const uint32_t target = _arena->node(node).is_dir() ? node : _arena->node(node).parent_idx;
    if (target != kInvalidNode) [self.actionHandler nodeSource:self didRequestFocusNode:target];
}

- (void)outlineView:(DPOutlineView *)__unused outlineView pointerMovedToRow:(NSInteger)row {
    const uint32_t node = (row >= 0 && _arena != nullptr) ? nodeOfItem([_outline itemAtRow:row]) : kInvalidNode;
    [self.actionHandler nodeSource:self didHoverNode:node];
}

- (nullable NSMenu *)outlineView:(DPOutlineView *)__unused outlineView menuForRow:(NSInteger)row {
    if (row < 0 || _arena == nullptr) return nil;
    return [self.actionHandler nodeSource:self menuForNode:nodeOfItem([_outline itemAtRow:row])];
}

- (BOOL)outlineView:(DPOutlineView *)__unused outlineView handleKeyDown:(NSEvent *)event {
    const uint32_t node = self.selectedNode;
    const NSEventModifierFlags modifiers = event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    switch (event.keyCode) {
        case kKeySpace:
            [self.actionHandler nodeSourceDidRequestQuickLook:self];
            return YES;
        case kKeyReturn:
        case kKeyEnter:
            if (node != kInvalidNode && _arena != nullptr && _arena->isValid(node)) {
                const uint32_t target = _arena->node(node).is_dir() ? node : _arena->node(node).parent_idx;
                if (target != kInvalidNode) [self.actionHandler nodeSource:self didRequestFocusNode:target];
            }
            return YES;
        case kKeyDelete:
            if ((modifiers & NSEventModifierFlagCommand) != 0 && node != kInvalidNode) {
                [self.actionHandler nodeSource:self didRequestTrashNode:node];
                return YES;
            }
            return NO;
        default:
            return NO;
    }
}

- (void)revealNode:(uint32_t)node {
    if (_arena == nullptr || !_arena->isValid(node)) return;
    // Expand from the root downwards so that every ancestor's row exists.
    std::vector<uint32_t> ancestors;
    for (uint32_t n = _arena->node(node).parent_idx; n != kInvalidNode; n = _arena->node(n).parent_idx) ancestors.push_back(n);
    _suppressSelectionCallback = YES;
    [NSAnimationContext beginGrouping];
    NSAnimationContext.currentContext.duration = 0;
    for (size_t i = ancestors.size(); i-- > 0;) [_outline expandItem:@(ancestors[i])];
    [NSAnimationContext endGrouping];
    [self applyNameColumnWidth];
    const NSInteger row = [_outline rowForItem:@(node)];
    if (row >= 0) {
        [_outline selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
        [_outline scrollRowToVisible:row];
        [self scrollNameOfRowToVisible:row];
    }
    _suppressSelectionCallback = NO;
}

- (void)setExternalHoverNode:(uint32_t)node {
    NSInteger row = -1;
    if (_arena != nullptr && node != kInvalidNode && _arena->isValid(node)) {
        // The hovered item is usually deep inside collapsed folders: light up the
        // nearest ancestor that has a row.
        for (uint32_t n = node; n != kInvalidNode && row < 0; n = _arena->node(n).parent_idx) row = [_outline rowForItem:@(n)];
    }
    if (row == _hoverRow) return;
    if (_hoverRow >= 0 && _hoverRow < _outline.numberOfRows) {
        DPRowView *old = [_outline rowViewAtRow:_hoverRow makeIfNecessary:NO];
        if ([old isKindOfClass:DPRowView.class]) old.hoverHighlighted = NO;
    }
    _hoverRow = row;
    if (row >= 0) {
        DPRowView *current = [_outline rowViewAtRow:row makeIfNecessary:NO];
        if ([current isKindOfClass:DPRowView.class]) current.hoverHighlighted = YES;
    }
}

- (NSRect)screenRectForNode:(uint32_t)node {
    if (_arena == nullptr || !_arena->isValid(node) || _outline.window == nil) return NSZeroRect;
    const NSInteger row = [_outline rowForItem:@(node)];
    if (row < 0) return NSZeroRect;
    NSTableCellView *cell = [_outline viewAtColumn:0 row:row makeIfNecessary:NO];
    NSView *anchor = cell.imageView != nil ? static_cast<NSView *>(cell.imageView) : static_cast<NSView *>(cell);
    if (anchor == nil) return NSZeroRect;
    const NSRect inWindow = [anchor convertRect:anchor.bounds toView:nil];
    return [_outline.window convertRectToScreen:inWindow];
}

@end
