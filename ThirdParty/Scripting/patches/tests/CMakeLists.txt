cmake_minimum_required(VERSION 3.20)
project(AngelScriptStackAlignment LANGUAGES C CXX)

# Standalone: no SparkEngine targets or dependencies are configured.
get_filename_component(SCRIPTING_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SDK_DIR "${SCRIPTING_DIR}/angelscript-mirror/sdk")
set(ALIGNMENT_PATCH "${SCRIPTING_DIR}/patches/angelscript-packed-bytecode.patch"
    CACHE FILEPATH "Patch to apply (can select the previous patch for a negative control)")
set(ALIGNMENT_CORE "" CACHE PATH "Optional already-patched scratch core for patch development")
option(ALIGNMENT_SANITIZE "Instrument bindings/add-ons with fatal UBSan alignment checks" OFF)
option(ALIGNMENT_GENERIC "Exercise AS_MAX_PORTABILITY generic native calls" OFF)

if(NOT ALIGNMENT_CORE)
    set(ALIGNMENT_CORE "${CMAKE_CURRENT_BINARY_DIR}/core")
    file(REMOVE_RECURSE "${ALIGNMENT_CORE}")
    file(COPY "${SDK_DIR}/angelscript/" DESTINATION "${ALIGNMENT_CORE}")
    find_package(Git REQUIRED)
    foreach(mode IN ITEMS check apply)
        set(check_flag)
        if(mode STREQUAL "check")
            set(check_flag --check)
        endif()
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}"
                "${GIT_EXECUTABLE}" apply ${check_flag} --whitespace=error-all "${ALIGNMENT_PATCH}"
            WORKING_DIRECTORY "${ALIGNMENT_CORE}"
            RESULT_VARIABLE patch_result ERROR_VARIABLE patch_error)
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR "AngelScript patch ${mode} failed: ${patch_error}")
        endif()
    endforeach()
endif()

set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(MSVC_COMPILE_FLAGS "" CACHE STRING "No unbounded /MP in this small standalone build" FORCE)
add_subdirectory("${ALIGNMENT_CORE}/projects/cmake" angelscript-build)
if(ALIGNMENT_GENERIC)
    target_compile_definitions(angelscript PUBLIC AS_MAX_PORTABILITY)
endif()

add_library(alignment_addons STATIC
    "${SDK_DIR}/add_on/scriptstdstring/scriptstdstring.cpp"
    "${SDK_DIR}/add_on/scriptarray/scriptarray.cpp"
    "${SDK_DIR}/add_on/scriptbuilder/scriptbuilder.cpp")
target_include_directories(alignment_addons PUBLIC "${SDK_DIR}/add_on")
target_link_libraries(alignment_addons PUBLIC angelscript)
add_executable(stack_alignment stack_alignment.cpp)
target_compile_features(stack_alignment PRIVATE cxx_std_17)
target_link_libraries(stack_alignment PRIVATE alignment_addons)

if(ALIGNMENT_SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR "ALIGNMENT_SANITIZE needs a Clang/GCC UBSan runtime")
    endif()
    if(MSVC)
        # LLVM's Windows UBSan libraries use the release static CRT.
        foreach(target IN ITEMS angelscript alignment_addons stack_alignment)
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
        endforeach()
    endif()
    foreach(target IN ITEMS alignment_addons stack_alignment)
        target_compile_options(${target} PRIVATE -fsanitize=alignment -fno-sanitize-recover=alignment)
    endforeach()
    target_link_options(stack_alignment PRIVATE -fsanitize=alignment)
endif()

enable_testing()
add_test(NAME stack_alignment COMMAND stack_alignment)
set_tests_properties(stack_alignment PROPERTIES TIMEOUT 90)
