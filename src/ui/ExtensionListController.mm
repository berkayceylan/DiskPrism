#import "ExtensionListController.h"

#import "Formatting.h"
#include "FlatArena.hpp"

#include <vector>

using namespace diskprism;

namespace {

NSUserInterfaceItemIdentifier const kColumnType = @"type";
NSUserInterfaceItemIdentifier const kColumnShare = @"share";
NSUserInterfaceItemIdentifier const kColumnBytes = @"bytes";
NSUserInterfaceItemIdentifier const kColumnFiles = @"files";

NSColor *colorFrom(Color color) {
    return [NSColor colorWithSRGBRed:color.r / 255.0 green:color.g / 255.0 blue:color.b / 255.0 alpha:1.0];
}

}  // namespace

/// Color swatch + extension name.
@interface DPTypeCellView : NSTableCellView
@property (nonatomic, readonly) NSView *swatch;
@end

@implementation DPTypeCellView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    _swatch = [[NSView alloc] initWithFrame:NSZeroRect];
    _swatch.wantsLayer = YES;
    _swatch.layer.cornerRadius = 5;
    _swatch.layer.borderWidth = 0.5;
    _swatch.translatesAutoresizingMaskIntoConstraints = NO;
    NSTextField *label = DPMakeLabel(@"", [NSFont systemFontOfSize:NSFont.systemFontSize], NSColor.labelColor);
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_swatch];
    [self addSubview:label];
    self.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [_swatch.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:2],
        [_swatch.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_swatch.widthAnchor constraintEqualToConstant:10],
        [_swatch.heightAnchor constraintEqualToConstant:10],
        [label.leadingAnchor constraintEqualToAnchor:_swatch.trailingAnchor constant:7],
        [label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2],
        [label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

@end

@interface DPNumberCellView : NSTableCellView
@end

@implementation DPNumberCellView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    NSTextField *label = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular],
                                     NSColor.secondaryLabelColor);
    label.alignment = NSTextAlignmentRight;
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:label];
    self.textField = label;
    [NSLayoutConstraint activateConstraints:@[
        [label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2],
        [label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    return self;
}

@end

@implementation ExtensionListController {
    NSTableView *_table;
    NSScrollView *_scrollView;
    NSTextField *_titleLabel;
    NSTextField *_detailLabel;
    NSButton *_clearButton;

    const FlatArena *_arena;
    SizeMetric _metric;
    std::vector<uint16_t> _ranked;
    uint64_t _totalBytes;
    int32_t _selectedExtension;
    BOOL _updatingSelection;
}

- (NSTableView *)tableView {
    return _table;
}

- (NSArray<NSNumber *> *)rankedExtensions {
    NSMutableArray<NSNumber *> *ids = [NSMutableArray arrayWithCapacity:_ranked.size()];
    for (const uint16_t id : _ranked) [ids addObject:@(id)];
    return ids;
}

- (void)loadView {
    _selectedExtension = -1;
    _metric = SizeMetric::Physical;

    // Translucent sidebar backdrop: the desktop shows through, like Finder's sidebar.
    NSVisualEffectView *backdrop = [[NSVisualEffectView alloc] initWithFrame:NSMakeRect(0, 0, 260, 600)];
    backdrop.material = NSVisualEffectMaterialSidebar;
    backdrop.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    backdrop.state = NSVisualEffectStateFollowsWindowActiveState;

    _titleLabel = DPMakeLabel(@"File Types", [NSFont systemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightSemibold],
                              NSColor.secondaryLabelColor);
    _detailLabel = DPMakeLabel(@"", [NSFont systemFontOfSize:NSFont.smallSystemFontSize], NSColor.tertiaryLabelColor);
    _detailLabel.lineBreakMode = NSLineBreakByTruncatingTail;

    _clearButton = [NSButton buttonWithTitle:@"Clear Filter" target:self action:@selector(clearFilter:)];
    _clearButton.bezelStyle = NSBezelStyleAccessoryBarAction;
    _clearButton.controlSize = NSControlSizeSmall;
    _clearButton.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    NSImage *clearImage = DPSymbol(@"xmark.circle.fill", @"Clear filter");
    if (clearImage != nil) {
        _clearButton.image = clearImage;
        _clearButton.imagePosition = NSImageLeading;
    }
    _clearButton.hidden = YES;
    _clearButton.toolTip = @"Show all file types again";
    _clearButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_clearButton setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    _table = [[NSTableView alloc] initWithFrame:NSZeroRect];
    _table.dataSource = self;
    _table.delegate = self;
    _table.style = NSTableViewStyleSourceList;
    _table.backgroundColor = NSColor.clearColor;
    _table.rowHeight = 22;
    _table.headerView = nil;
    _table.allowsMultipleSelection = NO;
    _table.allowsEmptySelection = YES;
    _table.intercellSpacing = NSMakeSize(4, 2);
    _table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    _table.target = self;
    _table.action = @selector(rowClicked:);
    [_table setAccessibilityLabel:@"File types"];

    struct ColumnSpec {
        NSUserInterfaceItemIdentifier identifier;
        CGFloat width;
        CGFloat minWidth;
    };
    for (const ColumnSpec &spec :
         {ColumnSpec{kColumnType, 84, 60}, ColumnSpec{kColumnShare, 40, 38}, ColumnSpec{kColumnBytes, 58, 54}, ColumnSpec{kColumnFiles, 34, 32}}) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:spec.identifier];
        column.width = spec.width;
        column.minWidth = spec.minWidth;
        column.resizingMask = spec.identifier == kColumnType ? NSTableColumnAutoresizingMask : NSTableColumnNoResizing;
        [_table addTableColumn:column];
    }

    _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    _scrollView.documentView = _table;
    _scrollView.hasVerticalScroller = YES;
    _scrollView.autohidesScrollers = YES;
    _scrollView.drawsBackground = NO;
    _scrollView.borderType = NSNoBorder;
    _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
    // The header sits below the toolbar; the table must not slide under it.
    _scrollView.automaticallyAdjustsContentInsets = NO;

    [backdrop addSubview:_titleLabel];
    [backdrop addSubview:_detailLabel];
    [backdrop addSubview:_clearButton];
    [backdrop addSubview:_scrollView];
    NSLayoutGuide *safe = backdrop.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [_titleLabel.topAnchor constraintEqualToAnchor:safe.topAnchor constant:10],
        [_titleLabel.leadingAnchor constraintEqualToAnchor:backdrop.leadingAnchor constant:16],
        [_clearButton.centerYAnchor constraintEqualToAnchor:_titleLabel.centerYAnchor],
        [_clearButton.trailingAnchor constraintEqualToAnchor:backdrop.trailingAnchor constant:-10],
        [_clearButton.leadingAnchor constraintGreaterThanOrEqualToAnchor:_titleLabel.trailingAnchor constant:6],
        [_detailLabel.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:2],
        [_detailLabel.leadingAnchor constraintEqualToAnchor:_titleLabel.leadingAnchor],
        [_detailLabel.trailingAnchor constraintLessThanOrEqualToAnchor:backdrop.trailingAnchor constant:-12],
        [_scrollView.topAnchor constraintEqualToAnchor:_detailLabel.bottomAnchor constant:6],
        [_scrollView.leadingAnchor constraintEqualToAnchor:backdrop.leadingAnchor],
        [_scrollView.trailingAnchor constraintEqualToAnchor:backdrop.trailingAnchor],
        [_scrollView.bottomAnchor constraintEqualToAnchor:backdrop.bottomAnchor],
    ]];
    self.view = backdrop;
}

