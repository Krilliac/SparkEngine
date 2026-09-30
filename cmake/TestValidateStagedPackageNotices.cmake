cmake_minimum_required(VERSION 3.25)

# GOV-400 LicenseInventory_* fixture contract for
# cmake/ValidateStagedPackageNotices.cmake. Each case stages a small package
# tree with a THIRD_PARTY_NOTICES.txt in the format cmake/SparkThirdPartyAudit.cmake
# generates, runs the gate as a separate CMake process, and checks the verdict
# and the uncovered-file report. Failing cases prove each detection path fires,
# so the passing cases are not the output of a gate that stopped checking.

foreach(_spark_required IN ITEMS SPARK_VALIDATOR SPARK_TEST_ROOT SPARK_BINARY_ROOT)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()

get_filename_component(_spark_validator_dir "${SPARK_VALIDATOR}" DIRECTORY)
file(READ "${_spark_validator_dir}/PackageNoticeCoverageRules.json" _spark_rules_default_text)
set(_spark_resolved_test_root "${SPARK_TEST_ROOT}")
set(_spark_resolved_binary_root "${SPARK_BINARY_ROOT}")
cmake_path(ABSOLUTE_PATH _spark_resolved_test_root NORMALIZE)
cmake_path(ABSOLUTE_PATH _spark_resolved_binary_root NORMALIZE)
cmake_path(IS_PREFIX _spark_resolved_binary_root "${_spark_resolved_test_root}"
    NORMALIZE _spark_test_root_is_bounded)
cmake_path(GET _spark_resolved_test_root FILENAME _spark_test_root_name)
if(NOT _spark_test_root_is_bounded OR
   _spark_resolved_test_root STREQUAL _spark_resolved_binary_root OR
   NOT _spark_test_root_name MATCHES "^package-notice-validator(-[A-Za-z0-9_.-]+)?$")
    message(FATAL_ERROR
        "Refusing to use package-notice scratch path unless it is a strict, named "
        "package-notice-validator descendant of the build tree")
endif()
file(REMOVE_RECURSE "${_spark_resolved_test_root}")
file(MAKE_DIRECTORY "${_spark_resolved_test_root}")

# Fixture license texts are written for this test; they only need the shape the
# gate checks (length, a copyright line, operative terms), not real authorship.
string(CONCAT _spark_fixture_font_license
    "Copyright 2026 Fixture Type Foundry\n\n"
    "This Font Software is licensed under a fixture license used only by the\n"
    "SparkEngine notice-coverage contract test.\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    "of the Font Software, to use, study, copy, merge, embed, modify, redistribute,\n"
    "and sell modified and unmodified copies of the Font Software.\n")
