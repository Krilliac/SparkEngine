cmake_minimum_required(VERSION 3.25)

# HEAD-220: source-tree NullRHI save/reload persistence of the production
# headless FPS host. Two fresh `-headless` processes share one isolated user
# root: the writer awards 37 XP and quicksaves, the reader proves it starts from
# 0 XP, quickloads and reads 37 XP back. Each process must pass the strict
# NullRHI lifecycle parser (one module, rendered=0, faults=0, no D3D11 device)
# and the same writer/reader audit validator as the installed-package WARP run
# (Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake). The reader must leave
# the save byte-identical. Source-tree evidence only, not packaged
# certification.
#
# Package mode (-DSPARK_PACKAGE_ROOT=<staged package> -DSPARK_FORBIDDEN_ROOTS=<source>|<build>,
# set by Tests/PackageSmoke/RunInstalledFPSPackage.cmake) also proves the staged
# package needs no repository content: both processes start in a fresh empty
# working directory outside the package and the repository and must leave it
# empty, no output or log may name a forbidden root in either slash spelling
# (case-insensitively on Windows), the output must name the package root at
# least once so that scrub cannot pass vacuously, and every file under the
# package root must hash the same after the reader as before the writer.
# -DSPARK_REPOSITORY_ISOLATION_SELF_TEST=ON runs only the scrub against
# synthetic output (CTest FPSHeadlessPackage_RepositoryIsolationDetection).

