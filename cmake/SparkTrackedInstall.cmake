include_guard(GLOBAL)

# Install a source-tree directory without allowing untracked checkout content
# into a package. SOURCE and DESTINATION are deliberately relative: SOURCE is
# resolved beneath CMAKE_SOURCE_DIR and DESTINATION beneath CMAKE_INSTALL_PREFIX.
#
# Exclusion regexes are matched against forward-slash paths with a leading slash
# (for example, /tests(/|$) or \\.pyc$). In a source archive without local .git
# metadata, the same expressions are passed to install(DIRECTORY ... REGEX ...).
function(spark_install_tracked_directory)
    if(ARGC EQUAL 0)
        message(FATAL_ERROR "spark_install_tracked_directory requires arguments")
    endif()

    math(EXPR _spark_last_argument "${ARGC} - 1")
    foreach(_spark_index RANGE 0 ${_spark_last_argument})
        set(_spark_raw_argument "${ARGV${_spark_index}}")
        if(_spark_raw_argument MATCHES "[\r\n;]")
            message(FATAL_ERROR
                "spark_install_tracked_directory rejects newline and semicolon characters in arguments")
        endif()
    endforeach()

    cmake_parse_arguments(PARSE_ARGV 0 SPARK_TRACKED
        "FLATTEN"
        "SOURCE;DESTINATION;COMPONENT"
        "EXCLUDE_REGEXES;INCLUDE_REGEXES")
    if(SPARK_TRACKED_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "spark_install_tracked_directory received unknown arguments: "
            "${SPARK_TRACKED_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_spark_required IN ITEMS SOURCE DESTINATION COMPONENT)
        if(NOT DEFINED SPARK_TRACKED_${_spark_required}
           OR SPARK_TRACKED_${_spark_required} STREQUAL "")
            message(FATAL_ERROR
                "spark_install_tracked_directory requires ${_spark_required}")
        endif()
    endforeach()

    foreach(_spark_path_name IN ITEMS SOURCE DESTINATION)
        set(_spark_path "${SPARK_TRACKED_${_spark_path_name}}")
        string(REPLACE "\\" "/" _spark_path "${_spark_path}")
        cmake_path(IS_ABSOLUTE _spark_path _spark_path_is_absolute)
        if(_spark_path_is_absolute
           OR _spark_path MATCHES "(^|/)\\.\\.?(/|$)"
           OR _spark_path MATCHES "(^|/)[^/]*:"
           OR NOT _spark_path STREQUAL SPARK_TRACKED_${_spark_path_name})
            message(FATAL_ERROR
                "${_spark_path_name} must be a normalized, source-relative forward-slash path: "
                "${SPARK_TRACKED_${_spark_path_name}}")
        endif()
        cmake_path(NORMAL_PATH _spark_path OUTPUT_VARIABLE _spark_normal_path)
        if(NOT _spark_normal_path STREQUAL _spark_path)
            message(FATAL_ERROR
                "${_spark_path_name} is not normalized: ${SPARK_TRACKED_${_spark_path_name}}")
        endif()
        set(SPARK_TRACKED_${_spark_path_name} "${_spark_normal_path}")
    endforeach()

    if(NOT SPARK_TRACKED_COMPONENT MATCHES "^[A-Za-z0-9_.+-]+$")
        message(FATAL_ERROR
            "COMPONENT contains unsupported characters: ${SPARK_TRACKED_COMPONENT}")
    endif()
    foreach(_spark_regex_list IN ITEMS INCLUDE_REGEXES EXCLUDE_REGEXES)
        foreach(_spark_regex IN LISTS SPARK_TRACKED_${_spark_regex_list})
            if(_spark_regex STREQUAL "" OR _spark_regex MATCHES "[\r\n;]")
                message(FATAL_ERROR
                    "${_spark_regex_list} entries must be non-empty and contain no newlines or semicolons")
            endif()
            # Force CMake to parse the expression now, so a malformed caller
            # regex fails during configuration rather than during packaging.
            if("/spark-regex-validation" MATCHES "${_spark_regex}")
            endif()
        endforeach()
    endforeach()

    set(_spark_source_root "${CMAKE_SOURCE_DIR}")
    cmake_path(NORMAL_PATH _spark_source_root OUTPUT_VARIABLE _spark_source_root)
    cmake_path(ABSOLUTE_PATH SPARK_TRACKED_SOURCE
        BASE_DIRECTORY "${_spark_source_root}"
        NORMALIZE
        OUTPUT_VARIABLE _spark_source_directory)
    cmake_path(IS_PREFIX _spark_source_root "${_spark_source_directory}"
        NORMALIZE _spark_source_is_bounded)
    if(NOT _spark_source_is_bounded OR NOT IS_DIRECTORY "${_spark_source_directory}")
        message(FATAL_ERROR
            "SOURCE must name an existing directory beneath CMAKE_SOURCE_DIR: "
            "${SPARK_TRACKED_SOURCE}")
    endif()
    file(REAL_PATH "${_spark_source_root}" _spark_source_root_real)
    file(REAL_PATH "${_spark_source_directory}" _spark_source_directory_real)
    cmake_path(IS_PREFIX _spark_source_root_real "${_spark_source_directory_real}"
        NORMALIZE _spark_source_real_is_bounded)
    if(NOT _spark_source_real_is_bounded)
        message(FATAL_ERROR "SOURCE resolves outside CMAKE_SOURCE_DIR: ${SPARK_TRACKED_SOURCE}")
    endif()

    if(NOT EXISTS "${_spark_source_root}/.git")
        # A source distribution has no index to consult. Enumerate its bounded
        # regular files, then pass them through the same validation and snapshot
        # path used for a checkout. In particular, never delegate links or
        # junctions to install(DIRECTORY), which can follow them later.
        file(GLOB_RECURSE _spark_tracked_paths
            LIST_DIRECTORIES FALSE
            RELATIVE "${_spark_source_root}"
            "${_spark_source_directory}/*")
        set(_spark_tracked_path_prefix "${SPARK_TRACKED_SOURCE}/")
        message(STATUS
            "Spark tracked install: ${SPARK_TRACKED_SOURCE} uses source-distribution fallback")
    else()
        find_package(Git QUIET)
        if(NOT Git_FOUND OR NOT GIT_EXECUTABLE)
            message(FATAL_ERROR
                "${_spark_source_root}/.git exists, but Git is unavailable; refusing an untracked-content fallback")
        endif()

        # Profile migrations can leave initialized submodules owned by the
        # prior Windows SID. Locate the nearest bounded .git marker and permit
        # only that exact repository for these read-only commands; never alter
        # global Git safe.directory policy.
        set(_spark_git_safe_hint "${_spark_source_directory}")
        while(NOT EXISTS "${_spark_git_safe_hint}/.git")
            if(_spark_git_safe_hint STREQUAL _spark_source_root)
                message(FATAL_ERROR
                    "Could not find bounded Git metadata for ${_spark_source_directory}")
            endif()
            cmake_path(GET _spark_git_safe_hint PARENT_PATH _spark_git_safe_parent)
            if(_spark_git_safe_parent STREQUAL _spark_git_safe_hint)
                message(FATAL_ERROR
                    "Git metadata search escaped its source root for ${_spark_source_directory}")
            endif()
            set(_spark_git_safe_hint "${_spark_git_safe_parent}")
        endwhile()

        execute_process(
            COMMAND "${GIT_EXECUTABLE}"
                -c "safe.directory=${_spark_git_safe_hint}"
                -C "${_spark_source_directory}" rev-parse --show-toplevel
            RESULT_VARIABLE _spark_git_root_result
            OUTPUT_VARIABLE _spark_git_root
            ERROR_VARIABLE _spark_git_root_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _spark_git_root_result EQUAL 0)
            message(FATAL_ERROR
                "Git checkout detection failed for ${_spark_source_directory}; refusing fallback:\n"
                "${_spark_git_root_error}")
        endif()
        file(REAL_PATH "${_spark_git_root}" _spark_git_root_real)
        cmake_path(IS_PREFIX _spark_source_root_real "${_spark_git_root_real}"
            NORMALIZE _spark_git_root_is_bounded)
        if(NOT _spark_git_root_is_bounded)
            message(FATAL_ERROR
                "Git resolved a repository root outside CMAKE_SOURCE_DIR: ${_spark_git_root}")
        endif()

        cmake_path(RELATIVE_PATH _spark_source_directory
            BASE_DIRECTORY "${_spark_git_root_real}"
            OUTPUT_VARIABLE _spark_git_source_relative)
        string(REPLACE "\\" "/" _spark_git_source_relative
            "${_spark_git_source_relative}")
        if(_spark_git_source_relative STREQUAL ""
           OR _spark_git_source_relative STREQUAL ".")
            set(_spark_git_pathspec ".")
            set(_spark_tracked_path_prefix "")
        else()
            set(_spark_git_pathspec "${_spark_git_source_relative}/")
            set(_spark_tracked_path_prefix "${_spark_git_source_relative}/")
        endif()

        string(SHA256 _spark_manifest_hash
            "${_spark_git_root_real}|${_spark_git_pathspec}")
        set(_spark_manifest_directory
            "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/spark-tracked-install")
        set(_spark_manifest_file
            "${_spark_manifest_directory}/${_spark_manifest_hash}.paths")
        file(MAKE_DIRECTORY "${_spark_manifest_directory}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}"
                -c "safe.directory=${_spark_git_root_real}"
                -C "${_spark_git_root_real}"
                --literal-pathspecs ls-files --cached -z -- "${_spark_git_pathspec}"
            RESULT_VARIABLE _spark_git_list_result
            OUTPUT_FILE "${_spark_manifest_file}"
            ERROR_VARIABLE _spark_git_list_error)
        if(NOT _spark_git_list_result EQUAL 0)
            message(FATAL_ERROR
                "Git tracked-file enumeration failed for ${SPARK_TRACKED_SOURCE}; refusing fallback:\n"
                "${_spark_git_list_error}")
        endif()

        file(READ "${_spark_manifest_file}" _spark_manifest_hex HEX)
        file(REMOVE "${_spark_manifest_file}")
        string(LENGTH "${_spark_manifest_hex}" _spark_manifest_hex_length)
        math(EXPR _spark_manifest_remainder "${_spark_manifest_hex_length} % 2")
        if(NOT _spark_manifest_remainder EQUAL 0)
            message(FATAL_ERROR "Git returned a malformed tracked-file manifest")
        endif()
        string(REGEX MATCHALL "[0-9A-Fa-f][0-9A-Fa-f]" _spark_manifest_bytes
            "${_spark_manifest_hex}")

        set(_spark_tracked_paths)
        set(_spark_path_codes)
        foreach(_spark_byte IN LISTS _spark_manifest_bytes)
            if(_spark_byte STREQUAL "00")
                if(NOT _spark_path_codes)
                    message(FATAL_ERROR "Git returned an empty or duplicate-NUL tracked path")
                endif()
                string(ASCII ${_spark_path_codes} _spark_tracked_path)
                list(APPEND _spark_tracked_paths "${_spark_tracked_path}")
                set(_spark_path_codes)
            else()
                math(EXPR _spark_byte_decimal "0x${_spark_byte}")
                if(_spark_byte_decimal LESS 32
                   OR _spark_byte_decimal EQUAL 59
                   OR _spark_byte_decimal EQUAL 92
                   OR _spark_byte_decimal EQUAL 127)
                    message(FATAL_ERROR
                        "Git returned a tracked path with a control, semicolon, or backslash byte")
                endif()
                list(APPEND _spark_path_codes "${_spark_byte_decimal}")
            endif()
        endforeach()
        if(_spark_path_codes)
            message(FATAL_ERROR "Git returned a tracked-file manifest without a final NUL delimiter")
        endif()
    endif()

    string(SHA256 _spark_snapshot_hash
        "${_spark_source_root_real}|${SPARK_TRACKED_SOURCE}|snapshot-v1")
    string(LENGTH "${_spark_snapshot_hash}" _spark_snapshot_hash_length)
    if(NOT _spark_snapshot_hash_length EQUAL 64
       OR _spark_snapshot_hash MATCHES "[^0-9a-f]")
        message(FATAL_ERROR "Failed to derive a safe tracked-install snapshot name")
    endif()
    set(_spark_snapshot_root
        "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/spark-tracked-install/${_spark_snapshot_hash}.payload")
    file(REMOVE_RECURSE "${_spark_snapshot_root}")
    file(MAKE_DIRECTORY "${_spark_snapshot_root}")

    list(SORT _spark_tracked_paths CASE SENSITIVE)
    set(_spark_previous_path)
    set(_spark_flattened_names)
    foreach(_spark_tracked_path IN LISTS _spark_tracked_paths)
        if(_spark_tracked_path STREQUAL _spark_previous_path)
            message(FATAL_ERROR "Git returned duplicate tracked path: ${_spark_tracked_path}")
        endif()
        set(_spark_previous_path "${_spark_tracked_path}")

        if(_spark_tracked_path_prefix STREQUAL "")
            set(_spark_relative_path "${_spark_tracked_path}")
        else()
            string(FIND "${_spark_tracked_path}" "${_spark_tracked_path_prefix}"
                _spark_prefix_index)
            if(NOT _spark_prefix_index EQUAL 0)
                message(FATAL_ERROR
                    "Git returned a path outside SOURCE: ${_spark_tracked_path}")
            endif()
            string(LENGTH "${_spark_tracked_path_prefix}" _spark_prefix_length)
            string(SUBSTRING "${_spark_tracked_path}" ${_spark_prefix_length} -1
                _spark_relative_path)
        endif()
        if(_spark_relative_path STREQUAL ""
           OR _spark_relative_path MATCHES "(^|/)\\.\\.?(/|$)"
           OR _spark_relative_path MATCHES "(^|/)[^/]*:")
            message(FATAL_ERROR "Git returned an unsafe tracked path: ${_spark_tracked_path}")
        endif()

        set(_spark_is_included TRUE)
        if(SPARK_TRACKED_INCLUDE_REGEXES)
            set(_spark_is_included FALSE)
            foreach(_spark_inclusion IN LISTS SPARK_TRACKED_INCLUDE_REGEXES)
                if("/${_spark_relative_path}" MATCHES "${_spark_inclusion}")
                    set(_spark_is_included TRUE)
                    break()
                endif()
            endforeach()
        endif()
        if(NOT _spark_is_included)
            continue()
        endif()

        set(_spark_is_excluded FALSE)
        foreach(_spark_exclusion IN LISTS SPARK_TRACKED_EXCLUDE_REGEXES)
            if("/${_spark_relative_path}" MATCHES "${_spark_exclusion}")
                set(_spark_is_excluded TRUE)
                break()
            endif()
        endforeach()
        if(_spark_is_excluded)
            continue()
        endif()

        set(_spark_absolute_file "${_spark_source_directory}/${_spark_relative_path}")
        if(NOT EXISTS "${_spark_absolute_file}"
           OR IS_DIRECTORY "${_spark_absolute_file}"
           OR IS_SYMLINK "${_spark_absolute_file}")
            message(FATAL_ERROR
                "Tracked install entry is missing, a directory/submodule, or a symlink: "
                "${_spark_tracked_path}")
        endif()
        file(REAL_PATH "${_spark_absolute_file}" _spark_absolute_file_real)
        cmake_path(IS_PREFIX _spark_source_directory_real "${_spark_absolute_file_real}"
            NORMALIZE _spark_file_is_bounded)
        if(NOT _spark_file_is_bounded)
            message(FATAL_ERROR "Tracked path resolves outside SOURCE: ${_spark_tracked_path}")
        endif()

        # Bind installation to a verified configure-time snapshot. This closes
        # the configure/install TOCTOU window where a tracked parent could be
        # replaced by a junction after validation but before cmake --install.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_spark_absolute_file}")
        set(_spark_snapshot_file
            "${_spark_snapshot_root}/${_spark_relative_path}")
        cmake_path(GET _spark_snapshot_file PARENT_PATH _spark_snapshot_parent)
        file(MAKE_DIRECTORY "${_spark_snapshot_parent}")
        file(SHA256 "${_spark_absolute_file}" _spark_source_hash_before)
        file(COPY_FILE "${_spark_absolute_file}" "${_spark_snapshot_file}" ONLY_IF_DIFFERENT)
        file(SHA256 "${_spark_snapshot_file}" _spark_snapshot_file_hash)
        file(SHA256 "${_spark_absolute_file}" _spark_source_hash_after)
        file(REAL_PATH "${_spark_absolute_file}" _spark_absolute_file_real_after)
        cmake_path(IS_PREFIX _spark_source_directory_real "${_spark_absolute_file_real_after}"
            NORMALIZE _spark_file_is_bounded_after)
        if(NOT _spark_file_is_bounded_after
           OR NOT _spark_absolute_file_real_after STREQUAL _spark_absolute_file_real
           OR NOT _spark_source_hash_before STREQUAL _spark_source_hash_after
           OR NOT _spark_source_hash_before STREQUAL _spark_snapshot_file_hash)
            message(FATAL_ERROR
                "Tracked install source changed or escaped while being snapshotted: ${_spark_tracked_path}")
        endif()

        cmake_path(GET _spark_relative_path PARENT_PATH _spark_relative_parent)
        if(SPARK_TRACKED_FLATTEN)
            cmake_path(GET _spark_relative_path FILENAME _spark_flattened_name)
            if(_spark_flattened_name IN_LIST _spark_flattened_names)
                message(FATAL_ERROR
                    "FLATTEN would install duplicate filename: ${_spark_flattened_name}")
            endif()
            list(APPEND _spark_flattened_names "${_spark_flattened_name}")
            set(_spark_file_destination "${SPARK_TRACKED_DESTINATION}")
        elseif(_spark_relative_parent STREQUAL "")
            set(_spark_file_destination "${SPARK_TRACKED_DESTINATION}")
        else()
            set(_spark_file_destination
                "${SPARK_TRACKED_DESTINATION}/${_spark_relative_parent}")
        endif()
        cmake_path(GET _spark_relative_path EXTENSION LAST_ONLY _spark_extension)
        string(TOLOWER "${_spark_extension}" _spark_extension)
        if(_spark_extension MATCHES "^\\.(sh|bash|zsh|py|pl|rb|cmd|bat|ps1)$")
            install(PROGRAMS "${_spark_snapshot_file}"
                DESTINATION "${_spark_file_destination}"
                COMPONENT "${SPARK_TRACKED_COMPONENT}")
        else()
            install(FILES "${_spark_snapshot_file}"
                DESTINATION "${_spark_file_destination}"
                COMPONENT "${SPARK_TRACKED_COMPONENT}")
        endif()
    endforeach()
endfunction()
