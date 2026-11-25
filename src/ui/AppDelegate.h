// DiskPrism UI — application delegate: menu bar, main window, open requests.
#import <AppKit/AppKit.h>

@class MainWindowController;

NS_ASSUME_NONNULL_BEGIN

@interface AppDelegate : NSObject <NSApplicationDelegate>

/// Folder to scan as soon as the window is up (from the command line).
@property (nonatomic, copy, nullable) NSString *initialPath;

/// Override for the bundled Shaders.metal.
@property (nonatomic, copy, nullable) NSString *shaderPath;

@property (nonatomic, readonly, nullable) MainWindowController *mainWindowController;

/// Builds the whole menu bar in code (there is no nib).
+ (NSMenu *)buildMainMenuWithApplicationName:(NSString *)name;

@end

NS_ASSUME_NONNULL_END
