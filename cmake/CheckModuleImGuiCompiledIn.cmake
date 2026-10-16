#[=============================================================================[
  CheckModuleImGuiCompiledIn.cmake - prove a game-module image carries its
  in-game ImGui UI.

  Usage (CTest, see Tests/CMakeLists.txt):
    cmake -DMODULE_IMAGE=<path to module DLL> -DMARKER=<UI string literal>
          [-DEXPECT_ABSENT=1] -P cmake/CheckModuleImGuiCompiledIn.cmake

  MARKER is a string literal that only the module's SPARK_HAS_IMGUI-guarded
  draw code references (for example a CollapsingHeader label). When the guard
  is compiled out, the literal is dead and absent from the image, so this
  check fails instead of letting the UI silently disappear from a build that
  links ImGui.
#]=============================================================================]

if(NOT MODULE_IMAGE OR NOT MARKER)
    message(FATAL_ERROR "CheckModuleImGuiCompiledIn: MODULE_IMAGE and MARKER are required")
endif()
if(NOT EXISTS "${MODULE_IMAGE}")
    message(FATAL_ERROR "CheckModuleImGuiCompiledIn: module image not found: ${MODULE_IMAGE}")
endif()

# file(STRINGS) splits the binary on non-printable bytes, so each NUL-terminated
# literal is one entry; match it exactly so a longer log string cannot pass.
string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _marker_regex "${MARKER}")
file(STRINGS "${MODULE_IMAGE}" _hits REGEX "^${_marker_regex}$")
list(LENGTH _hits _hit_count)
# EXPECT_ABSENT inverts the check for UI that a build profile deliberately
# compiles out (the FPS developer panel in plain Shipping).
if(EXPECT_ABSENT)
    if(NOT _hit_count EQUAL 0)
        message(FATAL_ERROR
            "CheckModuleImGuiCompiledIn: '${MARKER}' is present in ${MODULE_IMAGE}, "
            "but this build profile must compile that UI out")
    endif()
    message(STATUS "CheckModuleImGuiCompiledIn: '${MARKER}' correctly absent from ${MODULE_IMAGE}")
    return()
endif()
if(_hit_count EQUAL 0)
    message(FATAL_ERROR
        "CheckModuleImGuiCompiledIn: '${MARKER}' is missing from ${MODULE_IMAGE}; "
        "the module's in-game ImGui UI was compiled out (is SPARK_HAS_IMGUI defined for the module?)")
endif()
message(STATUS "CheckModuleImGuiCompiledIn: found '${MARKER}' in ${MODULE_IMAGE}")
