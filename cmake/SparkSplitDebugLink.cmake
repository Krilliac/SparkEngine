cmake_minimum_required(VERSION 3.25)

# SparkSplitDebugLink.cmake -- link, then split private symbols (BLD-100).
#
# Used as the C/CXX LINKER_LAUNCHER of every ELF executable and shared library
# when STRIP_DEBUG_SYMBOLS=ON (root CMakeLists.txt, section 6):
#
#   cmake -DSPARK_OBJCOPY=<objcopy> -P SparkSplitDebugLink.cmake -- <link command...>
#
# It runs the link command unchanged, then turns the linked image <out> into
#
#   <out>.debug  objcopy --only-keep-debug: DWARF and the symbol table, plus
#                the same NT_GNU_BUILD_ID note as the image;
#   <out>        objcopy --strip-all --add-gnu-debuglink: the stripped runtime
#                image, naming <out>.debug and its CRC-32.
#
# The split runs inside the link step rather than as a POST_BUILD command so
# that every POST_BUILD step (game-module .sparkabi sidecars hash the module
# binary; SparkEngine copies modules beside itself) sees the final stripped
# image. If the split fails, the image is deleted so the next build relinks it
# instead of treating an unstripped image as up to date.
# tools/shipping_symbol_manifest.py verifies the result.

if(NOT SPARK_OBJCOPY)
    message(FATAL_ERROR "SparkSplitDebugLink: SPARK_OBJCOPY is not set")
endif()

set(_spark_link_command "")
set(_spark_link_output "")
set(_spark_after_separator FALSE)
set(_spark_next_is_output FALSE)
math(EXPR _spark_last_argument "${CMAKE_ARGC} - 1")
foreach(_spark_index RANGE ${_spark_last_argument})
    set(_spark_argument "${CMAKE_ARGV${_spark_index}}")
    if(NOT _spark_after_separator)
        if(_spark_argument STREQUAL "--")
            set(_spark_after_separator TRUE)
        endif()
        continue()
    endif()
    # A semicolon would split one argument into several list elements.
    string(FIND "${_spark_argument}" ";" _spark_semicolon)
    if(NOT _spark_semicolon EQUAL -1)
        message(FATAL_ERROR "SparkSplitDebugLink: link argument contains ';': ${_spark_argument}")
    endif()
    list(APPEND _spark_link_command "${_spark_argument}")
    if(_spark_next_is_output)
        set(_spark_link_output "${_spark_argument}")
        set(_spark_next_is_output FALSE)
    elseif(_spark_argument STREQUAL "-o")
        set(_spark_next_is_output TRUE)
    endif()
endforeach()

if(NOT _spark_link_command)
    message(FATAL_ERROR "SparkSplitDebugLink: no link command after '--'")
endif()
if(_spark_link_output STREQUAL "")
    message(FATAL_ERROR "SparkSplitDebugLink: link command has no '-o <output>': ${_spark_link_command}")
endif()

execute_process(COMMAND ${_spark_link_command} RESULT_VARIABLE _spark_link_result)
if(NOT _spark_link_result EQUAL 0)
    message(FATAL_ERROR "SparkSplitDebugLink: link failed (${_spark_link_result}): ${_spark_link_output}")
endif()

set(_spark_debug_file "${_spark_link_output}.debug")
file(REMOVE "${_spark_debug_file}")
execute_process(
    COMMAND "${SPARK_OBJCOPY}" --only-keep-debug "${_spark_link_output}" "${_spark_debug_file}"
    RESULT_VARIABLE _spark_keep_result)
if(_spark_keep_result EQUAL 0)
    execute_process(
        COMMAND "${SPARK_OBJCOPY}" --strip-all "--add-gnu-debuglink=${_spark_debug_file}" "${_spark_link_output}"
        RESULT_VARIABLE _spark_strip_result)
endif()
if(NOT _spark_keep_result EQUAL 0 OR NOT _spark_strip_result EQUAL 0)
    file(REMOVE "${_spark_link_output}" "${_spark_debug_file}")
    message(FATAL_ERROR "SparkSplitDebugLink: splitting debug info from ${_spark_link_output} failed")
endif()
