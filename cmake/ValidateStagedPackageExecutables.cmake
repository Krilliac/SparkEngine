cmake_minimum_required(VERSION 3.25)

# Validate the executable bill of materials for SparkEngine's default release
# configuration. Run after `cmake --install` and before CPack uploads artifacts.

if(NOT DEFINED SPARK_PACKAGE_ROOT OR SPARK_PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT must name the staged install root")
endif()
if(SPARK_PACKAGE_ROOT MATCHES "[\r\n;]")
    message(FATAL_ERROR
        "SPARK_PACKAGE_ROOT contains unsupported control or list characters")
endif()

set(_spark_package_root_input "${SPARK_PACKAGE_ROOT}")
cmake_path(ABSOLUTE_PATH _spark_package_root_input
    BASE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    NORMALIZE
    OUTPUT_VARIABLE _spark_package_root_normalized)
if(NOT IS_DIRECTORY "${_spark_package_root_normalized}")
    message(FATAL_ERROR
        "SPARK_PACKAGE_ROOT is not an existing directory: ${SPARK_PACKAGE_ROOT}")
endif()

# CMake versions available on supported Windows runners do not all resolve
# directory junctions through file(REAL_PATH). Probe directory ancestry through
# the native file attributes so older CMake releases cannot silently follow a
# reparse point into package content outside the lexical root.
if(CMAKE_HOST_WIN32)
    set(_spark_windows_powershell
        "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
    cmake_path(NORMAL_PATH _spark_windows_powershell)
    if(NOT EXISTS "${_spark_windows_powershell}"
       OR IS_DIRECTORY "${_spark_windows_powershell}"
       OR IS_SYMLINK "${_spark_windows_powershell}")
        message(FATAL_ERROR
            "Could not locate the trusted Windows reparse-point probe")
    endif()
    set(_spark_windows_reparse_probe [=[
$ErrorActionPreference = 'Stop'
$stop = [IO.Path]::GetFullPath($env:SPARK_REPARSE_PROBE_STOP)
foreach ($path in ($env:SPARK_REPARSE_PROBE_PATHS -split ';')) {
    $current = [IO.Path]::GetFullPath($path)
    while ($true) {
        $attributes = [IO.File]::GetAttributes($current)
        if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            [Console]::Out.Write($current)
            exit 0
        }
        if ([StringComparer]::OrdinalIgnoreCase.Equals($current, $stop)) {
            break
        }
        $parent = [IO.Directory]::GetParent($current)
        if ($null -eq $parent -or
            [StringComparer]::OrdinalIgnoreCase.Equals($parent.FullName, $current)) {
            throw "Reparse probe escaped its bounded stop path"
        }
        $current = $parent.FullName
    }
}
]=])

    function(_spark_require_no_windows_reparse_traversal
             _spark_description _spark_stop)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env
                "SPARK_REPARSE_PROBE_STOP=${_spark_stop}"
                "SPARK_REPARSE_PROBE_PATHS=${ARGN}"
                "${_spark_windows_powershell}"
                -NoLogo -NoProfile -NonInteractive
                -Command "${_spark_windows_reparse_probe}"
            RESULT_VARIABLE _spark_reparse_probe_result
            OUTPUT_VARIABLE _spark_reparse_probe_output
            ERROR_VARIABLE _spark_reparse_probe_error
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT _spark_reparse_probe_result EQUAL 0)
            message(FATAL_ERROR
                "Could not inspect ${_spark_description} for Windows reparse traversal:\n"
                "${_spark_reparse_probe_error}")
        endif()
        if(NOT _spark_reparse_probe_output STREQUAL "")
            message(FATAL_ERROR
                "${_spark_description} crosses a symlink, junction, or reparse boundary:\n"
                "  reparse path: ${_spark_reparse_probe_output}")
        endif()
    endfunction()

    cmake_path(GET _spark_package_root_normalized ROOT_PATH
        _spark_package_volume_root)
    set(_spark_initial_windows_reparse_paths
        "${_spark_package_root_normalized}")
    foreach(_spark_fixed_directory IN ITEMS
            bin include/Spark lib/cmake/SparkEngine)
        if(IS_DIRECTORY
           "${_spark_package_root_normalized}/${_spark_fixed_directory}")
            list(APPEND _spark_initial_windows_reparse_paths
                "${_spark_package_root_normalized}/${_spark_fixed_directory}")
        endif()
    endforeach()
    _spark_require_no_windows_reparse_traversal(
        "SPARK_PACKAGE_ROOT and fixed package directories"
        "${_spark_package_volume_root}"
        ${_spark_initial_windows_reparse_paths})
    set_property(GLOBAL PROPERTY
        SPARK_PACKAGE_VALIDATOR_CHECKED_WINDOWS_DIRECTORIES
        "${_spark_initial_windows_reparse_paths}")
