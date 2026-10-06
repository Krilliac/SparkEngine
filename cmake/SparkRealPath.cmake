# spark_real_path(<out_var> <path>)
#
# The real, link-free path of <path>, identical on every supported CMake.
#
# file(REAL_PATH) resolves Windows symbolic links and directory junctions only
# on CMake 4.x. CMake 3.x -- including the 3.31 that Visual Studio 2022 bundles
# and puts first on PATH in a developer shell -- returns the spelled path
# through a link and only expands 8.3 short names. Every "does this resolve
# outside <root>" check built on file(REAL_PATH) then silently passes on those
# hosts. On Windows this walks the path one component at a time, replaces each
# link (IS_SYMLINK is true for symlinks and junctions on every version) with
# its target, and restarts until no component is a link; file(REAL_PATH) then
# only canonicalizes the spelling. Windows resolves ".." lexically before it
# follows links, so the lexical normalization used here matches the OS.
# Elsewhere file(REAL_PATH) is realpath(3) and is used directly.
#
# A relative <path> is taken relative to CMAKE_CURRENT_SOURCE_DIR, as
# file(REAL_PATH) does. More than 40 link hops is a FATAL_ERROR (a link loop).
#
# Thread/lifetime: a pure function of the filesystem; no cache or globals.
include_guard(GLOBAL)

function(spark_real_path out_var path)
    if(NOT CMAKE_HOST_WIN32)
        file(REAL_PATH "${path}" _spark_real)
        set(${out_var} "${_spark_real}" PARENT_SCOPE)
        return()
    endif()

    file(TO_CMAKE_PATH "${path}" _spark_pending)
    cmake_path(ABSOLUTE_PATH _spark_pending NORMALIZE)
    set(_spark_hops 0)
    set(_spark_followed_link TRUE)
    while(_spark_followed_link)
        set(_spark_followed_link FALSE)
        cmake_path(GET _spark_pending ROOT_PATH _spark_walk)
        cmake_path(GET _spark_pending RELATIVE_PART _spark_relative)
        string(REPLACE "/" ";" _spark_remaining "${_spark_relative}")
        set(_spark_components "${_spark_remaining}")
        foreach(_spark_component IN LISTS _spark_components)
            list(POP_FRONT _spark_remaining)
            if(_spark_component STREQUAL "")
                continue()
            endif()
            cmake_path(APPEND _spark_walk "${_spark_component}")
            if(NOT IS_SYMLINK "${_spark_walk}")
                continue()
            endif()

            math(EXPR _spark_hops "${_spark_hops} + 1")
            if(_spark_hops GREATER 40)
                message(FATAL_ERROR "spark_real_path: too many links resolving ${path}")
            endif()
            file(READ_SYMLINK "${_spark_walk}" _spark_target)
            # CMake 3.x returns a junction's raw NT target: \??\C:\dir or \??\UNC\host\share.
            string(REGEX REPLACE "^[\\/][\\/?][?][\\/]UNC[\\/]" "//" _spark_target "${_spark_target}")
            string(REGEX REPLACE "^[\\/][\\/?][?][\\/]" "" _spark_target "${_spark_target}")
            file(TO_CMAKE_PATH "${_spark_target}" _spark_target)
            cmake_path(IS_ABSOLUTE _spark_target _spark_target_is_absolute)
            if(NOT _spark_target_is_absolute)
                cmake_path(GET _spark_walk PARENT_PATH _spark_link_parent)
                cmake_path(APPEND _spark_link_parent "${_spark_target}" OUTPUT_VARIABLE _spark_target)
            endif()
            foreach(_spark_rest IN LISTS _spark_remaining)
                if(NOT _spark_rest STREQUAL "")
                    cmake_path(APPEND _spark_target "${_spark_rest}")
                endif()
            endforeach()
            cmake_path(NORMAL_PATH _spark_target OUTPUT_VARIABLE _spark_pending)
            set(_spark_followed_link TRUE)
            break()
        endforeach()
    endwhile()

    # No component is a link any more; this only canonicalizes the spelling.
    file(REAL_PATH "${_spark_pending}" _spark_real)
    set(${out_var} "${_spark_real}" PARENT_SCOPE)
endfunction()
