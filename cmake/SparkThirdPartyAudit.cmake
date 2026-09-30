# SparkThirdPartyAudit.cmake
# Validates ThirdParty dependency manifest and prints configure-time summary.

# Empty manifest fields are meaningful (for example, a missing license). Keep
# them in the list produced from each pipe-delimited entry so the Python
# reconciliation gate reports the policy violation instead of a CMake-version
# dependent malformed-entry failure.
if(POLICY CMP0007)
    cmake_policy(SET CMP0007 NEW)
endif()
# The notice renderer also runs as a bare `cmake -P` script (no
# cmake_minimum_required), where CMake 3.x leaves CMP0057 unset and rejects
# `if(... IN_LIST ...)`. CMake 4 forces it on, which hid this on Windows hosts.
if(POLICY CMP0057)
    cmake_policy(SET CMP0057 NEW)
endif()

# Strict-dependency closure: with SPARK_STRICT_DEPS=ON every entry in the
# manifest is required, whatever its severity. The manifest is the only list of
# dependencies a strict configure checks, so adding or removing an entry there
# changes the strict closure without a second hand-maintained list in
# CMakeLists.txt. Strict issues are collected and reported together by
# spark_thirdparty_audit() as one FATAL_ERROR; without strict mode the manifest
# severity decides only the wording of the configure-time warning.
function(_spark_dep_report severity message_text)
    if(SPARK_STRICT_DEPS)
        set_property(GLOBAL APPEND PROPERTY _SPARK_STRICT_DEP_ISSUES "${message_text}")
        message(STATUS "[ThirdParty Audit] STRICT: ${message_text}")
    elseif(severity STREQUAL "ERROR")
        message(WARNING "[ThirdParty Audit] ${message_text} (set -DSPARK_STRICT_DEPS=ON to make this fatal)")
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

# Fail unless `notice_rel` (relative to `root`) is a regular file inside `root`
# that carries a copyright statement and operative license terms. Shared by the
# dependencies.lock notices and the editor-font license texts.
function(_spark_thirdparty_check_notice_file label notice_rel root)
    if(notice_rel STREQUAL "")
        message(FATAL_ERROR "[ThirdParty Audit] ${label}: license notice path is empty")
    endif()
    file(TO_CMAKE_PATH "${root}/" _root_prefix)
    string(TOLOWER "${_root_prefix}" _root_prefix_lower)
    set(_notice_candidate "${root}/${notice_rel}")
    if(NOT EXISTS "${_notice_candidate}" OR IS_DIRECTORY "${_notice_candidate}")
        message(FATAL_ERROR "[ThirdParty Audit] ${label}: license notice file does not exist: ${notice_rel}")
    endif()
    get_filename_component(_notice_abs "${_notice_candidate}" REALPATH)
    file(TO_CMAKE_PATH "${_notice_abs}" _notice_abs_normalized)
    string(TOLOWER "${_notice_abs_normalized}" _notice_abs_lower)
    string(FIND "${_notice_abs_lower}" "${_root_prefix_lower}" _notice_root_index)
    if(NOT _notice_root_index EQUAL 0)
        message(FATAL_ERROR "[ThirdParty Audit] ${label}: license notice escapes the repository root: ${notice_rel}")
    endif()
    file(SIZE "${_notice_abs}" _notice_size)
    if(_notice_size LESS 200)
        message(FATAL_ERROR "[ThirdParty Audit] ${label}: license notice is implausibly short: ${notice_rel}")
    endif()
    file(READ "${_notice_abs}" _notice_content)
    if(NOT _notice_content MATCHES "[Cc]opyright")
        message(FATAL_ERROR
            "[ThirdParty Audit] ${label}: license notice lacks a copyright statement: ${notice_rel}")
    endif()
    if(NOT _notice_content MATCHES
        "Permission is (hereby )?granted|Permission to use, copy, modify|Redistribution and use|public domain|TERMS AND CONDITIONS FOR USE")
        message(FATAL_ERROR
            "[ThirdParty Audit] ${label}: license notice lacks operative license terms: ${notice_rel}")
    endif()
endfunction()