endif()

file(REAL_PATH "${_spark_package_root_normalized}" _spark_package_root_real)
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_spark_package_root_normalized}" _spark_package_root_compare)
    string(TOLOWER "${_spark_package_root_real}" _spark_package_root_real_compare)
else()
    set(_spark_package_root_compare "${_spark_package_root_normalized}")
    set(_spark_package_root_real_compare "${_spark_package_root_real}")
endif()
if(NOT _spark_package_root_compare STREQUAL _spark_package_root_real_compare)
    message(FATAL_ERROR
        "SPARK_PACKAGE_ROOT must not be a symlink, junction, or reparse traversal: "
        "${SPARK_PACKAGE_ROOT}")
endif()
set(SPARK_PACKAGE_ROOT "${_spark_package_root_normalized}")

function(_spark_require_bounded_regular_file _spark_candidate _spark_description)
    if(NOT EXISTS "${_spark_candidate}"
       OR IS_DIRECTORY "${_spark_candidate}"
       OR IS_SYMLINK "${_spark_candidate}")
        message(FATAL_ERROR
            "${_spark_description} is missing or is not a regular non-link file:\n"
            "  ${_spark_candidate}")
    endif()

    if(CMAKE_HOST_WIN32)
        cmake_path(GET _spark_candidate PARENT_PATH _spark_candidate_parent)
        get_property(_spark_checked_windows_directories GLOBAL PROPERTY
            SPARK_PACKAGE_VALIDATOR_CHECKED_WINDOWS_DIRECTORIES)
        if(NOT _spark_candidate_parent IN_LIST _spark_checked_windows_directories)
            _spark_require_no_windows_reparse_traversal(
                "${_spark_description} directory ancestry"
                "${_spark_package_volume_root}"
                "${_spark_candidate_parent}")
            set_property(GLOBAL APPEND PROPERTY
                SPARK_PACKAGE_VALIDATOR_CHECKED_WINDOWS_DIRECTORIES
                "${_spark_candidate_parent}")
        endif()
    endif()

    set(_spark_candidate_normalized "${_spark_candidate}")
    cmake_path(NORMAL_PATH _spark_candidate_normalized)
    file(REAL_PATH "${_spark_candidate_normalized}" _spark_candidate_real)
    cmake_path(IS_PREFIX _spark_package_root_real "${_spark_candidate_real}"
        NORMALIZE _spark_candidate_is_bounded)
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${_spark_candidate_normalized}" _spark_candidate_compare)
        string(TOLOWER "${_spark_candidate_real}" _spark_candidate_real_compare)
    else()
        set(_spark_candidate_compare "${_spark_candidate_normalized}")
        set(_spark_candidate_real_compare "${_spark_candidate_real}")
    endif()
    if(NOT _spark_candidate_is_bounded
       OR NOT _spark_candidate_compare STREQUAL _spark_candidate_real_compare)
        message(FATAL_ERROR
            "${_spark_description} crosses a symlink, junction, or reparse boundary:\n"
            "  package path: ${_spark_candidate_normalized}\n"
            "  resolved path: ${_spark_candidate_real}")
    endif()
endfunction()

if(NOT DEFINED SPARK_EXECUTABLE_SUFFIX)
    if(CMAKE_HOST_WIN32)
        set(SPARK_EXECUTABLE_SUFFIX ".exe")
    else()
        set(SPARK_EXECUTABLE_SUFFIX "")
    endif()
endif()

set(_spark_validate_modules_only OFF)
if(SPARK_PACKAGE_VALIDATE_MODULES_ONLY)
    set(_spark_validate_modules_only ON)
endif()

