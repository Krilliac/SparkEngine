cmake_minimum_required(VERSION 3.25)

# GOV-400 staged-package notice-coverage gate.
#
# Every shipped font file, and every file installed from a third-party
# dependency, must be covered by the package's own THIRD_PARTY_NOTICES.txt:
#
#   * a font (rules: fontSuffixes) is covered only when an inventory entry names
#     it on its "Files:" line AND reproduces license text for every notice file
#     that entry declares;
#   * a file matching a payload rule is covered when the rule's component has an
#     inventory entry with license text, or the rule is a documented first-party
#     exemption;
#   * a file under a third-party root that no payload rule maps is uncovered.
#
# The rule set lives in cmake/PackageNoticeCoverageRules.json and is shared with
# tools/governance/generate_third_party_notices.py (--check-package), so both
# tools apply one rule set. Package content is only read as bounded data.
#
# Standalone use (fails closed and lists every uncovered file):
#   cmake -DSPARK_PACKAGE_ROOT=<staged install root>
#         [-DSPARK_PACKAGE_NOTICE_RULES=<rules json>]
#         [-DSPARK_PACKAGE_NOTICE_COVERAGE=enforce|report]
#         -P cmake/ValidateStagedPackageNotices.cmake
#
# ValidateStagedPackageExecutables.cmake includes this file after its own
# package-root containment checks.

set(_spark_notice_default_rules "${CMAKE_CURRENT_LIST_DIR}/PackageNoticeCoverageRules.json")

# Longest THIRD_PARTY_NOTICES.txt the gate will read. The generated file is
# roughly 100 KiB today; the cap only exists so a hostile package cannot make
# the validator read an unbounded file.
set(_SPARK_NOTICE_MAX_BYTES 8388608)
set(_SPARK_NOTICE_RULES_MAX_BYTES 262144)
set(_SPARK_NOTICE_TEXTS_MARKER "\nComplete license and notice texts\n")
set(_SPARK_NOTICE_INVENTORY_MARKER "\nDependency inventory\n--------------------\n")

function(_spark_notice_fail _spark_message)
    message(FATAL_ERROR "Package notice-coverage gate: ${_spark_message}")
endfunction()

function(_spark_notice_json_get _spark_output _spark_json)
    string(JSON _spark_value ERROR_VARIABLE _spark_error GET "${_spark_json}" ${ARGN})
    if(_spark_error)
        list(JOIN ARGN "." _spark_member)
        _spark_notice_fail("malformed rules file member '${_spark_member}': ${_spark_error}")
    endif()
    set(${_spark_output} "${_spark_value}" PARENT_SCOPE)
endfunction()

function(_spark_notice_json_length _spark_output _spark_json)
    string(JSON _spark_type ERROR_VARIABLE _spark_error TYPE "${_spark_json}" ${ARGN})
    if(_spark_error OR NOT _spark_type STREQUAL "ARRAY")
        list(JOIN ARGN "." _spark_member)
        _spark_notice_fail("rules file member '${_spark_member}' must be an array")
    endif()
    string(JSON _spark_length LENGTH "${_spark_json}" ${ARGN})
    if(_spark_length EQUAL 0)
        list(JOIN ARGN "." _spark_member)
        _spark_notice_fail("rules file member '${_spark_member}' is empty")
    endif()
    set(${_spark_output} "${_spark_length}" PARENT_SCOPE)
endfunction()

function(_spark_notice_read_bounded _spark_output _spark_path _spark_limit _spark_description)
    if(NOT EXISTS "${_spark_path}" OR IS_DIRECTORY "${_spark_path}" OR IS_SYMLINK "${_spark_path}")
        _spark_notice_fail("${_spark_description} is missing or is not a regular non-link file: ${_spark_path}")
    endif()
    file(SIZE "${_spark_path}" _spark_size)
    if(_spark_size GREATER _spark_limit)
        _spark_notice_fail("${_spark_description} exceeds ${_spark_limit} bytes: ${_spark_path}")
    endif()
    file(READ "${_spark_path}" _spark_content)
    string(REPLACE "\r\n" "\n" _spark_content "${_spark_content}")
    set(${_spark_output} "${_spark_content}" PARENT_SCOPE)
endfunction()

# CMake lists split on ';' and treat unbalanced brackets specially. Inventory
# lines are free text (versions carry parentheses, commas, and URLs), so encode
# those characters before any list operation and decode each field after.
function(_spark_notice_encode _spark_output _spark_text)
    string(REPLACE ";" "<spark-semicolon>" _spark_text "${_spark_text}")
    string(REPLACE "[" "<spark-open-bracket>" _spark_text "${_spark_text}")
    string(REPLACE "]" "<spark-close-bracket>" _spark_text "${_spark_text}")
    set(${_spark_output} "${_spark_text}" PARENT_SCOPE)
