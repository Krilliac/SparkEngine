cmake_minimum_required(VERSION 3.25)

foreach(_spark_required IN ITEMS SPARK_VALIDATOR SPARK_TEST_ROOT SPARK_BINARY_ROOT)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()

set(_spark_resolved_test_root "${SPARK_TEST_ROOT}")
set(_spark_resolved_binary_root "${SPARK_BINARY_ROOT}")
cmake_path(ABSOLUTE_PATH _spark_resolved_test_root NORMALIZE)
cmake_path(ABSOLUTE_PATH _spark_resolved_binary_root NORMALIZE)
cmake_path(IS_PREFIX _spark_resolved_binary_root "${_spark_resolved_test_root}"
    NORMALIZE _spark_test_root_is_bounded)
cmake_path(GET _spark_resolved_test_root FILENAME _spark_test_root_name)
if(NOT _spark_test_root_is_bounded OR
   _spark_resolved_test_root STREQUAL _spark_resolved_binary_root OR
   NOT _spark_test_root_name MATCHES "^module-sidecar-validator(-[A-Za-z0-9_.-]+)?$")
    message(FATAL_ERROR
        "Refusing to use module-sidecar scratch path unless it is a strict, named "
        "module-sidecar-validator descendant of the build tree")
endif()
file(REMOVE_RECURSE "${_spark_resolved_test_root}")
file(MAKE_DIRECTORY "${_spark_resolved_test_root}")

# Deliberately differ from every host platform's normal shared-library naming.
# A passing fixture therefore proves that validation consumes target naming
# from the package manifest rather than inferring it from the validation host.
set(_spark_module_prefix "fixture-")
set(_spark_module_suffix ".module")

function(_spark_fixture_module_path _spark_root _spark_module _spark_output)
    set(${_spark_output}
        "${_spark_root}/bin/${_spark_module_prefix}${_spark_module}${_spark_module_suffix}"
        PARENT_SCOPE)
endfunction()

function(_spark_write_valid_fixture _spark_root _spark_modules)
    file(MAKE_DIRECTORY
        "${_spark_root}/bin"
        "${_spark_root}/include/Spark"
        "${_spark_root}/lib/cmake/SparkEngine")
    file(WRITE "${_spark_root}/include/Spark/Version.h"
        "#pragma once\n#define SPARK_SDK_VERSION 3\n")
    file(WRITE
        "${_spark_root}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake"
        "format=1\n"
        "target_system=FixtureOS\n"
        "module_prefix=${_spark_module_prefix}\n"
        "module_suffix=${_spark_module_suffix}\n"
        "modules=${_spark_modules}\n")

    foreach(_spark_module IN LISTS _spark_modules)
        _spark_fixture_module_path("${_spark_root}" "${_spark_module}" _spark_module_path)
        file(WRITE "${_spark_module_path}" "fixture binary for ${_spark_module}\n")
        file(SHA256 "${_spark_module_path}" _spark_module_hash)
        file(WRITE "${_spark_module_path}.sparkabi"
            "format=1\n"
            "struct_size=64\n"
            "magic=1263685715\n"
            "sdk_version=3\n"
            "runtime_abi_version=1\n"
            "compiler_family=1\n"
            "compiler_abi_version=1944\n"
            "cxx_language_level=202400\n"
            "runtime_library=1\n"
            "iterator_debug_level=0\n"
            "pointer_size=8\n"
            "binary_sha256=${_spark_module_hash}\n")
    endforeach()
endfunction()

function(_spark_run_validator _spark_root _spark_result_output _spark_log_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSPARK_PACKAGE_ROOT=${_spark_root}"
            -DSPARK_PACKAGE_VALIDATE_MODULES_ONLY=ON
            -P "${SPARK_VALIDATOR}"
        RESULT_VARIABLE _spark_result
        OUTPUT_VARIABLE _spark_stdout
        ERROR_VARIABLE _spark_stderr)
    set(${_spark_result_output} "${_spark_result}" PARENT_SCOPE)
    set(${_spark_log_output} "${_spark_stdout}\n${_spark_stderr}" PARENT_SCOPE)
endfunction()

