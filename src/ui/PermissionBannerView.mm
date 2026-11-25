#import "PermissionBannerView.h"

#import "Formatting.h"

@implementation PermissionBannerView {
    NSTextField *_label;
    NSBox *_background;
    NSButton *_settingsButton;
}

- (NSButton *)settingsButton {
    return _settingsButton;
}

+ (NSURL *)fullDiskAccessSettingsURL {
    return [NSURL URLWithString:@"x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles"];
}

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self == nil) return nil;

    // NSBox resolves dynamic colors itself, so the tint follows Dark / Light mode.
    _background = [[NSBox alloc] initWithFrame:NSZeroRect];
    _background.boxType = NSBoxCustom;
    _background.borderWidth = 0;
    _background.cornerRadius = 0;
    _background.fillColor = [NSColor.systemYellowColor colorWithAlphaComponent:0.22];
    _background.translatesAutoresizingMaskIntoConstraints = NO;

    NSImageView *icon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    icon.image = DPSymbol(@"lock.shield.fill", @"Protected folders");
    icon.contentTintColor = NSColor.systemOrangeColor;
    icon.translatesAutoresizingMaskIntoConstraints = NO;
    [icon setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    _label = DPMakeLabel(@"", [NSFont systemFontOfSize:NSFont.smallSystemFontSize], NSColor.labelColor);
    _label.lineBreakMode = NSLineBreakByTruncatingTail;

    NSButton *settings = [NSButton buttonWithTitle:@"Full Disk Access Settings…" target:self action:@selector(openSettings:)];
    settings.bezelStyle = NSBezelStyleAccessoryBarAction;
    settings.controlSize = NSControlSizeSmall;
    settings.font = [NSFont systemFontOfSize:NSFont.smallSystemFontSize];
    settings.toolTip = @"Open System Settings → Privacy & Security → Full Disk Access";
    _settingsButton = settings;
    settings.translatesAutoresizingMaskIntoConstraints = NO;
    [settings setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];
    [settings setContentCompressionResistancePriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSImage *closeImage = DPSymbol(@"xmark", @"Dismiss");
    NSButton *close = closeImage != nil ? [NSButton buttonWithImage:closeImage target:self action:@selector(dismiss:)]
                                        : [NSButton buttonWithTitle:@"✕" target:self action:@selector(dismiss:)];
    close.bordered = NO;
    close.toolTip = @"Dismiss";
    close.translatesAutoresizingMaskIntoConstraints = NO;
    [close setAccessibilityLabel:@"Dismiss"];
    [close setContentHuggingPriority:NSLayoutPriorityRequired forOrientation:NSLayoutConstraintOrientationHorizontal];

    NSBox *separator = [[NSBox alloc] initWithFrame:NSZeroRect];
    separator.boxType = NSBoxSeparator;
    separator.translatesAutoresizingMaskIntoConstraints = NO;

    [self addSubview:_background];
    [self addSubview:icon];
    [self addSubview:_label];
    [self addSubview:settings];
    [self addSubview:close];
    [self addSubview:separator];
    [NSLayoutConstraint activateConstraints:@[
        [self.heightAnchor constraintEqualToConstant:32],
        [_background.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [_background.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [_background.topAnchor constraintEqualToAnchor:self.topAnchor],
        [_background.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
        [icon.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:12],
        [icon.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [_label.leadingAnchor constraintEqualToAnchor:icon.trailingAnchor constant:8],
        [_label.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [settings.leadingAnchor constraintGreaterThanOrEqualToAnchor:_label.trailingAnchor constant:10],
        [settings.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [close.leadingAnchor constraintEqualToAnchor:settings.trailingAnchor constant:8],
        [close.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-10],
        [close.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        [separator.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [separator.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [separator.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
    self.hidden = YES;
    return self;
}

- (NSString *)message {
    return _label.stringValue;
}

- (void)showForDeniedCount:(uint64_t)count {
    if (count == 0) {
        self.hidden = YES;
        return;
    }
    // Short enough to stay readable in a narrow pane; the details are in the tooltip.
    _label.stringValue = [NSString stringWithFormat:@"%@ protected %@ skipped", DPFormatCount(count),
                                                    count == 1 ? @"folder was" : @"folders were"];
    _label.toolTip = [NSString stringWithFormat:@"macOS did not let DiskPrism read %@, so %@ not counted. Grant Full Disk Access in "
                                                @"System Settings → Privacy & Security, then scan again.",
                                                count == 1 ? @"this folder" : @"these folders", count == 1 ? @"it is" : @"they are"];
    self.hidden = NO;
}

- (void)openSettings:(id)__unused sender {
    [NSWorkspace.sharedWorkspace openURL:PermissionBannerView.fullDiskAccessSettingsURL];
}

- (void)dismiss:(id)__unused sender {
    self.hidden = YES;
    if (self.dismissHandler != nil) self.dismissHandler();
}

@end
