cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED SPARK_TEMPLATE_CONFIG OR SPARK_TEMPLATE_CONFIG STREQUAL "")
    set(SPARK_TEMPLATE_CONFIG Release)
endif()

# Opt-in install driver. With SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR set, stage a
# fresh SDK out of an existing engine build tree into SPARK_TEMPLATE_INSTALL_PREFIX
# and verify the templates against that prefix, so "a standalone CMake project
# consuming an installed SDK" can be checked from a plain CTest instead of only
# in the nightly publish workflow. Without it the caller supplies
# SPARK_TEMPLATE_ROOT and SPARK_ENGINE_DIR itself, exactly as before.
if(DEFINED SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR AND NOT SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR STREQUAL "")
    if(NOT EXISTS "${SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR}/CMakeCache.txt")
        message(FATAL_ERROR
            "SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR is not a configured build tree: "
            "${SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR}")
    endif()
    if(NOT DEFINED SPARK_TEMPLATE_INSTALL_PREFIX OR SPARK_TEMPLATE_INSTALL_PREFIX STREQUAL "")
        message(FATAL_ERROR
            "SPARK_TEMPLATE_INSTALL_PREFIX is required with SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR")
    endif()

    # The next step erases this prefix. Refuse to do that to anything that is not
    # provably scratch space owned by this driver: a mistyped -D would otherwise
    # recursively delete a build tree, a home directory, or an existing SDK.
    file(TO_CMAKE_PATH "${SPARK_TEMPLATE_INSTALL_PREFIX}" spark_install_prefix)
    string(REGEX REPLACE "/+$" "" spark_install_prefix "${spark_install_prefix}")
    if(NOT IS_ABSOLUTE "${spark_install_prefix}")
        message(FATAL_ERROR
            "SPARK_TEMPLATE_INSTALL_PREFIX must be an absolute path: ${spark_install_prefix}")
    endif()
    get_filename_component(spark_install_prefix_parent "${spark_install_prefix}" DIRECTORY)
    if(spark_install_prefix_parent STREQUAL "" OR spark_install_prefix_parent STREQUAL spark_install_prefix)
        message(FATAL_ERROR
            "SPARK_TEMPLATE_INSTALL_PREFIX must not be a filesystem root: ${spark_install_prefix}")
    endif()

    # Owned when this driver has staged here before (marker), or when the prefix
    # lives inside the build tree / template build root the caller already named.
    set(spark_install_marker "${spark_install_prefix}/.spark-template-install-prefix")
    set(spark_prefix_is_scratch FALSE)
    if(EXISTS "${spark_install_marker}")
        set(spark_prefix_is_scratch TRUE)
    endif()
    foreach(spark_owned_root IN ITEMS
            "${SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR}" "${SPARK_TEMPLATE_BUILD_ROOT}")
        if(spark_owned_root STREQUAL "")
            continue()
        endif()
        file(TO_CMAKE_PATH "${spark_owned_root}" spark_owned_root)
        string(REGEX REPLACE "/+$" "" spark_owned_root "${spark_owned_root}")
        string(FIND "${spark_install_prefix}/" "${spark_owned_root}/" spark_owned_position)
        if(spark_owned_position EQUAL 0 AND NOT spark_install_prefix STREQUAL spark_owned_root)
            set(spark_prefix_is_scratch TRUE)
        endif()
    endforeach()

    if(EXISTS "${spark_install_prefix}")
        if(NOT IS_DIRECTORY "${spark_install_prefix}")
            message(FATAL_ERROR
                "SPARK_TEMPLATE_INSTALL_PREFIX exists and is not a directory: ${spark_install_prefix}")
        endif()
        if(NOT spark_prefix_is_scratch)
            message(FATAL_ERROR
                "Refusing to erase ${spark_install_prefix}: it is not a staging prefix this driver owns.\n"
                "Point SPARK_TEMPLATE_INSTALL_PREFIX at a new directory, or at one inside "
                "SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR / SPARK_TEMPLATE_BUILD_ROOT, and delete the "
                "existing directory yourself if you really meant this one.")
        endif()
        file(REMOVE_RECURSE "${spark_install_prefix}")
    endif()
    file(MAKE_DIRECTORY "${spark_install_prefix}")
    file(WRITE "${spark_install_marker}"
        "Scratch install prefix staged by cmake/VerifyInstalledTemplates.cmake. Safe to delete.\n")
    set(SPARK_TEMPLATE_INSTALL_PREFIX "${spark_install_prefix}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${SPARK_TEMPLATE_INSTALL_FROM_BUILD_DIR}"
            --prefix "${SPARK_TEMPLATE_INSTALL_PREFIX}"
            --config "${SPARK_TEMPLATE_CONFIG}"
        RESULT_VARIABLE install_result
        OUTPUT_VARIABLE install_stdout
        ERROR_VARIABLE install_stderr
    )
    if(NOT install_result EQUAL 0)
        message(FATAL_ERROR
            "Staging an installed SDK failed (exit ${install_result})\n"
            "stdout:\n${install_stdout}\n"
            "stderr:\n${install_stderr}")
    endif()

    set(SPARK_TEMPLATE_ROOT "${SPARK_TEMPLATE_INSTALL_PREFIX}/share/SparkEngine/templates")
    set(SPARK_ENGINE_DIR "${SPARK_TEMPLATE_INSTALL_PREFIX}/lib/cmake/SparkEngine")
    message(STATUS "Staged an installed SDK for template verification in ${SPARK_TEMPLATE_INSTALL_PREFIX}")
