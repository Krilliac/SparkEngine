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
#   * a file matching a systemRuntime rule (a toolchain runtime such as the
#     Microsoft Visual C++ runtime, whose redistribution terms are not a file in
#     the repository) is covered when the named inventory entry has a "Terms:"
#     line and names the file on its "Files:" line;
#   * a file under a third-party root that no payload rule maps is uncovered;
#   * in closed-world classification, any other file must either be listed with
#     an identified license (not NOASSERTION) in the installed asset manifest of
#     a matching assetManifests rule (the RDY-020 assets.integrity.json), or
#     match a justified firstPartyRoots pattern; otherwise it is reported as
#     unclassified. Open world (the default) presumes it first-party under the
#     root LICENSE.
#
# The rule set lives in cmake/PackageNoticeCoverageRules.json and is shared with
# tools/governance/generate_third_party_notices.py (--check-package), so both
# tools apply one rule set. Package content is only read as bounded data.
#
# Standalone use (fails closed and lists every uncovered file):
#   cmake -DSPARK_PACKAGE_ROOT=<staged install root>
#         [-DSPARK_PACKAGE_NOTICE_RULES=<rules json>]
#         [-DSPARK_PACKAGE_NOTICE_COVERAGE=enforce|report]
#         [-DSPARK_PACKAGE_NOTICE_CLASSIFICATION=open|closed]
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