# Append one inventory block per editor-font license text to `output_file`, from
# SparkEditor/Fonts/LICENSES/fonts.json (the font inventory that
# tools/governance/generate_third_party_notices.py cross-checks against each
# font's name table), and add each license text to the list named by
# `notice_files_var`. Fonts sharing a license text form one block whose "Files:"
# line names them all. Fails when the inventory is malformed, names a missing
# font or license text, or misses a font shipped from SparkEditor/Fonts.
function(_spark_thirdparty_append_editor_fonts root output_file notice_files_var)
    set(_fonts_dir "${root}/SparkEditor/Fonts")
    set(_inventory "${_fonts_dir}/LICENSES/fonts.json")
    if(NOT EXISTS "${_inventory}")
        message(FATAL_ERROR "[ThirdParty Audit] Editor font license inventory not found: ${_inventory}")
    endif()
    file(READ "${_inventory}" _json)
    string(JSON _fonts ERROR_VARIABLE _error GET "${_json}" fonts)
    if(_error)
        message(FATAL_ERROR "[ThirdParty Audit] ${_inventory}: no 'fonts' object: ${_error}")
    endif()
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_fonts}")
    if(_error OR _count EQUAL 0)
        message(FATAL_ERROR "[ThirdParty Audit] ${_inventory}: 'fonts' is empty or malformed")
    endif()

    set(_declared "")
    set(_groups "")
    math(EXPR _last "${_count} - 1")
    foreach(_index RANGE ${_last})
        string(JSON _font MEMBER "${_fonts}" ${_index})
        list(APPEND _declared "${_font}")
        foreach(_field IN ITEMS family version license license_file license_source)
            string(JSON _value ERROR_VARIABLE _error GET "${_fonts}" "${_font}" ${_field})
            if(_error OR _value STREQUAL "")
                message(FATAL_ERROR "[ThirdParty Audit] ${_inventory}: ${_font} has no '${_field}'")
            endif()
            set(_${_field} "${_value}")
        endforeach()
        if(NOT EXISTS "${_fonts_dir}/${_font}" OR IS_DIRECTORY "${_fonts_dir}/${_font}")
            message(FATAL_ERROR "[ThirdParty Audit] ${_inventory} names a font that does not exist: ${_font}")
        endif()
        if(_license_file MATCHES "[/\\\\]" OR _license_file MATCHES "^\\.")
            message(FATAL_ERROR
                "[ThirdParty Audit] ${_inventory}: ${_font} license_file must name a file in LICENSES/")
        endif()
        set(_notice_rel "SparkEditor/Fonts/LICENSES/${_license_file}")
        string(CONCAT _header
            "${_family} (editor font)\n"
            "  Source: ${_license_source}\n"
            "  Version: ${_version}\n"
            "  License: ${_license}\n"
            "  Notice files: ${_notice_rel}\n")
        string(MAKE_C_IDENTIFIER "${_license_file}" _group)
        if(NOT _group IN_LIST _groups)
            _spark_thirdparty_check_notice_file("editor font ${_font}" "${_notice_rel}" "${root}")
            list(APPEND _groups ${_group})
            set(_group_header_${_group} "${_header}")
            set(_group_notice_${_group} "${_notice_rel}")
            set(_group_files_${_group} "")
        elseif(NOT _group_header_${_group} STREQUAL _header)
            message(FATAL_ERROR
                "[ThirdParty Audit] ${_inventory}: fonts sharing ${_license_file} disagree on "
                "family, version, license or source (${_font})")
        endif()
        list(APPEND _group_files_${_group} "${_font}")
    endforeach()

    file(GLOB _shipped RELATIVE "${_fonts_dir}" "${_fonts_dir}/*.ttf" "${_fonts_dir}/*.otf")
    foreach(_font IN LISTS _shipped)
        if(NOT _font IN_LIST _declared)
            message(FATAL_ERROR "[ThirdParty Audit] ${_inventory} has no entry for editor font ${_font}")
        endif()
    endforeach()

    set(_notice_files ${${notice_files_var}})
    foreach(_group IN LISTS _groups)
        list(JOIN _group_files_${_group} "," _files_csv)
        file(APPEND "${output_file}" "${_group_header_${_group}}  Files: ${_files_csv}\n\n")
        list(APPEND _notice_files "${_group_notice_${_group}}")
    endforeach()
    set(${notice_files_var} "${_notice_files}" PARENT_SCOPE)
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
        string(REPLACE "," ";" _notice_files "${_notice_files_csv}")
        foreach(_notice_rel IN LISTS _notice_files)
            _spark_thirdparty_check_notice_file("${_entry}" "${_notice_rel}" "${_manifest_root}")
        endforeach()
    endforeach()

    if(_entry_count EQUAL 0)
        message(FATAL_ERROR "[ThirdParty Audit] Manifest contains no dependency entries: ${manifest_file}")
    endif()

    message(STATUS "[ThirdParty Audit] Manifest schema valid (${_entry_count} entries)")