endif()

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
# Live smoke inventory. This is a list: by default every buildable template gets
# a staged-host run, because configure/build coverage says nothing about whether
# a package's authored scene actually appends, ticks and renders on a real host.
# Callers may narrow it (for example to one package in a time-boxed job).
if(NOT DEFINED SPARK_TEMPLATE_LIVE_SMOKE_NAME OR SPARK_TEMPLATE_LIVE_SMOKE_NAME STREQUAL "")
    set(SPARK_TEMPLATE_LIVE_SMOKE_NAME "")
    set(spark_live_smoke_all TRUE)
else()
    set(spark_live_smoke_all FALSE)
endif()
# Every live-smoke branch below is guarded on SPARK_ENGINE_EXECUTABLE, including
# the final "did each requested smoke actually run" count check. So an explicit
# SPARK_TEMPLATE_LIVE_SMOKE_NAME with no usable executable silently reconciles to
# zero runs and the script still reports "verification passed" -- a check that
# stopped checking, reporting the reassuring value. Refuse instead.
if(NOT spark_live_smoke_all)
    if(NOT DEFINED SPARK_ENGINE_EXECUTABLE OR SPARK_ENGINE_EXECUTABLE STREQUAL "")
        message(FATAL_ERROR
            "SPARK_TEMPLATE_LIVE_SMOKE_NAME requested a live smoke for "
            "'${SPARK_TEMPLATE_LIVE_SMOKE_NAME}' but SPARK_ENGINE_EXECUTABLE is not set, so no "
            "staged-host run can happen. Pass the installed engine executable, or leave "
            "SPARK_TEMPLATE_LIVE_SMOKE_NAME unset to skip live smokes deliberately.")
    endif()
    if(IS_DIRECTORY "${SPARK_ENGINE_EXECUTABLE}")
        message(FATAL_ERROR
            "SPARK_TEMPLATE_LIVE_SMOKE_NAME requested a live smoke for "
            "'${SPARK_TEMPLATE_LIVE_SMOKE_NAME}' but SPARK_ENGINE_EXECUTABLE is a directory, not an "
            "executable: ${SPARK_ENGINE_EXECUTABLE}")
    endif()
endif()
if(NOT DEFINED SPARK_TEMPLATE_LIVE_SMOKE_FRAMES OR SPARK_TEMPLATE_LIVE_SMOKE_FRAMES STREQUAL "")
    set(SPARK_TEMPLATE_LIVE_SMOKE_FRAMES 8)
endif()

file(MAKE_DIRECTORY "${SPARK_TEMPLATE_BUILD_ROOT}")
file(GLOB template_candidates LIST_DIRECTORIES true "${SPARK_TEMPLATE_ROOT}/*")
list(SORT template_candidates)

# Validate the operational files that SparkLauncher and SparkEditor's service
# topology open from each installed project before spending time building it.
include("${CMAKE_CURRENT_LIST_DIR}/VerifyTemplateServiceConfigs.cmake")