function(_spark_validate_package_notice_coverage _spark_root _spark_rules_path _spark_mode _spark_world)
    if(NOT _spark_mode STREQUAL "enforce" AND NOT _spark_mode STREQUAL "report")
        _spark_notice_fail("SPARK_PACKAGE_NOTICE_COVERAGE must be 'enforce' or 'report', not '${_spark_mode}'")
    endif()
    if(NOT _spark_world STREQUAL "open" AND NOT _spark_world STREQUAL "closed")
        _spark_notice_fail("SPARK_PACKAGE_NOTICE_CLASSIFICATION must be 'open' or 'closed', not '${_spark_world}'")
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
        string(JSON _spark_rule_runtime_${_spark_index} ERROR_VARIABLE _spark_runtime_error
            GET "${_spark_rules}" payloadRules ${_spark_index} systemRuntime)
        if(_spark_component_error)
            set(_spark_rule_component_${_spark_index} "")
        endif()
        if(_spark_first_party_error)
            set(_spark_rule_first_party "")
        endif()
        if(_spark_runtime_error)
            set(_spark_rule_runtime_${_spark_index} "")
        endif()
        # Exactly one of component/firstParty/systemRuntime, and a first-party
        # exemption must say why, so an empty rule cannot silently exempt payload.
        set(_spark_rule_targets 0)
        foreach(_spark_target IN ITEMS "${_spark_rule_component_${_spark_index}}" "${_spark_rule_first_party}"
                                       "${_spark_rule_runtime_${_spark_index}}")
            if(NOT _spark_target STREQUAL "")
                math(EXPR _spark_rule_targets "${_spark_rule_targets} + 1")
            endif()
        endforeach()
        if(NOT _spark_rule_targets EQUAL 1)
            _spark_notice_fail(
                "payloadRules[${_spark_index}] must name exactly one of 'component', 'firstParty' or 'systemRuntime'")
        endif()
    endforeach()

    # First-party roots: every entry needs a pattern and a written justification,
    # because closed-world classification accepts whatever they match.
    _spark_notice_json_length(_spark_first_party_count "${_spark_rules}" firstPartyRoots)
    math(EXPR _spark_last_first_party "${_spark_first_party_count} - 1")
    set(_spark_first_party_patterns "")
    foreach(_spark_index RANGE ${_spark_last_first_party})
        _spark_notice_json_get(_spark_pattern "${_spark_rules}" firstPartyRoots ${_spark_index} pattern)
        string(JSON _spark_justification ERROR_VARIABLE _spark_justification_error
            GET "${_spark_rules}" firstPartyRoots ${_spark_index} justification)
        string(STRIP "${_spark_justification}" _spark_justification)
        if(_spark_justification_error OR _spark_justification STREQUAL "" OR _spark_pattern STREQUAL "")
            _spark_notice_fail("firstPartyRoots[${_spark_index}] must carry a pattern and a non-empty 'justification'")
        endif()
        list(APPEND _spark_first_party_patterns "${_spark_pattern}")
    endforeach()

    _spark_notice_json_length(_spark_asset_rule_count "${_spark_rules}" assetManifests)
    math(EXPR _spark_last_asset_rule "${_spark_asset_rule_count} - 1")
    foreach(_spark_index RANGE ${_spark_last_asset_rule})
        _spark_notice_json_get(_spark_asset_pattern_${_spark_index} "${_spark_rules}"
            assetManifests ${_spark_index} pattern)
        _spark_notice_json_get(_spark_asset_manifest_${_spark_index} "${_spark_rules}"
            assetManifests ${_spark_index} manifest)
        string(JSON _spark_justification ERROR_VARIABLE _spark_justification_error
            GET "${_spark_rules}" assetManifests ${_spark_index} justification)
        string(STRIP "${_spark_justification}" _spark_justification)
        if(_spark_justification_error OR _spark_justification STREQUAL "" OR
           _spark_asset_manifest_${_spark_index} STREQUAL "" OR
           _spark_asset_manifest_${_spark_index} MATCHES "^/|(^|/)[.][.](/|$)")
            _spark_notice_fail(
                "assetManifests[${_spark_index}] must name a package-relative 'manifest' and a non-empty 'justification'")
        endif()
        set(_spark_asset_text_${_spark_index} "<unread>")
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
            set(_spark_block_terms_${_spark_block_count} "")
            set(_spark_in_block ON)
        elseif(_spark_line MATCHES "^  Notice files: (.*)$")
            _spark_notice_decode(_spark_value "${CMAKE_MATCH_1}")
            _spark_notice_split_csv(_spark_block_notices_${_spark_block_count} "${_spark_value}")
        elseif(_spark_line MATCHES "^  Files: (.*)$")
            _spark_notice_decode(_spark_value "${CMAKE_MATCH_1}")
            _spark_notice_split_csv(_spark_block_files_${_spark_block_count} "${_spark_value}")
        elseif(_spark_line MATCHES "^  Terms: (.*)$")
            _spark_notice_decode(_spark_value "${CMAKE_MATCH_1}")
            string(STRIP "${_spark_value}" _spark_block_terms_${_spark_block_count})
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
    set(_spark_first_party_files 0)
    set(_spark_asset_files 0)
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
            set(_spark_under_third_party_root OFF)
            foreach(_spark_pattern IN LISTS _spark_root_patterns)
                if(_spark_file MATCHES "${_spark_pattern}")
                    set(_spark_under_third_party_root ON)
                    math(EXPR _spark_payload_count "${_spark_payload_count} + 1")
                    list(APPEND _spark_uncovered
                        "${_spark_file}: third-party install path that no payload rule maps to a dependency")
                    break()
                endif()
            endforeach()
            set(_spark_asset_rule -1)
            if(NOT _spark_under_third_party_root AND _spark_world STREQUAL "closed")
                foreach(_spark_index RANGE ${_spark_last_asset_rule})
                    if(_spark_file MATCHES "${_spark_asset_pattern_${_spark_index}}")
                        set(_spark_asset_rule ${_spark_index})
                        break()
                    endif()
                endforeach()
            endif()
            if(NOT _spark_asset_rule EQUAL -1)
                # The manifest is the canonical json.dumps(indent=2) output of
                # tools/asset-integrity/verify_asset_integrity.py. CMake's JSON
                # parser re-reads the whole document per query, which is too slow
                # for a thousand entries, so each entry is located textually by
                # its "path" member and its "license" member is read from the
                # same object. A manifest in any other layout lists nothing, so
                # the gate fails closed.
                set(_spark_manifest "${_spark_asset_manifest_${_spark_asset_rule}}")
                if(_spark_asset_text_${_spark_asset_rule} STREQUAL "<unread>")
                    set(_spark_asset_text_${_spark_asset_rule} "")
                    set(_spark_manifest_path "${_spark_root}/${_spark_manifest}")
                    if(EXISTS "${_spark_manifest_path}" AND NOT IS_DIRECTORY "${_spark_manifest_path}")
                        _spark_notice_read_bounded(_spark_asset_text_${_spark_asset_rule} "${_spark_manifest_path}"
                            ${_SPARK_NOTICE_MAX_BYTES} "Asset manifest")
                    endif()
                endif()
                get_filename_component(_spark_manifest_dir "${_spark_manifest}" DIRECTORY)
                set(_spark_listed "${_spark_file}")
                string(FIND "${_spark_file}" "${_spark_manifest_dir}/" _spark_prefix_at)
                if(_spark_prefix_at EQUAL 0)
                    string(LENGTH "${_spark_manifest_dir}/" _spark_prefix_length)
                    string(SUBSTRING "${_spark_file}" ${_spark_prefix_length} -1 _spark_listed)
                endif()
                string(FIND "${_spark_asset_text_${_spark_asset_rule}}" "\"path\": \"${_spark_listed}\"," _spark_at)
                set(_spark_license "")
                if(NOT _spark_at EQUAL -1)
                    string(SUBSTRING "${_spark_asset_text_${_spark_asset_rule}}" ${_spark_at} -1 _spark_entry)
                    string(FIND "${_spark_entry}" "}" _spark_entry_end)
                    string(SUBSTRING "${_spark_entry}" 0 ${_spark_entry_end} _spark_entry)
                    if(_spark_entry MATCHES "\"license\": \"([^\"]*)\"")
                        set(_spark_license "${CMAKE_MATCH_1}")
                    endif()
                endif()
                if(_spark_file STREQUAL _spark_manifest)
                    math(EXPR _spark_asset_files "${_spark_asset_files} + 1")
                elseif(_spark_at EQUAL -1)
                    list(APPEND _spark_uncovered "${_spark_file}: not listed in asset manifest ${_spark_manifest}")
                elseif(_spark_license STREQUAL "" OR _spark_license STREQUAL "NOASSERTION")
                    list(APPEND _spark_uncovered
                        "${_spark_file}: asset manifest ${_spark_manifest} records no identified license")
                else()
                    math(EXPR _spark_asset_files "${_spark_asset_files} + 1")
                endif()
            elseif(NOT _spark_under_third_party_root AND _spark_world STREQUAL "closed")
                set(_spark_classified OFF)
                foreach(_spark_pattern IN LISTS _spark_first_party_patterns)
                    if(_spark_file MATCHES "${_spark_pattern}")
                        set(_spark_classified ON)
                        break()
                    endif()
                endforeach()
                if(_spark_classified)
                    math(EXPR _spark_first_party_files "${_spark_first_party_files} + 1")
                else()
                    list(APPEND _spark_uncovered
                        "${_spark_file}: unclassified: no font, payload, system-runtime or first-party rule covers it")
                endif()
            endif()
            continue()
        endif()

        math(EXPR _spark_payload_count "${_spark_payload_count} + 1")
        set(_spark_runtime "${_spark_rule_runtime_${_spark_matched_rule}}")
        if(NOT _spark_runtime STREQUAL "")
            list(FIND _spark_component_names "${_spark_runtime}" _spark_block_index)
            if(_spark_block_index EQUAL -1)
                list(APPEND _spark_uncovered
                    "${_spark_file}: system runtime '${_spark_runtime}' has no THIRD_PARTY_NOTICES.txt inventory entry")
                continue()
            endif()
            math(EXPR _spark_block "${_spark_block_index} + 1")
            if(_spark_block_terms_${_spark_block} STREQUAL "")
                list(APPEND _spark_uncovered "${_spark_file}: system runtime '${_spark_runtime}' has no 'Terms:' line")
                continue()
            endif()
            set(_spark_named OFF)
            foreach(_spark_named_file IN LISTS _spark_block_files_${_spark_block})
                get_filename_component(_spark_named_name "${_spark_named_file}" NAME)
                if(_spark_named_name STREQUAL _spark_name)
                    set(_spark_named ON)
                endif()
            endforeach()
            if(NOT _spark_named)
                list(APPEND _spark_uncovered
                    "${_spark_file}: not named on the 'Files:' line of system runtime '${_spark_runtime}'")
            endif()
            continue()
        endif()
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
    if(_spark_world STREQUAL "closed")
        message(STATUS
            "Closed world: ${_spark_first_party_files} first-party file(s), "
            "${_spark_asset_files} asset-manifest file(s), 0 unclassified")
    endif()
