cmake_minimum_required(VERSION 3.25)

# ENG-220: malformed content handed to the INSTALLED engine fails the launch with
# a named, actionable diagnostic and a non-zero exit instead of an empty running
# engine. The installed SparkEngine.exe accepts a content path only through
# `-headless -scene <file>` (the reflected-scene loader); it does not decode glTF
# or material files, so the corrupt GLB and material fixtures prove the content
# path rejects a corrupt asset by name, not that a glTF or material importer did
# (see Tests/Fixtures/MalformedContent/README.md).
#
# Inputs: SPARK_INSTALLED_ROOT (a staged runtime install prefix), SPARK_SOURCE_ROOT
# (fixtures live under Tests/Fixtures/MalformedContent), SPARK_TEST_ROOT (absent
# or empty; holds the copied fixtures and an isolated user-data root).
# SPARK_MALFORMED_CONTENT_PARSER_SELF_TEST=ON runs the verdict function against
# canned host output with no engine, so it works without a build.

# Exit status of a launch whose -scene could not load (kHostSceneLoadFailedExitCode).
set(_spark_scene_failed_exit 4)

# The verdict for one run. A rejected fixture must exit with the scene-failure
# status, name the fixture file, contain the expected reason, and never print a
# scene record. The positive control must exit 0 with exactly one scene record,
# which proves the rejections are about the content and not about a host that
# cannot start engine-only.
function(_spark_judge_content_run label expect_exit expected_text fixture_name result output out_ok out_reason)
    set(_ok TRUE)
    set(_reason "")
    string(REPLACE "\r\n" "\n" _text "${output}")
    if(NOT "${result}" STREQUAL "${expect_exit}")
        set(_ok FALSE)
        set(_reason "${label}: exited '${result}', expected ${expect_exit}")
    elseif("${expect_exit}" STREQUAL "0")
        string(REGEX MATCHALL "SPARK_SCENE_LOADED[^\n]*" _records "${_text}")
        list(LENGTH _records _record_count)
        if(NOT _record_count EQUAL 1 OR NOT _text MATCHES "(^|\n)${expected_text}")
            set(_ok FALSE)
            set(_reason "${label}: expected one '${expected_text}' record, found ${_record_count}: ${_records}")
        endif()
    else()
        if(_text MATCHES "SPARK_SCENE_LOADED")
            set(_ok FALSE)
            set(_reason "${label}: printed a scene record for content that must not load")
        else()
            string(FIND "${_text}" "${expected_text}" _text_position)
            string(FIND "${_text}" "${fixture_name}" _name_position)
            if(_text_position EQUAL -1)
                set(_ok FALSE)
                set(_reason "${label}: diagnostic lacks the reason '${expected_text}'")
            elseif(_name_position EQUAL -1)
                set(_ok FALSE)
                set(_reason "${label}: diagnostic does not name the fixture '${fixture_name}'")
            endif()
        endif()
    endif()
    set(${out_ok} "${_ok}" PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()

if(SPARK_MALFORMED_CONTENT_PARSER_SELF_TEST)
    function(_spark_expect_judgement name expect_exit expected_text fixture result output expected_ok)
        _spark_judge_content_run("${name}" "${expect_exit}" "${expected_text}" "${fixture}" "${result}"
            "${output}" _actual_ok _why)
        if(expected_ok AND NOT _actual_ok)
            message(FATAL_ERROR "Malformed-content verdict '${name}' unexpectedly failed: ${_why}")
        elseif(NOT expected_ok AND _actual_ok)
            message(FATAL_ERROR "Malformed-content verdict '${name}' unexpectedly passed")
        endif()
    endfunction()

    set(_rejected
        "[-scene] Failed to load 'C:/t/truncated.glb': Scene 'C:/t/truncated.glb' was rejected: scene could not be read: parse error. Previous-good backup 'C:/t/truncated.glb.bak' was not usable: file does not exist.\n")
    _spark_expect_judgement(rejected 4 "scene could not be read" truncated.glb 4 "${_rejected}" TRUE)
    string(REPLACE "\n" "\r\n" _rejected_crlf "${_rejected}")
    _spark_expect_judgement(rejected-crlf 4 "scene could not be read" truncated.glb 4 "${_rejected_crlf}" TRUE)
    _spark_expect_judgement(exit-zero 4 "scene could not be read" truncated.glb 0 "${_rejected}" FALSE)
    _spark_expect_judgement(crash-exit 4 "scene could not be read" truncated.glb -1 "${_rejected}" FALSE)
    _spark_expect_judgement(wrong-reason 4 "is unsupported" truncated.glb 4 "${_rejected}" FALSE)
    _spark_expect_judgement(unnamed 4 "scene could not be read" other.glb 4 "${_rejected}" FALSE)
    _spark_expect_judgement(silent 4 "scene could not be read" truncated.glb 4 "" FALSE)
    _spark_expect_judgement(loaded-anyway 4 "scene could not be read" truncated.glb 4
        "${_rejected}SPARK_SCENE_LOADED entities=0 renderables=0\n" FALSE)
    set(_loaded "SPARK_SCENE_LOADED entities=0 renderables=0\nSPARK_SCENE_ASSETS refs=0 missing=0\n")
    _spark_expect_judgement(control 0 "SPARK_SCENE_LOADED entities=0 renderables=0" valid-empty.sparkscene 0
        "${_loaded}" TRUE)
    _spark_expect_judgement(control-twice 0 "SPARK_SCENE_LOADED entities=0 renderables=0" valid-empty.sparkscene 0
        "${_loaded}${_loaded}" FALSE)
    _spark_expect_judgement(control-failed 0 "SPARK_SCENE_LOADED entities=0 renderables=0" valid-empty.sparkscene 4
        "${_rejected}" FALSE)
    message(STATUS "Installed malformed-content verdict contract passed")
    return()
endif()

foreach(_required IN ITEMS SPARK_INSTALLED_ROOT SPARK_SOURCE_ROOT SPARK_TEST_ROOT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunInstalledMalformedContent.cmake requires -D${_required}=<value>")
    endif()
    if(NOT IS_ABSOLUTE "${${_required}}" OR "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()

set(_engine "${SPARK_INSTALLED_ROOT}/bin/SparkEngine.exe")
if(NOT EXISTS "${_engine}" OR IS_DIRECTORY "${_engine}" OR IS_SYMLINK "${_engine}")
    message(FATAL_ERROR "Installed engine is missing or unsafe: ${_engine}")
endif()
set(_fixture_source "${SPARK_SOURCE_ROOT}/Tests/Fixtures/MalformedContent")
if(NOT IS_DIRECTORY "${_fixture_source}")
    message(FATAL_ERROR "Malformed-content fixtures are missing: ${_fixture_source}")
endif()

# label | fixture | expected exit | expected text. Kept in one table so a fixture
# added to the directory without a verdict is caught below.
set(_cases
    "control|valid-empty.sparkscene|0|SPARK_SCENE_LOADED entities=0 renderables=0"
    "truncated-scene|truncated-scene.sparkscene|${_spark_scene_failed_exit}|scene could not be read"
    "unsupported-version|unsupported-version.sparkscene|${_spark_scene_failed_exit}|scene 'version' 999 is unsupported"
    "missing-version|missing-version.sparkscene|${_spark_scene_failed_exit}|declares no 'version' field"
    "corrupt-material|corrupt-material.json|${_spark_scene_failed_exit}|scene could not be read"
    "truncated-glb|truncated.glb|${_spark_scene_failed_exit}|scene could not be read")

file(GLOB _fixture_files RELATIVE "${_fixture_source}" "${_fixture_source}/*")
list(REMOVE_ITEM _fixture_files README.md)
set(_covered)
foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_case}")
    list(GET _fields 1 _fixture)
    list(APPEND _covered "${_fixture}")
endforeach()
foreach(_fixture IN LISTS _fixture_files)
    if(NOT _fixture IN_LIST _covered)
        message(FATAL_ERROR "Fixture ${_fixture} has no verdict in RunInstalledMalformedContent.cmake")
    endif()
endforeach()
foreach(_fixture IN LISTS _covered)
    if(NOT _fixture IN_LIST _fixture_files)
        message(FATAL_ERROR "Verdict names missing fixture ${_fixture}")
    endif()
endforeach()

if(EXISTS "${SPARK_TEST_ROOT}" AND NOT IS_DIRECTORY "${SPARK_TEST_ROOT}")
    message(FATAL_ERROR "SPARK_TEST_ROOT is not a directory: ${SPARK_TEST_ROOT}")
endif()
file(REMOVE_RECURSE "${SPARK_TEST_ROOT}/content" "${SPARK_TEST_ROOT}/user")
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}/content" "${SPARK_TEST_ROOT}/user/localappdata" "${SPARK_TEST_ROOT}/user/appdata")

foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _fields "${_case}")
    list(GET _fields 0 _label)
    list(GET _fields 1 _fixture)
    list(GET _fields 2 _expect_exit)
    list(GET _fields 3 _expected_text)

    # The fixture is copied beside nothing else, outside the package and the
    # checkout, so the run cannot find a sibling .bak or project root.
    set(_content "${SPARK_TEST_ROOT}/content/${_label}")
    file(MAKE_DIRECTORY "${_content}")
    file(COPY "${_fixture_source}/${_fixture}" DESTINATION "${_content}")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "LOCALAPPDATA=${SPARK_TEST_ROOT}/user/localappdata"
            "APPDATA=${SPARK_TEST_ROOT}/user/appdata"
            "SPARK_RHI_BACKEND=null"
            "${_engine}" -headless -scene "${_content}/${_fixture}"
            -test-frames 3 -threads 1 -no-subprocess -no-jobsystem
        WORKING_DIRECTORY "${SPARK_INSTALLED_ROOT}/bin"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT 90
        ENCODING UTF-8)
    file(WRITE "${SPARK_TEST_ROOT}/${_label}-stdout.log" "${_stdout}")
    file(WRITE "${SPARK_TEST_ROOT}/${_label}-stderr.log" "${_stderr}")

    _spark_judge_content_run("${_label}" "${_expect_exit}" "${_expected_text}" "${_fixture}" "${_result}"
        "${_stdout}\n${_stderr}" _ok _reason)
    if(NOT _ok)
        message(FATAL_ERROR
            "Installed malformed-content case failed: ${_reason}\nstdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
    message(STATUS "Installed malformed-content case '${_label}' passed (exit ${_result})")
endforeach()
message(STATUS "Installed malformed-content diagnostics passed; evidence retained under ${SPARK_TEST_ROOT}")
