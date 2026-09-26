# Source-tree boundary check for an installed-package consumer build.
#
# An installed SparkEngine package is only proven consumable if a consumer
# compiles and links against the install prefix alone. A package export that
# still carries a build-tree include directory, or an imported library that
# points back into the engine build, links fine on the machine that produced it
# and fails everywhere else. This helper scans what the consumer build actually
# resolved and reports every absolute path that reaches back into the engine
# source or build tree:
#
#   *.d                   compiler and linker dependency files (resolved
#                         headers and libraries, written by the build itself)
#   link.txt              the exact link command lines of each target
#   compile_commands.json the declared include directories and defines
#
# Paths are lexically normalized before comparison, so an escape such as
# <allowed>/../../SparkEngine/Source/X.h is judged by where it really points.
# Only absolute paths are considered: the Unix Makefiles generator (the only
# one whose depfiles and link.txt survive the build) emits absolute paths for
# every include directory and imported library.
#
# spark_package_consumer_boundary_violations(
#     OUT_VAR <var>                 receives "<scanned file>: <path>" entries
#     [SCANNED_VAR <var>]           receives the list of files that were scanned
#     CONSUMER_BUILD_DIR <dir>      consumer build tree to scan
#     FORBIDDEN_ROOTS <dir>...      engine source and build roots
#     ALLOWED_ROOTS <dir>...        consumer-owned exceptions inside them
# )

function(_spark_consumer_boundary_normalize _out _path)
    cmake_path(SET _normalized NORMALIZE "${_path}")
    # Drop a trailing separator so prefix checks compare whole components.
    string(REGEX REPLACE "(.)/+$" "\\1" _normalized "${_normalized}")
    set(${_out} "${_normalized}" PARENT_SCOPE)
endfunction()

function(_spark_consumer_boundary_is_under _out _path _root)
    if(_path STREQUAL _root)
        set(${_out} TRUE PARENT_SCOPE)
        return()
    endif()
    string(LENGTH "${_root}/" _root_length)
    string(SUBSTRING "${_path}" 0 ${_root_length} _head)
    if(_head STREQUAL "${_root}/")
        set(${_out} TRUE PARENT_SCOPE)
    else()
        set(${_out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(spark_package_consumer_boundary_violations)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "" "OUT_VAR;SCANNED_VAR;CONSUMER_BUILD_DIR" "FORBIDDEN_ROOTS;ALLOWED_ROOTS")
    if(NOT _arg_OUT_VAR OR NOT _arg_CONSUMER_BUILD_DIR OR NOT _arg_FORBIDDEN_ROOTS)
        message(FATAL_ERROR
            "spark_package_consumer_boundary_violations requires OUT_VAR, CONSUMER_BUILD_DIR and FORBIDDEN_ROOTS")
    endif()
    if(NOT IS_DIRECTORY "${_arg_CONSUMER_BUILD_DIR}")
        message(FATAL_ERROR "Consumer build directory does not exist: ${_arg_CONSUMER_BUILD_DIR}")
    endif()

    # Compare against both the spelled and the symlink-resolved form of every
    # root, so a checkout reached through a symlink cannot hide a leak.
    foreach(_kind IN ITEMS FORBIDDEN ALLOWED)
        set(_roots_${_kind})
        foreach(_root IN LISTS _arg_${_kind}_ROOTS)
            _spark_consumer_boundary_normalize(_normalized "${_root}")
            list(APPEND _roots_${_kind} "${_normalized}")
            if(EXISTS "${_root}")
                file(REAL_PATH "${_root}" _real)
                _spark_consumer_boundary_normalize(_normalized "${_real}")
                list(APPEND _roots_${_kind} "${_normalized}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _roots_${_kind})
    endforeach()

    file(GLOB_RECURSE _scanned
        LIST_DIRECTORIES FALSE
        "${_arg_CONSUMER_BUILD_DIR}/*.d"
        "${_arg_CONSUMER_BUILD_DIR}/*/link.txt"
        "${_arg_CONSUMER_BUILD_DIR}/compile_commands.json")
    list(SORT _scanned)

    set(_violations)
    foreach(_file IN LISTS _scanned)
        file(READ "${_file}" _content)
        # Make the text safe to treat as a CMake list, keep make-escaped spaces
        # inside their path, then split on every separator these files use.
        string(REPLACE ";" " " _content "${_content}")
        string(REPLACE "[" " " _content "${_content}")
        string(REPLACE "]" " " _content "${_content}")
        string(REPLACE "\\ " "%SPARK_SPACE%" _content "${_content}")
        string(REGEX REPLACE "[ \t\r\n\"',=:\\\\]+" ";" _tokens "${_content}")
        list(REMOVE_DUPLICATES _tokens)
        foreach(_token IN LISTS _tokens)
            string(REPLACE "%SPARK_SPACE%" " " _token "${_token}")
            string(REGEX REPLACE "^-(isystem|iquote|idirafter|I|L)" "" _token "${_token}")
            if(NOT _token MATCHES "^/")
                continue()
            endif()
            # Judge the spelled path and, when it exists, where it really
            # resolves: a symlink must not carry a leak past the roots.
            _spark_consumer_boundary_normalize(_path "${_token}")
            set(_candidates "${_path}")
            if(EXISTS "${_path}")
                file(REAL_PATH "${_path}" _real)
                list(APPEND _candidates "${_real}")
            endif()
            foreach(_candidate IN LISTS _candidates)
                set(_forbidden FALSE)
                foreach(_root IN LISTS _roots_FORBIDDEN)
                    _spark_consumer_boundary_is_under(_under "${_candidate}" "${_root}")
                    if(_under)
                        set(_forbidden TRUE)
                        break()
                    endif()
                endforeach()
                set(_allowed FALSE)
                foreach(_root IN LISTS _roots_ALLOWED)
                    _spark_consumer_boundary_is_under(_under "${_candidate}" "${_root}")
                    if(_under)
                        set(_allowed TRUE)
                        break()
                    endif()
                endforeach()
                if(_forbidden AND NOT _allowed)
                    list(APPEND _violations "${_file}: ${_path}")
                    break()
                endif()
            endforeach()
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES _violations)
    set(${_arg_OUT_VAR} "${_violations}" PARENT_SCOPE)
    if(_arg_SCANNED_VAR)
        set(${_arg_SCANNED_VAR} "${_scanned}" PARENT_SCOPE)
    endif()
endfunction()
