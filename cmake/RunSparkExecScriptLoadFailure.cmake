cmake_minimum_required(VERSION 3.25)

# SEC3: an -exec script that cannot be loaded must fail the launch on every
# host instead of letting an automated run reach its -test-frames limit with
# an empty timeline and exit 0 (a false-green smoke). Drives the real
# SparkEngine host (-headless, NullRHI) three times, engine-only via -scene so
# no game module is discovered:
#   1. readable -exec script  -> exit 0, the loop ran the requested frames
#                                (control: proves the host itself can pass)
#   2. missing -exec script   -> exit 1, "cannot load -exec script" on stderr,
#                                and the frame loop never ran
#   3. -exec names a directory -> the same as 2
# The old Windows host discarded ExecScriptPlayer::LoadFile's result, so cases
# 2 and 3 exited 0 there.

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE SPARK_WORKING_DIRECTORY SPARK_EXEC_WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkExecScriptLoadFailure.cmake requires -D${_required}=<path>")
    endif()
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR "${_path} is missing: ${${_path}}")
    endif()
endforeach()

set(_test_frames 3)

# The work directory is recreated on every run and holds only files this script writes.
file(REMOVE_RECURSE "${SPARK_EXEC_WORK_DIR}")
file(MAKE_DIRECTORY "${SPARK_EXEC_WORK_DIR}/ScriptDirectory")
set(_valid_script "${SPARK_EXEC_WORK_DIR}/valid_exec.txt")
file(WRITE "${_valid_script}" "# SEC3 control timeline: comments only, nothing to run\n")
set(_missing_script "${SPARK_EXEC_WORK_DIR}/missing_exec.txt")
set(_directory_script "${SPARK_EXEC_WORK_DIR}/ScriptDirectory")
set(_audit "${SPARK_EXEC_WORK_DIR}/exec_audit.log")

# Run the engine; sets _exit, _stdout, _stderr in the caller's scope.
function(spark_run_engine script)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "SPARK_RHI_BACKEND=null" "SDL_VIDEODRIVER=dummy" "SDL_AUDIODRIVER=dummy"
                "${SPARK_ENGINE_EXECUTABLE}" -headless -scene "${SPARK_SCENE_FIXTURE}" -exec "${script}"
                -exec-audit "${_audit}" -test-frames ${_test_frames} -threads 1 -no-subprocess -no-jobsystem
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 90
        ENCODING UTF-8)
    string(REPLACE "\r\n" "\n" _out "${_out}")
    string(REPLACE "\r\n" "\n" _err "${_err}")
    set(_exit "${_result}" PARENT_SCOPE)
    set(_stdout "${_out}" PARENT_SCOPE)
    set(_stderr "${_err}" PARENT_SCOPE)
endfunction()

function(spark_expect_rejected label script)
    spark_run_engine("${script}")
    if(NOT "${_exit}" STREQUAL "1")
        message(FATAL_ERROR "${label}: exited '${_exit}', expected 1 (the launch must fail).\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
    if(NOT _stderr MATCHES "SparkEngine: cannot load -exec script")
        message(FATAL_ERROR "${label}: stderr does not name the unloadable -exec script.\nstderr:\n${_stderr}")
    endif()
    if(_stdout MATCHES "SPARK_HEADLESS_RHI")
        message(FATAL_ERROR "${label}: the frame loop ran although the -exec script did not load.\n"
                            "stdout:\n${_stdout}")
    endif()
endfunction()

spark_run_engine("${_valid_script}")
if(NOT "${_exit}" STREQUAL "0")
    message(FATAL_ERROR "readable -exec script: exited '${_exit}', expected 0.\n"
                        "stdout:\n${_stdout}\nstderr:\n${_stderr}")
endif()
if(NOT _stdout MATCHES "(^|\n)SPARK_HEADLESS_RHI backend=null initialized=1 frames=${_test_frames} shutdown=1\n")
    message(FATAL_ERROR "readable -exec script: the loop did not run ${_test_frames} NullRHI frames.\n"
                        "stdout:\n${_stdout}")
endif()

spark_expect_rejected("missing -exec script" "${_missing_script}")
spark_expect_rejected("directory as -exec script" "${_directory_script}")

message(STATUS "-exec load-failure host checks passed")
