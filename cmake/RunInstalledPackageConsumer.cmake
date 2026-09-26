# Linux installed-package consumer lane (ASSET-220), run as a CTest via
# `cmake -P`. It mirrors the per-PR Windows "installed SDK consumer" step in
# .github/workflows/build.yml on a Linux build tree:
#
#   1. `cmake --install` the configured engine build into a fresh, build-owned
#      prefix (the whole install, as release.yml stages it);
#   2. configure Tests/PackageSmoke against <prefix>/lib/cmake/SparkEngine with
#      SPARK_EXPECTED_ENGINE_VERSION read from the engine's CMake cache (never
#      a literal: a literal passes against a package built from another tag);
#   3. build it, then run its tests with --no-tests=error;
#   4. fail if the consumer resolved any header or library from the engine
#      source or build tree instead of the install prefix, or if it resolved
#      the package or its bundled SDL2 from anywhere but that prefix.
#
# This is local, non-hosted evidence. It is registered only when the engine is
# configured with -DSPARK_ENABLE_PACKAGE_CONSUMER_TESTS=ON and selected with
#   ctest -L package-consumer-linux --no-tests=error
#
# The consumer is always configured with "Unix Makefiles", whatever generator
# the engine build uses: the boundary proof in step 4 reads the compiler depfiles
# and per-target link.txt that only the Makefiles generator leaves on disk
# (Ninja consumes the depfiles into .ninja_deps and writes no link.txt).
#
# Required: SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
#           SPARK_CONSUMER_CTEST SPARK_STAGE_TIMEOUT (seconds per stage; the
#           caller keeps 4 x SPARK_STAGE_TIMEOUT below the CTest TIMEOUT)
# Optional: SPARK_CONSUMER_COMPILER

foreach(_required IN ITEMS
        SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
        SPARK_CONSUMER_CTEST SPARK_STAGE_TIMEOUT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed-package consumer test")
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/SparkPackageConsumerBoundary.cmake")

function(_spark_run_checked _stage _log)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT ${SPARK_STAGE_TIMEOUT})
    file(WRITE "${_log}" "${_output}\n${_error}")
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_stage} failed (${_result}); log: ${_log}\n${_output}\n${_error}")
    endif()
    message(STATUS "${_stage}: ok (log: ${_log})")
    set(_spark_last_output "${_output}" PARENT_SCOPE)
endfunction()

# The version the consumer must see is whatever this engine build was
# configured as, read from its cache exactly as build.yml reads it.
load_cache("${SPARK_ENGINE_BUILD_DIR}" READ_WITH_PREFIX _engine_ SPARK_ENGINE_VERSION)
if("${_engine_SPARK_ENGINE_VERSION}" STREQUAL "")
    message(FATAL_ERROR "SPARK_ENGINE_VERSION is not set in ${SPARK_ENGINE_BUILD_DIR}/CMakeCache.txt")
endif()

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
set(_prefix "${SPARK_TEST_ROOT}/prefix")
set(_consumer_build "${SPARK_TEST_ROOT}/consumer-build")
set(_package_dir "${_prefix}/lib/cmake/SparkEngine")

_spark_run_checked("Install engine build into ${_prefix}" "${SPARK_TEST_ROOT}/install.log"
    "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}" --config "${SPARK_CONFIG}" --prefix "${_prefix}")
if(NOT EXISTS "${_package_dir}/SparkEngineConfig.cmake")
    message(FATAL_ERROR "The install did not produce ${_package_dir}/SparkEngineConfig.cmake")
endif()

set(_configure
    "${CMAKE_COMMAND}" -S "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke" -B "${_consumer_build}"
    -G "Unix Makefiles"
    "-DCMAKE_BUILD_TYPE=${SPARK_CONFIG}"
    "-DSparkEngine_DIR=${_package_dir}"
    "-DSPARK_EXPECTED_ENGINE_VERSION=${_engine_SPARK_ENGINE_VERSION}"
    # The boundary scan below reads the declared include directories from here.
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    # A compiler launcher cache could serve objects compiled against another tree.
    "-DCMAKE_CXX_COMPILER_LAUNCHER=")
