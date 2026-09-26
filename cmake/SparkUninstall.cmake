# Manifest-driven uninstall for a `cmake --install` tree.
#
#   cmake -DPREFIX=<install prefix> -DMANIFEST=<build>/install_manifest.txt \
#         -P cmake/SparkUninstall.cmake
#
# The manifest is the list of absolute paths CMake wrote while installing
# (install_manifest.txt, or install_manifest_<component>.txt for a component
# install). Only those paths are touched, so anything the product or its users
# created later under the prefix (saves, settings, caches) is preserved.
#
# Safety contract (validated for every entry before anything is removed, so a
# rejected manifest leaves the prefix exactly as it was):
#   - PREFIX must be absolute and must not be a filesystem root.
#   - Each entry must be absolute, already normalized (no "." or ".." segments),
#     and lie strictly beneath PREFIX, both lexically and after resolving the
#     real path of its parent directory, so a symlinked or junctioned parent
#     cannot redirect removal outside the prefix.
#   - An entry that exists must be a regular file. Symlinks and directories are
#     refused: the SparkEngine install tree contains regular files only (the
#     SDL2 install step replaces its SONAME links with copies).
#   - Manifests containing semicolons are refused because CMake would split
#     such a path into several list elements, and entries whose square brackets
#     are unbalanced are refused because CMake list handling would merge them
#     with their neighbors.
# Entries that are already absent are skipped, so uninstalling twice succeeds.
# After removal, directories that held removed entries (and their ancestors up
# to, but excluding, PREFIX) are deleted only when empty and not links. The
# emptiness test escapes glob metacharacters, so a prefix such as
# "C:/Games [Beta]" is listed literally and never mistaken for an empty
# directory.
#
# DESTDIR is honored the same way `cmake --install` honors it. The manifest
# records paths without DESTDIR, so entries are validated against PREFIX and
# then removed at $ENV{DESTDIR}<entry>, with a leading drive letter ("C:")
# dropped first exactly as `cmake --install` drops it. A network (//host) path
# combined with DESTDIR is refused, as `cmake --install` refuses it.
cmake_minimum_required(VERSION 3.25)

foreach(_spark_required IN ITEMS PREFIX MANIFEST)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "SparkUninstall: -D${_spark_required}=... is required")
    endif()
endforeach()

if(NOT EXISTS "${MANIFEST}" OR IS_DIRECTORY "${MANIFEST}")
    message(FATAL_ERROR "SparkUninstall: install manifest not found: ${MANIFEST}")
endif()

file(TO_CMAKE_PATH "${PREFIX}" _spark_root)
file(TO_CMAKE_PATH "$ENV{DESTDIR}" _spark_destdir)
cmake_path(IS_ABSOLUTE _spark_root _spark_root_is_absolute)
if(NOT _spark_root_is_absolute)
    message(FATAL_ERROR "SparkUninstall: PREFIX must be absolute: ${_spark_root}")
endif()
cmake_path(NORMAL_PATH _spark_root)
string(REGEX REPLACE "/+$" "" _spark_root "${_spark_root}")
cmake_path(GET _spark_root ROOT_PATH _spark_root_of_root)
string(REGEX REPLACE "/+$" "" _spark_root_of_root_trimmed "${_spark_root_of_root}")
if(_spark_root STREQUAL "" OR _spark_root STREQUAL _spark_root_of_root_trimmed)
    message(FATAL_ERROR "SparkUninstall: refusing to uninstall from a filesystem root: ${PREFIX}")
endif()

