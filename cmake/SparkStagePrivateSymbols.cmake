# SparkStagePrivateSymbols.cmake - stage a configured tree as a release would and
# map every staged image to its private symbols (BLD-100).
#
# Run in script mode by the ShippingManifest_PrivateSymbols CTest:
#
#   cmake -DSTAGE=<dir> -DBUILD_DIR=<build> [-DCONFIG=<config>]
#         -DPYTHON=<python> -DTOOL=<tools/shipping_symbol_manifest.py>
#         -P cmake/SparkStagePrivateSymbols.cmake
#
# The packaged components (runtime, tools, samples) install to STAGE/runtime and
# the private "symbols" component to STAGE/private, exactly as build.yml's
# "Stage and map Shipping private symbols" step does for windows-shipping. The
# manifest tool then requires every staged EXE/DLL (PE: RSDS GUID+age and a bare
# PDB name) or ELF image (GNU build-id and .gnu_debuglink) to map to exactly one
# staged symbol file, no symbol file in the runtime tree, and at least one image.
# Any failing step fails the test. It is a cmake -P script rather than a shell
# body so the same registration runs on MSVC and ELF trees.

cmake_minimum_required(VERSION 3.25)

foreach(_spark_required IN ITEMS STAGE BUILD_DIR PYTHON TOOL)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "SparkStagePrivateSymbols: -D${_spark_required}=... is required")
    endif()
endforeach()
# A single-config tree without CMAKE_BUILD_TYPE passes an empty CONFIG.
set(_spark_config_args "")
if(NOT "${CONFIG}" STREQUAL "")
    set(_spark_config_args --config "${CONFIG}")
endif()

file(REMOVE_RECURSE "${STAGE}")

foreach(_spark_component IN ITEMS runtime tools samples)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" ${_spark_config_args}
                --component ${_spark_component} --prefix "${STAGE}/runtime"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" ${_spark_config_args}
            --component symbols --prefix "${STAGE}/private"
    COMMAND_ERROR_IS_FATAL ANY)

execute_process(
    COMMAND "${PYTHON}" -B "${TOOL}"
            --images "${STAGE}/runtime"
            --symbols "${STAGE}/private/symbols"
            --output "${STAGE}/shipping-symbol-manifest.json"
    COMMAND_ERROR_IS_FATAL ANY)