if(DEFINED SPARK_CONSUMER_COMPILER AND NOT "${SPARK_CONSUMER_COMPILER}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_CXX_COMPILER=${SPARK_CONSUMER_COMPILER}")
endif()
_spark_run_checked("Configure installed-package consumer" "${SPARK_TEST_ROOT}/configure.log" ${_configure})

# find_package must have resolved the package, and the SDL2 it bundles, from
# the install prefix. A host SDL2 package found instead is a different library
# from the one the installed engine was built and shipped with.
load_cache("${_consumer_build}" READ_WITH_PREFIX _consumer_ SparkEngine_DIR SDL2_DIR)
foreach(_pair IN ITEMS "SparkEngine_DIR|${_package_dir}" "SDL2_DIR|${_prefix}/lib/cmake/SDL2")
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _variable)
    list(GET _pair 1 _expected)
    if(_variable STREQUAL "SDL2_DIR" AND NOT EXISTS "${_expected}/SDL2Config.cmake")
        continue()
    endif()
    file(REAL_PATH "${_expected}" _expected_real)
    set(_actual "${_consumer_${_variable}}")
    if(NOT IS_DIRECTORY "${_actual}")
        message(FATAL_ERROR "The consumer resolved ${_variable}='${_actual}', expected ${_expected}")
    endif()
    file(REAL_PATH "${_actual}" _actual_real)
    if(NOT _actual_real STREQUAL _expected_real)
        message(FATAL_ERROR "The consumer resolved ${_variable}='${_actual}', expected ${_expected}")
    endif()
endforeach()

_spark_run_checked("Build installed-package consumer" "${SPARK_TEST_ROOT}/build.log"
    "${CMAKE_COMMAND}" --build "${_consumer_build}" --config "${SPARK_CONFIG}" --parallel 4)
_spark_run_checked("Test installed-package consumer" "${SPARK_TEST_ROOT}/ctest.log"
    "${SPARK_CONSUMER_CTEST}" --test-dir "${_consumer_build}" -C "${SPARK_CONFIG}"
    --output-on-failure --no-tests=error)
# --no-tests=error rejects an empty run; also require the package smoke itself,
# so a consumer whose root test silently disappeared cannot pass on the rest.
if(NOT _spark_last_output MATCHES "SparkInstalledPackageSmoke[ .]+Passed")
    message(FATAL_ERROR "The consumer ctest run did not pass SparkInstalledPackageSmoke:\n${_spark_last_output}")
endif()

# The consumer's own sources and the FPS production-source slice it compiles on
# purpose are the only source-tree inputs; everything the engine provides must
# come from the prefix, which lives under SPARK_TEST_ROOT.
spark_package_consumer_boundary_violations(
    OUT_VAR _violations
    SCANNED_VAR _scanned
    CONSUMER_BUILD_DIR "${_consumer_build}"
    FORBIDDEN_ROOTS "${SPARK_SOURCE_ROOT}" "${SPARK_ENGINE_BUILD_DIR}"
    ALLOWED_ROOTS
        "${SPARK_TEST_ROOT}"
        "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke"
        "${SPARK_SOURCE_ROOT}/GameModules/SparkGameFPS/Source")
set(_scanned_depfiles "${_scanned}")
list(FILTER _scanned_depfiles INCLUDE REGEX "\\.d$")
set(_scanned_links "${_scanned}")
list(FILTER _scanned_links INCLUDE REGEX "/link\\.txt$")
if(NOT _scanned_depfiles OR NOT _scanned_links OR NOT EXISTS "${_consumer_build}/compile_commands.json")
    message(FATAL_ERROR
        "The consumer build left no dependency files, link commands or compile_commands.json to "
        "scan; the source-tree boundary cannot be proven")
endif()
if(_violations)
    list(JOIN _violations "\n  " _report)
    message(FATAL_ERROR
        "The installed-package consumer resolved engine source/build-tree paths instead of the "
        "install prefix ${_prefix}:\n  ${_report}")
endif()
list(LENGTH _scanned _scanned_count)
message(STATUS
    "Installed-package consumer passed against ${_prefix} (SparkEngine ${_engine_SPARK_ENGINE_VERSION}); "
    "${_scanned_count} dependency/link/compile files resolve nothing from the engine source or build tree")