if(NOT _spark_validate_modules_only)
    set(_spark_required_executables
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
        SparkCrashReporter
    )

    set(_spark_missing_executables "")
    foreach(_spark_executable IN LISTS _spark_required_executables)
        set(_spark_path
            "${SPARK_PACKAGE_ROOT}/bin/${_spark_executable}${SPARK_EXECUTABLE_SUFFIX}")
        if(NOT EXISTS "${_spark_path}" OR IS_DIRECTORY "${_spark_path}" OR IS_SYMLINK "${_spark_path}")
            list(APPEND _spark_missing_executables "${_spark_path}")
        else()
            _spark_require_bounded_regular_file(
                "${_spark_path}" "Required staged executable ${_spark_executable}")
        endif()
    endforeach()

    if(_spark_missing_executables)
        list(JOIN _spark_missing_executables "\n  " _spark_missing_report)
        message(FATAL_ERROR
            "Staged SparkEngine package is missing required executables:\n"
            "  ${_spark_missing_report}")
    endif()
endif()

# Every auto-discovered in-tree game module is a shipped runtime choice, not
# merely a build sample. Read the generated configure-time inventory as a
# size-capped data file. Never include() package content: this validator may be
# pointed at an extracted artifact, and include() would execute staged code.
set(_spark_game_module_manifest
    "${SPARK_PACKAGE_ROOT}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake")
if(NOT EXISTS "${_spark_game_module_manifest}" OR
   IS_DIRECTORY "${_spark_game_module_manifest}" OR
   IS_SYMLINK "${_spark_game_module_manifest}")
    message(FATAL_ERROR
        "Staged SparkEngine package is missing its generated game-module inventory:\n"
        "  ${_spark_game_module_manifest}")
endif()
_spark_require_bounded_regular_file(
    "${_spark_game_module_manifest}" "Generated game-module inventory")
file(SIZE "${_spark_game_module_manifest}" _spark_game_module_manifest_size)
if(_spark_game_module_manifest_size LESS 1 OR
   _spark_game_module_manifest_size GREATER 4096)
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory has invalid size "
        "(${_spark_game_module_manifest_size} bytes): ${_spark_game_module_manifest}")
endif()
file(READ "${_spark_game_module_manifest}" _spark_game_module_manifest_content)
string(REPLACE "\r\n" "\n" _spark_game_module_manifest_content
    "${_spark_game_module_manifest_content}")
string(FIND "${_spark_game_module_manifest_content}" "\r"
    _spark_game_module_manifest_carriage_return)
if(NOT _spark_game_module_manifest_carriage_return EQUAL -1)
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory has an invalid line ending: "
        "${_spark_game_module_manifest}")
endif()
if(NOT _spark_game_module_manifest_content MATCHES "\n$")
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory is missing its final newline: "
        "${_spark_game_module_manifest}")
endif()
if(_spark_game_module_manifest_content MATCHES "(^|\n)\n")
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory contains a blank line: "
        "${_spark_game_module_manifest}")
endif()
string(CONCAT _spark_game_module_manifest_schema_regex
    "^format=([0-9]+)\n"
    "target_system=([A-Za-z0-9_.+-]+)\n"
    "module_prefix=([A-Za-z0-9_-]*)\n"
    "module_suffix=(\\.[A-Za-z0-9_.+-]+)\n"
    "modules=([A-Za-z0-9_;]+)\n$")
if(NOT _spark_game_module_manifest_content MATCHES
   "${_spark_game_module_manifest_schema_regex}")
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory does not match the fixed data schema: "
        "${_spark_game_module_manifest}")
endif()
set(_spark_game_module_manifest_format "${CMAKE_MATCH_1}")
set(_spark_package_target_system "${CMAKE_MATCH_2}")
set(_spark_module_prefix "${CMAKE_MATCH_3}")
set(_spark_module_suffix "${CMAKE_MATCH_4}")
set(_spark_required_game_modules "${CMAKE_MATCH_5}")
if(NOT _spark_game_module_manifest_format EQUAL 1)
    message(FATAL_ERROR
        "Unsupported staged game-module inventory format "
        "'${_spark_game_module_manifest_format}' in ${_spark_game_module_manifest}")
endif()
if(_spark_required_game_modules MATCHES "(^|;)(;|$)")
    message(FATAL_ERROR
        "Staged SparkEngine game-module inventory contains an empty module name: "
        "${_spark_game_module_manifest}")
