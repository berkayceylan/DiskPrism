// DiskPrism UI — small custom views used in the toolbar and its menus.
#import <AppKit/AppKit.h>

#include "VolumeInfo.hpp"

NS_ASSUME_NONNULL_BEGIN

/// Rounded "pill" that shows live scan telemetry (items, bytes, files per second).
@interface DPTelemetryPillView : NSView
@property (nonatomic, copy) NSString *text;
/// A scan is running: the status dot turns to the accent color.
@property (nonatomic) BOOL active;
@end

/// Menu item content for one mounted volume: icon, name, a capacity bar (used / purgeable
/// / free) and a free-space read-out. Built fresh every time the menu opens, so the
/// numbers are live.
@interface DPVolumeMenuItemView : NSView
- (instancetype)initWithVolume:(const diskprism::VolumeInfo &)volume;
@property (nonatomic, readonly) NSString *detailText;
@property (nonatomic, readonly) double usedFraction;
@end

/// Search field that also sends its action when its value is set through the
/// accessibility API (VoiceOver, Voice Control, automation), not only when typed into.
@interface DPSearchField : NSSearchField
@end

NS_ASSUME_NONNULL_END
