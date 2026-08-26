cmake_minimum_required(VERSION 3.25)

foreach(required IN ITEMS SPARK_TEMPLATE_ROOT SPARK_TEMPLATE_BUILD_ROOT SPARK_ENGINE_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${SPARK_TEMPLATE_ROOT}")
    message(FATAL_ERROR "Template root does not exist: ${SPARK_TEMPLATE_ROOT}")
endif()
if(NOT EXISTS "${SPARK_ENGINE_DIR}/SparkEngineConfig.cmake")
    message(FATAL_ERROR "Installed SparkEngine package was not found in: ${SPARK_ENGINE_DIR}")
endif()
if(DEFINED SPARK_TEMPLATE_SOURCE_ROOT AND
   NOT SPARK_TEMPLATE_SOURCE_ROOT STREQUAL "" AND
   NOT IS_DIRECTORY "${SPARK_TEMPLATE_SOURCE_ROOT}")
    message(FATAL_ERROR "Template source root does not exist: ${SPARK_TEMPLATE_SOURCE_ROOT}")
endif()
if(DEFINED SPARK_ENGINE_EXECUTABLE AND
   NOT SPARK_ENGINE_EXECUTABLE STREQUAL "" AND
   NOT EXISTS "${SPARK_ENGINE_EXECUTABLE}")
    message(FATAL_ERROR "Installed SparkEngine executable does not exist: ${SPARK_ENGINE_EXECUTABLE}")
endif()
if(NOT DEFINED SPARK_TEMPLATE_CONFIG OR SPARK_TEMPLATE_CONFIG STREQUAL "")
    set(SPARK_TEMPLATE_CONFIG Release)
endif()
if(NOT DEFINED SPARK_TEMPLATE_LIVE_SMOKE_NAME OR SPARK_TEMPLATE_LIVE_SMOKE_NAME STREQUAL "")
    set(SPARK_TEMPLATE_LIVE_SMOKE_NAME EmptyProject)
endif()

file(MAKE_DIRECTORY "${SPARK_TEMPLATE_BUILD_ROOT}")
file(GLOB template_candidates LIST_DIRECTORIES true "${SPARK_TEMPLATE_ROOT}/*")
list(SORT template_candidates)

set(installed_template_names "")
foreach(template_dir IN LISTS template_candidates)
    if(IS_DIRECTORY "${template_dir}" AND EXISTS "${template_dir}/CMakeLists.txt")
        get_filename_component(template_name "${template_dir}" NAME)
        list(APPEND installed_template_names "${template_name}")
    endif()
endforeach()

# Compare the installed distribution's buildable template inventory with the
# source inventory when CI supplies both roots. The builds below still consume
# only staged files; the source tree is used solely as the expected manifest.
if(DEFINED SPARK_TEMPLATE_SOURCE_ROOT AND NOT SPARK_TEMPLATE_SOURCE_ROOT STREQUAL "")
    file(GLOB source_template_candidates LIST_DIRECTORIES true "${SPARK_TEMPLATE_SOURCE_ROOT}/*")
    set(source_template_names "")
    foreach(source_template_dir IN LISTS source_template_candidates)
        if(IS_DIRECTORY "${source_template_dir}" AND EXISTS "${source_template_dir}/CMakeLists.txt")
            get_filename_component(source_template_name "${source_template_dir}" NAME)
            list(APPEND source_template_names "${source_template_name}")
        endif()
    endforeach()
    list(SORT source_template_names)
    if(NOT installed_template_names STREQUAL source_template_names)
        message(FATAL_ERROR
            "Installed template inventory differs from source inventory\n"
            "installed: ${installed_template_names}\n"
            "source: ${source_template_names}")
    endif()
endif()

set(template_count 0)
set(live_smoke_count 0)
foreach(template_dir IN LISTS template_candidates)
    if(NOT IS_DIRECTORY "${template_dir}" OR NOT EXISTS "${template_dir}/CMakeLists.txt")
        continue()
    endif()

    get_filename_component(template_name "${template_dir}" NAME)
    set(template_build "${SPARK_TEMPLATE_BUILD_ROOT}/${template_name}")
    set(configure_command
        "${CMAKE_COMMAND}"
        -S "${template_dir}"
        -B "${template_build}"
        "-DSparkEngine_DIR=${SPARK_ENGINE_DIR}"
        "-DCMAKE_BUILD_TYPE=${SPARK_TEMPLATE_CONFIG}"
    )
    if(DEFINED SPARK_CMAKE_GENERATOR AND NOT SPARK_CMAKE_GENERATOR STREQUAL "")
        list(APPEND configure_command -G "${SPARK_CMAKE_GENERATOR}")
    endif()
    if(DEFINED SPARK_CMAKE_GENERATOR_PLATFORM AND NOT SPARK_CMAKE_GENERATOR_PLATFORM STREQUAL "")
        list(APPEND configure_command -A "${SPARK_CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(DEFINED SPARK_CMAKE_GENERATOR_TOOLSET AND NOT SPARK_CMAKE_GENERATOR_TOOLSET STREQUAL "")
        list(APPEND configure_command -T "${SPARK_CMAKE_GENERATOR_TOOLSET}")
    endif()
    if(DEFINED SPARK_C_COMPILER AND NOT SPARK_C_COMPILER STREQUAL "")
        list(APPEND configure_command "-DCMAKE_C_COMPILER=${SPARK_C_COMPILER}")
    endif()
    if(DEFINED SPARK_CXX_COMPILER AND NOT SPARK_CXX_COMPILER STREQUAL "")
        list(APPEND configure_command "-DCMAKE_CXX_COMPILER=${SPARK_CXX_COMPILER}")
    endif()

    execute_process(
        COMMAND ${configure_command}
        RESULT_VARIABLE configure_result
        OUTPUT_VARIABLE configure_stdout
        ERROR_VARIABLE configure_stderr
    )
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR
            "Installed-template configure failed for ${template_name} (exit ${configure_result})\n"
            "stdout:\n${configure_stdout}\n"
            "stderr:\n${configure_stderr}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${template_build}" --config "${SPARK_TEMPLATE_CONFIG}" --parallel 2
        RESULT_VARIABLE build_result
        OUTPUT_VARIABLE build_stdout
        ERROR_VARIABLE build_stderr
    )
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR
            "Installed-template build failed for ${template_name} (exit ${build_result})\n"
            "stdout:\n${build_stdout}\n"
            "stderr:\n${build_stderr}")
    endif()

    math(EXPR template_count "${template_count} + 1")
    message(STATUS "Installed template ${template_name}: configure/build passed")

    if(DEFINED SPARK_ENGINE_EXECUTABLE AND
       NOT SPARK_ENGINE_EXECUTABLE STREQUAL "" AND
       template_name STREQUAL SPARK_TEMPLATE_LIVE_SMOKE_NAME)
        if(CMAKE_HOST_WIN32)
            set(module_prefix "")
            set(module_suffix ".dll")
        elseif(CMAKE_HOST_APPLE)
            set(module_prefix "lib")
            set(module_suffix ".dylib")
        else()
            set(module_prefix "lib")
            set(module_suffix ".so")
        endif()
        file(GLOB_RECURSE module_candidates LIST_DIRECTORIES false
            "${template_build}/${module_prefix}${template_name}${module_suffix}")
        list(LENGTH module_candidates module_candidate_count)
        if(NOT module_candidate_count EQUAL 1)
            message(FATAL_ERROR
                "Expected one built ${template_name} module for live smoke, found "
                "${module_candidate_count}: ${module_candidates}")
        endif()

        execute_process(
            COMMAND "${SPARK_ENGINE_EXECUTABLE}"
                -headless
                -game "${module_candidates}"
                -test-frames 1
                -require-game
                -no-subprocess
                -no-jobsystem
            WORKING_DIRECTORY "${template_dir}"
            RESULT_VARIABLE live_smoke_result
            OUTPUT_VARIABLE live_smoke_stdout
            ERROR_VARIABLE live_smoke_stderr
            TIMEOUT 30
        )
        if(NOT live_smoke_result EQUAL 0)
            message(FATAL_ERROR
                "Installed ${template_name} live-load smoke failed (exit ${live_smoke_result})\n"
                "stdout:\n${live_smoke_stdout}\n"
                "stderr:\n${live_smoke_stderr}")
        endif()
        math(EXPR live_smoke_count "${live_smoke_count} + 1")
        message(STATUS "Installed template ${template_name}: staged host live-load passed")
    endif()
endforeach()

if(template_count EQUAL 0)
    message(FATAL_ERROR "No buildable templates found under ${SPARK_TEMPLATE_ROOT}")
endif()
if(DEFINED SPARK_ENGINE_EXECUTABLE AND
   NOT SPARK_ENGINE_EXECUTABLE STREQUAL "" AND
   NOT live_smoke_count EQUAL 1)
    message(FATAL_ERROR
        "Requested live smoke template was not built: ${SPARK_TEMPLATE_LIVE_SMOKE_NAME}")
endif()

message(STATUS "Installed template SDK verification passed for ${template_count} template(s)")
