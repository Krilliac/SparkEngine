cmake_minimum_required(VERSION 3.25)

# RHI-240 / PLT-210: an explicit SPARK_RHI_BACKEND=opengl request must either
# bring the production SparkEngine host up on the OpenGL backend or fail
# startup, and a windowed run that cannot create its window must never degrade
# to NullRHI. Runs of the real executable:
#   1. SPARK_RHI_BACKEND=opengl                        -> exit 0 on OpenGL, never NullRHI
#   2. SPARK_RHI_BACKEND=opengl + SPARK_DISABLE_OPENGL=1 -> non-zero exit, explicit refusal
#   3. (SPARK_GAME_MODULE set) the FPS game for 30 frames with -require-game
#      -> exit 0, exactly one OpenGL initialization, never NullRHI
#   4. SDL_VIDEODRIVER=dummy (no OpenGL-capable video driver, so
#      SDL_CreateWindow fails on every host) -> non-zero exit with the SDL
#      reason and the windowed-startup refusal, never a NullRHI main loop
# The windowed SDL2 path needs an X server. An existing DISPLAY is used as-is;
# otherwise the runs are wrapped in xvfb-run. Neither being available is a
# failure, so the lane can never pass by skipping.

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_WORKING_DIRECTORY)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkOpenGLExplicitStartup.cmake requires -D${_required}=<path>")
    endif()
endforeach()

if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()

set(_display_wrapper)
if("$ENV{DISPLAY}" STREQUAL "")
    if(NOT DEFINED SPARK_XVFB_RUN OR NOT EXISTS "${SPARK_XVFB_RUN}")
        message(FATAL_ERROR
            "No DISPLAY is set and xvfb-run was not found at configure time; the explicit OpenGL "
            "startup lane needs an X server (install xvfb or run ctest under xvfb-run).")
    endif()
    set(_display_wrapper "${SPARK_XVFB_RUN}" -a -s "-screen 0 1280x720x24")
endif()

file(REMOVE_RECURSE "${SPARK_WORKING_DIRECTORY}")
file(MAKE_DIRECTORY "${SPARK_WORKING_DIRECTORY}")

# Runs SparkEngine for a few frames with the given extra environment and
# returns the exit code plus combined stdout/stderr (the logger writes to stderr).
# Extra environment entries come first in ARGN; ENGINE_ARGS <args...> replaces
# the default "-test-frames 5" engine arguments.
function(spark_run_engine out_result out_log)
    cmake_parse_arguments(PARSE_ARGV 2 _run "" "" "ENGINE_ARGS")
    set(_engine_args -test-frames 5)
    if(_run_ENGINE_ARGS)
        set(_engine_args ${_run_ENGINE_ARGS})
    endif()
    execute_process(
        COMMAND ${_display_wrapper} "${CMAKE_COMMAND}" -E env
            "SPARK_RHI_BACKEND=opengl"
            "SDL_AUDIODRIVER=dummy"
            ${_run_UNPARSED_ARGUMENTS}
            "${SPARK_ENGINE_EXECUTABLE}"
            ${_engine_args}
            -window-size 320x240
            -no-subprocess
        WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr
        TIMEOUT 90
        ENCODING UTF-8
    )
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_log} "${_stdout}\n${_stderr}" PARENT_SCOPE)
endfunction()

# --- 1. Explicit OpenGL request must come up on OpenGL ----------------------
spark_run_engine(_gl_result _gl_log)
if(NOT "${_gl_result}" STREQUAL "0")
    message(FATAL_ERROR "SPARK_RHI_BACKEND=opengl startup exited '${_gl_result}', expected 0.\n${_gl_log}")
endif()
string(FIND "${_gl_log}" "Initialized on Linux via RHI (OpenGL)" _gl_initialized)
if(_gl_initialized EQUAL -1)
    message(FATAL_ERROR
        "SPARK_RHI_BACKEND=opengl did not initialize the OpenGL backend "
        "(missing 'Initialized on Linux via RHI (OpenGL)').\n${_gl_log}")
endif()
# Match the NullRHI selection records, not the gVisor advisory that merely mentions NullRHIDevice.
string(REGEX MATCH "via RHI \\(NullRHI|falling back to NullRHIDevice|headless mode \\(NullRHIDevice\\)"
    _gl_nullrhi "${_gl_log}")
