# Stage the configured Windows package and run the real stable-v1 runtime
# validator against the installed SparkGameFPS payload. This is a package
# smoke slice; it does not certify the full single-player gameplay contract.
#
# SPARK_FPS_PACKAGE_MODE selects the phases after staging and validation:
#   full (default)        playtester launcher, D3D11/WARP smoke and WARP save/reload
#   headless-save-reload  HEAD-220 NullRHI writer/reader save/reload of the staged
#                         executable and module (cmake/RunSparkHeadlessFPSSaveReload.cmake)

foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed FPS package test")
    endif()
endforeach()
if(NOT DEFINED SPARK_FPS_PACKAGE_MODE OR SPARK_FPS_PACKAGE_MODE STREQUAL "")
    set(SPARK_FPS_PACKAGE_MODE full)
endif()
if(NOT SPARK_FPS_PACKAGE_MODE STREQUAL "full" AND NOT SPARK_FPS_PACKAGE_MODE STREQUAL "headless-save-reload")
    message(FATAL_ERROR "Unknown SPARK_FPS_PACKAGE_MODE '${SPARK_FPS_PACKAGE_MODE}'")
endif()

find_program(_git_executable NAMES git git.exe REQUIRED)
execute_process(
    COMMAND "${_git_executable}" -C "${SPARK_SOURCE_ROOT}" rev-parse HEAD
    RESULT_VARIABLE _git_head_result
    OUTPUT_VARIABLE _source_sha
    ERROR_VARIABLE _git_head_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 30)
string(LENGTH "${_source_sha}" _source_sha_length)
if(NOT _git_head_result EQUAL 0 OR NOT _source_sha_length EQUAL 40 OR _source_sha MATCHES "[^0-9a-f]")
    message(FATAL_ERROR "Could not bind FPS package evidence to source HEAD: ${_git_head_error}")
endif()
execute_process(
    COMMAND "${_git_executable}" -C "${SPARK_SOURCE_ROOT}"
        status --porcelain=v1 --untracked-files=no
    RESULT_VARIABLE _git_status_result
    OUTPUT_VARIABLE _git_status_output
    ERROR_VARIABLE _git_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
    TIMEOUT 30)
if(NOT _git_status_result EQUAL 0)
    message(FATAL_ERROR "Could not inspect FPS package source state: ${_git_status_error}")
endif()
if(_git_status_output STREQUAL "")
    set(_source_tree_state clean)
else()
    set(_source_tree_state dirty)
endif()

function(_run_checked _stage _timeout)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT ${_timeout})
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_stage} failed (${_result}):\n${_output}\n${_error}")
    endif()
endfunction()

