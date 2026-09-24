# SDK-240: the .sparkabi sidecar must carry the descriptor format, size and
# runtime ABI version that Spark/ModuleABI.h declares. They used to be passed
# to WriteSparkModuleABI.cmake as literals (1/64/1), so bumping any of them in
# the header left every freshly built module advertising the old values and the
# host rejected all of them before load.
#
# The fixture SDK below declares values no real header uses (7/72/9); a module
# configured against it must write exactly those into its sidecar. Malformed
# headers must fail at configure time instead of producing a sidecar.
foreach(required IN ITEMS SPARK_GAME_MODULE_CMAKE SPARK_SDK_INCLUDE FIXTURE CONSUMER_GENERATOR CONSUMER_COMPILER)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the module ABI sidecar derivation test")
    endif()
endforeach()

file(TO_CMAKE_PATH "${SPARK_GAME_MODULE_CMAKE}" _helper)
file(READ "${SPARK_SDK_INCLUDE}/Spark/ModuleABI.h" _real_module_abi)
file(READ "${SPARK_SDK_INCLUDE}/Spark/Version.h" _real_version)
string(REGEX MATCH "#define SPARK_SDK_VERSION ([0-9]+)" _ "${_real_version}")
set(_expected_sdk_version "${CMAKE_MATCH_1}")

# Writes a consumer project against a fixture SDK include root whose
# ModuleABI.h is `module_abi_text`, then configures (and optionally builds) it.
function(_spark_run_case name module_abi_text build result_var output_var)
    set(root "${FIXTURE}/${name}")
    file(REMOVE_RECURSE "${root}")
    file(MAKE_DIRECTORY "${root}/include/Spark" "${root}/project")
    file(WRITE "${root}/include/Spark/Version.h" "${_real_version}")
    if(NOT "${module_abi_text}" STREQUAL "<missing>")
        file(WRITE "${root}/include/Spark/ModuleABI.h" "${module_abi_text}")
    endif()
    file(WRITE "${root}/project/probe.cpp" "extern \"C\" int SparkSidecarProbe() { return 0; }\n")
    file(WRITE "${root}/project/CMakeLists.txt"
        "cmake_minimum_required(VERSION 3.25)\n"
        "project(SparkSidecarDerivation LANGUAGES CXX)\n"
        "include(\"${_helper}\")\n"
        "add_library(SparkSidecarProbe SHARED probe.cpp)\n"
        "spark_configure_module_abi(SparkSidecarProbe)\n"
        "file(GENERATE OUTPUT \"\${CMAKE_BINARY_DIR}/sidecar-path.txt\"\n"
        "    CONTENT \"$<TARGET_FILE:SparkSidecarProbe>.sparkabi\")\n")

    set(configure "${CMAKE_COMMAND}" -S "${root}/project" -B "${root}/build" -G "${CONSUMER_GENERATOR}"
        "-DSPARK_ENGINE_INCLUDE_DIR=${root}/include" "-DCMAKE_CXX_COMPILER=${CONSUMER_COMPILER}"
        "-DCMAKE_BUILD_TYPE=Release")
    if(CONSUMER_PLATFORM)
        list(APPEND configure -A "${CONSUMER_PLATFORM}")
    endif()
    if(CONSUMER_TOOLSET)
        list(APPEND configure -T "${CONSUMER_TOOLSET}")
    endif()
    if(CONSUMER_MAKE_PROGRAM)
        list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${CONSUMER_MAKE_PROGRAM}")
    endif()
    if(CONSUMER_TOOLCHAIN)
        list(APPEND configure "-DCMAKE_TOOLCHAIN_FILE=${CONSUMER_TOOLCHAIN}")
    endif()
    execute_process(COMMAND ${configure}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if("${result}" STREQUAL "0" AND build)
        execute_process(COMMAND "${CMAKE_COMMAND}" --build "${root}/build" --config Release
            RESULT_VARIABLE result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_error TIMEOUT 120)
        string(APPEND output "\n${build_output}")
        string(APPEND error "\n${build_error}")
    endif()
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${output_var} "${output}\n${error}" PARENT_SCOPE)
endfunction()

