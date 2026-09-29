cmake_minimum_required(VERSION 3.25)

# GOV-400 LicenseInventory_InstallTreeNoticeCoverage: the notice-coverage gate
# against this build's real install tree, not a fixture.
#
# Installs the components CPack packages (CPACK_COMPONENTS_ALL, read from this
# build's CPackConfig.cmake; the private "symbols" component is not among them)
# into a fresh prefix, then runs both gate implementations in closed-world mode:
# cmake/ValidateStagedPackageNotices.cmake and
# tools/governance/generate_third_party_notices.py --check-package --closed-world.
# Both must pass, agree on the font and third-party payload counts, and find a
# non-zero number of fonts and payload files. Probe files then prove the pass is
# not vacuous: an unmapped file under a third-party root and an unclassified
# file must each make both implementations fail and name the probe.
#
# This covers the install tree; the CPack archive and native installers are not
# run through the gate here.

foreach(_spark_required IN ITEMS SPARK_BINARY_DIR SPARK_SOURCE_DIR SPARK_CONFIG SPARK_PYTHON SPARK_INSTALL_ROOT)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()

# The prefix is deleted first, so refuse anything but a named directory inside
# the build tree.
set(_spark_root "${SPARK_INSTALL_ROOT}")
set(_spark_binary_root "${SPARK_BINARY_DIR}")
cmake_path(ABSOLUTE_PATH _spark_root NORMALIZE)
cmake_path(ABSOLUTE_PATH _spark_binary_root NORMALIZE)
cmake_path(IS_PREFIX _spark_binary_root "${_spark_root}" NORMALIZE _spark_root_is_bounded)
cmake_path(GET _spark_root FILENAME _spark_root_name)
if(NOT _spark_root_is_bounded OR _spark_root STREQUAL _spark_binary_root OR
   NOT _spark_root_name STREQUAL "notice-install-tree")
    message(FATAL_ERROR "Refusing to use install prefix '${_spark_root}': it must be <build tree>/.../notice-install-tree")
endif()
file(REMOVE_RECURSE "${_spark_root}")

# The package's component list, exactly as CPack will package it.
file(STRINGS "${SPARK_BINARY_DIR}/CPackConfig.cmake" _spark_components_line
    REGEX "^set[(]CPACK_COMPONENTS_ALL \"[^\"]*\"[)]$")
if(NOT _spark_components_line MATCHES "^set[(]CPACK_COMPONENTS_ALL \"([^\"]+)\"[)]$")
    message(FATAL_ERROR "No CPACK_COMPONENTS_ALL in ${SPARK_BINARY_DIR}/CPackConfig.cmake")
endif()
set(SPARK_COMPONENTS "${CMAKE_MATCH_1}")
foreach(_spark_component IN LISTS SPARK_COMPONENTS)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${SPARK_BINARY_DIR}" --config "${SPARK_CONFIG}"
            --prefix "${_spark_root}" --component "${_spark_component}"
        RESULT_VARIABLE _spark_result
        OUTPUT_QUIET
        ERROR_VARIABLE _spark_stderr)
    if(NOT _spark_result EQUAL 0)
        message(FATAL_ERROR "cmake --install --component ${_spark_component} failed (${_spark_result}):\n${_spark_stderr}")
    endif()
endforeach()
file(GLOB_RECURSE _spark_installed LIST_DIRECTORIES false "${_spark_root}/*")
list(LENGTH _spark_installed _spark_installed_count)
message(STATUS "Installed ${_spark_installed_count} file(s) from components: ${SPARK_COMPONENTS}")

set(_spark_gate "${SPARK_SOURCE_DIR}/cmake/ValidateStagedPackageNotices.cmake")
set(_spark_tool "${SPARK_SOURCE_DIR}/tools/governance/generate_third_party_notices.py")

# Runs both implementations; sets <prefix>_cmake_result/_cmake_log and
# <prefix>_python_result/_python_log in the caller.
function(_spark_run_both _spark_prefix)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSPARK_PACKAGE_ROOT=${_spark_root}"
            -DSPARK_PACKAGE_NOTICE_COVERAGE=enforce -DSPARK_PACKAGE_NOTICE_CLASSIFICATION=closed
            -P "${_spark_gate}"
        RESULT_VARIABLE _spark_cmake_result
        OUTPUT_VARIABLE _spark_cmake_stdout
        ERROR_VARIABLE _spark_cmake_stderr)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env PYTHONUTF8=1
            "${SPARK_PYTHON}" -B "${_spark_tool}" --check-package "${_spark_root}" --closed-world
        RESULT_VARIABLE _spark_python_result
        OUTPUT_VARIABLE _spark_python_stdout
        ERROR_VARIABLE _spark_python_stderr)
    string(REGEX REPLACE "[ \t\r\n]+" " " _spark_cmake_flat "${_spark_cmake_stdout} ${_spark_cmake_stderr}")
    string(REGEX REPLACE "[ \t\r\n]+" " " _spark_python_flat "${_spark_python_stdout} ${_spark_python_stderr}")
    set(${_spark_prefix}_cmake_result "${_spark_cmake_result}" PARENT_SCOPE)
    set(${_spark_prefix}_cmake_log "${_spark_cmake_flat}" PARENT_SCOPE)
    set(${_spark_prefix}_python_result "${_spark_python_result}" PARENT_SCOPE)
    set(${_spark_prefix}_python_log "${_spark_python_flat}" PARENT_SCOPE)
