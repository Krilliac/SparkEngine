cmake_minimum_required(VERSION 3.25)

# Exercise a copied FPS runtime bin directory with an isolated scene edit.
# The input may be a build-output layout or a separate cmake --install stage;
# callers must report which provenance they supplied. A hard-coded camera or
# spawn must fail this probe.
foreach(_required IN ITEMS SPARK_PACKAGE_BIN SPARK_TEST_ROOT SPARK_SOURCE_ROOT)
    if(NOT DEFINED ${_required} OR NOT IS_ABSOLUTE "${${_required}}" OR
       "${${_required}}" MATCHES "[\r\n;]")
        message(FATAL_ERROR "${_required} must be a safe absolute path")
    endif()
endforeach()
if(NOT EXISTS "${SPARK_PACKAGE_BIN}/SparkEngine.exe" OR
   NOT EXISTS "${SPARK_PACKAGE_BIN}/SparkGameFPS.dll" OR
   NOT EXISTS "${SPARK_PACKAGE_BIN}/Assets/Scenes/level1.scene")
    message(FATAL_ERROR "FPS package-layout engine, module, or scene is missing")
endif()

string(TIMESTAMP _stamp "%Y%m%dT%H%M%SZ" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _nonce)
set(_run_root "${SPARK_TEST_ROOT}/fps-authored-${_stamp}-${_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated FPS run root already exists")
endif()
file(MAKE_DIRECTORY "${_run_root}/bin")
file(COPY "${SPARK_PACKAGE_BIN}/" DESTINATION "${_run_root}/bin")

set(_scene_path "${_run_root}/bin/Assets/Scenes/level1.scene")
file(READ "${_scene_path}" _scene)
string(REGEX REPLACE "\\[Camera\\][\r\n]+position=0[.]0,2[.]0,-(5|20)[.]0"
    "[Camera]\nposition=7.0,3.0,-17.0" _changed_scene "${_scene}")
string(REPLACE "position=0.0,2.0,-20.0" "position=4.0,2.0,-19.0" _changed_scene "${_changed_scene}")
string(REPLACE "position=20.0,1.0,20.0" "position=11.0,1.5,22.0" _changed_scene "${_changed_scene}")
foreach(_edited_position IN ITEMS
    "position=7.0,3.0,-17.0"
    "position=4.0,2.0,-19.0"
    "position=11.0,1.5,22.0")
    if(NOT _changed_scene MATCHES "${_edited_position}")
        message(FATAL_ERROR "Expected authored edit missing: ${_edited_position}")
    endif()
endforeach()
string(REPLACE "projection=perspective" "projection=perspective\nrotation=5.0,30.0,0.0" _changed_scene "${_changed_scene}")
string(REPLACE "nearPlane=0.1" "nearPlane=0.25" _changed_scene "${_changed_scene}")
string(REPLACE "farPlane=1000.0" "farPlane=250.0" _changed_scene "${_changed_scene}")
file(WRITE "${_scene_path}" "${_changed_scene}")
set(_exec_script "${_run_root}/game-status.exec")
file(WRITE "${_exec_script}"
    "0 game_status\n4 game_status\n4 wave_status\n4 gfx_screenshot fps-authored.png\n"
    "5 wave_start\n6 game_status\n")

set(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY ON)
include("${SPARK_SOURCE_ROOT}/cmake/RunSparkModuleProfileLifecycle.cmake")
unset(SPARK_LIFECYCLE_PARSER_INCLUDE_ONLY)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=d3d11"
        "SPARK_D3D11_DRIVER=warp"
        "LOCALAPPDATA=${_run_root}/localappdata"
        "${_run_root}/bin/SparkEngine.exe"
        -game "${_run_root}/bin/SparkGameFPS.dll"
        -require-game -test-frames 8 -threads 2 -window-size 640x360 -no-subprocess
        -exec "${_exec_script}"
    WORKING_DIRECTORY "${_run_root}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 120
    ENCODING UTF-8)
