cmake_minimum_required(VERSION 3.25)

# RHI-240: an explicit SPARK_RHI_BACKEND=opengl request must either bring the
# production SparkEngine host up on the OpenGL backend or fail startup. Two
# runs of the real executable prove both halves:
#   1. SPARK_RHI_BACKEND=opengl                        -> exit 0 on OpenGL, never NullRHI
#   2. SPARK_RHI_BACKEND=opengl + SPARK_DISABLE_OPENGL=1 -> non-zero exit, explicit refusal
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
function(spark_run_engine out_result out_log)
    execute_process(
        COMMAND ${_display_wrapper} "${CMAKE_COMMAND}" -E env
            "SPARK_RHI_BACKEND=opengl"
            "SDL_AUDIODRIVER=dummy"
            ${ARGN}
            "${SPARK_ENGINE_EXECUTABLE}"
            -test-frames 5
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

message(STATUS "Explicit OpenGL request initialized OpenGL (${_gl_renderer}); "
               "unavailable explicit OpenGL request failed with exit ${_disabled_result}")