function(_spark_create_directory_reparse _spark_link _spark_target)
    if(WIN32)
        cmake_path(NATIVE_PATH _spark_link _spark_link_native)
        cmake_path(NATIVE_PATH _spark_target _spark_target_native)
        execute_process(
            COMMAND cmd /c mklink /J "${_spark_link_native}" "${_spark_target_native}"
            RESULT_VARIABLE _spark_link_result
            OUTPUT_VARIABLE _spark_link_output
            ERROR_VARIABLE _spark_link_error)
    else()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E create_symlink
                "${_spark_target}" "${_spark_link}"
            RESULT_VARIABLE _spark_link_result
            OUTPUT_VARIABLE _spark_link_output
            ERROR_VARIABLE _spark_link_error)
    endif()
    if(NOT _spark_link_result EQUAL 0)
        message(FATAL_ERROR
            "Could not create directory reparse fixture (${_spark_link_result})\n"
            "${_spark_link_output}\n${_spark_link_error}")
    endif()
endfunction()

function(_spark_remove_directory_reparse _spark_link)
    if(WIN32)
        cmake_path(NATIVE_PATH _spark_link _spark_link_native)
        execute_process(
            COMMAND cmd /c rmdir "${_spark_link_native}"
            RESULT_VARIABLE _spark_unlink_result
            OUTPUT_VARIABLE _spark_unlink_output
            ERROR_VARIABLE _spark_unlink_error)
    else()
        file(REMOVE "${_spark_link}")
        set(_spark_unlink_result 0)
    endif()
    if(NOT _spark_unlink_result EQUAL 0)
        message(FATAL_ERROR
            "Could not remove directory reparse fixture (${_spark_unlink_result})\n"
            "${_spark_unlink_output}\n${_spark_unlink_error}")
    endif()
endfunction()

function(_spark_assert_rejected _spark_case_name _spark_root _spark_expected_error)
    _spark_run_validator("${_spark_root}" _spark_result _spark_log)
    if(_spark_result EQUAL 0)
        message(FATAL_ERROR
            "${_spark_case_name}: validator accepted an invalid fixture\n${_spark_log}")
    endif()
    string(FIND "${_spark_log}" "${_spark_expected_error}" _spark_error_position)
    if(_spark_error_position EQUAL -1)
        message(FATAL_ERROR
            "${_spark_case_name}: validator did not report '${_spark_expected_error}'\n"
            "${_spark_log}")
    endif()
endfunction()

function(_spark_expect_invalid _spark_case_name _spark_search _spark_replacement
         _spark_expected_error)
    set(_spark_case_root "${_spark_resolved_test_root}/${_spark_case_name}")
    _spark_write_valid_fixture("${_spark_case_root}" "SparkGameFixture")
    _spark_fixture_module_path("${_spark_case_root}" "SparkGameFixture" _spark_module_path)
    set(_spark_sidecar_path "${_spark_module_path}.sparkabi")
    file(READ "${_spark_sidecar_path}" _spark_original)
    string(REPLACE "${_spark_search}" "${_spark_replacement}" _spark_mutated
        "${_spark_original}")
    if(_spark_mutated STREQUAL _spark_original)
        message(FATAL_ERROR "${_spark_case_name}: fixture mutation did not match")
    endif()
    file(WRITE "${_spark_sidecar_path}" "${_spark_mutated}")
    _spark_assert_rejected(
        "${_spark_case_name}" "${_spark_case_root}" "${_spark_expected_error}")
endfunction()

set(_spark_valid_root "${_spark_resolved_test_root}/valid")
_spark_write_valid_fixture("${_spark_valid_root}" "SparkGameFixture")
_spark_run_validator("${_spark_valid_root}" _spark_valid_result _spark_valid_log)
if(NOT _spark_valid_result EQUAL 0)
    message(FATAL_ERROR "Validator rejected the valid sidecar fixture\n${_spark_valid_log}")
endif()

_spark_expect_invalid(
    duplicate_field
    "sdk_version=3"
    "format=1"
    "Duplicate game-module ABI sidecar field 'format'")
_spark_expect_invalid(
    missing_field
    "pointer_size=8\n"
    ""
    "must contain exactly 12 fields")
_spark_expect_invalid(
    unknown_field
    "pointer_size=8"
    "future_pointer_size=8"
    "Unexpected game-module ABI sidecar field 'future_pointer_size'")