endfunction()

function(_spark_notice_decode _spark_output _spark_text)
    string(REPLACE "<spark-semicolon>" ";" _spark_text "${_spark_text}")
    string(REPLACE "<spark-open-bracket>" "[" _spark_text "${_spark_text}")
    string(REPLACE "<spark-close-bracket>" "]" _spark_text "${_spark_text}")
    set(${_spark_output} "${_spark_text}" PARENT_SCOPE)
endfunction()

function(_spark_notice_split_csv _spark_output _spark_csv)
    set(_spark_items "")
    string(REPLACE "," ";" _spark_parts "${_spark_csv}")
    foreach(_spark_part IN LISTS _spark_parts)
        string(STRIP "${_spark_part}" _spark_part)
        if(NOT _spark_part STREQUAL "")
            list(APPEND _spark_items "${_spark_part}")
        endif()
    endforeach()
    set(${_spark_output} "${_spark_items}" PARENT_SCOPE)
endfunction()

function(_spark_validate_package_notice_coverage _spark_root _spark_rules_path _spark_mode)
    if(NOT _spark_mode STREQUAL "enforce" AND NOT _spark_mode STREQUAL "report")
        _spark_notice_fail("SPARK_PACKAGE_NOTICE_COVERAGE must be 'enforce' or 'report', not '${_spark_mode}'")
    endif()
    if(NOT IS_DIRECTORY "${_spark_root}")
        _spark_notice_fail("package root is not an existing directory: ${_spark_root}")
    endif()

    # ---- rule set -----------------------------------------------------------
    _spark_notice_read_bounded(_spark_rules "${_spark_rules_path}"
        ${_SPARK_NOTICE_RULES_MAX_BYTES} "Notice-coverage rules file")
    _spark_notice_json_get(_spark_schema "${_spark_rules}" schema)
    if(NOT _spark_schema STREQUAL "1")
        _spark_notice_fail("unsupported rules schema '${_spark_schema}' in ${_spark_rules_path}")
    endif()

    set(_spark_font_suffixes "")
    _spark_notice_json_length(_spark_count "${_spark_rules}" fontSuffixes)
    math(EXPR _spark_last "${_spark_count} - 1")
    foreach(_spark_index RANGE ${_spark_last})
        _spark_notice_json_get(_spark_suffix "${_spark_rules}" fontSuffixes ${_spark_index})
        string(TOLOWER "${_spark_suffix}" _spark_suffix)
        list(APPEND _spark_font_suffixes "${_spark_suffix}")
    endforeach()

    _spark_notice_json_get(_spark_min_bytes "${_spark_rules}" licenseText minimumBytes)
    _spark_notice_json_get(_spark_copyright_regex "${_spark_rules}" licenseText copyrightPattern)
    _spark_notice_json_get(_spark_terms_regex "${_spark_rules}" licenseText operativeTermsPattern)
    if(NOT _spark_min_bytes MATCHES "^[0-9]+$")
        _spark_notice_fail("licenseText.minimumBytes must be a non-negative integer")
    endif()

    set(_spark_root_patterns "")
    _spark_notice_json_length(_spark_count "${_spark_rules}" thirdPartyRoots)
    math(EXPR _spark_last "${_spark_count} - 1")
    foreach(_spark_index RANGE ${_spark_last})
        _spark_notice_json_get(_spark_pattern "${_spark_rules}" thirdPartyRoots ${_spark_index})
        list(APPEND _spark_root_patterns "${_spark_pattern}")
    endforeach()

    _spark_notice_json_length(_spark_rule_count "${_spark_rules}" payloadRules)
    math(EXPR _spark_last_rule "${_spark_rule_count} - 1")
    foreach(_spark_index RANGE ${_spark_last_rule})
        _spark_notice_json_get(_spark_rule_pattern_${_spark_index} "${_spark_rules}"
            payloadRules ${_spark_index} pattern)
        string(JSON _spark_rule_component_${_spark_index} ERROR_VARIABLE _spark_component_error
            GET "${_spark_rules}" payloadRules ${_spark_index} component)
        string(JSON _spark_rule_first_party ERROR_VARIABLE _spark_first_party_error
            GET "${_spark_rules}" payloadRules ${_spark_index} firstParty)
        if(_spark_component_error)
            set(_spark_rule_component_${_spark_index} "")
        endif()
        if(_spark_first_party_error)
            set(_spark_rule_first_party "")
        endif()
        # Exactly one of component/firstParty, and a first-party exemption must
        # say why, so an empty rule cannot silently exempt payload.
        if(_spark_rule_component_${_spark_index} STREQUAL "" AND _spark_rule_first_party STREQUAL "")
            set(_spark_rule_shape_error ON)
        elseif(NOT _spark_rule_component_${_spark_index} STREQUAL "" AND NOT _spark_rule_first_party STREQUAL "")
            set(_spark_rule_shape_error ON)
        else()
            set(_spark_rule_shape_error OFF)
        endif()
        if(_spark_rule_shape_error)
            _spark_notice_fail("payloadRules[${_spark_index}] must name exactly one of 'component' or 'firstParty'")
        endif()
    endforeach()

    # ---- packaged THIRD_PARTY_NOTICES.txt -----------------------------------
    set(_spark_notice_path "${_spark_root}/THIRD_PARTY_NOTICES.txt")
    _spark_notice_read_bounded(_spark_notice "${_spark_notice_path}"
        ${_SPARK_NOTICE_MAX_BYTES} "Packaged THIRD_PARTY_NOTICES.txt")
    string(FIND "${_spark_notice}" "${_SPARK_NOTICE_INVENTORY_MARKER}" _spark_inventory_at)
    string(FIND "${_spark_notice}" "${_SPARK_NOTICE_TEXTS_MARKER}" _spark_texts_at)
    if(_spark_inventory_at EQUAL -1 OR _spark_texts_at EQUAL -1 OR _spark_texts_at LESS _spark_inventory_at)
        _spark_notice_fail(
            "${_spark_notice_path} does not have the generated 'Dependency inventory' and "
            "'Complete license and notice texts' sections (cmake/SparkThirdPartyAudit.cmake)")
    endif()
    string(LENGTH "${_SPARK_NOTICE_INVENTORY_MARKER}" _spark_marker_length)
    math(EXPR _spark_inventory_start "${_spark_inventory_at} + ${_spark_marker_length}")
    math(EXPR _spark_inventory_length "${_spark_texts_at} - ${_spark_inventory_start}")
    string(SUBSTRING "${_spark_notice}" ${_spark_inventory_start} ${_spark_inventory_length} _spark_inventory)
    string(LENGTH "${_SPARK_NOTICE_TEXTS_MARKER}" _spark_marker_length)
    math(EXPR _spark_texts_start "${_spark_texts_at} + ${_spark_marker_length}")
    string(SUBSTRING "${_spark_notice}" ${_spark_texts_start} -1 _spark_texts)

    # Inventory entries: a name line followed by "  Key: value" lines, separated
    # by blank lines.
    _spark_notice_encode(_spark_inventory "${_spark_inventory}")
    string(REPLACE "\n" ";" _spark_inventory_lines "${_spark_inventory}")
    set(_spark_block_count 0)
    set(_spark_in_block OFF)
    foreach(_spark_line IN LISTS _spark_inventory_lines)
        if(_spark_line STREQUAL "")
            set(_spark_in_block OFF)
            continue()
        endif()
        if(NOT _spark_in_block)
            if(_spark_line MATCHES "^ ")
                _spark_notice_decode(_spark_line "${_spark_line}")
                _spark_notice_fail("${_spark_notice_path}: inventory field without an entry name: '${_spark_line}'")
            endif()
            math(EXPR _spark_block_count "${_spark_block_count} + 1")
            _spark_notice_decode(_spark_block_name_${_spark_block_count} "${_spark_line}")
            set(_spark_block_notices_${_spark_block_count} "")
            set(_spark_block_files_${_spark_block_count} "")
            set(_spark_in_block ON)
        elseif(_spark_line MATCHES "^  Notice files: (.*)$")
            _spark_notice_decode(_spark_value "${CMAKE_MATCH_1}")
            _spark_notice_split_csv(_spark_block_notices_${_spark_block_count} "${_spark_value}")
        elseif(_spark_line MATCHES "^  Files: (.*)$")
            _spark_notice_decode(_spark_value "${CMAKE_MATCH_1}")
            _spark_notice_split_csv(_spark_block_files_${_spark_block_count} "${_spark_value}")
        endif()
    endforeach()
    if(_spark_block_count EQUAL 0)
        _spark_notice_fail("${_spark_notice_path} has an empty dependency inventory")
    endif()

    # Locate every declared notice section once; a section runs to the next
    # declared section header or the end of the file.
    set(_spark_all_notices "")
    foreach(_spark_block RANGE 1 ${_spark_block_count})
        list(APPEND _spark_all_notices ${_spark_block_notices_${_spark_block}})
    endforeach()
    list(REMOVE_DUPLICATES _spark_all_notices)
    set(_spark_header_positions "")
    set(_spark_notice_positions "")
    foreach(_spark_rel IN LISTS _spark_all_notices)
        string(FIND "${_spark_texts}" "----- ${_spark_rel} -----\n" _spark_position)
        list(APPEND _spark_notice_positions ${_spark_position})
        if(NOT _spark_position EQUAL -1)
            list(APPEND _spark_header_positions ${_spark_position})
        endif()
    endforeach()
    string(LENGTH "${_spark_texts}" _spark_texts_length)
    foreach(_spark_rel IN LISTS _spark_all_notices)
        list(FIND _spark_all_notices "${_spark_rel}" _spark_notice_index)
        list(GET _spark_notice_positions ${_spark_notice_index} _spark_position)
        if(_spark_position EQUAL -1)
            set(_spark_notice_problem_${_spark_notice_index} "license text for ${_spark_rel} is not reproduced")
            continue()
        endif()
        string(LENGTH "----- ${_spark_rel} -----\n" _spark_header_length)
        math(EXPR _spark_body_start "${_spark_position} + ${_spark_header_length}")
        set(_spark_body_end ${_spark_texts_length})
        foreach(_spark_other IN LISTS _spark_header_positions)
            if(_spark_other GREATER_EQUAL _spark_body_start AND _spark_other LESS _spark_body_end)
                set(_spark_body_end ${_spark_other})
            endif()
        endforeach()
        math(EXPR _spark_body_length "${_spark_body_end} - ${_spark_body_start}")
        string(SUBSTRING "${_spark_texts}" ${_spark_body_start} ${_spark_body_length} _spark_body)
        string(STRIP "${_spark_body}" _spark_body)
        string(LENGTH "${_spark_body}" _spark_body_bytes)
        if(_spark_body_bytes LESS _spark_min_bytes)
            set(_spark_notice_problem_${_spark_notice_index}
                "license text for ${_spark_rel} is shorter than ${_spark_min_bytes} bytes")
        elseif(NOT _spark_body MATCHES "${_spark_copyright_regex}")
            set(_spark_notice_problem_${_spark_notice_index} "license text for ${_spark_rel} has no copyright statement")
        elseif(NOT _spark_body MATCHES "${_spark_terms_regex}")
            set(_spark_notice_problem_${_spark_notice_index} "license text for ${_spark_rel} has no operative license terms")
        else()
            set(_spark_notice_problem_${_spark_notice_index} "")
        endif()
    endforeach()

    # An entry is licensed when it declares at least one notice file and every
    # declared notice file is reproduced with license text.
    set(_spark_component_names "")
    foreach(_spark_block RANGE 1 ${_spark_block_count})
        set(_spark_block_problem_${_spark_block} "")
        if(_spark_block_notices_${_spark_block} STREQUAL "")
            set(_spark_block_problem_${_spark_block} "declares no notice file")
        endif()
        foreach(_spark_rel IN LISTS _spark_block_notices_${_spark_block})
            list(FIND _spark_all_notices "${_spark_rel}" _spark_notice_index)
            if(NOT _spark_notice_problem_${_spark_notice_index} STREQUAL "")
                set(_spark_block_problem_${_spark_block} "${_spark_notice_problem_${_spark_notice_index}}")
                break()
            endif()
        endforeach()
        list(APPEND _spark_component_names "${_spark_block_name_${_spark_block}}")
    endforeach()

    # ---- package inventory --------------------------------------------------
    file(GLOB_RECURSE _spark_package_files LIST_DIRECTORIES false RELATIVE "${_spark_root}" "${_spark_root}/*")
    list(SORT _spark_package_files)
    set(_spark_uncovered "")
    set(_spark_font_count 0)
    set(_spark_payload_count 0)
    foreach(_spark_file IN LISTS _spark_package_files)
        get_filename_component(_spark_name "${_spark_file}" NAME)
        get_filename_component(_spark_suffix "${_spark_file}" LAST_EXT)
        string(TOLOWER "${_spark_suffix}" _spark_suffix)

        if(_spark_suffix IN_LIST _spark_font_suffixes)
            math(EXPR _spark_font_count "${_spark_font_count} + 1")
            set(_spark_reason "not named on any 'Files:' line of THIRD_PARTY_NOTICES.txt")
            foreach(_spark_block RANGE 1 ${_spark_block_count})
                set(_spark_named OFF)
                foreach(_spark_named_file IN LISTS _spark_block_files_${_spark_block})
                    get_filename_component(_spark_named_name "${_spark_named_file}" NAME)
                    if(_spark_named_name STREQUAL _spark_name)
                        set(_spark_named ON)
                    endif()
                endforeach()
                if(NOT _spark_named)
                    continue()
                endif()
                if(_spark_block_problem_${_spark_block} STREQUAL "")
                    set(_spark_reason "")
                    break()
                endif()
                set(_spark_reason
                    "named by '${_spark_block_name_${_spark_block}}' but ${_spark_block_problem_${_spark_block}}")
            endforeach()
            if(NOT _spark_reason STREQUAL "")
                list(APPEND _spark_uncovered "${_spark_file}: font ${_spark_reason}")
            endif()
            continue()
        endif()

        set(_spark_matched_rule -1)
        foreach(_spark_index RANGE ${_spark_last_rule})
            if(_spark_file MATCHES "${_spark_rule_pattern_${_spark_index}}")
                set(_spark_matched_rule ${_spark_index})
                break()
            endif()
        endforeach()
        if(_spark_matched_rule EQUAL -1)
            foreach(_spark_pattern IN LISTS _spark_root_patterns)
                if(_spark_file MATCHES "${_spark_pattern}")
                    math(EXPR _spark_payload_count "${_spark_payload_count} + 1")
                    list(APPEND _spark_uncovered
                        "${_spark_file}: third-party install path that no payload rule maps to a dependency")
                    break()
                endif()
            endforeach()
            continue()
        endif()

        math(EXPR _spark_payload_count "${_spark_payload_count} + 1")
        set(_spark_component "${_spark_rule_component_${_spark_matched_rule}}")
        if(_spark_component STREQUAL "")
            continue()
        endif()
        list(FIND _spark_component_names "${_spark_component}" _spark_block_index)
        if(_spark_block_index EQUAL -1)
            list(APPEND _spark_uncovered
                "${_spark_file}: component '${_spark_component}' has no THIRD_PARTY_NOTICES.txt inventory entry")
            continue()
        endif()
        math(EXPR _spark_block "${_spark_block_index} + 1")
        if(NOT _spark_block_problem_${_spark_block} STREQUAL "")
            list(APPEND _spark_uncovered
                "${_spark_file}: component '${_spark_component}' ${_spark_block_problem_${_spark_block}}")
        endif()
    endforeach()

    if(_spark_uncovered)
        list(LENGTH _spark_uncovered _spark_uncovered_count)
        list(JOIN _spark_uncovered "\n  " _spark_uncovered_report)
        string(CONCAT _spark_report
            "${_spark_uncovered_count} shipped file(s) are not covered by ${_spark_notice_path}:\n"
            "  ${_spark_uncovered_report}\n"
            "Add each dependency to ThirdParty/dependencies.lock with its on-disk license text "
            "(fonts must be listed in the entry's required files) or map the path in "
            "${_spark_rules_path}; never supply license text from memory.")
        if(_spark_mode STREQUAL "enforce")
            _spark_notice_fail("${_spark_report}")
        endif()
        message(WARNING "Package notice-coverage gate (report mode, not enforced): ${_spark_report}")
        return()
    endif()
    message(STATUS
        "Validated notice coverage for ${_spark_font_count} font file(s) and "
        "${_spark_payload_count} third-party payload file(s) in ${_spark_root}")
endfunction()

if(NOT DEFINED SPARK_PACKAGE_NOTICE_RULES OR SPARK_PACKAGE_NOTICE_RULES STREQUAL "")
    set(SPARK_PACKAGE_NOTICE_RULES "${_spark_notice_default_rules}")
endif()
if(NOT DEFINED SPARK_PACKAGE_NOTICE_COVERAGE OR SPARK_PACKAGE_NOTICE_COVERAGE STREQUAL "")
    set(SPARK_PACKAGE_NOTICE_COVERAGE enforce)
endif()
if(NOT DEFINED SPARK_PACKAGE_ROOT OR SPARK_PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT must name the staged install root")
endif()
if(SPARK_PACKAGE_ROOT MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT contains unsupported control or list characters")
endif()
_spark_validate_package_notice_coverage(
    "${SPARK_PACKAGE_ROOT}" "${SPARK_PACKAGE_NOTICE_RULES}" "${SPARK_PACKAGE_NOTICE_COVERAGE}")
