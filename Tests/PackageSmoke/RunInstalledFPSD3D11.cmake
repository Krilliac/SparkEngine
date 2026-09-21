cmake_minimum_required(VERSION 3.25)

# Run the real installed SparkEngine.exe against the installed SparkGameFPS.dll
# using D3D11 WARP.  This is deliberately separate from the NullRHI smoke and
# from save/reload semantics: it proves that the staged runtime can create a
# D3D11 device and complete rendered module frames from the package layout.

foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_SOURCE_ROOT SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed FPS D3D11 smoke")
    endif()
endforeach()

foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_TEST_ROOT)
    if(NOT IS_ABSOLUTE "${${_required}}" OR "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${SPARK_INSTALLED_ROOT}")
    message(FATAL_ERROR "Installed FPS package root is missing: ${SPARK_INSTALLED_ROOT}")
endif()
if(NOT IS_DIRECTORY "${SPARK_TEST_ROOT}")
    file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
endif()

set(_bin "${SPARK_INSTALLED_ROOT}/bin")
set(_engine "${_bin}/SparkEngine.exe")
set(_module "${_bin}/SparkGameFPS.dll")
set(_assets "${_bin}/Assets")
foreach(_required_file IN ITEMS "${_engine}" "${_module}")
    if(NOT EXISTS "${_required_file}" OR IS_DIRECTORY "${_required_file}" OR IS_SYMLINK "${_required_file}")
        message(FATAL_ERROR "Installed FPS D3D11 input is missing or unsafe: ${_required_file}")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${_assets}" OR IS_SYMLINK "${_assets}")
    message(FATAL_ERROR "Installed FPS D3D11 asset directory is missing or unsafe: ${_assets}")
endif()

file(SHA256 "${_engine}" _engine_sha256)
file(SHA256 "${_module}" _module_sha256)
string(TIMESTAMP _run_timestamp "%Y%m%dT%H%M%SZ" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_nonce)
set(_run_root "${SPARK_TEST_ROOT}/d3d11-${_run_timestamp}-${_run_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated installed FPS D3D11 run root already exists: ${_run_root}")
endif()
file(MAKE_DIRECTORY "${_run_root}")
set(_stdout_log "${_run_root}/stdout.log")
set(_stderr_log "${_run_root}/stderr.log")
set(_combined_log "${_run_root}/combined.log")

set(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY ON)
include("${SPARK_SOURCE_ROOT}/cmake/RunSparkModuleProfileLifecycle.cmake")
unset(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=d3d11"
        "SPARK_D3D11_DRIVER=warp"
        "LOCALAPPDATA=${_run_root}/localappdata"
        "${_engine}"
        -game "${_module}"
        -require-game
        -test-frames 8
        -threads 2
        -window-size 640x360
        -no-subprocess
    WORKING_DIRECTORY "${_bin}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 120
    ENCODING UTF-8)
file(WRITE "${_stdout_log}" "${_stdout}")
file(WRITE "${_stderr_log}" "${_stderr}")
file(WRITE "${_combined_log}" "${_stdout}\n${_stderr}")

_spark_validate_lifecycle_result(
    "${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _lifecycle_reason)
if(NOT _lifecycle_ok)
    message(FATAL_ERROR
        "Installed FPS D3D11 executable smoke failed: ${_lifecycle_reason}\n"
        "stdout: ${_stdout}\n"
        "stderr: ${_stderr}")
endif()

file(WRITE "${_run_root}/evidence.txt"
    "engine=${_engine}\n"
    "module=${_module}\n"
    "assets=${_assets}\n"
    "engine_sha256=${_engine_sha256}\n"
    "module_sha256=${_module_sha256}\n"
    "backend=d3d11-warp\n"
    "frames=8\n"
    "result=pass\n")
message(STATUS
    "Installed SparkGameFPS D3D11/WARP executable smoke passed with strict device, "
    "rendered-lifecycle, and clean-exit evidence; artifacts retained under ${_run_root}")