_spark_expect_invalid(
    malformed_line
    "pointer_size=8"
    "pointer_size = 8"
    "Malformed game-module ABI sidecar line")
_spark_expect_invalid(
    invalid_uint32
    "compiler_abi_version=1944"
    "compiler_abi_version=4294967296"
    "Out-of-range uint32 game-module ABI field")
_spark_expect_invalid(
    oversized_uint32
    "compiler_abi_version=1944"
    "compiler_abi_version=42949672960"
    "Out-of-range uint32 game-module ABI field")
_spark_expect_invalid(
    unsupported_schema
    "format=1"
    "format=2"
    "Unsupported game-module ABI descriptor schema")
_spark_expect_invalid(
    sdk_abi_mismatch
    "sdk_version=3"
    "sdk_version=4"
    "Game-module SDK ABI version mismatch")
_spark_expect_invalid(
    invalid_hash
    "binary_sha256="
    "binary_sha256=xyz"
    "Invalid game-module binary_sha256 field")
_spark_expect_invalid(
    leading_zero_uint32
    "compiler_abi_version=1944"
    "compiler_abi_version=01944"
    "Invalid uint32 game-module ABI field")

set(_spark_uint32_max_root "${_spark_resolved_test_root}/uint32_max")
_spark_write_valid_fixture("${_spark_uint32_max_root}" "SparkGameFixture")
_spark_fixture_module_path(
    "${_spark_uint32_max_root}" "SparkGameFixture" _spark_uint32_max_module)
set(_spark_uint32_max_sidecar "${_spark_uint32_max_module}.sparkabi")
file(READ "${_spark_uint32_max_sidecar}" _spark_uint32_max_content)
string(REPLACE "compiler_abi_version=1944" "compiler_abi_version=4294967295"
    _spark_uint32_max_content "${_spark_uint32_max_content}")
file(WRITE "${_spark_uint32_max_sidecar}" "${_spark_uint32_max_content}")
_spark_run_validator(
    "${_spark_uint32_max_root}" _spark_uint32_max_result _spark_uint32_max_log)
if(NOT _spark_uint32_max_result EQUAL 0)
    message(FATAL_ERROR
        "Validator rejected the maximum uint32 ABI value\n${_spark_uint32_max_log}")
endif()

set(_spark_crlf_root "${_spark_resolved_test_root}/crlf")
_spark_write_valid_fixture("${_spark_crlf_root}" "SparkGameFixture")
_spark_fixture_module_path("${_spark_crlf_root}" "SparkGameFixture" _spark_crlf_module)
set(_spark_crlf_sidecar "${_spark_crlf_module}.sparkabi")
file(READ "${_spark_crlf_sidecar}" _spark_crlf_content)
string(REPLACE "\n" "\r\n" _spark_crlf_content "${_spark_crlf_content}")
file(WRITE "${_spark_crlf_sidecar}" "${_spark_crlf_content}")
_spark_run_validator("${_spark_crlf_root}" _spark_crlf_result _spark_crlf_log)
if(NOT _spark_crlf_result EQUAL 0)
    message(FATAL_ERROR "Validator rejected valid CRLF line endings\n${_spark_crlf_log}")
endif()

set(_spark_bare_cr_root "${_spark_resolved_test_root}/bare_cr")
_spark_write_valid_fixture("${_spark_bare_cr_root}" "SparkGameFixture")
_spark_fixture_module_path("${_spark_bare_cr_root}" "SparkGameFixture" _spark_bare_cr_module)
set(_spark_bare_cr_sidecar "${_spark_bare_cr_module}.sparkabi")
file(READ "${_spark_bare_cr_sidecar}" _spark_bare_cr_content)
string(REPLACE "pointer_size=8\n" "pointer_size=8\r"
    _spark_bare_cr_content "${_spark_bare_cr_content}")
file(WRITE "${_spark_bare_cr_sidecar}" "${_spark_bare_cr_content}")
_spark_assert_rejected(
    bare_cr "${_spark_bare_cr_root}" "Malformed game-module ABI sidecar (invalid line ending)")

set(_spark_trailing_blank_root "${_spark_resolved_test_root}/trailing_blank")
_spark_write_valid_fixture("${_spark_trailing_blank_root}" "SparkGameFixture")
_spark_fixture_module_path(
    "${_spark_trailing_blank_root}" "SparkGameFixture" _spark_trailing_blank_module)
