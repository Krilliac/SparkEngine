cmake_minimum_required(VERSION 3.25)

# EDT-210: drives the real Linux SparkEngine host with -scene and checks that an
# editor-authored reflected scene actually runs and that a bad scene fails the
# launch instead of leaving an empty engine running:
#   1. editor-saved scene   -> exit 0, one SPARK_SCENE_LOADED record with the
#                              fixture's entity/renderable counts, no game module
#   2. missing scene        -> exit 4, no record
#   3. rejected scenes      -> exit 4, no record: a non-JSON file and a
#                              document with an unsupported scene version
#   4. -scene with -game    -> exit 1 before startup (the module would own the loop)
# Headless is checked when SPARK_CHECK_HEADLESS is ON and the SDL host (dummy
# video driver) when SPARK_CHECK_SDL is ON. The SDL host loads the scene into
# the ECS world only; the Linux basic draw path is a no-op, so this proves the
# scene runs, not that it renders.
#
# SPARK_SCENE_FIXTURE is Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene,
# written by `SparkEditor --test-mode --save-scene` (the editor's seeded World:
# Main Camera, Ground with a MeshRenderer, Directional Light).

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE SPARK_WORKING_DIRECTORY SPARK_SCENE_WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkLinuxScenePreview.cmake requires -D${_required}=<path>")
    endif()
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR "${_path} is missing: ${${_path}}")
    endif()
endforeach()
if(NOT SPARK_CHECK_HEADLESS AND NOT SPARK_CHECK_SDL)
    message(FATAL_ERROR "RunSparkLinuxScenePreview.cmake has no host to check (SPARK_CHECK_HEADLESS/SPARK_CHECK_SDL)")
endif()

set(_expected_entities 3)
set(_expected_renderables 1)

# The work directory is recreated on every run and holds only files this script writes.
file(REMOVE_RECURSE "${SPARK_SCENE_WORK_DIR}")
file(MAKE_DIRECTORY "${SPARK_SCENE_WORK_DIR}")
set(_missing_scene "${SPARK_SCENE_WORK_DIR}/Missing.sparkscene")
set(_not_json_scene "${SPARK_SCENE_WORK_DIR}/NotJson.sparkscene")
file(WRITE "${_not_json_scene}" "this is not a scene document\n")
set(_future_scene "${SPARK_SCENE_WORK_DIR}/FutureVersion.sparkscene")
file(WRITE "${_future_scene}" "{\"version\": 999, \"entities\": []}\n")

# Run the engine; sets _exit, _stdout, _stderr in the caller's scope.
function(spark_run_engine)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env
            "SPARK_RHI_BACKEND=null"
            "SDL_VIDEODRIVER=dummy"
            "SDL_AUDIODRIVER=dummy"
            "${SPARK_ENGINE_EXECUTABLE}" ${ARGN} -test-frames 5 -threads 1 -no-subprocess -no-jobsystem
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        TIMEOUT 90
        ENCODING UTF-8)
    string(REPLACE "\r\n" "\n" _out "${_out}")
    set(_exit "${_result}" PARENT_SCOPE)
    set(_stdout "${_out}" PARENT_SCOPE)
    set(_stderr "${_err}" PARENT_SCOPE)
endfunction()

function(spark_expect_exit label expected)
    if(NOT "${_exit}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: exited ${_exit}, expected ${expected}.\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
endfunction()

function(spark_expect_scene_record label)
    string(REGEX MATCHALL "SPARK_SCENE_LOADED[^\n]*" _records "${_stdout}")
    list(LENGTH _records _record_count)
    if(NOT _record_count EQUAL 1)
        message(FATAL_ERROR "${label}: expected exactly one SPARK_SCENE_LOADED record, found ${_record_count}.\n"
                            "stdout:\n${_stdout}\nstderr:\n${_stderr}")
    endif()
    if(NOT _stdout MATCHES "(^|\n)SPARK_SCENE_LOADED entities=([0-9]+) renderables=([0-9]+)\n")
        message(FATAL_ERROR "${label}: malformed scene record: ${_records}\nstdout:\n${_stdout}")
    endif()
    if(NOT CMAKE_MATCH_2 EQUAL _expected_entities OR NOT CMAKE_MATCH_3 EQUAL _expected_renderables)
        message(FATAL_ERROR "${label}: loaded entities=${CMAKE_MATCH_2} renderables=${CMAKE_MATCH_3}, expected "
                            "entities=${_expected_entities} renderables=${_expected_renderables}.\n"
                            "stdout:\n${_stdout}")
    endif()
    # An engine-only scene run must not let executable-directory discovery
    # load a game module that would own the loop instead of the scene.
    if(_stdout MATCHES "SPARK_MODULE_READY")
        message(FATAL_ERROR "${label}: a game module initialized during a -scene run.\nstdout:\n${_stdout}")
    endif()
endfunction()

function(spark_expect_no_scene_record label)
    if(_stdout MATCHES "SPARK_SCENE_LOADED")
        message(FATAL_ERROR "${label}: printed a scene record for a scene that did not load.\nstdout:\n${_stdout}")
    endif()
endfunction()

function(spark_check_host label)
    spark_run_engine(${ARGN} -scene "${SPARK_SCENE_FIXTURE}")
    spark_expect_exit("${label} editor scene" 0)
    spark_expect_scene_record("${label} editor scene")

    spark_run_engine(${ARGN} -scene "${_missing_scene}")
    spark_expect_exit("${label} missing scene" 4)
    spark_expect_no_scene_record("${label} missing scene")

    spark_run_engine(${ARGN} -scene "${_not_json_scene}")
    spark_expect_exit("${label} non-JSON scene" 4)
    spark_expect_no_scene_record("${label} non-JSON scene")

    spark_run_engine(${ARGN} -scene "${_future_scene}")
    spark_expect_exit("${label} unsupported-version scene" 4)
    spark_expect_no_scene_record("${label} unsupported-version scene")
endfunction()

if(SPARK_CHECK_HEADLESS)
    spark_check_host("headless" -headless)
    # The headless loop must actually have run over the loaded scene.
    spark_run_engine(-headless -scene "${SPARK_SCENE_FIXTURE}")
    if(NOT _stdout MATCHES "(^|\n)SPARK_HEADLESS_RHI backend=null initialized=1 frames=5 shutdown=1\n")
        message(FATAL_ERROR "headless editor scene: the loop did not run five NullRHI frames.\nstdout:\n${_stdout}")
    endif()
endif()
if(SPARK_CHECK_SDL)
    spark_check_host("SDL")
endif()

# Conflicting launch roots are refused before any subsystem starts.
spark_run_engine(-scene "${SPARK_SCENE_FIXTURE}" -game "${SPARK_WORKING_DIRECTORY}/libSparkNoSuchModule.so")
spark_expect_exit("-scene with -game" 1)
spark_expect_no_scene_record("-scene with -game")
if(NOT _stderr MATCHES "cannot be combined with -game or -manifest")
    message(FATAL_ERROR "-scene with -game: missing the conflict diagnostic.\nstderr:\n${_stderr}")
endif()

message(STATUS "Linux -scene host checks passed")
