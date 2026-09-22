cmake_minimum_required(VERSION 3.25)

# Run the real installed SparkEngine.exe against the installed SparkGameFPS.dll
# using D3D11 WARP.  This is deliberately separate from the NullRHI smoke and
# from save/reload semantics: it proves that the staged runtime can create a
# D3D11 device and complete rendered module frames from the package layout.

if(NOT SPARK_FPS_D3D11_PATH_POLICY_SELF_TEST)
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
    if(NOT IS_DIRECTORY "${SPARK_TEST_ROOT}")
        file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
    endif()
endif()

function(_spark_root_has_reparse path description out_bad out_reason)
    set(_bad FALSE)
    set(_reason "")
    if(EXISTS "${path}" AND IS_SYMLINK "${path}")
        set(_bad TRUE)
        set(_reason "${description} is a symlink")
    elseif(CMAKE_HOST_WIN32 AND EXISTS "${path}")
        set(_powershell "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "SPARK_D3D11_ROOT=${path}"
                "${_powershell}" -NoLogo -NoProfile -NonInteractive -Command
                "$a=[IO.File]::GetAttributes([IO.Path]::GetFullPath($env:SPARK_D3D11_ROOT)); if(($a -band [IO.FileAttributes]::ReparsePoint) -ne 0){exit 1}"
            RESULT_VARIABLE _probe_result)
        if(NOT _probe_result EQUAL 0)
            set(_bad TRUE)
            set(_reason "${description} is a Windows reparse point")
        endif()
    endif()
    set(${out_bad} "${_bad}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

function(_spark_validate_staged_root source_root installed_root test_root out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    _spark_root_has_reparse("${installed_root}" "installed root" _installed_reparse _installed_reparse_reason)
    _spark_root_has_reparse("${test_root}" "test root" _test_reparse _test_reparse_reason)
    if(_installed_reparse)
        set(_ok FALSE)
        set(_reason "${_installed_reparse_reason}")
    elseif(_test_reparse)
        set(_ok FALSE)
        set(_reason "${_test_reparse_reason}")
    endif()
    file(REAL_PATH "${source_root}" _source_real)
    file(REAL_PATH "${installed_root}" _installed_real)
    file(REAL_PATH "${test_root}" _test_real)
    if(_source_real STREQUAL _installed_real)
        set(_ok FALSE)
        set(_reason "installed root is the source tree")
    endif()

    cmake_path(GET _test_real PARENT_PATH _trusted_parent)
    cmake_path(IS_PREFIX _trusted_parent "${_installed_real}" NORMALIZE _installed_under_trusted)
    if(_ok AND NOT _installed_under_trusted)
        set(_ok FALSE)
        set(_reason "installed root is outside the trusted test/build parent")
    elseif(_ok AND _installed_real STREQUAL _trusted_parent)
        set(_ok FALSE)
        set(_reason "installed root is the trusted parent rather than an isolated package")
    endif()

    # A build-tree stage under the checkout is valid. Only source runtime,
    # asset, and module locations are forbidden as the package identity.
    set(_source_forbidden
        "${_source_real}/bin"
        "${_source_real}/Assets"
        "${_source_real}/GameModules/SparkGameFPS"
        "${_source_real}/GameModules/SparkGameFPS/Assets")
    foreach(_forbidden IN LISTS _source_forbidden)
        if(EXISTS "${_forbidden}")
            file(REAL_PATH "${_forbidden}" _forbidden_real)
            cmake_path(IS_PREFIX _forbidden_real "${_installed_real}" NORMALIZE _inside_forbidden)
            if(_inside_forbidden)
                set(_ok FALSE)
                set(_reason "installed root resolves to a source runtime/assets/module subtree")
            endif()
        endif()
    endforeach()
    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_FPS_D3D11_PATH_POLICY_SELF_TEST)
    # Windows exposes TEMP, while hosted Linux runners conventionally expose
    # RUNNER_TEMP or TMPDIR. Keep the contract self-test writable on either
    # host instead of silently resolving an empty variable to /spark-... .
    set(_self_temp "$ENV{RUNNER_TEMP}")
    if(NOT _self_temp)
        set(_self_temp "$ENV{TMPDIR}")
    endif()
    if(NOT _self_temp)
        set(_self_temp "$ENV{TEMP}")
    endif()
    if(NOT _self_temp)
        set(_self_temp "$ENV{TMP}")
    endif()
    if(NOT _self_temp)
        if(CMAKE_HOST_WIN32)
            message(FATAL_ERROR "path-policy self-test requires a host temporary directory")
        endif()
        set(_self_temp "/tmp")
    endif()
    if(NOT IS_ABSOLUTE "${_self_temp}" OR "${_self_temp}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "path-policy self-test temporary directory must be a safe absolute path")
    endif()
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _self_nonce)
    set(_self_root "${_self_temp}/spark-fps-d3d11-path-policy-${_self_nonce}")
    file(MAKE_DIRECTORY
        "${_self_root}/checkout/build/stage"
        "${_self_root}/checkout/build/test"
        "${_self_root}/outside/stage"
        "${_self_root}/checkout/Assets/stage"
        "${_self_root}/checkout/bin/stage"
        "${_self_root}/checkout/GameModules/SparkGameFPS/Assets/stage")
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/checkout/build/stage"
        "${_self_root}/checkout/build/test"
        _allowed_ok _allowed_reason)
    if(NOT _allowed_ok)
        message(FATAL_ERROR "build-tree staging policy self-test failed: ${_allowed_reason}")
    endif()
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/checkout"
        "${_self_root}/checkout/build/test"
        _source_ok _source_reason)
    if(_source_ok)
        message(FATAL_ERROR "source-root policy self-test unexpectedly passed")
    endif()
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/outside/stage"
        "${_self_root}/checkout/build/test"
        _parent_ok _parent_reason)
    if(_parent_ok)
        message(FATAL_ERROR "wrong-parent policy self-test unexpectedly passed")
    endif()
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/checkout/Assets/stage"
        "${_self_root}/checkout/build/test"
        _asset_descendant_ok _asset_descendant_reason)
    if(_asset_descendant_ok)
        message(FATAL_ERROR "source-assets descendant policy self-test unexpectedly passed")
    endif()
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/checkout/bin/stage"
        "${_self_root}/checkout/build/test"
        _bin_descendant_ok _bin_descendant_reason)
    if(_bin_descendant_ok)
        message(FATAL_ERROR "source-bin descendant policy self-test unexpectedly passed")
    endif()
    _spark_validate_staged_root(
        "${_self_root}/checkout"
        "${_self_root}/checkout/GameModules/SparkGameFPS/Assets/stage"
        "${_self_root}/checkout/build/test"
        _module_descendant_ok _module_descendant_reason)
    if(_module_descendant_ok)
        message(FATAL_ERROR "source-module descendant policy self-test unexpectedly passed")
    endif()
    if(UNIX)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E create_symlink
            "${_self_root}/checkout/build/stage" "${_self_root}/checkout/build/link"
            RESULT_VARIABLE _symlink_result)
        if(_symlink_result EQUAL 0)
            _spark_validate_staged_root(
                "${_self_root}/checkout"
                "${_self_root}/checkout/build/link"
                "${_self_root}/checkout/build/test"
                _symlink_ok _symlink_reason)
            if(_symlink_ok)
                message(FATAL_ERROR "symlink policy self-test unexpectedly passed")
            endif()
        endif()
    elseif(WIN32)
        execute_process(COMMAND cmd /c mklink /J
            "${_self_root}/checkout/build/link"
            "${_self_root}/checkout/build/stage"
            RESULT_VARIABLE _junction_result
            OUTPUT_QUIET ERROR_QUIET)
        if(_junction_result EQUAL 0)
            _spark_validate_staged_root(
                "${_self_root}/checkout"
                "${_self_root}/checkout/build/link"
                "${_self_root}/checkout/build/test"
                _junction_ok _junction_reason)
            execute_process(COMMAND cmd /c rmdir
                "${_self_root}/checkout/build/link"
                RESULT_VARIABLE _junction_remove_result
                OUTPUT_QUIET ERROR_QUIET)
            if(_junction_ok)
                message(FATAL_ERROR "junction policy self-test unexpectedly passed")
            endif()
        endif()
    endif()
    message(STATUS "Installed FPS D3D11 package path policy contract passed")
    file(REMOVE_RECURSE "${_self_root}")
    return()
