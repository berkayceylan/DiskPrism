# Verifies the built DiskPrism.app (run by CTest in script mode):
#
#   cmake -DBUNDLE=<path/DiskPrism.app> -DCHECK=<structure|linkage|plist> -P CheckBundle.cmake
#
# structure  the bundle has the expected layout and its resources are in place
# linkage    the executable links against system libraries and frameworks only
# plist      Info.plist is well-formed and carries the keys the app relies on

if(NOT DEFINED BUNDLE OR NOT DEFINED CHECK)
  message(FATAL_ERROR "usage: cmake -DBUNDLE=<app> -DCHECK=<structure|linkage|plist> -P CheckBundle.cmake")
endif()

set(executable "${BUNDLE}/Contents/MacOS/DiskPrism")

if(CHECK STREQUAL "structure")
  foreach(entry
      "Contents/MacOS/DiskPrism"
      "Contents/Info.plist"
      "Contents/Resources/Shaders.metal"
      "Contents/Resources/AppIcon.icns"
      "Contents/_CodeSignature/CodeResources")
    if(NOT EXISTS "${BUNDLE}/${entry}")
      message(FATAL_ERROR "missing from the bundle: ${entry}")
    endif()
    message(STATUS "found ${entry}")
  endforeach()
  # The shader ships as source text and is compiled at run time: no precompiled library.
  file(GLOB metallibs "${BUNDLE}/Contents/Resources/*.metallib")
  if(metallibs)
    message(FATAL_ERROR "unexpected precompiled Metal library in the bundle: ${metallibs}")
  endif()
  file(READ "${BUNDLE}/Contents/Resources/Shaders.metal" shader)
  foreach(function treemap_vertex treemap_fragment sunburst_vertex sunburst_fragment)
    string(FIND "${shader}" "${function}(" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "bundled Shaders.metal does not define ${function}")
    endif()
  endforeach()
  file(SIZE "${executable}" size)
  math(EXPR kilobytes "${size} / 1024")
  message(STATUS "executable size: ${size} bytes (${kilobytes} KiB)")

elseif(CHECK STREQUAL "linkage")
  execute_process(COMMAND otool -L "${executable}" OUTPUT_VARIABLE output RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "otool -L failed for ${executable}")
  endif()
  message(STATUS "${output}")
  string(REPLACE "\n" ";" lines "${output}")
  set(count 0)
  foreach(line IN LISTS lines)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES ":$")
      continue()  # the first line names the binary itself
    endif()
    if(NOT line MATCHES "^(/System/Library/|/usr/lib/)")
      message(FATAL_ERROR "links against something that is not part of macOS: ${line}")
    endif()
    math(EXPR count "${count} + 1")
  endforeach()
  if(count LESS 5)
    message(FATAL_ERROR "suspiciously few dependencies (${count}); is this the right binary?")
  endif()
  message(STATUS "${count} dependencies, all under /System/Library or /usr/lib")

elseif(CHECK STREQUAL "plist")
  execute_process(COMMAND plutil -lint "${BUNDLE}/Contents/Info.plist" OUTPUT_VARIABLE output RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Info.plist is not a valid property list: ${output}")
  endif()
  foreach(key CFBundleIdentifier CFBundleExecutable CFBundleIconFile NSHighResolutionCapable LSMinimumSystemVersion NSPrincipalClass)
    execute_process(COMMAND plutil -extract "${key}" raw "${BUNDLE}/Contents/Info.plist"
                    OUTPUT_VARIABLE value RESULT_VARIABLE result OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Info.plist has no ${key}")
    endif()
    message(STATUS "${key} = ${value}")
  endforeach()

else()
  message(FATAL_ERROR "unknown CHECK '${CHECK}'")
endif()
