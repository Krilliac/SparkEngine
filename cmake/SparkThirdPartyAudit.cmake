# SparkThirdPartyAudit.cmake
# Validates ThirdParty dependency manifest and prints configure-time summary.

function(_spark_dep_report severity message_text)
    if(severity STREQUAL "ERROR")
        if(SPARK_STRICT_DEPS)
            message(FATAL_ERROR "[ThirdParty Audit] ${message_text}")
        else()
            message(WARNING "[ThirdParty Audit] ${message_text} (set -DSPARK_STRICT_DEPS=ON to make this fatal)")
        endif()
    else()
        message(WARNING "[ThirdParty Audit] ${message_text}")
    endif()
endfunction()

function(_spark_gitmodules_get_url dep_path out_var)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/.gitmodules")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND git config --file .gitmodules --get-regexp ^submodule\..*\.path$
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _paths
        RESULT_VARIABLE _paths_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(NOT _paths_rc EQUAL 0)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    set(_matched_key "")
    string(REPLACE "\n" ";" _path_lines "${_paths}")
    foreach(_line IN LISTS _path_lines)
        if(_line MATCHES "^([^ ]+) +(.+)$")
            set(_key "${CMAKE_MATCH_1}")
            set(_value "${CMAKE_MATCH_2}")
            if(_value STREQUAL dep_path)
                string(REGEX REPLACE "\\.path$" "" _matched_key "${_key}")
                break()
            endif()
        endif()
    endforeach()

    if(_matched_key STREQUAL "")
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    execute_process(
        COMMAND git config --file .gitmodules --get "${_matched_key}.url"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _url
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(${out_var} "${_url}" PARENT_SCOPE)
endfunction()

function(_spark_git_tree_rev dep_path out_var)
    execute_process(
        COMMAND git rev-parse "HEAD:${dep_path}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE _rev
        RESULT_VARIABLE _rev_rc
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )

    if(_rev_rc EQUAL 0)
        set(${out_var} "${_rev}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(spark_thirdparty_validate_manifest_schema manifest_file)
    if(NOT EXISTS "${manifest_file}")
        message(FATAL_ERROR "[ThirdParty Audit] Manifest not found: ${manifest_file}")
    endif()

    include("${manifest_file}")
    if(NOT DEFINED SPARK_THIRDPARTY_AUDIT_ENTRIES)
        message(FATAL_ERROR "[ThirdParty Audit] Manifest does not define SPARK_THIRDPARTY_AUDIT_ENTRIES: ${manifest_file}")
    endif()

    set(_entry_count 0)
    foreach(_entry IN LISTS SPARK_THIRDPARTY_AUDIT_ENTRIES)
        math(EXPR _entry_count "${_entry_count}+1")
        string(REPLACE "|" ";" _fields "${_entry}")
        list(LENGTH _fields _field_count)
        if(NOT _field_count EQUAL 10)
            message(FATAL_ERROR "[ThirdParty Audit] Invalid manifest entry (expected 10 fields): ${_entry}")
        endif()
        list(GET _fields 8 _severity)
        if(NOT _severity STREQUAL "ERROR" AND NOT _severity STREQUAL "WARN")
            message(FATAL_ERROR "[ThirdParty Audit] Invalid severity '${_severity}' in entry: ${_entry}")
        endif()
        list(GET _fields 9 _notice_files_csv)
        if(_notice_files_csv STREQUAL "")
            message(FATAL_ERROR "[ThirdParty Audit] ${_entry}: license notice file list is empty")
        endif()

        get_filename_component(_manifest_directory "${manifest_file}" DIRECTORY)
        get_filename_component(_manifest_root "${_manifest_directory}/.." REALPATH)
        file(TO_CMAKE_PATH "${_manifest_root}/" _manifest_root_prefix)
        string(TOLOWER "${_manifest_root_prefix}" _manifest_root_prefix_lower)
        string(REPLACE "," ";" _notice_files "${_notice_files_csv}")
        foreach(_notice_rel IN LISTS _notice_files)
            if(_notice_rel STREQUAL "")
                message(FATAL_ERROR "[ThirdParty Audit] ${_entry}: license notice path is empty")
            endif()
            set(_notice_candidate "${_manifest_root}/${_notice_rel}")
            if(NOT EXISTS "${_notice_candidate}" OR IS_DIRECTORY "${_notice_candidate}")
                message(FATAL_ERROR
                    "[ThirdParty Audit] ${_entry}: license notice file does not exist: ${_notice_rel}")
            endif()
            get_filename_component(_notice_abs "${_notice_candidate}" REALPATH)
            file(TO_CMAKE_PATH "${_notice_abs}" _notice_abs_normalized)
            string(TOLOWER "${_notice_abs_normalized}" _notice_abs_lower)
            string(FIND "${_notice_abs_lower}" "${_manifest_root_prefix_lower}" _notice_root_index)
            if(NOT _notice_root_index EQUAL 0)
                message(FATAL_ERROR
                    "[ThirdParty Audit] ${_entry}: license notice escapes the repository root: ${_notice_rel}")
            endif()
            file(SIZE "${_notice_abs}" _notice_size)
            if(_notice_size LESS 200)
                message(FATAL_ERROR
                    "[ThirdParty Audit] ${_entry}: license notice is implausibly short: ${_notice_rel}")
            endif()
            file(READ "${_notice_abs}" _notice_content)
            if(NOT _notice_content MATCHES "[Cc]opyright")
                message(FATAL_ERROR
                    "[ThirdParty Audit] ${_entry}: license notice lacks a copyright statement: ${_notice_rel}")
            endif()
            if(NOT _notice_content MATCHES
                "Permission is (hereby )?granted|Redistribution and use|public domain|TERMS AND CONDITIONS FOR USE")
                message(FATAL_ERROR
                    "[ThirdParty Audit] ${_entry}: license notice lacks operative license terms: ${_notice_rel}")
            endif()
        endforeach()
    endforeach()

    if(_entry_count EQUAL 0)
        message(FATAL_ERROR "[ThirdParty Audit] Manifest contains no dependency entries: ${manifest_file}")
    endif()

    message(STATUS "[ThirdParty Audit] Manifest schema valid (${_entry_count} entries)")
endfunction()

function(spark_thirdparty_generate_notice manifest_file output_file)
    spark_thirdparty_validate_manifest_schema("${manifest_file}")
    include("${manifest_file}")

    file(WRITE "${output_file}"
        "SparkEngine Third-Party Notices\n"
        "================================\n\n"
        "SparkEngine includes or can link the dependencies listed below. "
        "Their copyrights and license terms remain with their respective owners.\n\n"
        "Dependency inventory\n"
        "--------------------\n\n")
    set(_all_notice_files "")
    foreach(_entry IN LISTS SPARK_THIRDPARTY_AUDIT_ENTRIES)
        string(REPLACE "|" ";" _fields "${_entry}")
        list(GET _fields 0 _name)
        list(GET _fields 1 _source)
        list(GET _fields 2 _version)
        list(GET _fields 3 _license)
        list(GET _fields 9 _notice_files_csv)
        file(APPEND "${output_file}"
            "${_name}\n"
            "  Source: ${_source}\n"
            "  Version: ${_version}\n"
            "  License: ${_license}\n"
            "  Notice files: ${_notice_files_csv}\n\n")
        string(REPLACE "," ";" _notice_files "${_notice_files_csv}")
        foreach(_notice_rel IN LISTS _notice_files)
            list(FIND _all_notice_files "${_notice_rel}" _notice_index)
            if(_notice_index EQUAL -1)
                list(APPEND _all_notice_files "${_notice_rel}")
            endif()
        endforeach()
    endforeach()

    get_filename_component(_manifest_directory "${manifest_file}" DIRECTORY)
    get_filename_component(_manifest_root "${_manifest_directory}/.." REALPATH)
    file(APPEND "${output_file}"
        "Complete license and notice texts\n"
        "=================================\n\n")
    foreach(_notice_rel IN LISTS _all_notice_files)
        file(READ "${_manifest_root}/${_notice_rel}" _notice_content)
        string(REPLACE "\r\n" "\n" _notice_content "${_notice_content}")
        string(REPLACE "\r" "\n" _notice_content "${_notice_content}")
        string(REGEX REPLACE "\n*$" "" _notice_content "${_notice_content}")
        file(APPEND "${output_file}"
            "----- ${_notice_rel} -----\n\n"
            "${_notice_content}\n\n")
    endforeach()
endfunction()

function(spark_thirdparty_audit manifest_file)
    if(NOT EXISTS "${manifest_file}")
        message(FATAL_ERROR "[ThirdParty Audit] Manifest not found: ${manifest_file}")
    endif()

    spark_thirdparty_validate_manifest_schema("${manifest_file}")
    include("${manifest_file}")

    if(NOT DEFINED SPARK_THIRDPARTY_AUDIT_ENTRIES)
        message(FATAL_ERROR "[ThirdParty Audit] Manifest does not define SPARK_THIRDPARTY_AUDIT_ENTRIES: ${manifest_file}")
    endif()

    message(STATUS "")
    message(STATUS "=== Spark Third-Party Dependency Audit ===")

    set(_audit_issues 0)

    foreach(_entry IN LISTS SPARK_THIRDPARTY_AUDIT_ENTRIES)
        string(REPLACE "|" ";" _fields "${_entry}")
        list(LENGTH _fields _field_count)
        # Schema validation above guarantees the ten fields used below.

        list(GET _fields 0 _name)
        list(GET _fields 1 _source)
        list(GET _fields 2 _version)
        list(GET _fields 3 _license)
        list(GET _fields 4 _path)
        list(GET _fields 5 _required_csv)
        list(GET _fields 6 _feature_macro)
        list(GET _fields 7 _fallback)
        list(GET _fields 8 _severity)

        set(_full_path "${CMAKE_SOURCE_DIR}/${_path}")
        set(_present YES)

        if(NOT EXISTS "${_full_path}")
            set(_present NO)
            math(EXPR _audit_issues "${_audit_issues}+1")
            _spark_dep_report("${_severity}" "${_name}: declared path '${_path}' does not exist")
        endif()

        string(REPLACE "," ";" _required_files "${_required_csv}")
        foreach(_required_rel IN LISTS _required_files)
            if(_required_rel STREQUAL "")
                continue()
            endif()
            if(NOT EXISTS "${_full_path}/${_required_rel}")
                set(_present NO)
                math(EXPR _audit_issues "${_audit_issues}+1")
                _spark_dep_report("${_severity}" "${_name}: missing required file '${_path}/${_required_rel}'")
            endif()
        endforeach()

        _spark_gitmodules_get_url("${_path}" _gitmodules_url)
        if(NOT _gitmodules_url STREQUAL "")
            if(NOT _source MATCHES "^${_gitmodules_url}($| .*)")
                math(EXPR _audit_issues "${_audit_issues}+1")
                _spark_dep_report("${_severity}" "${_name}: source URL mismatch (manifest='${_source}', .gitmodules='${_gitmodules_url}')")
            endif()

            _spark_git_tree_rev("${_path}" _tree_rev)
            if(NOT _tree_rev STREQUAL "")
                if(NOT _version MATCHES "^${_tree_rev}($| .*)")
                    math(EXPR _audit_issues "${_audit_issues}+1")
                    _spark_dep_report("${_severity}" "${_name}: version mismatch (manifest='${_version}', repository='${_tree_rev}')")
                endif()
            endif()
        endif()

        if(_present)
            set(_state "OK")
        else()
            set(_state "MISSING")
        endif()

        message(STATUS "  [${_state}] ${_name}")
        message(STATUS "         source   : ${_source}")
        message(STATUS "         version  : ${_version}")
        message(STATUS "         license  : ${_license}")
        message(STATUS "         path     : ${_path}")
        message(STATUS "         feature  : ${_feature_macro}")
        message(STATUS "         fallback : ${_fallback}")
    endforeach()

    message(STATUS "=== End Third-Party Audit (${_audit_issues} issue(s)) ===")
    message(STATUS "")
endfunction()

if(SPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY)
    if(NOT DEFINED SPARK_THIRDPARTY_MANIFEST)
        message(FATAL_ERROR "SPARK_THIRDPARTY_MANIFEST is required in validation-only mode")
    endif()
    spark_thirdparty_validate_manifest_schema("${SPARK_THIRDPARTY_MANIFEST}")
    if(DEFINED SPARK_THIRDPARTY_NOTICE_OUTPUT)
        spark_thirdparty_generate_notice(
            "${SPARK_THIRDPARTY_MANIFEST}"
            "${SPARK_THIRDPARTY_NOTICE_OUTPUT}")
    endif()
endif()