endfunction()

_spark_run_both(_spark_clean)
if(NOT _spark_clean_cmake_result EQUAL 0 OR NOT _spark_clean_python_result EQUAL 0)
    message(FATAL_ERROR
        "The install tree is not fully covered (cmake=${_spark_clean_cmake_result}, "
        "python=${_spark_clean_python_result}).\nCMake gate: ${_spark_clean_cmake_log}\n"
        "Python gate: ${_spark_clean_python_log}")
endif()
if(NOT _spark_clean_cmake_log MATCHES
   "Validated notice coverage for ([0-9]+) font file\\(s\\) and ([0-9]+) third-party payload file\\(s\\)")
    message(FATAL_ERROR "CMake gate passed without a coverage summary: ${_spark_clean_cmake_log}")
endif()
set(_spark_fonts "${CMAKE_MATCH_1}")
set(_spark_payload "${CMAKE_MATCH_2}")
if(NOT _spark_clean_cmake_log MATCHES "Closed world: ([0-9]+) first-party file\\(s\\), ([0-9]+) asset-manifest file\\(s\\)")
    message(FATAL_ERROR "CMake gate passed without a closed-world summary: ${_spark_clean_cmake_log}")
endif()
set(_spark_first_party "${CMAKE_MATCH_1}")
set(_spark_assets "${CMAKE_MATCH_2}")
string(CONCAT _spark_expected_python
    "notice coverage ok: ${_spark_fonts} font file(s) and ${_spark_payload} third-party payload file(s)")
string(FIND "${_spark_clean_python_log}" "${_spark_expected_python}" _spark_at)
string(FIND "${_spark_clean_python_log}"
    "closed world: ${_spark_first_party} first-party file(s), ${_spark_assets} asset-manifest file(s)" _spark_world_at)
if(_spark_at EQUAL -1 OR _spark_world_at EQUAL -1)
    message(FATAL_ERROR
        "The implementations disagree on the counts.\nCMake: ${_spark_clean_cmake_log}\nPython: ${_spark_clean_python_log}")
endif()
if(_spark_fonts EQUAL 0 OR _spark_payload EQUAL 0 OR _spark_first_party EQUAL 0)
    message(FATAL_ERROR
        "Vacuous install tree: ${_spark_fonts} font(s), ${_spark_payload} payload file(s), "
        "${_spark_first_party} first-party file(s)")
endif()
math(EXPR _spark_classified "${_spark_fonts} + ${_spark_payload} + ${_spark_first_party} + ${_spark_assets}")
message(STATUS
    "Install tree covered by both gates: ${_spark_fonts} font(s), ${_spark_payload} third-party payload file(s), "
    "${_spark_first_party} first-party file(s), ${_spark_assets} asset-manifest file(s) "
    "(${_spark_classified} of ${_spark_installed_count} installed)")
if(NOT _spark_classified EQUAL _spark_installed_count)
    message(FATAL_ERROR "The gates classified ${_spark_classified} of ${_spark_installed_count} installed files")
endif()

# Non-vacuous: each probe must fail both implementations by name.
set(_spark_probe_third_party "include/SparkEngine/ThirdParty/notice-probe/probe.h")
set(_spark_probe_unclassified "notice-probe-unclassified.bin")
file(WRITE "${_spark_root}/${_spark_probe_third_party}" "#pragma once\n")
file(WRITE "${_spark_root}/${_spark_probe_unclassified}" "probe\n")
_spark_run_both(_spark_probe)
foreach(_spark_implementation IN ITEMS cmake python)
    if(_spark_probe_${_spark_implementation}_result EQUAL 0)
        message(FATAL_ERROR "The ${_spark_implementation} gate passed with probe files present")
    endif()
    foreach(_spark_expected IN ITEMS
            "${_spark_probe_third_party}: third-party install path that no payload rule maps"
            "${_spark_probe_unclassified}: unclassified")
        string(FIND "${_spark_probe_${_spark_implementation}_log}" "${_spark_expected}" _spark_at)
        if(_spark_at EQUAL -1)
            message(FATAL_ERROR
                "The ${_spark_implementation} gate did not name '${_spark_expected}': "
                "${_spark_probe_${_spark_implementation}_log}")
        endif()
    endforeach()
endforeach()
file(REMOVE_RECURSE "${_spark_root}")
message(STATUS "LicenseInventory_InstallTreeNoticeCoverage passed; both probes rejected by both gates")