if(NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR "${SPARK_TEST_ROOT}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()
cmake_path(SET _test_root NORMALIZE "${SPARK_TEST_ROOT}")
cmake_path(GET _test_root ROOT_PATH _test_volume_root)
if(_test_root STREQUAL _test_volume_root)
    message(FATAL_ERROR "SPARK_TEST_ROOT must not be a volume root")
endif()
if(EXISTS "${_test_root}" AND (NOT IS_DIRECTORY "${_test_root}" OR IS_SYMLINK "${_test_root}"))
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a regular directory when it exists")
endif()
file(MAKE_DIRECTORY "${_test_root}")
file(REAL_PATH "${_test_root}" _test_root_real)
if(CMAKE_HOST_WIN32)
    string(TOLOWER "${_test_root}" _test_root_compare)
    string(TOLOWER "${_test_root_real}" _test_root_real_compare)
else()
    set(_test_root_compare "${_test_root}")
    set(_test_root_real_compare "${_test_root_real}")
endif()
if(NOT _test_root_compare STREQUAL _test_root_real_compare)
    message(FATAL_ERROR "SPARK_TEST_ROOT must not cross a symlink or reparse point")
endif()

string(TIMESTAMP _run_timestamp "%Y%m%dT%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_nonce)
set(_run_root "${_test_root}/run-${_run_timestamp}-${_run_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated FPS package run root already exists: ${_run_root}")
endif()
file(MAKE_DIRECTORY "${_run_root}")
set(_install_root "${_run_root}/install")
set(_expected_manifest "${SPARK_ENGINE_BUILD_DIR}/SparkEngineGameModules.cmake")

_run_checked("Install configured FPS runtime component" 90
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component runtime)
_run_checked("Install configured FPS module component" 90
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component samples)
# ENG-220: the packaged Visual C++ runtime (CMakeLists.txt, "redist" component).
_run_checked("Install configured FPS Visual C++ runtime component" 90
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
    --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component redist)

if(SPARK_FPS_PACKAGE_MODE STREQUAL "full")
    _run_checked("Exercise installed FPS playtester entry point" 120
        "${CMAKE_COMMAND}"
        "-DSPARK_INSTALLED_ROOT=${_install_root}"
        "-DSPARK_PLAYTEST_TEST_ROOT=${_run_root}/launcher-smoke"
        -P "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/TestPlaytestFPSLauncher.cmake")
endif()

_run_checked("Validate installed FPS asset manifest and payload" 120
    "${CMAKE_COMMAND}"
    "-DSPARK_ASSETS_ROOT=${_install_root}/bin/Assets"
    "-DSPARK_ASSET_VERIFIER=${SPARK_SOURCE_ROOT}/tools/asset-integrity/verify_asset_integrity.py"
    -P "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/ValidateInstalledFPSAssets.cmake")

if(NOT EXISTS "${_expected_manifest}" OR IS_DIRECTORY "${_expected_manifest}")
    message(FATAL_ERROR
        "Configured build is missing its generated game-module manifest: ${_expected_manifest}")
endif()

_run_checked("Validate installed FPS runtime package" 120
    "${CMAKE_COMMAND}" -E env
    "LOCALAPPDATA=${_run_root}/validator-localappdata"
    "${CMAKE_COMMAND}"
    "-DSPARK_PACKAGE_ROOT=${_install_root}"
    "-DSPARK_PACKAGE_LAYOUT=runtime"
    "-DSPARK_PACKAGE_PROFILE=stable-v1"
    "-DSPARK_PACKAGE_VALIDATE_MODULES_ONLY=ON"
    "-DSPARK_PACKAGE_EXPECTED_MODULE_MANIFEST=${_expected_manifest}"
    "-DSPARK_EXECUTABLE_SUFFIX=.exe"
    -P "${SPARK_SOURCE_ROOT}/cmake/ValidateStagedPackageExecutables.cmake")

# ENG-220: every DLL a staged EXE/DLL imports (or delay-imports) must sit beside
# it, be an API set, or be an allowlisted OS DLL present in System32. PATH and
# the build tree are never consulted, so a DLL this developer machine happens
# to have installed does not hide a dependency a clean machine lacks. Debug
# images import the Debug CRT, which is not redistributable, so a Debug stage
# is a developer layout and is not checked.
if(SPARK_CONFIG STREQUAL "Debug")
    message(STATUS "PE import closure not checked: Debug packages import the non-redistributable Debug CRT")
else()
    find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
    _run_checked("Validate installed FPS package DLL import closure" 120
        "${Python3_EXECUTABLE}" -B "${SPARK_SOURCE_ROOT}/tools/pe_import_closure.py" "${_install_root}")
endif()

if(SPARK_FPS_PACKAGE_MODE STREQUAL "headless-save-reload")
    # The staged executable and module run on NullRHI with no D3D11 device; the
    # runner gives both processes one isolated LOCALAPPDATA/APPDATA user root
    # under the run root and requires the reader to leave the save unchanged.
    # Package mode launches them from an empty directory outside the package and
    # the repository, and fails on any output naming the source or build tree or
    # any change to the staged package (HEAD-220).
    _run_checked("Validate installed FPS NullRHI save/reload persistence" 300
        "${CMAKE_COMMAND}"
        "-DSPARK_ENGINE_EXECUTABLE=${_install_root}/bin/SparkEngine.exe"
        "-DSPARK_GAME_MODULE=${_install_root}/bin/SparkGameFPS.dll"
        "-DSPARK_PACKAGE_ROOT=${_install_root}"
        "-DSPARK_FORBIDDEN_ROOTS=${SPARK_SOURCE_ROOT}|${SPARK_ENGINE_BUILD_DIR}"
        "-DSPARK_TEST_ROOT=${_run_root}/headless-save-reload"
        -P "${SPARK_SOURCE_ROOT}/cmake/RunSparkHeadlessFPSSaveReload.cmake")
    message(STATUS
        "Installed SparkGameFPS runtime package passed NullRHI save/reload at "
        "${_source_sha} (${_source_tree_state}, ${SPARK_CONFIG}); evidence retained under ${_run_root}")
    return()
endif()

_run_checked("Run installed FPS D3D11/WARP executable smoke" 150
    "${CMAKE_COMMAND}"
    "-DSPARK_INSTALLED_ROOT=${_install_root}"
    "-DSPARK_SOURCE_ROOT=${SPARK_SOURCE_ROOT}"
    "-DSPARK_TEST_ROOT=${_run_root}/d3d11-smoke"
    -P "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/RunInstalledFPSD3D11.cmake")

_run_checked("Validate installed FPS save/reload persistence" 210
    "${CMAKE_COMMAND}"
    "-DSPARK_INSTALLED_ROOT=${_install_root}"
    "-DSPARK_SOURCE_ROOT=${SPARK_SOURCE_ROOT}"
    "-DSPARK_FPS_SAVE_TEST_ROOT=${_run_root}/save-reload"
    -DSPARK_FPS_PACKAGE_PREVALIDATED=ON
    "-DSPARK_SOURCE_SHA=${_source_sha}"
    "-DSPARK_SOURCE_TREE_STATE=${_source_tree_state}"
    "-DSPARK_BUILD_CONFIG=${SPARK_CONFIG}"
    -P "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake")

message(STATUS
    "Installed SparkGameFPS runtime package and save/reload smoke completed; "
    "evidence retained under ${_run_root}")