endfunction()

function(spark_thirdparty_export_entries manifest_file output_file)
    # Emit the fully expanded entry list so external checkers reconcile against
    # what CMake actually evaluates rather than against a text scrape of the
    # manifest. Variable expansions, list appends after the closing paren, and
    # multi-line records are all resolved here; a text-only parser sees none of
    # them and reports a clean tree it never examined.
    spark_thirdparty_validate_manifest_schema("${manifest_file}")
    include("${manifest_file}")

    set(_export_body "")
    foreach(_entry IN LISTS SPARK_THIRDPARTY_AUDIT_ENTRIES)
        if(_entry MATCHES "\n")
            message(FATAL_ERROR
                "[ThirdParty Audit] Manifest entry contains a newline and cannot be exported: ${_entry}")
        endif()
        string(APPEND _export_body "${_entry}\n")
    endforeach()

    file(WRITE "${output_file}" "${_export_body}")
endfunction()

# Append the inventory entry for a toolchain runtime that the package ships
# app-local (the Microsoft Visual C++ runtime that InstallRequiredSystemLibraries
# installs into bin/). Its redistribution terms are not a file in this
# repository, so the entry names them on a "Terms:" line instead of reproducing
# license text, and its "Files:" line names every shipped DLL. The staged-package
# gate's systemRuntime rule (cmake/PackageNoticeCoverageRules.json) requires both.
# This identifies the governing terms; whether shipping satisfies them is a
# legal-review item (docs/governance/GOV-400-DECISIONS.md, D8).
function(_spark_thirdparty_append_system_runtime output_file runtime_libs toolset_version)
    set(_files "")
    foreach(_lib IN LISTS runtime_libs)
        get_filename_component(_name "${_lib}" NAME)
        string(TOLOWER "${_name}" _name)
        list(APPEND _files "${_name}")
    endforeach()
    list(REMOVE_DUPLICATES _files)
    list(SORT _files)
    list(JOIN _files "," _files_csv)
    file(APPEND "${output_file}"
        "Microsoft Visual C++ Runtime\n"
        "  Source: Microsoft Visual C++ Redistributable, installed app-local by CMake InstallRequiredSystemLibraries\n"
        "  Version: MSVC ${toolset_version}\n"
        "  License: Microsoft Software License Terms for the Visual Studio toolset (Distributable Code)\n"
        "  Terms: Redistributed unmodified as Distributable Code under the Microsoft Software License Terms of the "
        "Visual Studio installation that built this package (MSVC ${toolset_version}); the terms are not reproduced here\n"
        "  Files: ${_files_csv}\n\n")
endfunction()

