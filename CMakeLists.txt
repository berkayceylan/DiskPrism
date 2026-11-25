cmake_minimum_required(VERSION 3.24)

# The deployment target has to be known before project() probes the compilers.
# 14.0 is the first macOS with -[NSView displayLinkWithTarget:selector:], which drives the
# canvas animations.
set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version")

project(DiskPrism
  VERSION 1.0.0
  DESCRIPTION "Native macOS GPU-accelerated disk space analyzer"
  LANGUAGES CXX OBJCXX)

if(NOT APPLE)
  message(FATAL_ERROR "DiskPrism is a macOS application (AppKit, Metal, getattrlistbulk).")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug or Release)" FORCE)
endif()

option(ENABLE_SANITIZERS "Instrument everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(DISKPRISM_BUILD_TESTS "Build the test suite" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Some Command Line Tools installations carry a stale, nearly empty usr/include/c++/v1 left
# behind by an old update. clang searches it first and then finds no <cstdint> at all. If
# that is the case here, use the complete libc++ headers of the active SDK instead.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <cstdint>\nint main() { return 0; }" DISKPRISM_TOOLCHAIN_HAS_LIBCXX)
if(NOT DISKPRISM_TOOLCHAIN_HAS_LIBCXX)
  set(diskprism_sdk "${CMAKE_OSX_SYSROOT}")
  if(NOT IS_DIRECTORY "${diskprism_sdk}")
    execute_process(COMMAND xcrun --sdk macosx --show-sdk-path OUTPUT_VARIABLE diskprism_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
  endif()
  set(diskprism_libcxx "${diskprism_sdk}/usr/include/c++/v1")
  if(NOT EXISTS "${diskprism_libcxx}/cstdint")
    message(FATAL_ERROR "The C++ standard library headers were not found. Reinstall the Command Line Tools (xcode-select --install).")
  endif()
  message(WARNING
    "The active toolchain does not find its C++ standard headers (usually a stale "
    "/Library/Developer/CommandLineTools/usr/include/c++ folder; removing it fixes the toolchain). "
    "Building against ${diskprism_libcxx} instead.")
  foreach(lang CXX OBJCXX)
    string(APPEND CMAKE_${lang}_FLAGS " -nostdinc++ -isystem ${diskprism_libcxx}")
  endforeach()
endif()

# ---------------------------------------------------------------------------------------
# Build profiles
#   Debug    -g -O0            (+ ASan/UBSan with -DENABLE_SANITIZERS=ON)
#   Release  -O3 -flto         link-time optimised, dead code and local symbols stripped
# ---------------------------------------------------------------------------------------
foreach(lang CXX OBJCXX)
  set(CMAKE_${lang}_FLAGS_DEBUG "-g -O0")
  set(CMAKE_${lang}_FLAGS_RELEASE "-O3 -flto -DNDEBUG")
endforeach()
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-flto -Wl,-dead_strip -Wl,-x")
# Automatic Reference Counting for all Objective-C++ sources. Cocoa treats exceptions as
# fatal, so the code ARC would add to release references while one unwinds is dead weight
# (about 7 % of the binary). C++ destructors still run.
set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fobjc-arc -fno-objc-arc-exceptions")

# Flags shared by every target of the project. Warnings are part of the contract: the
# sources build with zero diagnostics under -Wall -Wextra -Wpedantic in both profiles.
add_library(diskprism_build_flags INTERFACE)
target_compile_options(diskprism_build_flags INTERFACE -Wall -Wextra -Wpedantic)
if(ENABLE_SANITIZERS)
  set(sanitizer_flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_compile_options(diskprism_build_flags INTERFACE ${sanitizer_flags})
  target_link_options(diskprism_build_flags INTERFACE ${sanitizer_flags})
endif()

# ---------------------------------------------------------------------------------------
# Core engine: pure C++20, no AppKit, no Metal (CoreFoundation only, for volume names
# and purgeable-space capacity).
# ---------------------------------------------------------------------------------------
add_library(diskprism_core STATIC
  src/core/Benchmark.cpp
  src/core/BulkAttr.cpp
  src/core/BulkScanner.cpp
  src/core/ColorSpace.cpp
  src/core/EntryClassifier.cpp
  src/core/ExtensionTable.cpp
  src/core/FlatArena.cpp
  src/core/NodeFilter.cpp
  src/core/StringPool.cpp
  src/core/SunburstLayout.cpp
  src/core/SunburstTransition.cpp
  src/core/TreemapLayout.cpp
  src/core/VirtualRegion.cpp
  src/core/VolumeInfo.cpp)
target_include_directories(diskprism_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_link_libraries(diskprism_core PUBLIC diskprism_build_flags "-framework CoreFoundation")

# ---------------------------------------------------------------------------------------
# Metal renderer and canvas view.
# ---------------------------------------------------------------------------------------
set(DISKPRISM_SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/Shaders.metal)

add_library(diskprism_gpu STATIC
  src/gpu/DiskMapMetalView.mm
  src/gpu/DiskMapRenderer.mm
  src/gpu/GpuInstances.cpp)
target_include_directories(diskprism_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/gpu)
target_link_libraries(diskprism_gpu PUBLIC
  diskprism_core
  "-framework AppKit"
  "-framework Metal"
  "-framework MetalKit"
  "-framework QuartzCore"
  "-framework CoreGraphics")

# ---------------------------------------------------------------------------------------
# Programmatic AppKit user interface (everything except main()).
# ---------------------------------------------------------------------------------------
add_library(diskprism_ui STATIC
  src/ui/AppDelegate.mm
  src/ui/BreadcrumbBar.mm
  src/ui/CanvasViewController.mm
  src/ui/CommandLine.mm
  src/ui/ExtensionListController.mm
  src/ui/Formatting.mm
  src/ui/HoverHudView.mm
  src/ui/MainWindowController.mm
  src/ui/PermissionBannerView.mm
  src/ui/ScanCoordinator.mm
  src/ui/ToolbarViews.mm
  src/ui/TreeOutlineController.mm)
target_include_directories(diskprism_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/ui)
target_compile_definitions(diskprism_ui PRIVATE DISKPRISM_VERSION="${PROJECT_VERSION}")
target_link_libraries(diskprism_ui PUBLIC
  diskprism_gpu
  "-framework UniformTypeIdentifiers"
  "-framework QuickLookUI")

# ---------------------------------------------------------------------------------------
# App icon, drawn at build time by a tiny Core Graphics tool (no binary assets in the
# repository, no external utilities).
# ---------------------------------------------------------------------------------------
add_executable(dp_make_icon tools/make_icon.mm)
target_link_libraries(dp_make_icon PRIVATE diskprism_build_flags
  "-framework Foundation" "-framework CoreGraphics")

set(DISKPRISM_ICON ${CMAKE_CURRENT_BINARY_DIR}/AppIcon.icns)
add_custom_command(
  OUTPUT ${DISKPRISM_ICON}
  COMMAND dp_make_icon ${DISKPRISM_ICON}
  DEPENDS dp_make_icon
  COMMENT "Drawing AppIcon.icns"
  VERBATIM)

# ---------------------------------------------------------------------------------------
# DiskPrism.app
#   Contents/MacOS/DiskPrism
#   Contents/Info.plist
#   Contents/Resources/Shaders.metal   shader *source*; compiled at run time with
#                                      -[MTLDevice newLibraryWithSource:options:error:]
#   Contents/Resources/AppIcon.icns
# ---------------------------------------------------------------------------------------
add_executable(DiskPrism MACOSX_BUNDLE
  src/ui/main.mm
  ${DISKPRISM_SHADER_SOURCE}
  ${DISKPRISM_ICON})
set_source_files_properties(${DISKPRISM_SHADER_SOURCE} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties(${DISKPRISM_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources GENERATED TRUE)
target_link_libraries(DiskPrism PRIVATE diskprism_ui)
set_target_properties(DiskPrism PROPERTIES
  OUTPUT_NAME "DiskPrism"
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_NAME "DiskPrism"
  MACOSX_BUNDLE_GUI_IDENTIFIER "com.berkayceylan.diskprism"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  MACOSX_BUNDLE_COPYRIGHT "DiskPrism ${PROJECT_VERSION}")

# Ad-hoc signature. A separate stamped step (rather than POST_BUILD) so that the bundle
# is re-sealed whenever the binary *or* one of its resources changes.
set(DISKPRISM_SIGN_STAMP ${CMAKE_CURRENT_BINARY_DIR}/DiskPrism.signed.stamp)
add_custom_command(
  OUTPUT ${DISKPRISM_SIGN_STAMP}
  COMMAND codesign --force --sign - --timestamp=none $<TARGET_BUNDLE_DIR:DiskPrism>
  COMMAND ${CMAKE_COMMAND} -E touch ${DISKPRISM_SIGN_STAMP}
  DEPENDS DiskPrism ${DISKPRISM_SHADER_SOURCE} ${DISKPRISM_ICON} ${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist.in
  COMMENT "Ad-hoc signing DiskPrism.app"
  VERBATIM)
add_custom_target(DiskPrismSigned ALL DEPENDS ${DISKPRISM_SIGN_STAMP})

# ---------------------------------------------------------------------------------------
# Stand-alone headless benchmark (core only).
# ---------------------------------------------------------------------------------------
add_executable(diskprism-bench tools/bench_main.cpp)
target_link_libraries(diskprism-bench PRIVATE diskprism_core)

# ---------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------
if(DISKPRISM_BUILD_TESTS)
  enable_testing()

  # With sanitizers, any report must fail the test (UBSan is built non-recoverable above).
  set(test_environment "MallocNanoZone=0")
  if(ENABLE_SANITIZERS)
    # (strict_string_checks is deliberately left off: it flags a legal strndup() inside
    # CoreGraphics' PDF reader when AppKit loads SF Symbols, i.e. system code, not ours.)
    list(APPEND test_environment
      "ASAN_OPTIONS=detect_stack_use_after_return=1:check_initialization_order=1"
      "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
  endif()

  function(diskprism_add_test name)
    cmake_parse_arguments(arg "" "LIBRARY;TIMEOUT" "SOURCES" ${ARGN})
    add_executable(${name} ${arg_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(${name} PRIVATE ${arg_LIBRARY})
    target_compile_definitions(${name} PRIVATE DP_SHADER_SOURCE_PATH="${DISKPRISM_SHADER_SOURCE}")
    add_test(NAME ${name} COMMAND ${name})
    if(NOT arg_TIMEOUT)
      set(arg_TIMEOUT 300)
    endif()
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${test_environment}" TIMEOUT ${arg_TIMEOUT})
  endfunction()

  # Core (pure C++): arena + incremental deletion, getattrlistbulk scanner + APFS edge
  # cases, treemap + sunburst geometry.
  diskprism_add_test(test_arena LIBRARY diskprism_core SOURCES tests/test_arena.cpp)
  diskprism_add_test(test_scanner LIBRARY diskprism_core SOURCES tests/test_scanner.cpp)
  diskprism_add_test(test_layout LIBRARY diskprism_core SOURCES tests/test_layout.cpp)
  # Metal: run-time shader compilation, struct layout vs. reflection, off-screen frames.
  diskprism_add_test(test_metal LIBRARY diskprism_gpu SOURCES tests/test_metal.mm)
  # AppKit: the real window controller and panes, driven without a visible window.
  diskprism_add_test(test_ui LIBRARY diskprism_ui SOURCES tests/test_ui.mm)

  # The app binary and the bundle around it.
  set(bundle $<TARGET_BUNDLE_DIR:DiskPrism>)
  set(app_binary $<TARGET_FILE:DiskPrism>)
  add_test(NAME app_version COMMAND ${app_binary} --version)
  set_tests_properties(app_version PROPERTIES PASS_REGULAR_EXPRESSION "DiskPrism ${PROJECT_VERSION}")
  add_test(NAME app_benchmark COMMAND ${app_binary} --benchmark ${CMAKE_CURRENT_SOURCE_DIR}/src --runs 2)
  set_tests_properties(app_benchmark PROPERTIES PASS_REGULAR_EXPRESSION "BENCHMARK files=[1-9]")
  add_test(NAME tool_benchmark COMMAND diskprism-bench ${CMAKE_CURRENT_SOURCE_DIR}/src --runs 1)
  set_tests_properties(tool_benchmark PROPERTIES PASS_REGULAR_EXPRESSION "BENCHMARK files=[1-9]")
  # These two load Shaders.metal from Contents/Resources, exactly as the running app does.
  add_test(NAME app_snapshot_treemap
    COMMAND ${app_binary} --snapshot ${CMAKE_CURRENT_SOURCE_DIR} --out ${CMAKE_CURRENT_BINARY_DIR}/snapshot-treemap.png
            --mode treemap --size 1200x750)
  add_test(NAME app_snapshot_sunburst
    COMMAND ${app_binary} --snapshot ${CMAKE_CURRENT_SOURCE_DIR} --out ${CMAKE_CURRENT_BINARY_DIR}/snapshot-sunburst.png
            --mode sunburst --size 1200x750 --appearance light)
  set_tests_properties(app_snapshot_treemap app_snapshot_sunburst PROPERTIES
    PASS_REGULAR_EXPRESSION "instances in one draw call"
    SKIP_REGULAR_EXPRESSION "Metal is not available")
  set_tests_properties(app_version app_benchmark tool_benchmark app_snapshot_treemap app_snapshot_sunburst PROPERTIES
    ENVIRONMENT "${test_environment}" TIMEOUT 300)

  add_test(NAME bundle_structure
    COMMAND ${CMAKE_COMMAND} -DBUNDLE=${bundle} -DCHECK=structure -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBundle.cmake)
  add_test(NAME bundle_plist
    COMMAND ${CMAKE_COMMAND} -DBUNDLE=${bundle} -DCHECK=plist -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBundle.cmake)
  add_test(NAME bundle_codesign COMMAND codesign --verify --deep --strict --verbose=2 ${bundle})

  if(NOT ENABLE_SANITIZERS)
    # (A sanitizer build legitimately links the ASan runtime, so this is checked on the
    # uninstrumented build.)
    add_test(NAME bundle_linkage
      COMMAND ${CMAKE_COMMAND} -DBUNDLE=${bundle} -DCHECK=linkage -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckBundle.cmake)

    # Apple's AddressSanitizer has no LeakSanitizer, so leaks are checked with the
    # system's `leaks` tool on the uninstrumented test binaries (exit status 1 = leak).
    find_program(LEAKS_EXECUTABLE leaks)
    if(LEAKS_EXECUTABLE)
      foreach(name test_arena test_scanner test_layout)
        add_test(NAME leaks_${name} COMMAND ${LEAKS_EXECUTABLE} --atExit -- $<TARGET_FILE:${name}>)
        set_tests_properties(leaks_${name} PROPERTIES
          ENVIRONMENT "MallocStackLogging=1" TIMEOUT 600
          PASS_REGULAR_EXPRESSION " 0 leaks for 0 total leaked bytes")
      endforeach()
    endif()
  endif()
endif()