set(SPARK_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(SPARK_FPS_SAVE_RELOAD_PARSER_ONLY ON)
include("${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/RunInstalledFPSSaveReload.cmake")
unset(SPARK_FPS_SAVE_RELOAD_PARSER_ONLY)
set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${CMAKE_CURRENT_LIST_DIR}/RunSparkHeadlessNullRHILifecycle.cmake")
unset(SPARK_HEADLESS_NULLRHI_PARSER_ONLY)

# Forward slashes, no repeated or trailing slash (a JSON-escaped "C:\\a" becomes
# "C:/a"), lower case when the host file system ignores case.
function(_spark_isolation_normalize value case_insensitive out_var)
    string(REPLACE "\\" "/" _normalized "${value}")
    string(REGEX REPLACE "/+" "/" _normalized "${_normalized}")
    string(REGEX REPLACE "(.)/$" "\\1" _normalized "${_normalized}")
    if(case_insensitive)
        string(TOLOWER "${_normalized}" _normalized)
    endif()
    set(${out_var} "${_normalized}" PARENT_SCOPE)
endfunction()

# OUT_VAR receives one violation per forbidden-root mention in the text held by
# TEXT_VAR, after the PACKAGE_ROOTS and ALLOWED_ROOTS spellings are scrubbed,
# plus a violation when the text names no PACKAGE_ROOTS spelling. An empty
# result is a pass.
function(_spark_forbidden_root_mentions)
    cmake_parse_arguments(PARSE_ARGV 0 _arg "CASE_INSENSITIVE" "TEXT_VAR;OUT_VAR"
        "PACKAGE_ROOTS;ALLOWED_ROOTS;FORBIDDEN_ROOTS")
    set(_violations "")
    _spark_isolation_normalize("${${_arg_TEXT_VAR}}" "${_arg_CASE_INSENSITIVE}" _text)
    set(_package_named FALSE)
    foreach(_package IN LISTS _arg_PACKAGE_ROOTS)
        _spark_isolation_normalize("${_package}" "${_arg_CASE_INSENSITIVE}" _package)
        string(FIND "${_text}" "${_package}" _package_position)
        if(NOT _package_position EQUAL -1)
            set(_package_named TRUE)
        endif()
    endforeach()
    if(NOT _package_named)
        list(APPEND _violations "isolation check saw no package path (${_arg_PACKAGE_ROOTS}) in the run output")
    endif()
    foreach(_allowed IN LISTS _arg_PACKAGE_ROOTS _arg_ALLOWED_ROOTS)
        _spark_isolation_normalize("${_allowed}" "${_arg_CASE_INSENSITIVE}" _allowed)
        string(REPLACE "${_allowed}" "<allowed-root>" _text "${_text}")
    endforeach()
    foreach(_root IN LISTS _arg_FORBIDDEN_ROOTS)
        _spark_isolation_normalize("${_root}" "${_arg_CASE_INSENSITIVE}" _needle)
        # A sibling directory that merely shares the prefix ("<root>-other") is not a mention.
        string(REGEX REPLACE "([][+.*()^$?|\\\\])" "\\\\\\1" _pattern "${_needle}")
        string(REGEX MATCH "${_pattern}([^A-Za-z0-9_.+-].*|$)" _match "${_text}")
        if(NOT _match STREQUAL "")
            string(SUBSTRING "${_match}" 0 200 _context)
            string(REPLACE ";" "," _context "${_context}")
            list(APPEND _violations "run output names forbidden root ${_root}: ${_context}")
        endif()
    endforeach()
    set(${_arg_OUT_VAR} "${_violations}" PARENT_SCOPE)
endfunction()

if(SPARK_REPOSITORY_ISOLATION_SELF_TEST)
    set(_package "C:/Spark Run/m310/install")
    set(_test_root "C:/Spark Run/m310/headless-save-reload")
    set(_forbidden "D:/Work/SparkEngine" "D:/Work/SparkEngine/build/windows-shipping")
    set(_cases
        "backslash source root|FAIL|Loading game module: C:\\Spark Run\\m310\\install\\bin\\SparkGameFPS.dll\nopened D:\\Work\\SparkEngine\\Assets\\Scenes\\level1.scene"
        "forward-slash other-case source root|FAIL|Loading game module: C:/Spark Run/m310/install/bin/SparkGameFPS.dll\nopened d:/WORK/sparkengine/Assets/x.obj"
        "build directory|FAIL|module C:/Spark Run/m310/install/bin/SparkGameFPS.dll from D:/Work/SparkEngine/build/windows-shipping/bin/SparkEngine.pdb"
        "JSON-escaped source root|FAIL|{\"module\":\"C:\\\\Spark Run\\\\m310\\\\install\\\\bin\\\\SparkGameFPS.dll\",\"asset\":\"D:\\\\Work\\\\SparkEngine\\\\Assets\"}"
        "package and test root only|PASS|Loading game module: C:\\Spark Run\\m310\\install\\bin\\SparkGameFPS.dll\nsave C:/Spark Run/m310/headless-save-reload/user/localappdata/SparkEngine/Saves/fps_quicksave.spark_save"
        "sibling of the source root|PASS|module C:/Spark Run/m310/install/bin/SparkGameFPS.dll\nread D:/Work/SparkEngine-release/notes.txt"
        "no package path|NO-PACKAGE|headless run finished without naming any path")
    set(_case_count 0)
    foreach(_case IN LISTS _cases)
        string(FIND "${_case}" "|" _first)
        string(SUBSTRING "${_case}" 0 ${_first} _name)
        math(EXPR _rest_start "${_first} + 1")
        string(SUBSTRING "${_case}" ${_rest_start} -1 _rest)
        string(FIND "${_rest}" "|" _second)
        string(SUBSTRING "${_rest}" 0 ${_second} _expected)
        math(EXPR _text_start "${_second} + 1")
        string(SUBSTRING "${_rest}" ${_text_start} -1 _output)
        _spark_forbidden_root_mentions(CASE_INSENSITIVE TEXT_VAR _output PACKAGE_ROOTS "${_package}"
            ALLOWED_ROOTS "${_test_root}" FORBIDDEN_ROOTS ${_forbidden} OUT_VAR _violations)
        if(_expected STREQUAL "PASS")
            set(_ok FALSE)
            if(_violations STREQUAL "")
                set(_ok TRUE)
            endif()
        elseif(_expected STREQUAL "NO-PACKAGE")
            string(FIND "${_violations}" "isolation check saw no package path" _found)
            set(_ok FALSE)
            if(NOT _found EQUAL -1)
                set(_ok TRUE)
            endif()
        else()
            string(FIND "${_violations}" "names forbidden root" _found)
            set(_ok FALSE)
            if(NOT _found EQUAL -1 AND NOT _violations MATCHES "saw no package path")
                set(_ok TRUE)
            endif()
        endif()
        if(NOT _ok)
            message(FATAL_ERROR "Repository isolation self-test '${_name}' expected ${_expected}, got: '${_violations}'")
        endif()
        math(EXPR _case_count "${_case_count} + 1")
    endforeach()
    message(STATUS "Repository isolation detection self-test passed ${_case_count} cases")
    return()
endif()

set(_package_mode FALSE)
if(DEFINED SPARK_PACKAGE_ROOT OR DEFINED SPARK_FORBIDDEN_ROOTS)
    if(NOT IS_DIRECTORY "${SPARK_PACKAGE_ROOT}" OR "${SPARK_FORBIDDEN_ROOTS}" STREQUAL "")
        message(FATAL_ERROR "Package mode needs -DSPARK_PACKAGE_ROOT=<staged package> and -DSPARK_FORBIDDEN_ROOTS")
    endif()
    # '|'-separated on the command line, so a caller's argument list cannot split it.
    string(REPLACE "|" ";" SPARK_FORBIDDEN_ROOTS "${SPARK_FORBIDDEN_ROOTS}")
    set(_package_mode TRUE)
endif()

set(_required_inputs SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_TEST_ROOT)
if(NOT _package_mode)
    list(APPEND _required_inputs SPARK_WORKING_DIRECTORY)
endif()
foreach(_required IN LISTS _required_inputs)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkHeadlessFPSSaveReload.cmake requires -D${_required}=<value>")
    endif()
endforeach()
if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPARK_GAME_MODULE}")
    message(FATAL_ERROR "SparkGameFPS module is missing: ${SPARK_GAME_MODULE}")
