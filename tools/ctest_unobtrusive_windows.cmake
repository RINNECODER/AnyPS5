# Read by ctest (TEST_INCLUDE_FILES) before any test starts; tests inherit ctest's environment.
# Test windows stay on screen but never take focus or cover the user's work; see
# core/libs/prx/libSceVideoOut/include/UnobtrusiveWindows.hpp. Export ANYPS5_UNOBTRUSIVE_WINDOWS=0
# before running ctest to get the default AppKit behaviour back. Real game runs never read this file.
if(NOT DEFINED ENV{ANYPS5_UNOBTRUSIVE_WINDOWS})
    set(ENV{ANYPS5_UNOBTRUSIVE_WINDOWS} 1)
endif()
