include_guard(GLOBAL)

# C++ standard-library types cross the host/module boundary. Directory flags
# select the producer's library but are not part of installed target exports.
function(spark_detect_cxx_runtime output)
    if(WIN32)
        # Keep the existing Windows CRT/toolset contract unchanged.
        set(${output} "" PARENT_SCOPE)
        return()
    endif()

    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    # Identity is a header property; do not require a runnable/linkable probe.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    if(CMAKE_CONFIGURATION_TYPES)
        set(_configs ${CMAKE_CONFIGURATION_TYPES})
    elseif(CMAKE_BUILD_TYPE)
        set(_configs "${CMAKE_BUILD_TYPE}")
    else()
        set(_configs "__default")
    endif()

    set(_runtime "")
    foreach(_config IN LISTS _configs)
        if(_config STREQUAL "__default")
            # An unset normal variable would reveal a cached configuration.
            # Match the producer's empty build type, not that cache fallback.
            set(CMAKE_TRY_COMPILE_CONFIGURATION "")
        else()
            set(CMAKE_TRY_COMPILE_CONFIGURATION "${_config}")
        endif()
        # Do not reuse a result from a different configuration or toolchain.
        unset(_spark_has_libcxx)
        unset(_spark_has_libstdcxx)
        unset(_spark_has_libcxx CACHE)
        unset(_spark_has_libstdcxx CACHE)
        check_cxx_source_compiles("#include <version>
#ifndef _LIBCPP_VERSION
#error Not libc++
#endif
int main() { return 0; }" _spark_has_libcxx)
        check_cxx_source_compiles("#include <version>
#ifndef __GLIBCXX__
#error Not libstdc++
#endif
int main() { return 0; }" _spark_has_libstdcxx)
        if(_spark_has_libcxx AND NOT _spark_has_libstdcxx)
            set(_selected "libc++")
        elseif(_spark_has_libstdcxx AND NOT _spark_has_libcxx)
            set(_selected "libstdc++")
        else()
            message(FATAL_ERROR "Cannot identify the Spark C++ runtime for configuration '${_config}'")
        endif()
        if(NOT _runtime STREQUAL "" AND NOT _runtime STREQUAL _selected)
            message(FATAL_ERROR "Spark SDK configurations must use the same C++ runtime: ${_runtime} vs ${_selected}")
        endif()
        set(_runtime "${_selected}")
    endforeach()
    unset(_spark_has_libcxx CACHE)
    unset(_spark_has_libstdcxx CACHE)
    cmake_pop_check_state()
    set(${output} "${_runtime}" PARENT_SCOPE)
endfunction()

# Also used by configure-only export/import contracts. This accepts the family
# already measured above, never arbitrary producer flags or filesystem paths.
function(spark_apply_cxx_runtime target visibility runtime)
    if(WIN32)
        return()
    endif()
    if(NOT visibility STREQUAL "PUBLIC" AND NOT visibility STREQUAL "INTERFACE")
        message(FATAL_ERROR "Spark C++ runtime requirements must be PUBLIC or INTERFACE")
    endif()
    if(runtime STREQUAL "libc++")
        # Do not silently drop this ABI requirement for an unsupported driver.
        # A GCC consumer cannot select libc++ and will reject this option.
        set(_compile "$<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>")
        set(_link "$<$<LINK_LANGUAGE:CXX>:-stdlib=libc++>")
    elseif(runtime STREQUAL "libstdc++")
        # GCC already uses libstdc++; only Clang needs an explicit selection.
        set(_compile "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-stdlib=libstdc++>")
        set(_link "$<$<LINK_LANG_AND_ID:CXX,Clang,AppleClang>:-stdlib=libstdc++>")
    else()
        message(FATAL_ERROR "Unsupported Spark C++ runtime '${runtime}'")
    endif()
    target_compile_options(${target} ${visibility} "${_compile}")
    target_link_options(${target} ${visibility} "${_link}")
endfunction()