# True when child lies strictly beneath parent (or equals it when ALLOW_EQUAL).
# Comparisons are case-insensitive on Windows, where the filesystem is.
function(_spark_uninstall_is_beneath parent child out_var)
    cmake_parse_arguments(PARSE_ARGV 3 _spark "ALLOW_EQUAL" "" "")
    set(_spark_parent "${parent}")
    set(_spark_child "${child}")
    if(WIN32)
        string(TOLOWER "${_spark_parent}" _spark_parent)
        string(TOLOWER "${_spark_child}" _spark_child)
    endif()
    cmake_path(IS_PREFIX _spark_parent "${_spark_child}" NORMALIZE _spark_is_prefix)
    cmake_path(COMPARE "${_spark_parent}" EQUAL "${_spark_child}" _spark_is_equal)
    if(_spark_is_prefix AND (_spark_ALLOW_EQUAL OR NOT _spark_is_equal))
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Maps an install path to where it lives on disk once DESTDIR is applied.
function(_spark_uninstall_staged_path path out_var)
    if(_spark_destdir STREQUAL "")
        set(${out_var} "${path}" PARENT_SCOPE)
    elseif(path MATCHES "^//")
        message(FATAL_ERROR "SparkUninstall: a network path cannot be combined with DESTDIR: ${path}")
    elseif(path MATCHES "^[A-Za-z]:/")
        string(SUBSTRING "${path}" 2 -1 _spark_driveless)
        set(${out_var} "${_spark_destdir}${_spark_driveless}" PARENT_SCOPE)
    else()
        set(${out_var} "${_spark_destdir}${path}" PARENT_SCOPE)
    endif()
endfunction()

file(READ "${MANIFEST}" _spark_manifest_text)
if(_spark_manifest_text MATCHES ";")
    message(FATAL_ERROR
        "SparkUninstall: ${MANIFEST} contains a semicolon; refusing a path CMake would split")
endif()
string(REPLACE "\r" "" _spark_manifest_text "${_spark_manifest_text}")
string(REPLACE "\n" ";" _spark_manifest_entries "${_spark_manifest_text}")

_spark_uninstall_staged_path("${_spark_root}" _spark_staged_root)
set(_spark_real_root "")
if(IS_DIRECTORY "${_spark_staged_root}")
    file(REAL_PATH "${_spark_staged_root}" _spark_real_root)
endif()

# Phase 1: validate every entry. Nothing is removed unless all of them pass.
set(_spark_to_remove "")
set(_spark_absent_count 0)
foreach(_spark_entry IN LISTS _spark_manifest_entries)
    if(_spark_entry STREQUAL "")
        continue()
    endif()
    # An entry with unbalanced square brackets swallows the following entries
    # when CMake expands the list, so one element would then hold several paths.
    string(REGEX REPLACE "[^][]" "" _spark_brackets "${_spark_entry}")
    set(_spark_previous_brackets "")
    while(NOT _spark_brackets STREQUAL _spark_previous_brackets)
        set(_spark_previous_brackets "${_spark_brackets}")
        string(REPLACE "[]" "" _spark_brackets "${_spark_brackets}")
    endwhile()
    if(NOT _spark_brackets STREQUAL "" OR _spark_entry MATCHES ";")
        message(FATAL_ERROR "SparkUninstall: manifest entry has unbalanced square brackets: ${_spark_entry}")
    endif()
    cmake_path(IS_ABSOLUTE _spark_entry _spark_entry_is_absolute)
    if(NOT _spark_entry_is_absolute)
        message(FATAL_ERROR "SparkUninstall: manifest entry is not absolute: ${_spark_entry}")
    endif()
    set(_spark_normal_entry "${_spark_entry}")
    cmake_path(NORMAL_PATH _spark_normal_entry)
    if(NOT _spark_normal_entry STREQUAL _spark_entry)
        message(FATAL_ERROR "SparkUninstall: manifest entry is not a normalized path: ${_spark_entry}")
    endif()
    _spark_uninstall_is_beneath("${_spark_root}" "${_spark_entry}" _spark_entry_is_beneath)
    if(NOT _spark_entry_is_beneath)
        message(FATAL_ERROR
            "SparkUninstall: manifest entry lies outside the install prefix ${_spark_root}: ${_spark_entry}")
    endif()

    _spark_uninstall_staged_path("${_spark_entry}" _spark_target)
    if(IS_SYMLINK "${_spark_target}")
        message(FATAL_ERROR "SparkUninstall: refusing to remove a link: ${_spark_target}")
    endif()
    if(NOT EXISTS "${_spark_target}")
        math(EXPR _spark_absent_count "${_spark_absent_count} + 1")
        continue()
    endif()
    if(IS_DIRECTORY "${_spark_target}")
        message(FATAL_ERROR "SparkUninstall: manifest entry is a directory, not a file: ${_spark_target}")
    endif()

    cmake_path(GET _spark_target PARENT_PATH _spark_target_parent)
    file(REAL_PATH "${_spark_target_parent}" _spark_real_parent)
    _spark_uninstall_is_beneath("${_spark_real_root}" "${_spark_real_parent}" _spark_parent_is_beneath
        ALLOW_EQUAL)
    if(NOT _spark_parent_is_beneath)
        message(FATAL_ERROR
            "SparkUninstall: manifest entry resolves outside the install prefix "
            "(${_spark_real_parent} is not beneath ${_spark_real_root}): ${_spark_target}")
    endif()
    list(APPEND _spark_to_remove "${_spark_entry}")