if(_gl_nullrhi)
    message(FATAL_ERROR "SPARK_RHI_BACKEND=opengl degraded to NullRHI ('${_gl_nullrhi}').\n${_gl_log}")
endif()
string(REGEX MATCH "Renderer: [^\n(]*" _gl_renderer "${_gl_log}")

# --- 2. Explicit OpenGL request that cannot be honored must fail -------------
spark_run_engine(_disabled_result _disabled_log "SPARK_DISABLE_OPENGL=1")
if("${_disabled_result}" STREQUAL "0")
    message(FATAL_ERROR
        "SPARK_RHI_BACKEND=opengl with SPARK_DISABLE_OPENGL=1 exited 0; an unavailable explicit backend "
        "must fail startup instead of degrading.\n${_disabled_log}")
endif()
if(NOT "${_disabled_result}" MATCHES "^[0-9]+$")
    message(FATAL_ERROR
        "SPARK_RHI_BACKEND=opengl with SPARK_DISABLE_OPENGL=1 did not exit cleanly "
        "(result '${_disabled_result}'); expected a controlled non-zero exit.\n${_disabled_log}")
endif()
string(FIND "${_disabled_log}" "SPARK_RHI_BACKEND explicitly requested OpenGL" _disabled_refusal)
if(_disabled_refusal EQUAL -1)
    message(FATAL_ERROR
        "SPARK_DISABLE_OPENGL=1 run exited ${_disabled_result} without the explicit-backend refusal.\n"
        "${_disabled_log}")
endif()

# --- 3. A real game module on the OpenGL window ------------------------------
if(DEFINED SPARK_GAME_MODULE AND NOT "${SPARK_GAME_MODULE}" STREQUAL "")
    if(NOT EXISTS "${SPARK_GAME_MODULE}")
        message(FATAL_ERROR "Game module is missing: ${SPARK_GAME_MODULE}")
    endif()
    spark_run_engine(_game_result _game_log
        ENGINE_ARGS -game "${SPARK_GAME_MODULE}" -require-game -test-frames 30)
    if(NOT "${_game_result}" STREQUAL "0")
        message(FATAL_ERROR "OpenGL windowed game run exited '${_game_result}', expected 0.\n${_game_log}")
    endif()
    string(REGEX MATCHALL "Initialized on Linux via RHI \\(OpenGL\\)" _game_inits "${_game_log}")
    list(LENGTH _game_inits _game_init_count)
    if(NOT _game_init_count EQUAL 1)
        message(FATAL_ERROR
            "OpenGL windowed game run logged 'Initialized on Linux via RHI (OpenGL)' ${_game_init_count} "
            "times, expected exactly 1.\n${_game_log}")
    endif()
    string(REGEX MATCH "via RHI \\(NullRHI|falling back to NullRHIDevice|headless mode \\(NullRHIDevice\\)"
        _game_nullrhi "${_game_log}")
    if(_game_nullrhi)
        message(FATAL_ERROR "OpenGL windowed game run degraded to NullRHI ('${_game_nullrhi}').\n${_game_log}")
    endif()
endif()

# --- 4. A windowed run whose window cannot be created must fail --------------
spark_run_engine(_nowindow_result _nowindow_log "SDL_VIDEODRIVER=dummy")
if(NOT "${_nowindow_result}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR
        "Windowed run on the SDL dummy video driver (no window possible) returned '${_nowindow_result}'; "
        "expected a controlled non-zero exit instead of a NullRHI run.\n${_nowindow_log}")
endif()
foreach(_expected "SDL_CreateWindow failed:" "Windowed startup could not create its render window or context")
    string(FIND "${_nowindow_log}" "${_expected}" _nowindow_found)
    if(_nowindow_found EQUAL -1)
        message(FATAL_ERROR "Window-failure run is missing '${_expected}'.\n${_nowindow_log}")
    endif()
endforeach()
string(FIND "${_nowindow_log}" "Initialized on Linux via RHI" _nowindow_initialized)
if(NOT _nowindow_initialized EQUAL -1)
    message(FATAL_ERROR "Window-failure run still initialized an RHI device.\n${_nowindow_log}")
endif()

message(STATUS "Explicit OpenGL request initialized OpenGL (${_gl_renderer}); "
               "unavailable explicit OpenGL request failed with exit ${_disabled_result}; "
               "window-creation failure refused startup with exit ${_nowindow_result}")
