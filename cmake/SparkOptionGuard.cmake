# SparkOptionGuard.cmake - reject undeclared ENABLE_*/SPARK_*/BUILD_* options.
#
# CMake accepts any -D NAME=VALUE silently. A typo such as -DENABLE_EDTIOR=OFF
# or an option that a refactor removed configures "successfully" and quietly
# builds the default feature set, so a shipping or CI profile can drift from
# what its command line claims (CI-120: "unknown or unused options fail").
#
# A -D entry given without a type is stored with cache TYPE UNINITIALIZED.
# option() and set(... CACHE <type> ...) adopt such an entry and give it a real
# type, so an entry in the project's option namespaces that is still
# UNINITIALIZED after the whole tree has been configured was supplied on the
# command line (or by a preset string value) but never declared on this
# configuration - either a misspelling or an option that does not apply here.
#
# A -D entry given WITH a type (-DNAME:BOOL=ON, or a preset boolean or typed
# value) arrives already typed, so it would look exactly like a declared option.
# spark_capture_cli_options() closes that gap: called at the top of the root
# CMakeLists.txt, before any option() or cache declaration, it returns every
# guarded entry that still carries CMake's command-line helpstring to TYPE
# UNINITIALIZED. option() and set(CACHE) then adopt it exactly as they adopt an
# untyped -D entry (keeping its value and replacing the helpstring), and the
# undeclared ones are left for the end-of-configure check. Re-passing a declared
# typed option on a later reconfigure restores the command-line helpstring, so
# the same path handles it again.
#
# Call spark_reject_undeclared_options() once, at the very end of the root
# CMakeLists.txt, after every add_subdirectory() has had a chance to declare its
# options. Options that are declared but never read are caught statically by
# Tools/buildmatrix/check_parity.py (finding "declared-option-unread").

include_guard(GLOBAL)

set(_SPARK_OPTION_GUARD_PATTERN "^(ENABLE|SPARK|BUILD)_")
# The helpstring CMake gives every cache entry created by -D on the command line
# (including the -D arguments a configure preset expands to).
set(_SPARK_COMMAND_LINE_HELPSTRING "No help, variable specified on the command line.")

function(spark_capture_cli_options)
    get_property(_spark_cache_names DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY CACHE_VARIABLES)
    foreach(_spark_name IN LISTS _spark_cache_names)
        if(NOT _spark_name MATCHES "${_SPARK_OPTION_GUARD_PATTERN}")
            continue()
        endif()
        get_property(_spark_help CACHE "${_spark_name}" PROPERTY HELPSTRING)
        if(_spark_help STREQUAL _SPARK_COMMAND_LINE_HELPSTRING)
            set_property(CACHE "${_spark_name}" PROPERTY TYPE UNINITIALIZED)
        endif()
    endforeach()
endfunction()

function(spark_reject_undeclared_options)
    get_property(_spark_cache_names DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTY CACHE_VARIABLES)

    set(_spark_undeclared "")
    foreach(_spark_name IN LISTS _spark_cache_names)
        if(NOT _spark_name MATCHES "${_SPARK_OPTION_GUARD_PATTERN}")
            continue()
        endif()
        get_property(_spark_type CACHE "${_spark_name}" PROPERTY TYPE)
        if(_spark_type STREQUAL "UNINITIALIZED")
            list(APPEND _spark_undeclared "${_spark_name}")
        endif()
    endforeach()

    if(NOT _spark_undeclared)
        return()
    endif()

    list(SORT _spark_undeclared)
    set(_spark_report "")
    set(_spark_unset_args "")
    foreach(_spark_name IN LISTS _spark_undeclared)
        string(APPEND _spark_report "\n  ${_spark_name}=${${_spark_name}}")
        string(APPEND _spark_unset_args " -U ${_spark_name}")
    endforeach()

    message(FATAL_ERROR
        "SparkOptionGuard: undeclared build option(s) were passed to this configure:"
        "${_spark_report}\n"
        "No option() or cache declaration consumes these names on this configuration, so they "
        "would be silently ignored. Check the spelling against the declared options "
        "(cmake -LH, or SparkBuild), or drop options that do not apply to this platform. "
        "The rejected entries stay in CMakeCache.txt; remove them with:\n"
        "  cmake${_spark_unset_args} <build-dir>")
endfunction()