endif()

if(NOT IS_DIRECTORY "${SPARK_INSTALLED_ROOT}")
    message(FATAL_ERROR "Installed FPS package root is missing: ${SPARK_INSTALLED_ROOT}")
endif()
_spark_validate_staged_root(
    "${SPARK_SOURCE_ROOT}" "${SPARK_INSTALLED_ROOT}" "${SPARK_TEST_ROOT}"
    _root_ok _root_reason)
if(NOT _root_ok)
    message(FATAL_ERROR "Installed FPS D3D11 package path policy rejected the run: ${_root_reason}")
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
set(_visual_image "${_run_root}/fps-visible.png")
set(_visual_script "${_run_root}/visual.exec")
file(WRITE "${_visual_script}" "0 gfx_screenshot fps-visible.png\n")

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
        -exec "${_visual_script}"
    WORKING_DIRECTORY "${_run_root}"
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

if(NOT EXISTS "${_visual_image}" OR IS_DIRECTORY "${_visual_image}" OR IS_SYMLINK "${_visual_image}")
    message(FATAL_ERROR "Installed FPS D3D11 run did not save its pre-present visual frame: ${_visual_image}")
endif()
set(_powershell "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe")
if(NOT EXISTS "${_powershell}")
    message(FATAL_ERROR "Installed FPS D3D11 visual smoke requires Windows PowerShell")
endif()
execute_process(
    COMMAND "${_powershell}" -NoProfile -NonInteractive -File
        "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/CheckFPSVisibleFrame.ps1"
        -ImagePath "${_visual_image}"
    RESULT_VARIABLE _visual_result
    OUTPUT_VARIABLE _visual_stdout
    ERROR_VARIABLE _visual_stderr
    TIMEOUT 30)
if(NOT _visual_result EQUAL 0)
    message(FATAL_ERROR
        "Installed FPS D3D11 visual smoke failed (${_visual_result}):\n"
        "${_visual_stdout}\n${_visual_stderr}")
endif()
file(SHA256 "${_visual_image}" _visual_sha256)

file(WRITE "${_run_root}/evidence.txt"
    "engine=${_engine}\n"
    "module=${_module}\n"
    "assets=${_assets}\n"
    "engine_sha256=${_engine_sha256}\n"
    "module_sha256=${_module_sha256}\n"
    "backend=d3d11-warp\n"
    "asset_root_guard=installed-bin\n"
    "frames=8\n"
    "visual_screenshot_sha256=${_visual_sha256}\n"
    "visual_result=pass\n"
    "result=pass\n")
message(STATUS
    "Installed SparkGameFPS D3D11/WARP executable smoke passed with strict device, "
    "rendered-lifecycle, visible-frame, and clean-exit evidence; artifacts retained under ${_run_root}")