# spark_thirdparty_generate_notice(<manifest> <output>
#     [SYSTEM_RUNTIME_LIBS <dll>...] [SYSTEM_RUNTIME_VERSION <toolset version>])
function(spark_thirdparty_generate_notice manifest_file output_file)
    cmake_parse_arguments(PARSE_ARGV 2 _notice "" "SYSTEM_RUNTIME_VERSION" "SYSTEM_RUNTIME_LIBS")
    if(_notice_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "spark_thirdparty_generate_notice: unknown arguments ${_notice_UNPARSED_ARGUMENTS}")
    endif()
    if(_notice_SYSTEM_RUNTIME_LIBS AND "${_notice_SYSTEM_RUNTIME_VERSION}" STREQUAL "")
        message(FATAL_ERROR "spark_thirdparty_generate_notice: SYSTEM_RUNTIME_LIBS needs SYSTEM_RUNTIME_VERSION")
    endif()
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
        list(GET _fields 5 _required_files_csv)
        list(GET _fields 9 _notice_files_csv)
        # "Files:" names the shipped files each entry covers. The staged-package
        # notice gate (cmake/ValidateStagedPackageNotices.cmake) matches shipped
        # fonts against these names, so a font is covered only when its entry
        # also reproduces license text below.
        file(APPEND "${output_file}"
            "${_name}\n"
            "  Source: ${_source}\n"
            "  Version: ${_version}\n"
            "  License: ${_license}\n"
            "  Notice files: ${_notice_files_csv}\n"
            "  Files: ${_required_files_csv}\n\n")
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
    _spark_thirdparty_append_editor_fonts("${_manifest_root}" "${output_file}" _all_notice_files)
    if(_notice_SYSTEM_RUNTIME_LIBS)
        _spark_thirdparty_append_system_runtime("${output_file}" "${_notice_SYSTEM_RUNTIME_LIBS}"
            "${_notice_SYSTEM_RUNTIME_VERSION}")
    endif()
    file(APPEND "${output_file}"
        "Complete license and notice texts\n"
        "=================================\n\n")
    foreach(_notice_rel IN LISTS _all_notice_files)
        file(READ "${_manifest_root}/${_notice_rel}" _notice_content)
        string(REPLACE "\r\n" "\n" _notice_content "${_notice_content}")
        string(REPLACE "\r" "\n" _notice_content "${_notice_content}")
        # Require at least one trailing newline in the match. Newer CMake
        # rejects REGEX REPLACE expressions that can match the empty string.
        string(REGEX REPLACE "\n+$" "" _notice_content "${_notice_content}")
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
    set_property(GLOBAL PROPERTY _SPARK_STRICT_DEP_ISSUES "")

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

    if(SPARK_STRICT_DEPS)
        list(LENGTH SPARK_THIRDPARTY_AUDIT_ENTRIES _closure_size)
        get_property(_strict_issues GLOBAL PROPERTY _SPARK_STRICT_DEP_ISSUES)
        if(_strict_issues)
            list(LENGTH _strict_issues _strict_issue_count)
            list(JOIN _strict_issues "\n  - " _strict_issue_text)
            message(FATAL_ERROR
                "[ThirdParty Audit] SPARK_STRICT_DEPS: ${_strict_issue_count} issue(s) in the "
                "${_closure_size}-entry dependency closure declared by ${manifest_file}:\n"
                "  - ${_strict_issue_text}\n"
                "Restore the pinned tree with 'git submodule update --init --recursive' or configure "
                "with -DSPARK_STRICT_DEPS=OFF for a degraded development build.")
        endif()
        message(STATUS "[ThirdParty Audit] SPARK_STRICT_DEPS: all ${_closure_size} locked dependencies present")
    endif()
endfunction()

if(SPARK_THIRDPARTY_AUDIT_VALIDATE_ONLY)
    if(NOT DEFINED SPARK_THIRDPARTY_MANIFEST)
        message(FATAL_ERROR "SPARK_THIRDPARTY_MANIFEST is required in validation-only mode")
    endif()
    spark_thirdparty_validate_manifest_schema("${SPARK_THIRDPARTY_MANIFEST}")
    if(DEFINED SPARK_THIRDPARTY_ENTRIES_OUTPUT)
        spark_thirdparty_export_entries(
            "${SPARK_THIRDPARTY_MANIFEST}"
            "${SPARK_THIRDPARTY_ENTRIES_OUTPUT}")
    endif()
    if(DEFINED SPARK_THIRDPARTY_NOTICE_OUTPUT)
        spark_thirdparty_generate_notice(
            "${SPARK_THIRDPARTY_MANIFEST}"
            "${SPARK_THIRDPARTY_NOTICE_OUTPUT}")
    endif()
endif()
