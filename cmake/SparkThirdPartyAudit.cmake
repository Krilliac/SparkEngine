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

function(spark_thirdparty_audit manifest_file)
    if(NOT EXISTS "${manifest_file}")
        message(FATAL_ERROR "[ThirdParty Audit] Manifest not found: ${manifest_file}")
    endif()

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
        if(NOT _field_count EQUAL 9)
            math(EXPR _audit_issues "${_audit_issues}+1")
            message(WARNING "[ThirdParty Audit] Invalid manifest entry (expected 9 fields): ${_entry}")
            continue()
        endif()

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
