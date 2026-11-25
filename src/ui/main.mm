// DiskPrism — entry point. The whole UI is built in code: no nib, no storyboard.
#import <AppKit/AppKit.h>

#import "AppDelegate.h"
#import "CommandLine.h"

int main(int argc, const char *argv[]) {
    // Headless modes (--benchmark, --snapshot, --help, --version) never start AppKit's
    // event loop and never show a window.
    const diskprism::cli::Invocation invocation = diskprism::cli::handleCommandLine(argc, argv);
    if (invocation.handled) return invocation.exit_code;

    @autoreleasepool {
        NSApplication *application = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        if (!invocation.initial_path.empty()) delegate.initialPath = @(invocation.initial_path.c_str());
        if (!invocation.shader_path.empty()) delegate.shaderPath = @(invocation.shader_path.c_str());
        application.delegate = delegate;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        [application run];
    }
    return 0;
}
