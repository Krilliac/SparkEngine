cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED SPARK_SOURCE_DIR OR SPARK_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "SPARK_SOURCE_DIR is required")
endif()
if(NOT DEFINED SPARK_TEST_OUTPUT_DIR OR SPARK_TEST_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "SPARK_TEST_OUTPUT_DIR is required")
endif()

include("${SPARK_SOURCE_DIR}/cmake/SparkWindowsVersionInfo.cmake")

unset(SPARK_ENGINE_VERSION)
spark_resolve_engine_version(_spark_default_version)
if(NOT _spark_default_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "Root version did not resolve: '${_spark_default_version}'")
endif()

set(SPARK_ENGINE_VERSION "7.8.9")
set(_spark_targets
    SparkEngine
    SparkConsole
    SparkEditor
    SparkLauncher
    SparkServer
    SparkGateway
    SparkDaemon
    SparkCollabServer
    SparkOrchestrator
    SparkCooker
    SparkWorker
    SparkAutomation
    SparkBuild
    SparkInstaller
    SparkShaderCompiler
    SparkCrashReporter)

foreach(_spark_target IN LISTS _spark_targets)
    set(_spark_expected_description "${_spark_target} - SparkEngine executable")
    set(_spark_expected_product "SparkEngine")
    if(_spark_target STREQUAL "SparkBuild")
        set(_spark_expected_description "SparkBuild - Cross-Platform Build Tool")
        set(_spark_expected_product "SparkBuild")
    elseif(_spark_target STREQUAL "SparkInstaller")
        set(_spark_expected_description
            "SparkInstaller - SparkEngine Bootstrap Installer/Updater")
        set(_spark_expected_product "SparkInstaller")
    endif()
    set(_spark_resource "${SPARK_TEST_OUTPUT_DIR}/${_spark_target}.rc")
    spark_generate_windows_version_resource("${_spark_target}" "${_spark_resource}")
    file(READ "${_spark_resource}" _spark_resource_contents)
    foreach(_spark_expected IN ITEMS
            "FILEVERSION 7,8,9,0"
            "PRODUCTVERSION 7,8,9,0"
            "VALUE \"CompanyName\", \"SparkEngine\""
            "VALUE \"FileDescription\", \"${_spark_expected_description}\""
            "VALUE \"FileVersion\", \"7.8.9.0\""
            "VALUE \"InternalName\", \"${_spark_target}\""
            "VALUE \"OriginalFilename\", \"${_spark_target}.exe\""
            "VALUE \"ProductName\", \"${_spark_expected_product}\""
            "VALUE \"ProductVersion\", \"7.8.9.0\"")
        string(FIND "${_spark_resource_contents}" "${_spark_expected}" _spark_match)
        if(_spark_match EQUAL -1)
            message(FATAL_ERROR
                "${_spark_resource} is missing '${_spark_expected}'")
        endif()
    endforeach()
endforeach()

list(LENGTH _spark_targets _spark_target_count)
if(NOT _spark_target_count EQUAL 16)
    message(FATAL_ERROR "Expected 16 shipped executable resources, got ${_spark_target_count}")
endif()

message(STATUS
    "Validated ${_spark_target_count} Windows VERSIONINFO resources "
    "with override ${SPARK_ENGINE_VERSION} (root default ${_spark_default_version})")