endforeach()
list(REMOVE_DUPLICATES _spark_to_remove)

# Phase 2: remove the validated files and remember every directory that may
# have become empty, up to but excluding the prefix itself.
set(_spark_candidate_dirs "")
foreach(_spark_entry IN LISTS _spark_to_remove)
    _spark_uninstall_staged_path("${_spark_entry}" _spark_target)
    message(VERBOSE "SparkUninstall: removing ${_spark_target}")
    file(REMOVE "${_spark_target}")
    if(EXISTS "${_spark_target}" OR IS_SYMLINK "${_spark_target}")
        message(FATAL_ERROR "SparkUninstall: could not remove ${_spark_target}")
    endif()
    cmake_path(GET _spark_entry PARENT_PATH _spark_dir)
    while(TRUE)
        _spark_uninstall_is_beneath("${_spark_root}" "${_spark_dir}" _spark_dir_is_beneath)
        if(NOT _spark_dir_is_beneath)
            break()
        endif()
        list(APPEND _spark_candidate_dirs "${_spark_dir}")
        cmake_path(GET _spark_dir PARENT_PATH _spark_dir)
    endwhile()
endforeach()
list(REMOVE_DUPLICATES _spark_candidate_dirs)
# A child path sorts after its parent, so descending order prunes leaves first.
list(SORT _spark_candidate_dirs ORDER DESCENDING)

set(_spark_pruned_count 0)
foreach(_spark_candidate IN LISTS _spark_candidate_dirs)
    _spark_uninstall_staged_path("${_spark_candidate}" _spark_dir)
    if(IS_SYMLINK "${_spark_dir}" OR NOT IS_DIRECTORY "${_spark_dir}")
        continue()
    endif()
    # file(GLOB) reads its argument as a pattern, so "[", "*" and "?" in the
    # path are escaped as one-character classes. The escaped path must match
    # the directory itself before an empty child listing is trusted; otherwise
    # the directory is kept.
    string(REGEX REPLACE "([][*?])" "[\\1]" _spark_glob_dir "${_spark_dir}")
    file(GLOB _spark_dir_self LIST_DIRECTORIES true "${_spark_glob_dir}")
    set(_spark_dir_expected "${_spark_dir}")
    if(WIN32)
        string(TOLOWER "${_spark_dir_self}" _spark_dir_self)
        string(TOLOWER "${_spark_dir_expected}" _spark_dir_expected)
    endif()
    if(NOT _spark_dir_self STREQUAL _spark_dir_expected)
        message(WARNING "SparkUninstall: could not list ${_spark_dir}; leaving it in place")
        continue()
    endif()
    file(GLOB _spark_dir_children LIST_DIRECTORIES true "${_spark_glob_dir}/*")
    if(NOT _spark_dir_children STREQUAL "")
        continue()
    endif()
    file(REMOVE_RECURSE "${_spark_dir}")
    math(EXPR _spark_pruned_count "${_spark_pruned_count} + 1")
endforeach()

list(LENGTH _spark_to_remove _spark_removed_count)
message(STATUS
    "SparkUninstall: removed ${_spark_removed_count} file(s), skipped ${_spark_absent_count} already absent, "
    "pruned ${_spark_pruned_count} empty director(ies) under ${_spark_staged_root}")