file(WRITE "${_run_root}/stdout.log" "${_stdout}")
file(WRITE "${_run_root}/stderr.log" "${_stderr}")
_spark_validate_lifecycle_result("${_result}" "${_stdout}" "${_stderr}" _lifecycle_ok _reason)
if(NOT _lifecycle_ok)
    message(FATAL_ERROR "FPS package-layout authored-scene run failed: ${_reason}; logs: ${_run_root}")
endif()

set(_audit "${_run_root}/exec_audit.log")
if(NOT EXISTS "${_audit}")
    message(FATAL_ERROR "FPS package-layout runtime did not execute game_status; logs: ${_run_root}")
endif()
file(READ "${_audit}" _status)
if(NOT _status MATCHES "Camera initialized from authored scene at \\(7[.]000000, 3[.]000000, -17[.]000000\\)")
    message(FATAL_ERROR "FPS package-layout runtime ignored edited [Camera] position in live startup; logs: ${_run_root}")
endif()
if(NOT _status MATCHES "Camera authored state: rotation \\(5[.]0, 30[.]0, 0[.]0\\) near/far \\(0[.]25, 250[.]0\\)")
    message(FATAL_ERROR "FPS package-layout runtime ignored edited [Camera] rotation/clipping; logs: ${_run_root}")
endif()
if(NOT _status MATCHES "Scene-authored respawn points: 4; preferred at \\(4[.]000000, 2[.]000000, -19[.]000000\\)")
    message(FATAL_ERROR "FPS package-layout runtime ignored edited [SpawnPoint] position; logs: ${_run_root}")
endif()
string(FIND "${_status}" "frame 4 " _post_frame_offset)
if(_post_frame_offset LESS 0)
    message(FATAL_ERROR "FPS package-layout runtime missed post-update frame-4 diagnostics; logs: ${_run_root}")
endif()
string(SUBSTRING "${_status}" ${_post_frame_offset} -1 _post_frame_status)
if(NOT _post_frame_status MATCHES "Camera/Player XZ: \\(7[.]0, -17[.]0\\) / \\(7[.]0, -17[.]0\\)")
    message(FATAL_ERROR "FPS package-layout runtime lost authored camera/player location after gameplay; logs: ${_run_root}")
endif()
if(NOT _post_frame_status MATCHES "Wave Spawn Points: 10; first \\(11[.]0, 1[.]5, 22[.]0\\)")
    message(FATAL_ERROR "FPS package-layout runtime did not configure authored wave spawns; logs: ${_run_root}")
endif()
if(NOT _status MATCHES "frame 5 [^\n]*[|] ok  [|] wave_start")
    message(FATAL_ERROR "FPS package-layout runtime did not start the survival match; logs: ${_run_root}")
endif()
string(FIND "${_status}" "frame 6 " _restart_frame_offset)
if(_restart_frame_offset LESS 0)
    message(FATAL_ERROR "FPS package-layout runtime missed post-restart game_status; logs: ${_run_root}")
endif()
string(SUBSTRING "${_status}" ${_restart_frame_offset} -1 _restart_frame_status)
if(NOT _restart_frame_status MATCHES "Camera/Player XZ: \\(4[.]0, -19[.]0\\) / \\(4[.]0, -19[.]0\\)")
    message(FATAL_ERROR "FPS package-layout restart did not use edited North_Spawn; logs: ${_run_root}")
endif()
set(_image "${_run_root}/fps-authored.png")
if(NOT EXISTS "${_image}" OR IS_DIRECTORY "${_image}")
    message(FATAL_ERROR "FPS package-layout runtime did not capture a rendered frame; logs: ${_run_root}")
endif()
execute_process(
    COMMAND "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
        -NoProfile -NonInteractive -File
        "${SPARK_SOURCE_ROOT}/Tests/PackageSmoke/CheckFPSVisibleFrame.ps1"
        -ImagePath "${_image}"
    RESULT_VARIABLE _visual_result
    OUTPUT_VARIABLE _visual_stdout
    ERROR_VARIABLE _visual_stderr
    TIMEOUT 30)
if(NOT _visual_result EQUAL 0)
    message(FATAL_ERROR "FPS package-layout frame failed visual geometry check: ${_visual_stderr}; logs: ${_run_root}")
endif()
message(STATUS "FPS package-layout runtime honored edited scene camera and spawns; logs: ${_run_root}")