file(APPEND "${_spark_trailing_blank_module}.sparkabi" "\n")
_spark_assert_rejected(
    trailing_blank "${_spark_trailing_blank_root}" "Malformed game-module ABI sidecar (blank line)")

set(_spark_oversized_sidecar_root "${_spark_resolved_test_root}/oversized_sidecar")
_spark_write_valid_fixture("${_spark_oversized_sidecar_root}" "SparkGameFixture")
_spark_fixture_module_path(
    "${_spark_oversized_sidecar_root}" "SparkGameFixture" _spark_oversized_sidecar_module)
string(REPEAT "x" 4097 _spark_oversized_sidecar_content)
file(WRITE "${_spark_oversized_sidecar_module}.sparkabi"
    "${_spark_oversized_sidecar_content}")
_spark_assert_rejected(
    oversized_sidecar "${_spark_oversized_sidecar_root}"
    "Game-module ABI sidecar has invalid size")

set(_spark_uppercase_hash_root "${_spark_resolved_test_root}/uppercase_hash")
_spark_write_valid_fixture("${_spark_uppercase_hash_root}" "SparkGameFixture")
_spark_fixture_module_path(
    "${_spark_uppercase_hash_root}" "SparkGameFixture" _spark_uppercase_hash_module)
set(_spark_uppercase_hash_sidecar "${_spark_uppercase_hash_module}.sparkabi")
file(SHA256 "${_spark_uppercase_hash_module}" _spark_lowercase_hash)
string(TOUPPER "${_spark_lowercase_hash}" _spark_uppercase_hash)
file(READ "${_spark_uppercase_hash_sidecar}" _spark_uppercase_hash_content)
string(REPLACE "binary_sha256=${_spark_lowercase_hash}"
    "binary_sha256=${_spark_uppercase_hash}"
    _spark_uppercase_hash_content "${_spark_uppercase_hash_content}")
file(WRITE "${_spark_uppercase_hash_sidecar}" "${_spark_uppercase_hash_content}")
_spark_assert_rejected(
    uppercase_hash "${_spark_uppercase_hash_root}"
    "Invalid game-module binary_sha256 field")

set(_spark_manifest_blank_root "${_spark_resolved_test_root}/manifest_trailing_blank")
_spark_write_valid_fixture("${_spark_manifest_blank_root}" "SparkGameFixture")
set(_spark_manifest_blank_path
    "${_spark_manifest_blank_root}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake")
file(APPEND "${_spark_manifest_blank_path}" "\n")
_spark_assert_rejected(
    manifest_trailing_blank "${_spark_manifest_blank_root}"
    "game-module inventory contains a blank line")

set(_spark_manifest_cr_root "${_spark_resolved_test_root}/manifest_bare_cr")
_spark_write_valid_fixture("${_spark_manifest_cr_root}" "SparkGameFixture")
set(_spark_manifest_cr_path
    "${_spark_manifest_cr_root}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake")
file(READ "${_spark_manifest_cr_path}" _spark_manifest_cr_content)
string(REPLACE "modules=SparkGameFixture\n" "modules=SparkGameFixture\r"
    _spark_manifest_cr_content "${_spark_manifest_cr_content}")
file(WRITE "${_spark_manifest_cr_path}" "${_spark_manifest_cr_content}")
_spark_assert_rejected(
    manifest_bare_cr "${_spark_manifest_cr_root}"
    "game-module inventory is missing its final newline")

set(_spark_manifest_oversized_root "${_spark_resolved_test_root}/manifest_oversized")
_spark_write_valid_fixture("${_spark_manifest_oversized_root}" "SparkGameFixture")
set(_spark_manifest_oversized_path
    "${_spark_manifest_oversized_root}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake")
string(REPEAT "x" 4097 _spark_manifest_oversized_content)
file(WRITE "${_spark_manifest_oversized_path}" "${_spark_manifest_oversized_content}")
_spark_assert_rejected(
    manifest_oversized "${_spark_manifest_oversized_root}"
    "game-module inventory has invalid size")