string(CONCAT _spark_fixture_library_license
    "MIT License\n\nCopyright (c) 2026 Fixture Library Author\n\n"
    "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    "of this software and associated documentation files (the \"Software\"), to deal\n"
    "in the Software without restriction.\n")
string(CONCAT _spark_fixture_terms_only
    "Fixture notice that names its author and nothing else. Copyright 2026 Fixture\n"
    "Type Foundry. This text is long enough to pass the length check but grants no\n"
    "rights, so the gate must not accept it as license text for a shipped font.\n")

# _spark_write_notice(<root> <font license body|MISSING> <jolt license body|MISSING> <font files csv>
#                     [<extra inventory entries>])
function(_spark_write_notice _spark_root _spark_font_body _spark_jolt_body _spark_font_files)
    set(_spark_extra_inventory "")
    if(ARGC GREATER 4)
        set(_spark_extra_inventory "${ARGV4}")
    endif()
    string(CONCAT _spark_notice
        "SparkEngine Third-Party Notices\n"
        "================================\n\n"
        "SparkEngine includes or can link the dependencies listed below. "
        "Their copyrights and license terms remain with their respective owners.\n\n"
        "Dependency inventory\n"
        "--------------------\n\n"
        "Jolt Physics\n"
        "  Source: https://example.invalid/jolt\n"
        "  Version: v5.5.1 (fixture; snapshot [pinned])\n"
        "  License: MIT\n"
        "  Notice files: ThirdParty/Physics/JoltPhysics/LICENSE\n"
        "  Files: Jolt/Jolt.h,Build/CMakeLists.txt\n\n"
        "Fixture Sans\n"
        "  Source: https://example.invalid/fixture-sans\n"
        "  Version: 1.0\n"
        "  License: OFL-1.1\n"
        "  Notice files: SparkEditor/Fonts/FixtureSans-LICENSE.txt\n"
        "  Files: ${_spark_font_files}\n\n"
        "${_spark_extra_inventory}"
        "Complete license and notice texts\n"
        "=================================\n\n")
    if(NOT _spark_jolt_body STREQUAL "MISSING")
        string(APPEND _spark_notice
            "----- ThirdParty/Physics/JoltPhysics/LICENSE -----\n\n${_spark_jolt_body}\n\n")
    endif()
    if(NOT _spark_font_body STREQUAL "MISSING")
        string(APPEND _spark_notice
            "----- SparkEditor/Fonts/FixtureSans-LICENSE.txt -----\n\n${_spark_font_body}\n\n")
    endif()
    file(WRITE "${_spark_root}/THIRD_PARTY_NOTICES.txt" "${_spark_notice}")
endfunction()

function(_spark_write_package _spark_root)
    file(MAKE_DIRECTORY
        "${_spark_root}/bin/EditorAssets/Fonts"
        "${_spark_root}/include/Jolt/Core"
        "${_spark_root}/include/SparkEngine/ThirdParty"
        "${_spark_root}/include/SparkEngine/Core")
    file(WRITE "${_spark_root}/LICENSE.txt" "fixture first-party license\n")
    file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/FixtureSans-Regular.ttf" "fixture font bytes\n")
    file(WRITE "${_spark_root}/include/Jolt/Jolt.h" "#pragma once\n")
    file(WRITE "${_spark_root}/include/Jolt/Core/Core.h" "#pragma once\n")
    # First-party headers outside every third-party root are not payload.
    file(WRITE "${_spark_root}/include/SparkEngine/Core/Engine.h" "#pragma once\n")
    # Documented first-party exemption inside a third-party root.
    file(WRITE "${_spark_root}/include/SparkEngine/ThirdParty/angelscript.h" "#pragma once\n")
    _spark_write_notice("${_spark_root}"
        "${_spark_fixture_font_license}" "${_spark_fixture_library_license}" "FixtureSans-Regular.ttf")
endfunction()

function(_spark_new_case _spark_name _spark_output)
    set(_spark_root "${_spark_resolved_test_root}/${_spark_name}")
    file(REMOVE_RECURSE "${_spark_root}")
    _spark_write_package("${_spark_root}")
    set(${_spark_output} "${_spark_root}" PARENT_SCOPE)
endfunction()

# The classification (open|closed) comes from _spark_world in the caller's scope.
set(_spark_world open)
function(_spark_run_gate _spark_root _spark_mode _spark_result_output _spark_log_output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSPARK_PACKAGE_ROOT=${_spark_root}"
            "-DSPARK_PACKAGE_NOTICE_COVERAGE=${_spark_mode}"
            "-DSPARK_PACKAGE_NOTICE_CLASSIFICATION=${_spark_world}"
            -P "${SPARK_VALIDATOR}"
        RESULT_VARIABLE _spark_result
        OUTPUT_VARIABLE _spark_stdout
        ERROR_VARIABLE _spark_stderr
        TIMEOUT 60)
    set(${_spark_result_output} "${_spark_result}" PARENT_SCOPE)
    set(${_spark_log_output} "${_spark_stdout}\n${_spark_stderr}" PARENT_SCOPE)
endfunction()

function(_spark_expect_pass _spark_label _spark_root)
    _spark_run_gate("${_spark_root}" enforce _spark_result _spark_log)
    if(NOT _spark_result EQUAL 0)
        message(FATAL_ERROR "LicenseInventory_${_spark_label}: expected the gate to pass:\n${_spark_log}")
    endif()
    if(NOT _spark_log MATCHES "Validated notice coverage for")
        message(FATAL_ERROR "LicenseInventory_${_spark_label}: pass without a coverage summary:\n${_spark_log}")
    endif()
    message(STATUS "LicenseInventory_${_spark_label}: passed")
endfunction()

# Every expected fragment must appear in the failure log (CMake wraps long
# messages, so fragments are matched after collapsing whitespace).
function(_spark_expect_fail _spark_label _spark_root)
    _spark_run_gate("${_spark_root}" enforce _spark_result _spark_log)
    if(_spark_result EQUAL 0)
        message(FATAL_ERROR "LicenseInventory_${_spark_label}: expected the gate to fail:\n${_spark_log}")
    endif()
    string(REGEX REPLACE "[ \t\r\n]+" " " _spark_flat "${_spark_log}")
    foreach(_spark_fragment IN LISTS ARGN)
        string(FIND "${_spark_flat}" "${_spark_fragment}" _spark_at)
        if(_spark_at EQUAL -1)
            message(FATAL_ERROR
                "LicenseInventory_${_spark_label}: failure log does not contain '${_spark_fragment}':\n${_spark_log}")
        endif()
    endforeach()
    message(STATUS "LicenseInventory_${_spark_label}: rejected as expected")
endfunction()

# 1. A named font with reproduced license text, mapped third-party headers, and
#    a documented first-party exemption pass.
_spark_new_case(covered_font _spark_root)
_spark_expect_pass(CoveredFont "${_spark_root}")

# 2. A shipped font that no inventory entry names is reported by path.
_spark_new_case(uncovered_font _spark_root)
file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/Unlisted-Bold.otf" "fixture font bytes\n")
_spark_expect_fail(UncoveredFont "${_spark_root}"
    "bin/EditorAssets/Fonts/Unlisted-Bold.otf: font not named on any 'Files:' line")

# 3. An entry names the font but its notice file is not reproduced at all.
_spark_new_case(font_named_without_text _spark_root)
_spark_write_notice("${_spark_root}" MISSING "${_spark_fixture_library_license}" "FixtureSans-Regular.ttf")
_spark_expect_fail(FontNamedWithoutLicenseText "${_spark_root}"
    "bin/EditorAssets/Fonts/FixtureSans-Regular.ttf: font named by 'Fixture Sans' but license text for SparkEditor/Fonts/FixtureSans-LICENSE.txt is not reproduced")

# 4. The notice section exists but carries no operative license terms.
_spark_new_case(font_named_without_terms _spark_root)
_spark_write_notice("${_spark_root}" "${_spark_fixture_terms_only}" "${_spark_fixture_library_license}"
    "FixtureSans-Regular.ttf")
_spark_expect_fail(FontNamedWithoutLicenseTerms "${_spark_root}"
    "license text for SparkEditor/Fonts/FixtureSans-LICENSE.txt has no operative license terms")

# 5. An uncovered font in report mode is listed as a warning without failing.
_spark_new_case(report_mode _spark_root)
file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/Unlisted.woff2" "fixture font bytes\n")
_spark_run_gate("${_spark_root}" report _spark_result _spark_log)
string(REGEX REPLACE "[ \t\r\n]+" " " _spark_flat "${_spark_log}")
string(FIND "${_spark_flat}" "bin/EditorAssets/Fonts/Unlisted.woff2: font not named" _spark_at)
if(NOT _spark_result EQUAL 0 OR _spark_at EQUAL -1 OR NOT _spark_flat MATCHES "report mode, not enforced")
    message(FATAL_ERROR "LicenseInventory_ReportMode: expected a non-failing warning listing the font:\n${_spark_log}")
endif()
message(STATUS "LicenseInventory_ReportMode: warned as expected")

# 6. Third-party payload whose component has no license text is uncovered.
_spark_new_case(payload_without_text _spark_root)
_spark_write_notice("${_spark_root}" "${_spark_fixture_font_license}" MISSING "FixtureSans-Regular.ttf")
_spark_expect_fail(PayloadWithoutLicenseText "${_spark_root}"
    "include/Jolt/Core/Core.h: component 'Jolt Physics' license text for ThirdParty/Physics/JoltPhysics/LICENSE is not reproduced"
    "include/Jolt/Jolt.h: component 'Jolt Physics'")

# 7. A file under a third-party root that no payload rule maps fails closed.
_spark_new_case(unmapped_payload _spark_root)
file(MAKE_DIRECTORY "${_spark_root}/include/SparkEngine/ThirdParty/newlib")
file(WRITE "${_spark_root}/include/SparkEngine/ThirdParty/newlib/newlib.h" "#pragma once\n")
_spark_expect_fail(UnmappedPayload "${_spark_root}"
    "include/SparkEngine/ThirdParty/newlib/newlib.h: third-party install path that no payload rule maps")

# 8. A mapped component with no inventory entry at all is uncovered.
_spark_new_case(payload_without_entry _spark_root)
file(MAKE_DIRECTORY "${_spark_root}/include/SparkEngine/ThirdParty/zstd")
file(WRITE "${_spark_root}/include/SparkEngine/ThirdParty/zstd/zstd.h" "#pragma once\n")
_spark_expect_fail(PayloadWithoutInventoryEntry "${_spark_root}"
    "include/SparkEngine/ThirdParty/zstd/zstd.h: component 'zstd' has no THIRD_PARTY_NOTICES.txt inventory entry")

# 9. A missing or unstructured notice file fails closed.
_spark_new_case(missing_notice _spark_root)
file(REMOVE "${_spark_root}/THIRD_PARTY_NOTICES.txt")
_spark_expect_fail(MissingNoticeFile "${_spark_root}"
    "Packaged THIRD_PARTY_NOTICES.txt is missing")
_spark_new_case(unstructured_notice _spark_root)
file(WRITE "${_spark_root}/THIRD_PARTY_NOTICES.txt"
    "Fixture Sans: FixtureSans-Regular.ttf\n${_spark_fixture_font_license}")
_spark_expect_fail(UnstructuredNoticeFile "${_spark_root}"
    "does not have the generated 'Dependency inventory'")

# 10. Malformed rules fail closed instead of exempting everything.
_spark_new_case(malformed_rules _spark_root)
set(_spark_bad_rules "${_spark_resolved_test_root}/bad-rules.json")
file(WRITE "${_spark_bad_rules}"
    "{\"schema\": 1, \"fontSuffixes\": [\".ttf\"], "
    "\"licenseText\": {\"minimumBytes\": 200, \"copyrightPattern\": \"[Cc]opyright\", "
    "\"operativeTermsPattern\": \"Permission\"}, \"thirdPartyRoots\": [\"^include/Jolt/\"], "
    "\"payloadRules\": [{\"pattern\": \"^include/Jolt/\"}]}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSPARK_PACKAGE_ROOT=${_spark_root}"
        "-DSPARK_PACKAGE_NOTICE_RULES=${_spark_bad_rules}"
        -P "${SPARK_VALIDATOR}"
    RESULT_VARIABLE _spark_result
    OUTPUT_VARIABLE _spark_stdout
    ERROR_VARIABLE _spark_stderr
    TIMEOUT 60)
if(_spark_result EQUAL 0 OR NOT _spark_stderr MATCHES "must name exactly one of")
    message(FATAL_ERROR "LicenseInventory_MalformedRules: expected a rules error:\n${_spark_stdout}\n${_spark_stderr}")
endif()
message(STATUS "LicenseInventory_MalformedRules: rejected as expected")

# 11-13. The Microsoft Visual C++ runtime that InstallRequiredSystemLibraries
#        installs into bin/ must be named, DLL by DLL, on the 'Files:' line of a
#        "Microsoft Visual C++ Runtime" entry that carries a 'Terms:' line.
set(_spark_runtime "Microsoft Visual C++ Runtime")
function(_spark_runtime_entry _spark_output _spark_files _spark_with_terms)
    set(_spark_entry "${_spark_runtime}\n  Source: fixture\n  Version: MSVC 19.44 (fixture)\n  License: fixture terms\n")
    if(_spark_with_terms)
        string(APPEND _spark_entry "  Terms: Microsoft Visual C++ Redistributable, Distributable Code (fixture)\n")
    endif()
    set(${_spark_output} "${_spark_entry}  Files: ${_spark_files}\n\n" PARENT_SCOPE)
endfunction()
function(_spark_new_runtime_case _spark_name _spark_output)
    _spark_new_case(${_spark_name} _spark_root)
    file(WRITE "${_spark_root}/bin/vcruntime140.dll" "pe\n")
    file(WRITE "${_spark_root}/bin/msvcp140.dll" "pe\n")
    set(${_spark_output} "${_spark_root}" PARENT_SCOPE)
endfunction()

_spark_new_runtime_case(runtime_without_entry _spark_root)
_spark_expect_fail(RuntimeWithoutEntry "${_spark_root}"
    "bin/msvcp140.dll: system runtime '${_spark_runtime}' has no THIRD_PARTY_NOTICES.txt inventory entry"
    "bin/vcruntime140.dll: system runtime '${_spark_runtime}' has no THIRD_PARTY_NOTICES.txt inventory entry")

_spark_new_runtime_case(runtime_names_one_dll _spark_root)
_spark_runtime_entry(_spark_entry "vcruntime140.dll" ON)
_spark_write_notice("${_spark_root}" "${_spark_fixture_font_license}" "${_spark_fixture_library_license}"
    "FixtureSans-Regular.ttf" "${_spark_entry}")
_spark_expect_fail(RuntimeEntryNamesOneDll "${_spark_root}"
    "1 shipped file(s) are not covered"
    "bin/msvcp140.dll: not named on the 'Files:' line of system runtime '${_spark_runtime}'")

_spark_new_runtime_case(runtime_without_terms _spark_root)
_spark_runtime_entry(_spark_entry "vcruntime140.dll,msvcp140.dll" OFF)
_spark_write_notice("${_spark_root}" "${_spark_fixture_font_license}" "${_spark_fixture_library_license}"
    "FixtureSans-Regular.ttf" "${_spark_entry}")
_spark_expect_fail(RuntimeEntryWithoutTerms "${_spark_root}"
    "bin/vcruntime140.dll: system runtime '${_spark_runtime}' has no 'Terms:' line")

_spark_new_runtime_case(runtime_covered _spark_root)
_spark_runtime_entry(_spark_entry "vcruntime140.dll,msvcp140.dll" ON)
_spark_write_notice("${_spark_root}" "${_spark_fixture_font_license}" "${_spark_fixture_library_license}"
    "FixtureSans-Regular.ttf" "${_spark_entry}")
_spark_expect_pass(RuntimeCovered "${_spark_root}")

# 13b. A shipped license text (noticeText rule: the editor fonts' LICENSES
#      directory) must be a 'Notice files:' text the notice reproduces.
_spark_new_case(license_text_reproduced _spark_root)
file(MAKE_DIRECTORY "${_spark_root}/bin/EditorAssets/Fonts/LICENSES")
file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/LICENSES/FixtureSans-LICENSE.txt" "fixture license\n")
_spark_expect_pass(LicenseTextReproduced "${_spark_root}")
file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/LICENSES/Other-LICENSE.txt" "fixture license\n")
_spark_expect_fail(LicenseTextNotInNotice "${_spark_root}"
    "1 shipped file(s) are not covered"
    "bin/EditorAssets/Fonts/LICENSES/Other-LICENSE.txt: license text not a 'Notice files:' text")
_spark_new_case(license_text_not_reproduced _spark_root)
file(MAKE_DIRECTORY "${_spark_root}/bin/EditorAssets/Fonts/LICENSES")
file(WRITE "${_spark_root}/bin/EditorAssets/Fonts/LICENSES/FixtureSans-LICENSE.txt" "fixture license\n")
_spark_write_notice("${_spark_root}" MISSING "${_spark_fixture_library_license}" "FixtureSans-Regular.ttf")
_spark_expect_fail(LicenseTextNotReproduced "${_spark_root}"
    "bin/EditorAssets/Fonts/LICENSES/FixtureSans-LICENSE.txt: license text named by 'Fixture Sans' but license text for SparkEditor/Fonts/FixtureSans-LICENSE.txt is not reproduced")

# 14. Closed world: nothing is presumed first-party. The base package is fully
#     classified; an unmapped DLL that open world accepts is unclassified.
_spark_new_case(closed_world _spark_root)
file(WRITE "${_spark_root}/bin/SparkEngine.exe" "pe\n")
file(WRITE "${_spark_root}/bin/foo.dll" "pe\n")
_spark_expect_pass(OpenWorldPresumesFirstParty "${_spark_root}")
set(_spark_world closed)
_spark_expect_fail(ClosedWorldUnclassified "${_spark_root}"
    "1 shipped file(s) are not covered"
    "bin/foo.dll: unclassified: no font, payload, system-runtime or first-party rule covers it")
file(REMOVE "${_spark_root}/bin/foo.dll")
_spark_expect_pass(ClosedWorldClassified "${_spark_root}")
_spark_run_gate("${_spark_root}" enforce _spark_result _spark_log)
string(REGEX REPLACE "[ \t\r\n]+" " " _spark_flat "${_spark_log}")
string(FIND "${_spark_flat}" "Closed world: 4 first-party file(s), 0 asset-manifest file(s), 0 unclassified" _spark_at)
if(_spark_at EQUAL -1)
    message(FATAL_ERROR "LicenseInventory_ClosedWorldCounts: expected 4 first-party files:\n${_spark_log}")
endif()
set(_spark_world open)

# 15. A first-party root without a written justification is a rules error.
string(JSON _spark_rules_text ERROR_VARIABLE _spark_error
    SET "${_spark_rules_default_text}" firstPartyRoots 0 justification "\"\"")
file(WRITE "${_spark_bad_rules}" "${_spark_rules_text}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DSPARK_PACKAGE_ROOT=${_spark_root}"
        "-DSPARK_PACKAGE_NOTICE_RULES=${_spark_bad_rules}"
        -P "${SPARK_VALIDATOR}"
    RESULT_VARIABLE _spark_result
    OUTPUT_VARIABLE _spark_stdout
    ERROR_VARIABLE _spark_stderr
    TIMEOUT 60)
if(_spark_error OR _spark_result EQUAL 0 OR NOT _spark_stderr MATCHES "non-empty 'justification'")
    message(FATAL_ERROR
        "LicenseInventory_FirstPartyRootWithoutJustification: expected a rules error:\n"
        "${_spark_error}\n${_spark_stdout}\n${_spark_stderr}")
endif()
message(STATUS "LicenseInventory_FirstPartyRootWithoutJustification: rejected as expected")

# 16. A shipped binary that embeds a font (Dear ImGui compiles ProggyClean.ttf
#     into every binary that links it) is uncovered until an entry names the
#     font; the marker is found in the binary's bytes, not its file name.
string(ASCII 127 1 _spark_nonprint)
string(CONCAT _spark_fixture_binary
    "${_spark_nonprint}ELF${_spark_nonprint}code${_spark_nonprint}ProggyClean.ttf${_spark_nonprint}more")
_spark_new_case(binary_embeds_unlisted_font _spark_root)
file(WRITE "${_spark_root}/bin/SparkEditor" "${_spark_fixture_binary}")
_spark_expect_fail(BinaryEmbedsUnlistedFont "${_spark_root}"
    "bin/SparkEditor: embeds font ProggyClean.ttf not named on any 'Files:' line")

# 17. The same binary passes once an entry names the font and reproduces its
#     license text, and the pass counts the embedded font.
_spark_new_case(binary_embeds_listed_font _spark_root)
file(WRITE "${_spark_root}/bin/SparkEditor" "${_spark_fixture_binary}")
file(READ "${_spark_root}/THIRD_PARTY_NOTICES.txt" _spark_notice)
string(CONCAT _spark_embedded_entry
    "Fixture Pixel (font embedded in Fixture UI)\n  Source: fixture\n  Version: 1\n  License: MIT\n"
    "  Notice files: ThirdParty/Licenses/FixturePixel-LICENSE.txt\n  Files: ProggyClean.ttf\n\n"
    "Complete license and notice texts\n")
string(REPLACE "Complete license and notice texts\n" "${_spark_embedded_entry}" _spark_notice "${_spark_notice}")
string(APPEND _spark_notice
    "----- ThirdParty/Licenses/FixturePixel-LICENSE.txt -----\n\n${_spark_fixture_library_license}\n\n")
file(WRITE "${_spark_root}/THIRD_PARTY_NOTICES.txt" "${_spark_notice}")
_spark_run_gate("${_spark_root}" enforce _spark_result _spark_log)
if(NOT _spark_result EQUAL 0 OR NOT _spark_log MATCHES "1 font file\\(s\\), 1 embedded font\\(s\\)")
    message(FATAL_ERROR "LicenseInventory_BinaryEmbedsListedFont: expected a pass counting the embedded font:\n${_spark_log}")
endif()
message(STATUS "LicenseInventory_BinaryEmbedsListedFont: passed")

message(STATUS "LicenseInventory package notice-coverage contract passed")
