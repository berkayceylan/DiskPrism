#import "BreadcrumbBar.h"

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#import "Formatting.h"
#include "FlatArena.hpp"

#include <vector>

using namespace diskprism;

@implementation BreadcrumbBar {
    NSPathControl *_pathControl;
    NSButton *_upButton;
    NSTextField *_summaryLabel;
    NSMutableArray<NSNumber *> *_nodes;
    uint32_t _parentOfFocus;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    _nodes = [NSMutableArray array];
    _parentOfFocus = kInvalidNode;

    _upButton = [NSButton buttonWithImage:[self upImage] target:self action:@selector(goUp:)];
    _upButton.bordered = NO;
    _upButton.toolTip = @"Zoom out to the enclosing folder";
    _upButton.enabled = NO;
    _upButton.translatesAutoresizingMaskIntoConstraints = NO;
    [_upButton setAccessibilityLabel:@"Enclosing Folder"];

    _pathControl = [[NSPathControl alloc] initWithFrame:NSZeroRect];
    _pathControl.pathStyle = NSPathStyleStandard;
    _pathControl.controlSize = NSControlSizeSmall;
    _pathControl.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    _pathControl.backgroundColor = NSColor.clearColor;
    _pathControl.editable = NO;
    _pathControl.target = self;
    _pathControl.action = @selector(pathClicked:);
    _pathControl.translatesAutoresizingMaskIntoConstraints = NO;
    [_pathControl setContentCompressionResistancePriority:NSLayoutPriorityDefaultLow - 10
                                           forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_pathControl setContentHuggingPriority:NSLayoutPriorityDefaultLow - 10 forOrientation:NSLayoutConstraintOrientationHorizontal];

    _summaryLabel = DPMakeLabel(@"", [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular],
                                NSColor.secondaryLabelColor);
    _summaryLabel.alignment = NSTextAlignmentRight;
    [_summaryLabel setContentCompressionResistancePriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];
    [_summaryLabel setContentHuggingPriority:NSLayoutPriorityDefaultHigh forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSBox *separator = [[NSBox alloc] initWithFrame:NSZeroRect];
    separator.boxType = NSBoxSeparator;
    separator.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:_upButton];
    [self addSubview:_pathControl];
    [self addSubview:_summaryLabel];
    [self addSubview:separator];
    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:30],
        [_upButton.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
        [_upButton.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_upButton.widthAnchor constraintEqualToConstant:22],
        [_pathControl.leadingAnchor constraintEqualToAnchor:_upButton.trailingAnchor constant:4],
        [_pathControl.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_summaryLabel.leadingAnchor constraintGreaterThanOrEqualToAnchor:_pathControl.trailingAnchor constant:8],
        [_summaryLabel.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
        [_summaryLabel.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [separator.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [separator.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [separator.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    return self;
}

- (NSImage *)upImage {
    NSImage *image = DPSymbol(@"chevron.up", @"Enclosing Folder");
    if (image != nil) return image;
    return [NSImage imageNamed:NSImageNameGoBackTemplate];
}

- (NSArray<NSNumber *> *)segmentNodes {
    return [_nodes copy];
}

- (void)setArena:(nullable const FlatArena *)arena focus:(uint32_t)focus metric:(SizeMetric)metric {
    [_nodes removeAllObjects];
    _parentOfFocus = kInvalidNode;
    if (arena == nullptr || focus == kInvalidNode || !arena->isValid(focus)) {
        _pathControl.pathItems = @[];
        _summaryLabel.stringValue = @"";
        _upButton.enabled = NO;
        return;
    }

    std::vector<uint32_t> chain;
    for (uint32_t n = focus; n != kInvalidNode; n = arena->node(n).parent_idx) chain.push_back(n);

    NSImage *folderIcon = [NSWorkspace.sharedWorkspace iconForContentType:UTTypeFolder];
    folderIcon.size = NSMakeSize(16, 16);
    NSMutableArray<NSPathControlItem *> *items = [NSMutableArray arrayWithCapacity:chain.size()];
    for (size_t i = chain.size(); i-- > 0;) {
        const uint32_t node = chain[i];
        NSPathControlItem *item = [[NSPathControlItem alloc] init];
        item.title = DPNodeDisplayName(*arena, node);
        if (node == arena->root()) {
            NSImage *rootIcon = [NSWorkspace.sharedWorkspace iconForFile:DPNodePath(*arena, node)];
            rootIcon.size = NSMakeSize(16, 16);
            item.image = rootIcon;
        } else {
            item.image = folderIcon;
        }
        [items addObject:item];
        [_nodes addObject:@(node)];
    }
    _pathControl.pathItems = items;

    const FsNode &n = arena->node(focus);
    _parentOfFocus = n.parent_idx;
    _upButton.enabled = _parentOfFocus != kInvalidNode;
    _summaryLabel.stringValue = [NSString stringWithFormat:@"%@  ·  %@ %@", DPFormatBytes(n.bytes(metric)), DPFormatCount(n.file_count),
                                                           n.file_count == 1 ? @"file" : @"files"];
    _summaryLabel.toolTip = metric == SizeMetric::Physical ? @"Space these files occupy on disk" : @"Logical size of these files";
}

- (void)pathClicked:(NSPathControl *)sender {
    NSPathControlItem *clicked = sender.clickedPathItem;
    if (clicked == nil) return;
    const NSUInteger index = [sender.pathItems indexOfObjectIdenticalTo:clicked];
    if (index == NSNotFound || index >= _nodes.count) return;
    [self.delegate breadcrumbBar:self didSelectNode:_nodes[index].unsignedIntValue];
}

- (void)goUp:(id)__unused sender {
    if (_parentOfFocus != kInvalidNode) [self.delegate breadcrumbBar:self didSelectNode:_parentOfFocus];
}

@end