endif()
if(NOT _package_mode AND NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Headless working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()
if(NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR "${SPARK_TEST_ROOT}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()

# A fresh user root per run: the save path comes from Spark::UserPaths, so a
# save left by an earlier run (or the developer's own profile) cannot be read.
file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
if(_package_mode)
    # The staged runtime anchors itself to its executable directory from its
    # manifest.json/spark.modules.json; it must not need, or write to, the cwd.
    set(SPARK_WORKING_DIRECTORY "${SPARK_TEST_ROOT}/cwd")
    file(MAKE_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
endif()

# Every path and hash under a root, so a run that writes beside the package
# binaries is caught.
function(_spark_tree_snapshot root out_var)
    file(GLOB_RECURSE _entries LIST_DIRECTORIES true RELATIVE "${root}" "${root}/*")
    set(_snapshot "")
    foreach(_entry IN LISTS _entries)
        if(IS_DIRECTORY "${root}/${_entry}")
            list(APPEND _snapshot "${_entry}|dir")
        else()
            file(SHA256 "${root}/${_entry}" _hash)
            list(APPEND _snapshot "${_entry}|${_hash}")
        endif()
    endforeach()
    list(SORT _snapshot)
    set(${out_var} "${_snapshot}" PARENT_SCOPE)
endfunction()
if(_package_mode)
    _spark_tree_snapshot("${SPARK_PACKAGE_ROOT}" _package_before)
endif()
set(_user_root "${SPARK_TEST_ROOT}/user")
if(CMAKE_HOST_WIN32)
    set(_user_env
        "LOCALAPPDATA=${_user_root}/localappdata"
        "APPDATA=${_user_root}/appdata")
    set(_save_dir "${_user_root}/localappdata/SparkEngine/Saves")
else()
    set(_user_env
        "HOME=${_user_root}/home"
        "XDG_DATA_HOME=${_user_root}/data"
        "XDG_CONFIG_HOME=${_user_root}/config"
        "XDG_CACHE_HOME=${_user_root}/cache"
        "XDG_STATE_HOME=${_user_root}/state")
    set(_save_dir "${_user_root}/data/SparkEngine/Saves")
endif()
foreach(_assignment IN LISTS _user_env)
    string(REGEX REPLACE "^[A-Z_]+=" "" _dir "${_assignment}")
    file(MAKE_DIRECTORY "${_dir}")
endforeach()
set(_save "${_save_dir}/fps_quicksave.spark_save")

set(_writer_script "${SPARK_TEST_ROOT}/writer-exec.txt")
set(_reader_script "${SPARK_TEST_ROOT}/reader-exec.txt")
file(WRITE "${_writer_script}" "1 level\n2 xp 37\n3 level\n4 quicksave\n")
file(WRITE "${_reader_script}" "1 level\n2 quickload\n3 level\n")

function(_spark_run_headless_fps_phase phase script)
    set(_audit "${SPARK_TEST_ROOT}/${phase}-exec-audit.log")
    if(EXISTS "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Could not establish a fresh exec audit before the ${phase} run")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            ${_user_env}
            "SPARK_RHI_BACKEND=null"
            "${SPARK_ENGINE_EXECUTABLE}"
            -headless
            -game "${SPARK_GAME_MODULE}"
            -require-game
            -exec "${script}"
            -exec-audit "${_audit}"
            -test-frames 30
            -threads 2
            -no-subprocess
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT 120
        ENCODING UTF-8)
    file(WRITE "${SPARK_TEST_ROOT}/${phase}-stdout.log" "${_stdout}")
    file(WRITE "${SPARK_TEST_ROOT}/${phase}-stderr.log" "${_stderr}")

    _spark_validate_headless_nullrhi_result("${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _lifecycle_reason)
    if(NOT _lifecycle_ok)
        message(FATAL_ERROR
            "Headless FPS ${phase} NullRHI lifecycle failed: ${_lifecycle_reason}\n"
            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()

    if(NOT EXISTS "${_audit}" OR IS_DIRECTORY "${_audit}" OR IS_SYMLINK "${_audit}")
        message(FATAL_ERROR "Headless FPS ${phase} run did not write a regular exec audit: ${_audit}")
    endif()
    file(READ "${_audit}" _audit_content)
    _spark_validate_fps_audit("${phase}" "${_result}" TRUE "${_audit_content}" _audit_ok _audit_reason)
    if(NOT _audit_ok)
        message(FATAL_ERROR
            "Headless FPS ${phase} exec audit failed: ${_audit_reason}\n"
            "audit (${_audit}):\n${_audit_content}")
    endif()
endfunction()

_spark_run_headless_fps_phase(writer "${_writer_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Headless FPS writer did not create a regular quicksave under the isolated user root: ${_save}")
endif()
file(SIZE "${_save}" _save_size)
if(_save_size LESS 1 OR _save_size GREATER 67108864)
    message(FATAL_ERROR "Headless FPS quicksave size ${_save_size} is invalid")
endif()
file(SHA256 "${_save}" _writer_save_sha256)

_spark_run_headless_fps_phase(reader "${_reader_script}")
if(NOT EXISTS "${_save}" OR IS_DIRECTORY "${_save}" OR IS_SYMLINK "${_save}")
    message(FATAL_ERROR "Headless FPS reader lost the quicksave: ${_save}")
endif()
file(SHA256 "${_save}" _reader_save_sha256)
if(NOT _writer_save_sha256 STREQUAL _reader_save_sha256)
    message(FATAL_ERROR
        "Headless FPS reader mutated the persisted quicksave:\n"
        "  before: ${_writer_save_sha256}\n  after:  ${_reader_save_sha256}")
endif()

if(_package_mode)
    file(GLOB _cwd_entries LIST_DIRECTORIES true "${SPARK_WORKING_DIRECTORY}/*" "${SPARK_WORKING_DIRECTORY}/.*")
    if(_cwd_entries)
        message(FATAL_ERROR "The staged package wrote into its launch working directory: ${_cwd_entries}")
    endif()

    _spark_tree_snapshot("${SPARK_PACKAGE_ROOT}" _package_after)
    if(NOT _package_after STREQUAL _package_before)
        set(_added ${_package_after})
        list(REMOVE_ITEM _added ${_package_before})
        set(_removed ${_package_before})
        list(REMOVE_ITEM _removed ${_package_after})
        message(FATAL_ERROR
            "The headless run modified the staged package (new or changed: ${_added}; removed or changed: ${_removed})")
    endif()

    # stdout, stderr and the exec audit of both phases, plus every log the runtime
    # wrote under the isolated user root.
    file(GLOB_RECURSE _run_logs LIST_DIRECTORIES false "${SPARK_TEST_ROOT}/*.log")
    set(_run_output "")
    foreach(_log IN LISTS _run_logs)
        file(READ "${_log}" _log_content)
        string(APPEND _run_output "${_log_content}\n")
    endforeach()
    set(_case_insensitive "")
    if(CMAKE_HOST_WIN32)
        set(_case_insensitive CASE_INSENSITIVE)
    endif()
    # The runtime prints canonical paths, which can differ from the configured
    # spelling (an 8.3 TEMP such as C:/Users/RUNNER~1), so match both.
    function(_spark_root_spellings out_var)
        set(_spellings "")
        foreach(_root IN LISTS ARGN)
            file(REAL_PATH "${_root}" _real_root)
            list(APPEND _spellings "${_root}" "${_real_root}")
        endforeach()
        set(${out_var} "${_spellings}" PARENT_SCOPE)
    endfunction()
    _spark_root_spellings(_package_spellings "${SPARK_PACKAGE_ROOT}")
    _spark_root_spellings(_test_root_spellings "${SPARK_TEST_ROOT}")
    _spark_root_spellings(_forbidden_spellings ${SPARK_FORBIDDEN_ROOTS})
    _spark_forbidden_root_mentions(${_case_insensitive} TEXT_VAR _run_output PACKAGE_ROOTS ${_package_spellings}
        ALLOWED_ROOTS ${_test_root_spellings} FORBIDDEN_ROOTS ${_forbidden_spellings}
        OUT_VAR _isolation_violations)
    if(_isolation_violations)
        list(JOIN _isolation_violations "\n  " _isolation_report)
        message(FATAL_ERROR "The staged package depends on repository content:\n  ${_isolation_report}")
    endif()
    list(LENGTH _run_logs _run_log_count)
    list(LENGTH _package_after _package_entry_count)
    message(STATUS
        "Staged package isolation passed: ${_run_log_count} output/log file(s) name no source or build root, "
        "${_package_entry_count} package entries unchanged, launch directory left empty")
endif()

message(STATUS
    "Headless FPS NullRHI save/reload passed across two fresh processes "
    "(${_save_size}-byte save ${_writer_save_sha256}, ${CMAKE_HOST_SYSTEM_NAME} host)")