set(installed_template_names "")
foreach(template_dir IN LISTS template_candidates)
    if(IS_DIRECTORY "${template_dir}" AND EXISTS "${template_dir}/CMakeLists.txt")
        get_filename_component(template_name "${template_dir}" NAME)
        list(APPEND installed_template_names "${template_name}")
    endif()
endforeach()

if(spark_live_smoke_all)
    set(SPARK_TEMPLATE_LIVE_SMOKE_NAME ${installed_template_names})
endif()
list(LENGTH SPARK_TEMPLATE_LIVE_SMOKE_NAME expected_live_smoke_count)

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

    list(FIND SPARK_TEMPLATE_LIVE_SMOKE_NAME "${template_name}" live_smoke_index)
    if(DEFINED SPARK_ENGINE_EXECUTABLE AND
       NOT SPARK_ENGINE_EXECUTABLE STREQUAL "" AND
       NOT live_smoke_index EQUAL -1)
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

        # The runtime emits logs, traces, saved state, and other mutable files
        # relative to its working directory. Run against a disposable copy so
        # package verification cannot modify the staged template it validates.
        string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef live_smoke_nonce)
        set(live_smoke_work
            "${SPARK_TEMPLATE_BUILD_ROOT}/live-smoke-${template_name}-${live_smoke_nonce}")
        file(MAKE_DIRECTORY "${live_smoke_work}")
        file(COPY "${template_dir}/" DESTINATION "${live_smoke_work}")

        execute_process(
            COMMAND "${SPARK_ENGINE_EXECUTABLE}"
                -headless
                -game "${module_candidates}"
                -test-frames ${SPARK_TEMPLATE_LIVE_SMOKE_FRAMES}
                -require-game
                -no-subprocess
                -no-jobsystem
            WORKING_DIRECTORY "${live_smoke_work}"
            RESULT_VARIABLE live_smoke_result
            OUTPUT_VARIABLE live_smoke_stdout
            ERROR_VARIABLE live_smoke_stderr
            TIMEOUT 60
        )
        file(REMOVE_RECURSE "${live_smoke_work}")
        if(NOT live_smoke_result EQUAL 0)
            message(FATAL_ERROR
                "Installed ${template_name} live-load smoke failed (exit ${live_smoke_result})\n"
                "stdout:\n${live_smoke_stdout}\n"
                "stderr:\n${live_smoke_stderr}")
        endif()

        # A zero exit only proves the host started and stopped. Require the
        # module's own evidence that it resolved a scene and took ownership of at
        # least one entity, so a template whose scene contract silently falls
        # through can no longer pass this gate.
        set(live_smoke_output "${live_smoke_stdout}${live_smoke_stderr}")
        if(NOT live_smoke_output MATCHES
           "${template_name} loaded scene '[^']+' with ([1-9][0-9]*) owned entities")
            message(FATAL_ERROR
                "Installed ${template_name} live-load smoke produced no scene-ownership evidence.\n"
                "Expected a log line \"${template_name} loaded scene '<path>' with <n> owned entities\" "
                "with a non-zero count.\n"
                "stdout:\n${live_smoke_stdout}\n"
                "stderr:\n${live_smoke_stderr}")
        endif()
        set(live_smoke_entities "${CMAKE_MATCH_1}")
        math(EXPR live_smoke_count "${live_smoke_count} + 1")
        message(STATUS
            "Installed template ${template_name}: staged host live-load passed "
            "(${live_smoke_entities} owned entities over ${SPARK_TEMPLATE_LIVE_SMOKE_FRAMES} frames)")
    endif()
endforeach()

if(template_count EQUAL 0)
    message(FATAL_ERROR "No buildable templates found under ${SPARK_TEMPLATE_ROOT}")
endif()
if(DEFINED SPARK_ENGINE_EXECUTABLE AND
   NOT SPARK_ENGINE_EXECUTABLE STREQUAL "" AND
   NOT live_smoke_count EQUAL expected_live_smoke_count)
    message(FATAL_ERROR
        "Live smoke ran for ${live_smoke_count} of ${expected_live_smoke_count} requested template(s): "
        "${SPARK_TEMPLATE_LIVE_SMOKE_NAME}")
endif()

message(STATUS "Installed template SDK verification passed for ${template_count} template(s)")
