cmake_minimum_required(VERSION 3.25)

# EDT-210: drives the real Windows SparkEngine host (-headless, NullRHI) with
# -scene and checks that an editor-authored reflected scene actually runs and
# that a bad scene fails the launch instead of leaving an empty engine running:
#   1. editor-saved scene      -> exit 0, exactly one SPARK_SCENE_LOADED record
#                                 with the fixture's entity/renderable counts,
#                                 SPARK_SCENE_ASSETS refs=0 missing=0, no game
#                                 module, and the loop ran the requested frames
#   2. project-rooted scene    -> the same scene staged as <project>/Scenes/...
#                                 with one present and one missing Assets/...
#                                 reference reports refs=2 missing=1
#   3. missing scene           -> exit 4, no record
#   4. rejected scenes         -> exit 4, no record: a non-JSON file and a
#                                 document with an unsupported scene version
# The old host logged a failed load, cleared the scene and exited 0, which case 3
# rejects.
#
# SPARK_SCENE_FIXTURE is Tests/Fixtures/EditorScene/Scenes/EditorSeeded.sparkscene,
# written by `SparkEditor --test-mode --save-scene` (the editor's seeded World:
# Main Camera, Ground with a MeshRenderer, Directional Light).

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE SPARK_WORKING_DIRECTORY SPARK_SCENE_WORK_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkWindowsScenePreview.cmake requires -D${_required}=<path>")
    endif()
endforeach()
foreach(_path SPARK_ENGINE_EXECUTABLE SPARK_SCENE_FIXTURE)
    if(NOT EXISTS "${${_path}}")
        message(FATAL_ERROR "${_path} is missing: ${${_path}}")
    endif()
endforeach()

set(_expected_entities 3)
set(_expected_renderables 1)
set(_test_frames 3)

# The work directory is recreated on every run and holds only files this script writes.
file(REMOVE_RECURSE "${SPARK_SCENE_WORK_DIR}")
file(MAKE_DIRECTORY "${SPARK_SCENE_WORK_DIR}")
set(_missing_scene "${SPARK_SCENE_WORK_DIR}/Missing.sparkscene")
set(_not_json_scene "${SPARK_SCENE_WORK_DIR}/NotJson.sparkscene")
file(WRITE "${_not_json_scene}" "this is not a scene document\n")
set(_future_scene "${SPARK_SCENE_WORK_DIR}/FutureVersion.sparkscene")
file(WRITE "${_future_scene}" "{\"version\": 999, \"entities\": []}\n")

# A project-rooted copy of the fixture whose renderable references one asset
# that exists under <project>/Assets and one that does not.
set(_project_dir "${SPARK_SCENE_WORK_DIR}/Project")
set(_project_scene "${_project_dir}/Scenes/Authored.sparkscene")
file(MAKE_DIRECTORY "${_project_dir}/Scenes" "${_project_dir}/Assets/Models")
file(WRITE "${_project_dir}/Assets/Models/present.obj" "v 0 0 0\n")
file(READ "${SPARK_SCENE_FIXTURE}" _fixture_text)
string(REPLACE "\"meshPath\": \"__spark_primitive_ground__.obj\"" "\"meshPath\": \"Assets/Models/present.obj\""
               _project_text "${_fixture_text}")
string(REPLACE "\"materialPath\": \"\"" "\"materialPath\": \"Assets/Materials/missing.mat\"" _project_text
               "${_project_text}")
if(_project_text STREQUAL _fixture_text OR NOT _project_text MATCHES "Assets/Models/present.obj" OR
   NOT _project_text MATCHES "Assets/Materials/missing.mat")
    message(FATAL_ERROR "Could not stage asset references into the scene fixture; its MeshRenderer layout changed.")
endif()
file(WRITE "${_project_scene}" "${_project_text}")

# Run the engine; sets _exit, _stdout, _stderr in the caller's scope.
function(spark_run_engine)
    execute_process(
        COMMAND "${SPARK_ENGINE_EXECUTABLE}" -headless ${ARGN} -test-frames ${_test_frames} -threads 1
                -no-subprocess -no-jobsystem
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

function(spark_expect_scene_records label refs missing)
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

    string(REGEX MATCHALL "SPARK_SCENE_ASSETS[^\n]*" _asset_records "${_stdout}")
    list(LENGTH _asset_records _asset_record_count)
    if(NOT _asset_record_count EQUAL 1 OR
       NOT _stdout MATCHES "(^|\n)SPARK_SCENE_ASSETS refs=([0-9]+) missing=([0-9]+)\n")
        message(FATAL_ERROR "${label}: expected exactly one well-formed SPARK_SCENE_ASSETS record, found "
                            "'${_asset_records}'.\nstdout:\n${_stdout}")
    endif()
    if(NOT CMAKE_MATCH_2 EQUAL refs OR NOT CMAKE_MATCH_3 EQUAL missing)
        message(FATAL_ERROR "${label}: asset refs=${CMAKE_MATCH_2} missing=${CMAKE_MATCH_3}, expected "
                            "refs=${refs} missing=${missing}.\nstdout:\n${_stdout}")
    endif()

    # An engine-only scene run must not let executable-directory discovery
    # load a game module that would own the loop instead of the scene.
    if(_stdout MATCHES "SPARK_MODULE_READY")
        message(FATAL_ERROR "${label}: a game module initialized during a -scene run.\nstdout:\n${_stdout}")
    endif()
    # The headless loop must actually have run over the loaded scene.
    if(NOT _stdout MATCHES "(^|\n)SPARK_HEADLESS_RHI backend=null initialized=1 frames=${_test_frames} shutdown=1\n")
        message(FATAL_ERROR "${label}: the loop did not run ${_test_frames} NullRHI frames.\nstdout:\n${_stdout}")
    endif()
endfunction()

function(spark_expect_no_scene_record label)
    if(_stdout MATCHES "SPARK_SCENE_LOADED" OR _stdout MATCHES "SPARK_SCENE_ASSETS")
        message(FATAL_ERROR "${label}: printed a scene record for a scene that did not load.\nstdout:\n${_stdout}")
    endif()
endfunction()

spark_run_engine(-scene "${SPARK_SCENE_FIXTURE}")
spark_expect_exit("editor scene" 0)
spark_expect_scene_records("editor scene" 0 0)

spark_run_engine(-scene "${_project_scene}")
spark_expect_exit("project-rooted scene" 0)
spark_expect_scene_records("project-rooted scene" 2 1)

spark_run_engine(-scene "${_missing_scene}")
spark_expect_exit("missing scene" 4)
spark_expect_no_scene_record("missing scene")

spark_run_engine(-scene "${_not_json_scene}")
spark_expect_exit("non-JSON scene" 4)
spark_expect_no_scene_record("non-JSON scene")

spark_run_engine(-scene "${_future_scene}")
spark_expect_exit("unsupported-version scene" 4)
spark_expect_no_scene_record("unsupported-version scene")

message(STATUS "Windows -scene host checks passed")