set(_spark_manifest_injection_root "${_spark_resolved_test_root}/manifest_injection")
_spark_write_valid_fixture("${_spark_manifest_injection_root}" "SparkGameFixture")
set(_spark_manifest_injection_path
    "${_spark_manifest_injection_root}/lib/cmake/SparkEngine/SparkEngineGameModules.cmake")
set(_spark_manifest_injection_marker "${_spark_manifest_injection_root}/injection-ran")
file(WRITE "${_spark_manifest_injection_path}"
    "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E touch "
    "\"${_spark_manifest_injection_marker}\")\n")
_spark_assert_rejected(
    manifest_injection "${_spark_manifest_injection_root}"
    "game-module inventory does not match the fixed data")
if(EXISTS "${_spark_manifest_injection_marker}")
    message(FATAL_ERROR "Manifest injection executed staged package content")
endif()

# A complete, internally consistent module and sidecar outside the package
# root must not become valid merely because package/bin is a junction/symlink.
set(_spark_bin_reparse_root "${_spark_resolved_test_root}/bin_reparse")
_spark_write_valid_fixture("${_spark_bin_reparse_root}" "SparkGameFixture")
set(_spark_bin_reparse_outside "${_spark_resolved_test_root}/bin_reparse_outside")
file(RENAME
    "${_spark_bin_reparse_root}/bin"
    "${_spark_bin_reparse_outside}")
_spark_create_directory_reparse(
    "${_spark_bin_reparse_root}/bin" "${_spark_bin_reparse_outside}")
_spark_assert_rejected(
    bin_reparse "${_spark_bin_reparse_root}" "crosses a symlink")
_spark_remove_directory_reparse("${_spark_bin_reparse_root}/bin")

set(_spark_hash_mismatch_root "${_spark_resolved_test_root}/hash_mismatch")
_spark_write_valid_fixture("${_spark_hash_mismatch_root}" "SparkGameFixture")
_spark_fixture_module_path(
    "${_spark_hash_mismatch_root}" "SparkGameFixture" _spark_hash_mismatch_module)
set(_spark_hash_mismatch_sidecar "${_spark_hash_mismatch_module}.sparkabi")
file(READ "${_spark_hash_mismatch_sidecar}" _spark_hash_mismatch_content)
string(REGEX REPLACE
    "binary_sha256=[0-9a-f]+"
    "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000"
    _spark_hash_mismatch_content
    "${_spark_hash_mismatch_content}")
file(WRITE "${_spark_hash_mismatch_sidecar}" "${_spark_hash_mismatch_content}")
_spark_run_validator(
    "${_spark_hash_mismatch_root}" _spark_hash_mismatch_result _spark_hash_mismatch_log)
if(_spark_hash_mismatch_result EQUAL 0 OR
   NOT _spark_hash_mismatch_log MATCHES "Game-module SHA-256 mismatch")
    message(FATAL_ERROR
        "Validator did not reject a mismatched binary hash\n${_spark_hash_mismatch_log}")
endif()

set(_spark_mixed_abi_root "${_spark_resolved_test_root}/mixed_abi")
_spark_write_valid_fixture(
    "${_spark_mixed_abi_root}" "SparkGameFixture;SparkGameSecond")
_spark_fixture_module_path(
    "${_spark_mixed_abi_root}" "SparkGameSecond" _spark_mixed_abi_module)
set(_spark_mixed_abi_sidecar "${_spark_mixed_abi_module}.sparkabi")
file(READ "${_spark_mixed_abi_sidecar}" _spark_mixed_abi_content)
string(REPLACE "compiler_abi_version=1944" "compiler_abi_version=1945"
    _spark_mixed_abi_content
    "${_spark_mixed_abi_content}")
file(WRITE "${_spark_mixed_abi_sidecar}" "${_spark_mixed_abi_content}")
_spark_run_validator("${_spark_mixed_abi_root}" _spark_mixed_abi_result _spark_mixed_abi_log)
if(_spark_mixed_abi_result EQUAL 0 OR
   NOT _spark_mixed_abi_log MATCHES "do not share one ABI descriptor")
    message(FATAL_ERROR
        "Validator did not reject mixed module ABIs\n${_spark_mixed_abi_log}")
endif()

message(STATUS
    "Module-sidecar validator accepted the valid fixture and rejected all malformed fixtures")
