cmake_minimum_required(VERSION 3.25)

foreach(_required IN ITEMS SPARK_ASSETS_ROOT SPARK_ASSET_VERIFIER)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for installed FPS asset validation")
    endif()
    if("${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} contains unsupported control or list characters")
    endif()
    if(NOT IS_ABSOLUTE "${${_required}}")
        message(FATAL_ERROR "${_required} must be an absolute path")
    endif()
endforeach()

cmake_path(SET _assets_root NORMALIZE "${SPARK_ASSETS_ROOT}")
cmake_path(GET _assets_root ROOT_PATH _assets_volume_root)
if(_assets_root STREQUAL _assets_volume_root)
    message(FATAL_ERROR "SPARK_ASSETS_ROOT must not be a volume root")
endif()
if(NOT IS_DIRECTORY "${_assets_root}" OR IS_SYMLINK "${_assets_root}")
    message(FATAL_ERROR "SPARK_ASSETS_ROOT must be a regular non-link directory: ${_assets_root}")
endif()

# The Python verifier is authoritative for ancestry, junction/reparse, path,
# completeness, size, and SHA-256 checks. Reject a lexical root that resolves
# elsewhere before handing it that caller-authorized package boundary.
file(REAL_PATH "${_assets_root}" _assets_root_real)
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_assets_root}" _assets_root_compare)
    string(TOLOWER "${_assets_root_real}" _assets_root_real_compare)
else()
    set(_assets_root_compare "${_assets_root}")
    set(_assets_root_real_compare "${_assets_root_real}")
endif()
if(NOT _assets_root_compare STREQUAL _assets_root_real_compare)
    message(FATAL_ERROR
        "SPARK_ASSETS_ROOT must not cross a symlink, junction, or reparse boundary: ${_assets_root}")
endif()

cmake_path(SET _asset_verifier NORMALIZE "${SPARK_ASSET_VERIFIER}")
if(NOT EXISTS "${_asset_verifier}" OR
   IS_DIRECTORY "${_asset_verifier}" OR
   IS_SYMLINK "${_asset_verifier}")
    message(FATAL_ERROR
        "SPARK_ASSET_VERIFIER must be a regular non-link file: ${_asset_verifier}")
endif()

set(_asset_manifest "${_assets_root}/assets.integrity.json")
if(NOT EXISTS "${_asset_manifest}" OR
   IS_DIRECTORY "${_asset_manifest}" OR
   IS_SYMLINK "${_asset_manifest}")
    message(FATAL_ERROR
        "Installed FPS asset integrity manifest is missing or link-like: ${_asset_manifest}")
endif()

find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${_asset_verifier}"
        verify "${_asset_manifest}" --root "${_assets_root}"
    RESULT_VARIABLE _asset_result
    OUTPUT_VARIABLE _asset_output
    ERROR_VARIABLE _asset_error
    TIMEOUT 120
    ENCODING UTF-8)
if(NOT _asset_result EQUAL 0)
    message(FATAL_ERROR
        "Installed FPS asset integrity validation failed (exit ${_asset_result}):\n"
        "${_asset_output}${_asset_error}")
endif()

string(STRIP "${_asset_output}" _asset_summary)
message(STATUS "Installed FPS asset integrity passed: ${_asset_summary}")
