// DiskPrism UI — slim banner shown when macOS denied access to some folders.
#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

/// "N folders could not be read — grant Full Disk Access" with a button that opens the
/// matching pane of System Settings and a close button. Hidden until needed.
@interface PermissionBannerView : NSView

/// Shows the banner for `count` skipped folders (hides it when `count` is 0).
- (void)showForDeniedCount:(uint64_t)count;

/// Called when the user closes the banner.
@property (nonatomic, copy, nullable) void (^dismissHandler)(void);

@property (nonatomic, readonly) NSString *message;

/// The one-click button that opens the Full Disk Access pane of System Settings.
@property (nonatomic, readonly) NSButton *settingsButton;

/// URL of System Settings → Privacy & Security → Full Disk Access.
@property (class, nonatomic, readonly) NSURL *fullDiskAccessSettingsURL;

@end

NS_ASSUME_NONNULL_END
