#import "ToolbarViews.h"

#import "Formatting.h"

#include <algorithm>

using diskprism::VolumeInfo;

// ---------------------------------------------------------------------------------------
#pragma mark - Telemetry pill
// ---------------------------------------------------------------------------------------

@implementation DPTelemetryPillView {
    NSTextField *_label;
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;
    _label = DPMakeLabel(@"Ready", [NSFont monospacedDigitSystemFontOfSize:NSFont.smallSystemFontSize weight:NSFontWeightRegular],
                         NSColor.secondaryLabelColor);
    _label.lineBreakMode = NSLineBreakByTruncatingTail;
    [self addSubview:_label];
    self.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:24],
        [self.widthAnchor constraintGreaterThanOrEqualToConstant:96],
        [self.widthAnchor constraintLessThanOrEqualToConstant:400],
        [_label.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:24],
        [_label.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
        [_label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
    ]];
    [self setAccessibilityElement:YES];
    [self setAccessibilityRole:NSAccessibilityStaticTextRole];
    [self setAccessibilityLabel:@"Scan status"];
    return self;
}

- (NSString *)text {
    return _label.stringValue;
}

- (void)setText:(NSString *)text {
    _label.stringValue = text;
    self.toolTip = text;
    [self setAccessibilityValue:text];
}

- (void)setActive:(BOOL)active {
    if (active == _active) return;
    _active = active;
    self.needsDisplay = YES;
}

- (void)drawRect:(NSRect)__unused dirtyRect {
    const NSRect bounds = self.bounds;
    const CGFloat radius = bounds.size.height / 2;
    [[NSColor.labelColor colorWithAlphaComponent:0.07] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:bounds xRadius:radius yRadius:radius] fill];

    // Status dot: accent while a scan runs, neutral otherwise.
    const NSRect dot = NSMakeRect(10, NSMidY(bounds) - 3.5, 7, 7);
    [(_active ? NSColor.controlAccentColor : [NSColor.labelColor colorWithAlphaComponent:0.28]) setFill];
    [[NSBezierPath bezierPathWithOvalInRect:dot] fill];
}

@end

// ---------------------------------------------------------------------------------------
#pragma mark - Volume menu item
// ---------------------------------------------------------------------------------------

@implementation DPVolumeMenuItemView {
    NSString *_name;
    NSString *_detail;
    NSImage *_icon;
    double _used;
    double _purgeable;
    NSTrackingArea *_trackingArea;
}

- (instancetype)initWithVolume:(const VolumeInfo &)volume {
    self = [super initWithFrame:NSMakeRect(0, 0, 320, 54)];
    if (self == nil) return nil;
    NSString *mountPoint = DPStringFromFileSystemBytes(volume.mount_point);
    _name = DPStringFromFileSystemBytes(volume.name);
    _icon = [NSWorkspace.sharedWorkspace iconForFile:mountPoint];

    const double total = static_cast<double>(std::max<uint64_t>(volume.total_bytes, 1));
    // "Available" as Finder reports it includes space the system can purge on demand.
    const uint64_t available = std::max(volume.important_available_bytes, volume.available_bytes);
    _purgeable = std::clamp(static_cast<double>(volume.purgeable_bytes()) / total, 0.0, 1.0);
    _used = std::clamp(1.0 - static_cast<double>(available) / total, 0.0, 1.0);
    NSMutableString *detail = [NSMutableString stringWithFormat:@"%@ available of %@", DPFormatBytes(available), DPFormatBytes(volume.total_bytes)];
    if (volume.purgeable_bytes() > 0) [detail appendFormat:@"  ·  %@ purgeable", DPFormatBytes(volume.purgeable_bytes())];
    _detail = detail;

    [self setAccessibilityElement:YES];
    [self setAccessibilityRole:NSAccessibilityMenuItemRole];
    [self setAccessibilityLabel:[NSString stringWithFormat:@"%@, %@", _name, _detail]];
    return self;
}

- (NSString *)detailText {
    return _detail;
}