// ---------------------------------------------------------------------------------------
#pragma mark - Model
// ---------------------------------------------------------------------------------------

- (void)viewDidLayout {
    [super viewDidLayout];
    [_table sizeToFit];  // the type-name column takes the width the numbers leave
}

- (void)setArena:(nullable const FlatArena *)arena metric:(SizeMetric)metric {
    [self loadViewIfNeeded];
    _arena = arena;
    _metric = metric;
    [self reloadKeepingSelection];
}

- (void)reloadKeepingSelection {
    _ranked.clear();
    _totalBytes = 0;
    if (_arena != nullptr) {
        const ExtensionTable &table = _arena->extensions();
        _ranked = table.rankedBySize(_metric);
        for (const uint16_t id : _ranked) _totalBytes += table.stats(id).bytes(_metric);
        // A type whose last file was removed is no longer a valid filter.
        if (_selectedExtension >= 0 && table.stats(static_cast<uint16_t>(_selectedExtension)).file_count == 0) _selectedExtension = -1;
    } else {
        _selectedExtension = -1;
    }
    [_table reloadData];
    [self syncSelection];
    [self updateHeader];
}

- (int32_t)selectedExtension {
    return _selectedExtension;
}

- (void)setSelectedExtension:(int32_t)selectedExtension {
    if (selectedExtension == _selectedExtension) return;
    _selectedExtension = selectedExtension;
    [self syncSelection];
    [self updateHeader];
}

- (void)syncSelection {
    _updatingSelection = YES;
    NSInteger row = -1;
    for (size_t i = 0; i < _ranked.size(); ++i) {
        if (static_cast<int32_t>(_ranked[i]) == _selectedExtension) row = static_cast<NSInteger>(i);
    }
    if (row >= 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:static_cast<NSUInteger>(row)] byExtendingSelection:NO];
    } else {
        [_table deselectAll:nil];
    }
    _updatingSelection = NO;
}

- (NSString *)displayNameForExtension:(uint16_t)extensionId {
    if (extensionId == ExtensionTable::kNone) return @"(none)";
    if (extensionId == ExtensionTable::kOther) return @"Other";
    return [@"." stringByAppendingString:DPStringFromFileSystemBytes(_arena->extensions().name(extensionId))];
}