endfunction()

if(NOT DEFINED SPARK_PACKAGE_NOTICE_RULES OR SPARK_PACKAGE_NOTICE_RULES STREQUAL "")
    set(SPARK_PACKAGE_NOTICE_RULES "${_spark_notice_default_rules}")
endif()
if(NOT DEFINED SPARK_PACKAGE_NOTICE_COVERAGE OR SPARK_PACKAGE_NOTICE_COVERAGE STREQUAL "")
    set(SPARK_PACKAGE_NOTICE_COVERAGE enforce)
endif()
if(NOT DEFINED SPARK_PACKAGE_NOTICE_CLASSIFICATION OR SPARK_PACKAGE_NOTICE_CLASSIFICATION STREQUAL "")
    set(SPARK_PACKAGE_NOTICE_CLASSIFICATION open)
endif()
if(NOT DEFINED SPARK_PACKAGE_ROOT OR SPARK_PACKAGE_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT must name the staged install root")
endif()
if(SPARK_PACKAGE_ROOT MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_PACKAGE_ROOT contains unsupported control or list characters")
endif()
_spark_validate_package_notice_coverage(
    "${SPARK_PACKAGE_ROOT}" "${SPARK_PACKAGE_NOTICE_RULES}" "${SPARK_PACKAGE_NOTICE_COVERAGE}"
    "${SPARK_PACKAGE_NOTICE_CLASSIFICATION}")
