cmake_minimum_required(VERSION 3.25)

foreach(_required SPARK_ENGINE_EXECUTABLE SPARK_GAME_MODULE SPARK_WORKING_DIRECTORY)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "RunSparkLinuxSDLConsoleStartup.cmake requires -D${_required}=<path>")
    endif()
endforeach()

if(NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "SparkEngine executable is missing: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT EXISTS "${SPARK_GAME_MODULE}")
    message(FATAL_ERROR "SparkGame module is missing: ${SPARK_GAME_MODULE}")
endif()
if(NOT IS_DIRECTORY "${SPARK_WORKING_DIRECTORY}")
    message(FATAL_ERROR "Linux SDL startup working directory is missing: ${SPARK_WORKING_DIRECTORY}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SPARK_RHI_BACKEND=null"
        "SDL_VIDEODRIVER=dummy"
        "SDL_AUDIODRIVER=dummy"
        "${SPARK_ENGINE_EXECUTABLE}"
        -game "${SPARK_GAME_MODULE}"
        -test-frames 1
        -threads 1
        -no-subprocess
        -minimal-init
        -no-jobsystem
    WORKING_DIRECTORY "${SPARK_WORKING_DIRECTORY}"
    RESULT_VARIABLE _result
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr
    TIMEOUT 30
    ENCODING UTF-8
)

string(REPLACE "\r\n" "\n" _stdout "${_stdout}")
string(REPLACE "\r" "\n" _stdout "${_stdout}")
string(REPLACE "\r\n" "\n" _stderr "${_stderr}")
string(REPLACE "\r" "\n" _stderr "${_stderr}")

if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR
        "Linux SDL production startup exited ${_result}, expected 0.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

string(FIND "${_stdout}" "SPARK_MODULE_READY count=1\n" _module_ready_position)
if(_module_ready_position EQUAL -1)
    message(FATAL_ERROR
        "Linux SDL startup did not initialize exactly one real game module.\n"
        "stdout:\n${_stdout}\n"
        "stderr:\n${_stderr}")
endif()

string(FIND "${_stderr}" "SimpleConsole initialized" _console_initialized_position)
string(FIND "${_stderr}" "Loading SparkGame showcase module" _module_load_position)
if(_console_initialized_position EQUAL -1)
    message(FATAL_ERROR "Linux SDL startup emitted no SimpleConsole initialization evidence.\nstderr:\n${_stderr}")
endif()
if(_module_load_position EQUAL -1)
    message(FATAL_ERROR "Linux SDL startup emitted no SparkGame OnLoad evidence.\nstderr:\n${_stderr}")
endif()
if(NOT _console_initialized_position LESS _module_load_position)
    message(FATAL_ERROR
        "Linux SDL startup loaded SparkGame before SimpleConsole was initialized.\n"
        "stderr:\n${_stderr}")
endif()

string(REGEX MATCHALL "InitConsole: complete" _console_complete_records "${_stderr}")
list(LENGTH _console_complete_records _console_complete_count)
if(NOT _console_complete_count EQUAL 1)
    message(FATAL_ERROR
        "Linux SDL startup completed InitConsole ${_console_complete_count} times, expected exactly 1.\n"
        "stderr:\n${_stderr}")
endif()

string(FIND "${_stderr}" "InitConsole: complete" _console_complete_position)
if(NOT _module_load_position LESS _console_complete_position)
    message(FATAL_ERROR
        "Linux SDL startup completed full InitConsole before SparkGame OnLoad.\n"
        "stderr:\n${_stderr}")
endif()

message(STATUS "Linux SDL initialized SimpleConsole before the real SparkGame module")
