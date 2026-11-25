#import "AppDelegate.h"

#import "MainWindowController.h"

namespace {

NSMenuItem *addItem(NSMenu *menu, NSString *title, SEL action, NSString *key, NSEventModifierFlags modifiers) {
    NSMenuItem *item = [menu addItemWithTitle:title action:action keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    return item;
}

NSMenuItem *addItem(NSMenu *menu, NSString *title, SEL action, NSString *key) {
    return addItem(menu, title, action, key, NSEventModifierFlagCommand);
}

NSMenu *addSubmenu(NSMenu *bar, NSString *title) {
    NSMenuItem *item = [bar addItemWithTitle:title action:nil keyEquivalent:@""];
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    item.submenu = menu;
    return menu;
}

NSString *arrowKey(unichar key) {
    return [NSString stringWithCharacters:&key length:1];
}

}  // namespace

@implementation AppDelegate {
    MainWindowController *_mainWindowController;
}

- (nullable MainWindowController *)mainWindowController {
    return _mainWindowController;
}

/// Every item except the standard application ones has a nil target: the action travels
/// up the responder chain to the main window controller, which also validates it.
+ (NSMenu *)buildMainMenuWithApplicationName:(NSString *)name {
    NSMenu *bar = [[NSMenu alloc] initWithTitle:@"Main Menu"];

    // Application
    NSMenu *app = addSubmenu(bar, name);
    addItem(app, [@"About " stringByAppendingString:name], @selector(orderFrontStandardAboutPanel:), @"");
    [app addItem:NSMenuItem.separatorItem];
    NSMenuItem *services = [app addItemWithTitle:@"Services" action:nil keyEquivalent:@""];
    NSMenu *servicesMenu = [[NSMenu alloc] initWithTitle:@"Services"];
    services.submenu = servicesMenu;
    NSApp.servicesMenu = servicesMenu;
    [app addItem:NSMenuItem.separatorItem];
    addItem(app, [@"Hide " stringByAppendingString:name], @selector(hide:), @"h");
    addItem(app, @"Hide Others", @selector(hideOtherApplications:), @"h", NSEventModifierFlagCommand | NSEventModifierFlagOption);
    addItem(app, @"Show All", @selector(unhideAllApplications:), @"");
    [app addItem:NSMenuItem.separatorItem];
    addItem(app, [@"Quit " stringByAppendingString:name], @selector(terminate:), @"q");

    // File
    NSMenu *file = addSubmenu(bar, @"File");
    addItem(file, @"Scan Folder…", @selector(chooseFolder:), @"o");
    addItem(file, @"Scan Startup Disk", @selector(scanDefaultTarget:), @"O");
    addItem(file, @"Scan Again", @selector(rescan:), @"r");
    addItem(file, @"Cancel Scan", @selector(cancelScan:), @".");
    [file addItem:NSMenuItem.separatorItem];
    addItem(file, @"Reveal in Finder", @selector(revealInFinder:), @"R");
    addItem(file, @"Quick Look", @selector(quickLookSelection:), @"y");
    addItem(file, @"Open in Terminal", @selector(openInTerminal:), @"T");
    addItem(file, @"Move to Trash", @selector(moveSelectionToTrash:), arrowKey(NSBackspaceCharacter));
    [file addItem:NSMenuItem.separatorItem];
    addItem(file, @"Close Window", @selector(performClose:), @"w");

    // Edit (the standard items keep the search field fully functional)
    NSMenu *edit = addSubmenu(bar, @"Edit");
    addItem(edit, @"Undo", NSSelectorFromString(@"undo:"), @"z");
    addItem(edit, @"Redo", NSSelectorFromString(@"redo:"), @"Z");
    [edit addItem:NSMenuItem.separatorItem];
    addItem(edit, @"Cut", @selector(cut:), @"x");
    addItem(edit, @"Copy", @selector(copy:), @"c");
    addItem(edit, @"Paste", @selector(paste:), @"v");
    addItem(edit, @"Select All", @selector(selectAll:), @"a");
    [edit addItem:NSMenuItem.separatorItem];
    addItem(edit, @"Copy Full Path", @selector(copyFullPath:), @"c", NSEventModifierFlagCommand | NSEventModifierFlagOption);
    [edit addItem:NSMenuItem.separatorItem];
    addItem(edit, @"Filter by Name…", @selector(focusSearchField:), @"f");
    addItem(edit, @"Clear Filters", @selector(clearFilters:), @"f", NSEventModifierFlagCommand | NSEventModifierFlagOption);

    // View
    NSMenu *view = addSubmenu(bar, @"View");
    addItem(view, @"as Treemap", @selector(showTreemap:), @"1");
    addItem(view, @"as Sunburst", @selector(showSunburst:), @"2");
    [view addItem:NSMenuItem.separatorItem];
    addItem(view, @"Physical Size (On Disk)", @selector(usePhysicalSize:), @"3");
    addItem(view, @"Logical Size", @selector(useLogicalSize:), @"4");
    [view addItem:NSMenuItem.separatorItem];
    addItem(view, @"Enter Full Screen", @selector(toggleFullScreen:), @"f", NSEventModifierFlagCommand | NSEventModifierFlagControl);

    // Go
    NSMenu *go = addSubmenu(bar, @"Go");
    addItem(go, @"Zoom Into Selection", @selector(zoomIntoSelection:), arrowKey(NSDownArrowFunctionKey));
    addItem(go, @"Enclosing Folder", @selector(zoomOut:), arrowKey(NSUpArrowFunctionKey));
    addItem(go, @"Top of Scan", @selector(zoomToScanRoot:), arrowKey(NSUpArrowFunctionKey), NSEventModifierFlagCommand | NSEventModifierFlagShift);

    // Window
    NSMenu *window = addSubmenu(bar, @"Window");
    addItem(window, @"Minimize", @selector(performMiniaturize:), @"m");
    addItem(window, @"Zoom", @selector(performZoom:), @"");
    [window addItem:NSMenuItem.separatorItem];
    addItem(window, @"Bring All to Front", @selector(arrangeInFront:), @"");
    NSApp.windowsMenu = window;

    // Help
    NSMenu *help = addSubmenu(bar, @"Help");
    NSApp.helpMenu = help;
    return bar;
}

- (void)applicationWillFinishLaunching:(NSNotification *)__unused notification {
    NSString *name = NSProcessInfo.processInfo.processName;
    NSApp.mainMenu = [AppDelegate buildMainMenuWithApplicationName:name.length > 0 ? name : @"DiskPrism"];
}

- (void)applicationDidFinishLaunching:(NSNotification *)__unused notification {
    _mainWindowController = [[MainWindowController alloc] initWithShaderPath:self.shaderPath];
    [_mainWindowController showWindow:self];
    [_mainWindowController.window makeKeyAndOrderFront:self];
    [NSApp activateIgnoringOtherApps:YES];
    if (self.initialPath.length > 0) [_mainWindowController scanPath:self.initialPath];
}

/// Folders dropped on the Dock icon or opened with "Open With".
- (void)application:(NSApplication *)__unused application openURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (url == nil || !url.isFileURL) return;
    if (_mainWindowController != nil) {
        [_mainWindowController scanPath:url.path];
    } else {
        self.initialPath = url.path;  // still launching
    }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)__unused sender {
    return YES;
}

- (BOOL)applicationSupportsSecureRestorableState:(NSApplication *)__unused app {
    return YES;
}

@end