- (double)usedFraction {
    return _used;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea != nil) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                 options:NSTrackingMouseEnteredAndExited | NSTrackingActiveAlways | NSTrackingInVisibleRect
                                                   owner:self
                                                userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseEntered:(NSEvent *)__unused event {
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent *)__unused event {
    self.needsDisplay = YES;
}

/// A custom view replaces the menu item's own click handling: choose the item manually.
- (void)mouseUp:(NSEvent *)__unused event {
    NSMenuItem *item = self.enclosingMenuItem;
    NSMenu *menu = item.menu;
    if (item == nil || menu == nil) return;
    [menu cancelTracking];
    [menu performActionForItemAtIndex:[menu indexOfItem:item]];
}

- (BOOL)isFlipped {
    return YES;
}

- (void)drawRect:(NSRect)__unused dirtyRect {
    const NSRect bounds = self.bounds;
    const BOOL highlighted = self.enclosingMenuItem.isHighlighted;
    if (highlighted) {
        [NSColor.selectedContentBackgroundColor setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bounds, 5, 1) xRadius:6 yRadius:6] fill];
    }
    NSColor *primary = highlighted ? NSColor.alternateSelectedControlTextColor : NSColor.labelColor;
    NSColor *secondary = highlighted ? [NSColor.alternateSelectedControlTextColor colorWithAlphaComponent:0.85] : NSColor.secondaryLabelColor;

    [_icon drawInRect:NSMakeRect(14, 9, 36, 36) fromRect:NSZeroRect operation:NSCompositingOperationSourceOver fraction:1.0
       respectFlipped:YES
                hints:nil];

    const CGFloat textX = 60;
    const CGFloat textWidth = bounds.size.width - textX - 16;
    NSMutableParagraphStyle *truncating = [[NSMutableParagraphStyle alloc] init];
    truncating.lineBreakMode = NSLineBreakByTruncatingTail;
    [_name drawInRect:NSMakeRect(textX, 6, textWidth, 17)
       withAttributes:@{
           NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightMedium],
           NSForegroundColorAttributeName : primary,
           NSParagraphStyleAttributeName : truncating,
       }];

    // Capacity bar: used space, then the purgeable part of it in a lighter tone.
    const NSRect track = NSMakeRect(textX, 26, textWidth, 6);
    [(highlighted ? [NSColor.alternateSelectedControlTextColor colorWithAlphaComponent:0.3] : [NSColor.labelColor colorWithAlphaComponent:0.12]) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:track xRadius:3 yRadius:3] fill];
    if (_used + _purgeable > 0) {
        [NSGraphicsContext saveGraphicsState];
        [[NSBezierPath bezierPathWithRoundedRect:track xRadius:3 yRadius:3] addClip];
        NSColor *usedColor = highlighted ? NSColor.alternateSelectedControlTextColor
                                         : (_used > 0.9 ? NSColor.systemRedColor : (_used > 0.75 ? NSColor.systemOrangeColor : NSColor.controlAccentColor));
        NSRect usedRect = track;
        usedRect.size.width = track.size.width * _used;
        [usedColor setFill];
        NSRectFill(usedRect);
        NSRect purgeableRect = track;
        purgeableRect.origin.x = NSMaxX(usedRect);
        purgeableRect.size.width = track.size.width * _purgeable;
        [[usedColor colorWithAlphaComponent:0.38] setFill];
        NSRectFillUsingOperation(purgeableRect, NSCompositingOperationSourceOver);
        [NSGraphicsContext restoreGraphicsState];
    }

    [_detail drawInRect:NSMakeRect(textX, 35, textWidth, 14)
         withAttributes:@{
             NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:10.5 weight:NSFontWeightRegular],
             NSForegroundColorAttributeName : secondary,
             NSParagraphStyleAttributeName : truncating,
         }];
}

@end

// ---------------------------------------------------------------------------------------
#pragma mark - Search field
// ---------------------------------------------------------------------------------------

@implementation DPSearchField

- (void)setAccessibilityValue:(nullable id)accessibilityValue {
    [super setAccessibilityValue:accessibilityValue];
    [self sendAction:self.action to:self.target];
}

@end