endif()
set(_spark_seen_game_modules "")
foreach(_spark_module IN LISTS _spark_required_game_modules)
    if(NOT _spark_module MATCHES "^SparkGame[A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "Invalid game-module name '${_spark_module}' in ${_spark_game_module_manifest}")
    endif()
    if(_spark_module IN_LIST _spark_seen_game_modules)
        message(FATAL_ERROR
            "Duplicate game-module name '${_spark_module}' in ${_spark_game_module_manifest}")
    endif()
    list(APPEND _spark_seen_game_modules "${_spark_module}")
endforeach()

set(_spark_sdk_version_header "${SPARK_PACKAGE_ROOT}/include/Spark/Version.h")
if(NOT EXISTS "${_spark_sdk_version_header}" OR
   IS_DIRECTORY "${_spark_sdk_version_header}" OR
   IS_SYMLINK "${_spark_sdk_version_header}")
    message(FATAL_ERROR
        "Staged SparkEngine package is missing its SDK ABI version header:\n"
        "  ${_spark_sdk_version_header}")
endif()
_spark_require_bounded_regular_file(
    "${_spark_sdk_version_header}" "SDK ABI version header")
file(STRINGS "${_spark_sdk_version_header}" _spark_sdk_version_lines
    REGEX "^#[ \t]*define[ \t]+SPARK_SDK_VERSION[ \t]+[0-9]+[ \t]*$")
list(LENGTH _spark_sdk_version_lines _spark_sdk_version_line_count)
if(NOT _spark_sdk_version_line_count EQUAL 1 OR
   NOT _spark_sdk_version_lines MATCHES
       "SPARK_SDK_VERSION[ \t]+([0-9]+)")
    message(FATAL_ERROR
        "Could not determine one unambiguous SDK ABI version from "
        "${_spark_sdk_version_header}")
endif()
set(_spark_expected_sdk_version "${CMAKE_MATCH_1}")

set(_spark_missing_game_modules "")
foreach(_spark_module IN LISTS _spark_required_game_modules)
    set(_spark_module_path
        "${SPARK_PACKAGE_ROOT}/bin/${_spark_module_prefix}${_spark_module}${_spark_module_suffix}")
    if(NOT EXISTS "${_spark_module_path}" OR IS_DIRECTORY "${_spark_module_path}")
        list(APPEND _spark_missing_game_modules "${_spark_module_path}")
    else()
        _spark_require_bounded_regular_file(
            "${_spark_module_path}" "Staged game module ${_spark_module}")
    endif()
    if(NOT EXISTS "${_spark_module_path}.sparkabi" OR
       IS_DIRECTORY "${_spark_module_path}.sparkabi" OR
       IS_SYMLINK "${_spark_module_path}.sparkabi")
        list(APPEND _spark_missing_game_modules "${_spark_module_path}.sparkabi")
    else()
        _spark_require_bounded_regular_file(
            "${_spark_module_path}.sparkabi"
            "Game-module ABI sidecar for ${_spark_module}")
    endif()
endforeach()
if(_spark_missing_game_modules)
    list(JOIN _spark_missing_game_modules "\n  " _spark_missing_game_module_report)
    message(FATAL_ERROR
        "Staged SparkEngine package is missing required game modules or ABI sidecars:\n"
        "  ${_spark_missing_game_module_report}")
endif()

# The module loader treats this file as a pre-load security boundary. Keep the
# package-time check just as strict: accept only the writer's fixed v1 schema,
# reject values outside the descriptor's uint32_t domain, and bind each
# sidecar to the exact binary that will ship.
function(_spark_validate_game_module_sidecar _spark_module_path _spark_module_name
         _spark_abi_signature_output)
    set(_spark_sidecar_path "${_spark_module_path}.sparkabi")
    _spark_require_bounded_regular_file(
        "${_spark_module_path}" "Staged game module ${_spark_module_name}")
    _spark_require_bounded_regular_file(
        "${_spark_sidecar_path}" "Game-module ABI sidecar for ${_spark_module_name}")
    file(SIZE "${_spark_sidecar_path}" _spark_sidecar_size)
    if(_spark_sidecar_size LESS 1 OR _spark_sidecar_size GREATER 4096)
        message(FATAL_ERROR
            "Game-module ABI sidecar has invalid size (${_spark_sidecar_size} bytes): "
            "${_spark_sidecar_path}")
    endif()

    file(READ "${_spark_sidecar_path}" _spark_sidecar_content)
    string(FIND "${_spark_sidecar_content}" ";" _spark_semicolon_position)
    if(NOT _spark_semicolon_position EQUAL -1)
        message(FATAL_ERROR
            "Malformed game-module ABI sidecar (semicolon is not valid schema data): "
            "${_spark_sidecar_path}")
    endif()
    string(REPLACE "\r\n" "\n" _spark_sidecar_content "${_spark_sidecar_content}")
    string(FIND "${_spark_sidecar_content}" "\r" _spark_carriage_return_position)
    if(NOT _spark_carriage_return_position EQUAL -1)
        message(FATAL_ERROR
            "Malformed game-module ABI sidecar (invalid line ending): ${_spark_sidecar_path}")
    endif()
    if(NOT _spark_sidecar_content MATCHES "\n$")
        message(FATAL_ERROR
            "Malformed game-module ABI sidecar (missing final newline): ${_spark_sidecar_path}")
    endif()
    if(_spark_sidecar_content MATCHES "(^|\n)\n")
        message(FATAL_ERROR
            "Malformed game-module ABI sidecar (blank line): ${_spark_sidecar_path}")
    endif()
    string(REGEX REPLACE "\n$" "" _spark_sidecar_content "${_spark_sidecar_content}")
    string(REPLACE "\n" ";" _spark_sidecar_lines "${_spark_sidecar_content}")

    set(_spark_expected_sidecar_fields
        format
        struct_size
        magic
        sdk_version
        runtime_abi_version
        compiler_family
        compiler_abi_version
        cxx_language_level
        runtime_library
        iterator_debug_level
        pointer_size
        binary_sha256
    )
    list(LENGTH _spark_sidecar_lines _spark_sidecar_line_count)
    list(LENGTH _spark_expected_sidecar_fields _spark_expected_sidecar_line_count)
    if(NOT _spark_sidecar_line_count EQUAL _spark_expected_sidecar_line_count)
        message(FATAL_ERROR
            "Game-module ABI sidecar must contain exactly ${_spark_expected_sidecar_line_count} "
            "fields, found ${_spark_sidecar_line_count}: ${_spark_sidecar_path}")
    endif()

    set(_spark_seen_sidecar_fields "")
    foreach(_spark_sidecar_line IN LISTS _spark_sidecar_lines)
        if(NOT _spark_sidecar_line MATCHES "^([a-z][a-z0-9_]*)=([A-Za-z0-9]+)$")
            message(FATAL_ERROR
                "Malformed game-module ABI sidecar line '${_spark_sidecar_line}' in "
                "${_spark_sidecar_path}")
        endif()
        set(_spark_sidecar_key "${CMAKE_MATCH_1}")
        set(_spark_sidecar_value "${CMAKE_MATCH_2}")
        if(NOT _spark_sidecar_key IN_LIST _spark_expected_sidecar_fields)
            message(FATAL_ERROR
                "Unexpected game-module ABI sidecar field '${_spark_sidecar_key}' in "
                "${_spark_sidecar_path}")
        endif()
        if(_spark_sidecar_key IN_LIST _spark_seen_sidecar_fields)
            message(FATAL_ERROR
                "Duplicate game-module ABI sidecar field '${_spark_sidecar_key}' in "
                "${_spark_sidecar_path}")
        endif()
        list(APPEND _spark_seen_sidecar_fields "${_spark_sidecar_key}")
        set("_spark_sidecar_${_spark_sidecar_key}" "${_spark_sidecar_value}")
    endforeach()
    foreach(_spark_required_sidecar_field IN LISTS _spark_expected_sidecar_fields)
        if(NOT _spark_required_sidecar_field IN_LIST _spark_seen_sidecar_fields)
            message(FATAL_ERROR
                "Missing game-module ABI sidecar field '${_spark_required_sidecar_field}' in "
                "${_spark_sidecar_path}")
        endif()
    endforeach()

    set(_spark_numeric_sidecar_fields
        format
        struct_size
        magic
        sdk_version
        runtime_abi_version
        compiler_family
        compiler_abi_version
        cxx_language_level
        runtime_library
        iterator_debug_level
        pointer_size
    )
    foreach(_spark_numeric_sidecar_field IN LISTS _spark_numeric_sidecar_fields)
        set(_spark_numeric_value "${_spark_sidecar_${_spark_numeric_sidecar_field}}")
        if(NOT _spark_numeric_value MATCHES "^(0|[1-9][0-9]*)$")
            message(FATAL_ERROR
                "Invalid uint32 game-module ABI field '${_spark_numeric_sidecar_field}="
                "${_spark_numeric_value}' in ${_spark_sidecar_path}")
        endif()
        string(LENGTH "${_spark_numeric_value}" _spark_numeric_value_length)
        if(_spark_numeric_value_length GREATER 10)
            message(FATAL_ERROR
                "Out-of-range uint32 game-module ABI field '${_spark_numeric_sidecar_field}="
                "${_spark_numeric_value}' in ${_spark_sidecar_path}")
        endif()
        if(_spark_numeric_value GREATER 4294967295)
            message(FATAL_ERROR
                "Out-of-range uint32 game-module ABI field '${_spark_numeric_sidecar_field}="
                "${_spark_numeric_value}' in ${_spark_sidecar_path}")
        endif()
    endforeach()

    if(NOT _spark_sidecar_format EQUAL 1 OR
       NOT _spark_sidecar_struct_size EQUAL 64 OR
       NOT _spark_sidecar_magic EQUAL 1263685715 OR
       NOT _spark_sidecar_runtime_abi_version EQUAL 1)
        message(FATAL_ERROR
            "Unsupported game-module ABI descriptor schema in ${_spark_sidecar_path}")
    endif()
    if(NOT _spark_sidecar_sdk_version EQUAL _spark_expected_sdk_version)
        message(FATAL_ERROR
            "Game-module SDK ABI version mismatch in ${_spark_sidecar_path}: "
            "expected ${_spark_expected_sdk_version}, found ${_spark_sidecar_sdk_version}")
    endif()
    if(_spark_sidecar_sdk_version LESS 1 OR
       _spark_sidecar_compiler_family GREATER 3 OR
       _spark_sidecar_cxx_language_level LESS 202002 OR
       _spark_sidecar_runtime_library GREATER 4 OR
       _spark_sidecar_iterator_debug_level GREATER 2 OR
       (NOT _spark_sidecar_pointer_size EQUAL 4 AND
        NOT _spark_sidecar_pointer_size EQUAL 8))
        message(FATAL_ERROR
            "Out-of-range game-module ABI descriptor value in ${_spark_sidecar_path}")
    endif()
    if((_spark_sidecar_compiler_family EQUAL 0 AND
        NOT _spark_sidecar_compiler_abi_version EQUAL 0) OR
       (NOT _spark_sidecar_compiler_family EQUAL 0 AND
        _spark_sidecar_compiler_abi_version EQUAL 0) OR
       (_spark_sidecar_compiler_family EQUAL 1 AND
        _spark_sidecar_runtime_library EQUAL 0) OR
       (NOT _spark_sidecar_compiler_family EQUAL 1 AND
        NOT _spark_sidecar_runtime_library EQUAL 0) OR
       (NOT _spark_sidecar_compiler_family EQUAL 1 AND
        NOT _spark_sidecar_iterator_debug_level EQUAL 0))
        message(FATAL_ERROR
            "Inconsistent game-module compiler ABI fields in ${_spark_sidecar_path}")
    endif()

    string(LENGTH "${_spark_sidecar_binary_sha256}" _spark_declared_hash_length)
    if(NOT _spark_declared_hash_length EQUAL 64 OR
       NOT _spark_sidecar_binary_sha256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "Invalid game-module binary_sha256 field in ${_spark_sidecar_path}")
    endif()
    string(TOLOWER "${_spark_sidecar_binary_sha256}" _spark_declared_hash)
    file(SHA256 "${_spark_module_path}" _spark_actual_hash)
    if(NOT _spark_declared_hash STREQUAL _spark_actual_hash)
        message(FATAL_ERROR
            "Game-module SHA-256 mismatch for ${_spark_module_name}:\n"
            "  binary:  ${_spark_module_path}\n"
            "  sidecar: ${_spark_sidecar_path}\n"
            "  declared: ${_spark_declared_hash}\n"
            "  actual:   ${_spark_actual_hash}")
    endif()

    set(_spark_abi_signature "")
    foreach(_spark_abi_field IN LISTS _spark_numeric_sidecar_fields)
        string(APPEND _spark_abi_signature
            "${_spark_abi_field}=${_spark_sidecar_${_spark_abi_field}};")
    endforeach()
    set(${_spark_abi_signature_output} "${_spark_abi_signature}" PARENT_SCOPE)
endfunction()

set(_spark_package_abi_signature "")
foreach(_spark_module IN LISTS _spark_required_game_modules)
    set(_spark_module_path
        "${SPARK_PACKAGE_ROOT}/bin/${_spark_module_prefix}${_spark_module}${_spark_module_suffix}")
    _spark_validate_game_module_sidecar(
        "${_spark_module_path}" "${_spark_module}" _spark_module_abi_signature)
    if(_spark_package_abi_signature STREQUAL "")
        set(_spark_package_abi_signature "${_spark_module_abi_signature}")
    elseif(NOT _spark_module_abi_signature STREQUAL _spark_package_abi_signature)
        message(FATAL_ERROR
            "Staged game modules do not share one ABI descriptor; mismatch at ${_spark_module}")
    endif()
endforeach()

list(LENGTH _spark_required_game_modules _spark_game_module_count)
if(_spark_validate_modules_only)
    message(STATUS
        "Validated sidecar schema, ABI consistency, and SHA-256 for "
        "${_spark_game_module_count} staged game modules for "
        "${_spark_package_target_system}")
    return()
endif()

set(_spark_required_runtime_files
    LICENSE.txt
    THIRD_PARTY_NOTICES.txt
    bin/Shaders/BasicVS.hlsl
    bin/Shaders/HLSL/BasicVS.hlsl
    bin/Shaders/HLSL/Compute/GPUCull.hlsl
    bin/Assets/MMOFPS/Data/continents.json
    bin/Assets/Engine/Branding/sparkengine_wordmark.svg
    bin/Resources/Config/settings.ini
    bin/Resources/Config/controls.cfg
)
set(_spark_missing_runtime_files "")
foreach(_spark_relative_path IN LISTS _spark_required_runtime_files)
    set(_spark_runtime_path "${SPARK_PACKAGE_ROOT}/${_spark_relative_path}")
    if(NOT EXISTS "${_spark_runtime_path}"
       OR IS_DIRECTORY "${_spark_runtime_path}"
       OR IS_SYMLINK "${_spark_runtime_path}")
        list(APPEND _spark_missing_runtime_files
            "${_spark_runtime_path}")
    else()
        _spark_require_bounded_regular_file(
            "${_spark_runtime_path}" "Required staged runtime content")
    endif()
endforeach()
if(_spark_missing_runtime_files)
    list(JOIN _spark_missing_runtime_files "\n  " _spark_missing_runtime_report)
    message(FATAL_ERROR
        "Staged SparkEngine package is missing required runtime content:\n"
        "  ${_spark_missing_runtime_report}")
endif()

set(_spark_help_smoke_executables
    SparkEngine
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
    SparkCrashReporter
)
foreach(_spark_executable IN LISTS _spark_help_smoke_executables)
    set(_spark_help_path
        "${SPARK_PACKAGE_ROOT}/bin/${_spark_executable}${SPARK_EXECUTABLE_SUFFIX}")
    execute_process(
        COMMAND "${_spark_help_path}" --help
        RESULT_VARIABLE _spark_help_result
        OUTPUT_VARIABLE _spark_help_stdout
        ERROR_VARIABLE _spark_help_stderr
        TIMEOUT 10
    )
    if(NOT _spark_help_result EQUAL 0)
        message(FATAL_ERROR
            "Staged ${_spark_executable} --help smoke failed (exit ${_spark_help_result})\n"
            "stdout:\n${_spark_help_stdout}\n"
            "stderr:\n${_spark_help_stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${SPARK_PACKAGE_ROOT}/bin/SparkEngine${SPARK_EXECUTABLE_SUFFIX}" --version
    RESULT_VARIABLE _spark_version_result
    OUTPUT_VARIABLE _spark_version_stdout
    ERROR_VARIABLE _spark_version_stderr
    TIMEOUT 10
)
if(NOT _spark_version_result EQUAL 0 OR
   NOT _spark_version_stdout MATCHES "SparkEngine [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR
        "Staged SparkEngine --version smoke failed (exit ${_spark_version_result})\n"
        "stdout:\n${_spark_version_stdout}\n"
        "stderr:\n${_spark_version_stderr}")
endif()

list(LENGTH _spark_required_executables _spark_executable_count)
message(STATUS
    "Validated ${_spark_executable_count} required executables and "
    "${_spark_game_module_count} game modules in ${SPARK_PACKAGE_ROOT}/bin")
