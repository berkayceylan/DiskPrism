// DiskPrism UI — headless command-line modes of the app binary.
#pragma once

#include <string>

namespace diskprism::cli {

/// What the process was started to do.
struct Invocation {
    bool handled = false;      ///< a headless command ran; exit with `exit_code`
    int exit_code = 0;
    std::string initial_path;  ///< folder to scan as soon as the window is up (may be empty)
    std::string shader_path;   ///< override for the bundled Shaders.metal (may be empty)
};

/// Parses the command line and runs a headless command if one was requested:
///
///   --benchmark <path> [--runs N] [--threads N] [--logical]
///         scan <path> without any UI and report throughput and memory
///   --snapshot <path> --out <file.png> [--mode treemap|sunburst] [--size WxH]
///              [--scale S] [--appearance dark|light] [--logical] [--filter TEXT]
///              [--focus REL/PATH] [--shader FILE]
///         scan <path> and render the map off screen with the real Metal pipeline
///   --help, --version
///
/// Anything else starts the normal app; a single positional argument is scanned on launch.
Invocation handleCommandLine(int argc, const char* const* argv);

}  // namespace diskprism::cli