# 1. Derivation: every descriptor field comes from the header.
set(_bumped "${_real_module_abi}")
foreach(pair IN ITEMS "DESCRIPTOR_SIZE 64u|DESCRIPTOR_SIZE 72u" "DESCRIPTOR_VERSION 1u|DESCRIPTOR_VERSION 7u"
                      "RUNTIME_ABI_VERSION 1u|RUNTIME_ABI_VERSION 9u")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 _from)
    list(GET pair 1 _to)
    string(FIND "${_bumped}" "SPARK_MODULE_ABI_${_from}" _at)
    string(FIND "${_bumped}" "SPARK_MODULE_${_from}" _at_runtime)
    if(_at EQUAL -1 AND _at_runtime EQUAL -1)
        message(FATAL_ERROR "ModuleABI.h no longer declares '${_from}'; update this test")
    endif()
    string(REPLACE "_${_from}" "_${_to}" _bumped "${_bumped}")
endforeach()
_spark_run_case(derived "${_bumped}" TRUE _result _output)
if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR "Consumer against the bumped fixture SDK failed (${_result}):\n${_output}")
endif()
file(READ "${FIXTURE}/derived/build/sidecar-path.txt" _sidecar_path)
if(NOT EXISTS "${_sidecar_path}")
    message(FATAL_ERROR "Module build did not write its sidecar ${_sidecar_path}:\n${_output}")
endif()
file(READ "${_sidecar_path}" _sidecar)
foreach(line IN ITEMS "format=7" "struct_size=72" "runtime_abi_version=9" "magic=1263685715"
                      "sdk_version=${_expected_sdk_version}")
    if(NOT "\n${_sidecar}" MATCHES "\n${line}\n")
        message(FATAL_ERROR "Sidecar was not derived from Spark/ModuleABI.h: expected '${line}' in:\n${_sidecar}")
    endif()
endforeach()

# 2-4. Headers the helper cannot trust must stop configuration.
string(REPLACE "#define SPARK_MODULE_RUNTIME_ABI_VERSION 1u"
    "#define SPARK_MODULE_RUNTIME_ABI_VERSION 1u\n#define SPARK_MODULE_RUNTIME_ABI_VERSION 2u"
    _duplicate "${_real_module_abi}")
string(REPLACE "#define SPARK_MODULE_ABI_MAGIC 0x4B525053u" "#define SPARK_MODULE_ABI_MAGIC 0x4B525054u"
    _bad_magic "${_real_module_abi}")
foreach(case IN ITEMS "missing|<missing>|ModuleABI[.]h"
                      "duplicate|DUPLICATE|exactly one SPARK_MODULE_RUNTIME_ABI_VERSION"
                      "magic|MAGIC|SPARK_MODULE_ABI_MAGIC")
    string(REPLACE "|" ";" case "${case}")
    list(GET case 0 _name)
    list(GET case 1 _text)
    list(GET case 2 _needle)
    if(_text STREQUAL "DUPLICATE")
        set(_text "${_duplicate}")
    elseif(_text STREQUAL "MAGIC")
        set(_text "${_bad_magic}")
    endif()
    if("${_text}" STREQUAL "${_real_module_abi}")
        message(FATAL_ERROR "Fixture '${_name}' did not change ModuleABI.h; update this test")
    endif()
    _spark_run_case("${_name}" "${_text}" FALSE _result _output)
    if("${_result}" STREQUAL "0")
        message(FATAL_ERROR "SparkGameModule accepted a '${_name}' ModuleABI.h fixture:\n${_output}")
    endif()
    if(NOT "${_output}" MATCHES "${_needle}")
        message(FATAL_ERROR "SparkGameModule rejected '${_name}' for an unrelated reason:\n${_output}")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
message(STATUS "Module ABI sidecar fields are derived from Spark/ModuleABI.h")
