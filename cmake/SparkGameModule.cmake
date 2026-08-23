#[=============================================================================[
  SparkGameModule.cmake - Helper function for creating game module DLLs

  Usage in a standalone game project:

    find_package(SparkEngine REQUIRED)

    file(GLOB_RECURSE GAME_SOURCES "Source/*.cpp" "Source/*.h")
    spark_add_game_module(MyGame ${GAME_SOURCES})

  This creates a SHARED library with the correct definitions and links
  against SparkEngineLib. The module will export CreateModule/DestroyModule
  when you use SPARK_IMPLEMENT_MODULE(YourModuleClass) in a .cpp file.
#]=============================================================================]

include_guard(GLOBAL)

set(_SPARK_MODULE_ABI_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(_spark_detect_cxx_language_abi OUTPUT_VARIABLE)
    if(DEFINED SPARK_MODULE_CXX_LANGUAGE_ABI)
        set(${OUTPUT_VARIABLE} "${SPARK_MODULE_CXX_LANGUAGE_ABI}" PARENT_SCOPE)
        return()
    endif()

    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "spark_configure_module_abi: cross-compiling requires SPARK_MODULE_CXX_LANGUAGE_ABI "
            "to be set to the target compiler's _MSVC_LANG or __cplusplus value")
    endif()

    set(_probe_source "${CMAKE_BINARY_DIR}/CMakeFiles/SparkModuleCxxLanguageProbe.cpp")
    file(WRITE "${_probe_source}" [=[
#include <cstdio>
int main()
{
#if defined(_MSC_VER)
    std::printf("%ld", static_cast<long>(_MSVC_LANG));
#else
    std::printf("%ld", static_cast<long>(__cplusplus));
#endif
    return 0;
}
]=])
    try_run(_probe_run_result _probe_compile_result
        SOURCES "${_probe_source}"
        CMAKE_FLAGS
            "-DCMAKE_CXX_STANDARD=23"
            "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
            "-DCMAKE_CXX_EXTENSIONS=OFF"
        RUN_OUTPUT_VARIABLE _probe_output)
    string(STRIP "${_probe_output}" _probe_output)
    if(NOT _probe_compile_result OR NOT _probe_run_result EQUAL 0 OR
       NOT _probe_output MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "spark_configure_module_abi: failed to detect the compiler's C++ language ABI value")
    endif()

    set(SPARK_MODULE_CXX_LANGUAGE_ABI "${_probe_output}" CACHE INTERNAL
        "Exact _MSVC_LANG/__cplusplus value for Spark module compatibility")
    set(${OUTPUT_VARIABLE} "${_probe_output}" PARENT_SCOPE)
endfunction()

function(spark_configure_module_abi TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "spark_configure_module_abi: target '${TARGET_NAME}' does not exist")
    endif()

    cmake_parse_arguments(SPARK_ABI "" "SDK_VERSION" "" ${ARGN})
    if(NOT SPARK_ABI_SDK_VERSION)
        set(_sdk_version 2)
    else()
        set(_sdk_version "${SPARK_ABI_SDK_VERSION}")
    endif()

    if(MSVC)
        set(_compiler_family 1)
        set(_compiler_abi_version "${MSVC_VERSION}")
        set(_runtime_library "$<IF:$<CONFIG:Debug>,2,1>")
        # SparkEngine publishes _ITERATOR_DEBUG_LEVEL=1 for its Debug ABI.
        set(_iterator_debug_level "$<IF:$<CONFIG:Debug>,1,0>")
    elseif(CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # clang-cl defines _MSC_VER, so mirror ModuleABI.h's first branch and
        # encode its simulated MSVC ABI version rather than the Clang version.
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _compiler_version_match
            "${CMAKE_CXX_SIMULATE_VERSION}")
        if(NOT _compiler_version_match)
            message(FATAL_ERROR
                "spark_configure_module_abi: cannot parse simulated MSVC version '${CMAKE_CXX_SIMULATE_VERSION}'")
        endif()
        set(_compiler_family 1)
        math(EXPR _compiler_abi_version "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
        set(_runtime_library "$<IF:$<CONFIG:Debug>,2,1>")
        set(_iterator_debug_level "$<IF:$<CONFIG:Debug>,1,0>")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
        set(_compiler_family 2)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _compiler_version_match
            "${CMAKE_CXX_COMPILER_VERSION}")
        if(NOT _compiler_version_match)
            message(FATAL_ERROR
                "spark_configure_module_abi: cannot parse Clang version '${CMAKE_CXX_COMPILER_VERSION}'")
        endif()
        math(EXPR _compiler_abi_version "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
        set(_runtime_library 0)
        set(_iterator_debug_level 0)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_compiler_family 3)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _compiler_version_match
            "${CMAKE_CXX_COMPILER_VERSION}")
        if(NOT _compiler_version_match)
            message(FATAL_ERROR
                "spark_configure_module_abi: cannot parse GCC version '${CMAKE_CXX_COMPILER_VERSION}'")
        endif()
        math(EXPR _compiler_abi_version "${CMAKE_MATCH_1} * 100 + ${CMAKE_MATCH_2}")
        set(_runtime_library 0)
        set(_iterator_debug_level 0)
    else()
        set(_compiler_family 0)
        set(_compiler_abi_version 0)
        set(_runtime_library 0)
        set(_iterator_debug_level 0)
    endif()

    _spark_detect_cxx_language_abi(_cxx_language_abi)
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DMODULE_PATH=$<TARGET_FILE:${TARGET_NAME}>"
            "-DSIDECAR_PATH=$<TARGET_FILE:${TARGET_NAME}>.sparkabi"
            "-DDESCRIPTOR_VERSION=1"
            "-DDESCRIPTOR_SIZE=64"
            "-DSDK_VERSION=${_sdk_version}"
            "-DRUNTIME_ABI_VERSION=1"
            "-DCOMPILER_FAMILY=${_compiler_family}"
            "-DCOMPILER_ABI_VERSION=${_compiler_abi_version}"
            "-DCXX_LANGUAGE_LEVEL=${_cxx_language_abi}"
            "-DRUNTIME_LIBRARY=${_runtime_library}"
            "-DITERATOR_DEBUG_LEVEL=${_iterator_debug_level}"
            "-DCMAKE_SIZEOF_VOID_P=${CMAKE_SIZEOF_VOID_P}"
            -P "${_SPARK_MODULE_ABI_CMAKE_DIR}/WriteSparkModuleABI.cmake"
        COMMENT "Writing ${TARGET_NAME} pre-load ABI sidecar"
        VERBATIM)
endfunction()

function(spark_add_game_module TARGET_NAME)
    add_library(${TARGET_NAME} SHARED ${ARGN})

    # Export macros
    target_compile_definitions(${TARGET_NAME} PRIVATE
        SPARK_MODULE_DLL
        SPARK_GAME_DLL
    )

    # Link against the engine static library
    target_link_libraries(${TARGET_NAME} PRIVATE Spark::SparkEngineLib)

    # Include SDK headers
    target_include_directories(${TARGET_NAME} PRIVATE
        ${SPARK_ENGINE_INCLUDE_DIR}
        ${SPARK_ENGINE_INCLUDE_DIR}/Spark
        ${SPARK_ENGINE_INCLUDE_DIR}/SparkEngine
    )

    # C++23
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_23)

    # Platform-specific settings
    if(MSVC)
        set_property(TARGET ${TARGET_NAME} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    spark_configure_module_abi(${TARGET_NAME})

    message(STATUS "spark_add_game_module: ${TARGET_NAME} configured as game module DLL")
endfunction()