- (void)updateHeader {
    const BOOL filtering = _selectedExtension >= 0 && _arena != nullptr;
    _clearButton.hidden = !filtering;
    if (_arena == nullptr) {
        _detailLabel.stringValue = @"";
    } else if (filtering) {
        _detailLabel.stringValue = [NSString stringWithFormat:@"Highlighting %@", [self displayNameForExtension:static_cast<uint16_t>(_selectedExtension)]];
    } else {
        _detailLabel.stringValue = [NSString stringWithFormat:@"%@ %@ by %@", DPFormatCount(_ranked.size()), _ranked.size() == 1 ? @"type" : @"types",
                                                              _metric == SizeMetric::Physical ? @"space on disk" : @"logical size"];
    }
}

// ---------------------------------------------------------------------------------------
#pragma mark - Table
// ---------------------------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView *)__unused tableView {
    return static_cast<NSInteger>(_ranked.size());
}

- (nullable NSView *)tableView:(NSTableView *)tableView viewForTableColumn:(nullable NSTableColumn *)tableColumn row:(NSInteger)row {
    if (tableColumn == nil || _arena == nullptr || row < 0 || static_cast<size_t>(row) >= _ranked.size()) return nil;
    const uint16_t extensionId = _ranked[static_cast<size_t>(row)];
    const ExtensionTable &table = _arena->extensions();
    const ExtensionStats stats = table.stats(extensionId);
    NSUserInterfaceItemIdentifier identifier = tableColumn.identifier;

    if ([identifier isEqualToString:kColumnType]) {
        DPTypeCellView *cell = [tableView makeViewWithIdentifier:identifier owner:self];
        if (cell == nil) {
            cell = [[DPTypeCellView alloc] initWithFrame:NSZeroRect];
            cell.identifier = identifier;
        }
        cell.textField.stringValue = [self displayNameForExtension:extensionId];
        cell.swatch.layer.backgroundColor = colorFrom(table.color(extensionId)).CGColor;
        cell.swatch.layer.borderColor = [NSColor.labelColor colorWithAlphaComponent:0.25].CGColor;
        cell.toolTip = [NSString stringWithFormat:@"%@\n%@ on disk, %@ logical\n%@ %@",
                                                  extensionId == ExtensionTable::kNone ? @"Files without an extension"
                                                                                       : [self displayNameForExtension:extensionId],
                                                  DPFormatBytes(stats.allocated_bytes), DPFormatBytes(stats.logical_bytes),
                                                  DPFormatCount(stats.file_count), stats.file_count == 1 ? @"file" : @"files"];
        return cell;
    }

    DPNumberCellView *cell = [tableView makeViewWithIdentifier:identifier owner:self];
    if (cell == nil) {
        cell = [[DPNumberCellView alloc] initWithFrame:NSZeroRect];
        cell.identifier = identifier;
    }
    if ([identifier isEqualToString:kColumnShare]) {
        const double fraction = _totalBytes > 0 ? static_cast<double>(stats.bytes(_metric)) / static_cast<double>(_totalBytes) : 0.0;
        cell.textField.stringValue = DPFormatPercent(fraction);
    } else if ([identifier isEqualToString:kColumnBytes]) {
        cell.textField.stringValue = DPFormatBytes(stats.bytes(_metric));
    } else {
        cell.textField.stringValue = DPFormatCompactCount(static_cast<double>(stats.file_count));
    }
    return cell;
}

/// A click on the row that is already the filter turns the filter off again.
- (void)rowClicked:(id)__unused sender {
    const NSInteger row = _table.clickedRow;
    if (row < 0 || static_cast<size_t>(row) >= _ranked.size()) return;
    const int32_t clicked = _ranked[static_cast<size_t>(row)];
    [self applySelection:clicked == _selectedExtension ? -1 : clicked];
}

- (void)tableViewSelectionDidChange:(NSNotification *)__unused notification {
    if (_updatingSelection) return;
    // Mouse clicks are handled by -rowClicked: (which also implements "click again to
    // clear"). Every other way of selecting a row — arrow keys, VoiceOver, scripting
    // through the accessibility API — changes the filter here.
    const NSInteger row = _table.selectedRow;
    const NSEvent *event = NSApp.currentEvent;
    const BOOL fromMouse = event != nil && (event.type == NSEventTypeLeftMouseDown || event.type == NSEventTypeLeftMouseUp);
    if (fromMouse) return;
    [self applySelection:(row >= 0 && static_cast<size_t>(row) < _ranked.size()) ? static_cast<int32_t>(_ranked[static_cast<size_t>(row)]) : -1];
}

- (void)applySelection:(int32_t)extensionId {
    if (extensionId == _selectedExtension) {
        [self syncSelection];
        return;
    }
    _selectedExtension = extensionId;
    [self syncSelection];
    [self updateHeader];
    [self.delegate extensionList:self didSelectExtension:extensionId];
}

- (void)clearFilter:(id)__unused sender {
    [self applySelection:-1];
}

@end