# Configure a compiler-free imported DLL target to exercise the actual target
# type branch. A text search for VFT_DLL cannot prove the generated resource.
set(_spark_dll_fixture "${SPARK_TEST_OUTPUT_DIR}/dll-fixture")
file(MAKE_DIRECTORY "${_spark_dll_fixture}")
file(WRITE "${_spark_dll_fixture}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.16)
project(SparkVersionResourceFixture NONE)
include(\"${SPARK_SOURCE_DIR}/cmake/SparkWindowsVersionInfo.cmake\")
set(SPARK_ENGINE_VERSION 7.8.9)
add_library(SparkGameVersionFixture SHARED IMPORTED)
spark_generate_windows_version_resource(SparkGameVersionFixture \"${_spark_dll_fixture}/module.rc\")
")
execute_process(COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${_spark_dll_fixture}" -B "${_spark_dll_fixture}/build"
    RESULT_VARIABLE _spark_dll_result OUTPUT_VARIABLE _spark_dll_stdout ERROR_VARIABLE _spark_dll_stderr)
if(NOT _spark_dll_result EQUAL 0)
    message(FATAL_ERROR "DLL resource configure failed: ${_spark_dll_stdout}\n${_spark_dll_stderr}")
endif()
file(READ "${_spark_dll_fixture}/module.rc" _spark_dll_resource)
foreach(_spark_expected IN ITEMS
        "FILEVERSION 7,8,9,0" "PRODUCTVERSION 7,8,9,0" "FILETYPE VFT_DLL"
        "VALUE \"OriginalFilename\", \"SparkGameVersionFixture.dll\""
        "VALUE \"FileVersion\", \"7.8.9.0\"" "VALUE \"ProductVersion\", \"7.8.9.0\"")
    string(FIND "${_spark_dll_resource}" "${_spark_expected}" _spark_match)
    if(_spark_match EQUAL -1)
        message(FATAL_ERROR "DLL resource is missing '${_spark_expected}'")
    endif()
endforeach()
message(STATUS "Validated first-party DLL VERSIONINFO resource")

# Exercise the production attachment helper, not only its resource generator.
# Only selected discovered modules are shipped, and a repeated helper call must
# not attach a second resource to the same target.
set(_spark_attachment_fixture "${SPARK_TEST_OUTPUT_DIR}/attachment-fixture")
file(MAKE_DIRECTORY "${_spark_attachment_fixture}")
file(WRITE "${_spark_attachment_fixture}/module.cpp" "int fixture() { return 0; }\n")
file(WRITE "${_spark_attachment_fixture}/CMakeLists.txt" "
cmake_minimum_required(VERSION 3.25)
project(SparkVersionAttachmentFixture LANGUAGES CXX)
include(\"${SPARK_SOURCE_DIR}/cmake/SparkWindowsVersionInfo.cmake\")
set(WIN32 TRUE)
set(SPARK_ENGINE_VERSION 7.8.9)
add_library(SparkGameVersionFixture SHARED module.cpp)
add_library(SparkGameUnselectedFixture SHARED module.cpp)
set(_SPARK_GAME_MODULE_NAMES SparkGameVersionFixture)
spark_attach_shipped_windows_version_info()
spark_attach_shipped_windows_version_info()
get_target_property(_attached SparkGameVersionFixture SPARK_WINDOWS_VERSIONINFO_ATTACHED)
get_target_property(_unselected SparkGameUnselectedFixture SPARK_WINDOWS_VERSIONINFO_ATTACHED)
if(NOT _attached OR _unselected)
    message(FATAL_ERROR \"Selected module version attachment is incorrect\")
endif()
get_target_property(_sources SparkGameVersionFixture SOURCES)
list(FILTER _sources INCLUDE REGEX \"VersionInfo[.]rc$\")
list(LENGTH _sources _count)
if(NOT _count EQUAL 1)
    message(FATAL_ERROR \"Expected exactly one attached module resource\")
endif()
list(GET _sources 0 _resource)
configure_file(\"\${_resource}\" \"${_spark_attachment_fixture}/attached-module.rc\" COPYONLY)
")
execute_process(COMMAND "${CMAKE_COMMAND}" -G Ninja
    -S "${_spark_attachment_fixture}" -B "${_spark_attachment_fixture}/build"
    RESULT_VARIABLE _spark_attachment_result
    OUTPUT_VARIABLE _spark_attachment_stdout ERROR_VARIABLE _spark_attachment_stderr)
if(NOT _spark_attachment_result EQUAL 0)
    message(FATAL_ERROR
        "Module attachment configure failed: ${_spark_attachment_stdout}\n${_spark_attachment_stderr}")
endif()
file(READ "${_spark_attachment_fixture}/attached-module.rc" _spark_attached_resource)
if(NOT _spark_attached_resource STREQUAL _spark_dll_resource)
    message(FATAL_ERROR "Attached module resource differs from the validated DLL resource")
endif()
message(STATUS "Validated selected module VERSIONINFO attachment and idempotence")
