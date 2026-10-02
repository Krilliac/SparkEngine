# SDK-240 / ASSET-220: generate a game with the INSTALLED spark-cli from the
# SDK component's shipped EmptyProject example and build it from a component
# install (sdk, tools and their declared runtime dependency), with nothing from
# the engine source or build tree.
#
# The sdk component installs Templates/EmptyProject as
# share/SparkEngine/sdk/examples/EmptyProject and the SDK README calls it
# buildable, but SparkSDKComponentCompleteness only checks that its files exist
# and SparkInstalledTemplates builds templates from a FULL install. This runner:
#
#   1. installs ONLY the sdk and tools components of the configured engine build,
#      plus runtime, which both declare as a dependency (CPACK_COMPONENT_SDK_DEPENDS
#      and CPACK_COMPONENT_TOOLS_DEPENDS): the bundled SDL2's shared library ships in
#      runtime while its CMake export ships in sdk, so sdk alone is not installable;
#   2. runs `python <prefix>/tools/spark-cli/spark_cli.py new SparkGeneratedGame
#      --template EmptyProject` with SPARK_ENGINE_DIR unset and the working
#      directory outside the source tree, so the CLI must find the prefix and
#      its sdk example on its own and rename the project; then configures and
#      builds the generated project against <prefix>/lib/cmake/SparkEngine, in
#      the engine's configuration (the example links the static
#      Spark::SparkEngineLib on Windows, so the CRT flavour and iterator debug
#      level must match the engine's);
#   3. requires exactly one module image and its <image>.sparkabi sidecar, the
#      sidecar's sdk_version to equal the installed Spark/Version.h and its
#      binary_sha256 to hash the image, and (with SPARK_REFERENCE_SIDECAR) every
#      other field to equal a host-built module's sidecar, i.e. the SDK-only
#      build passes ModuleManager's pre-load ABI gate;
#   4. dumps the Ninja dependency and command logs with '/' separators and
#      fails on any resolved header, library or flag path inside the engine
#      source or build tree (cmake/SparkPackageConsumerBoundary.cmake splits on
#      backslashes, so raw MSVC paths would never match, and judges only
#      absolute paths, so the build-relative dependencies ninja records are
#      anchored at the build directory first), and fails if those dumps were
#      not scanned or never mention the prefix.
#
# Required: SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
#           SPARK_CONSUMER_GENERATOR (must be "Ninja Multi-Config")
#           SPARK_CONSUMER_MAKE_PROGRAM (the ninja executable)
#           SPARK_PYTHON_EXECUTABLE (runs the installed spark-cli)
# Optional: SPARK_CONSUMER_PLATFORM SPARK_CONSUMER_TOOLSET SPARK_CONSUMER_COMPILER
#           SPARK_CONSUMER_TOOLCHAIN SPARK_REFERENCE_SIDECAR
#
# -DSPARK_SDK_TEMPLATE_SELF_TEST=ON (with SPARK_TEST_ROOT) instead runs the
# sidecar and boundary checks against inline fixtures; every broken fixture
# must be rejected by name and every good one accepted.
#
# Success prints one record (module=SparkGeneratedGame proves the CLI renamed it):
#   SPARK_SDK_TEMPLATE module=<name> sdk_version=<n> scanned=<k> violations=0

cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/SparkPackageConsumerBoundary.cmake")

set(_spark_sdk_template_keys
    format struct_size magic sdk_version runtime_abi_version compiler_family compiler_abi_version
    cxx_language_level runtime_library iterator_debug_level pointer_size binary_sha256)

# Parses the key=value lines cmake/WriteSparkModuleABI.cmake writes. Sets
# <prefix>_<key> in the caller for every key; <out_error> names the first
# malformed, unknown, duplicate, empty or missing key.
function(_spark_sdk_template_parse_sidecar text prefix out_error)
    set(_error "")
    set(_seen)
    string(REPLACE "\r" "" text "${text}")
    if(text MATCHES ";")
        set(${out_error} "sidecar contains a ';'" PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "\n" ";" _lines "${text}")
    foreach(_line IN LISTS _lines)
        if(_line STREQUAL "")
            continue()
        endif()
        if(NOT _line MATCHES "^([a-z0-9_]+)=([^=]*)$")
            set(_error "malformed sidecar line '${_line}'")
            break()
        endif()
        set(_key "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_2}")
        if(NOT _key IN_LIST _spark_sdk_template_keys)
            set(_error "unknown sidecar key '${_key}'")
            break()
        endif()
        if(_key IN_LIST _seen)
            set(_error "duplicate sidecar key '${_key}'")
            break()
        endif()
        if(_value STREQUAL "")
            set(_error "sidecar key '${_key}' is empty")
            break()
        endif()
        list(APPEND _seen "${_key}")
        set(${prefix}_${_key} "${_value}" PARENT_SCOPE)
    endforeach()
    if(_error STREQUAL "")
        foreach(_key IN LISTS _spark_sdk_template_keys)
            if(NOT _key IN_LIST _seen)
                set(_error "sidecar is missing key '${_key}'")
                break()
            endif()
        endforeach()
    endif()
    set(${out_error} "${_error}" PARENT_SCOPE)
endfunction()

# <out_error> is empty when the module sidecar would pass the pre-load gate the
# host applies, or names the first rejected field. <reference_text> and
# <image_sha256> are skipped when empty.
function(_spark_sdk_template_check_sidecar text expected_sdk reference_text image_sha256 out_error)
    _spark_sdk_template_parse_sidecar("${text}" _module _error)
    if(NOT _error STREQUAL "")
        set(${out_error} "module sidecar: ${_error}" PARENT_SCOPE)
        return()
    endif()
    if(NOT "${_module_sdk_version}" STREQUAL "${expected_sdk}")
        set(${out_error}
            "sdk_version: module sidecar has ${_module_sdk_version}, installed Spark/Version.h defines ${expected_sdk}"
            PARENT_SCOPE)
        return()
    endif()
    if(NOT image_sha256 STREQUAL "")
        string(TOLOWER "${_module_binary_sha256}" _recorded)
        string(TOLOWER "${image_sha256}" _actual)
        if(NOT _recorded STREQUAL _actual)
            set(${out_error} "binary_sha256: sidecar records ${_recorded}, module image hashes to ${_actual}"
                PARENT_SCOPE)
            return()
        endif()
    endif()
    if(NOT reference_text STREQUAL "")
        _spark_sdk_template_parse_sidecar("${reference_text}" _reference _error)
        if(NOT _error STREQUAL "")
            set(${out_error} "reference sidecar: ${_error}" PARENT_SCOPE)
            return()
        endif()
        foreach(_key IN LISTS _spark_sdk_template_keys)
            if(_key STREQUAL "binary_sha256")
                continue()
            endif()
            if(NOT "${_module_${_key}}" STREQUAL "${_reference_${_key}}")
                set(${out_error}
                    "${_key}: SDK-only build has ${_module_${_key}}, host-built reference has ${_reference_${_key}}"
                    PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endif()
    set(${out_error} "" PARENT_SCOPE)
endfunction()

# Writes tool output for the boundary scanner: '/' separators, and a space in
# front of every drive path so a flag glued to it (-ID:/..., -external:ID:/...,
# /LIBPATH:D:/...) cannot hide the path inside a non-absolute token.
function(_spark_sdk_template_write_boundary_dump path text)
    string(REPLACE "\\" "/" text "${text}")
    string(REGEX REPLACE "([A-Za-z]:/)" " \\1" text "${text}")
    file(WRITE "${path}" "${text}")
endfunction()

# `ninja -t deps` records dependencies relative to the directory ninja ran in
# whenever it can: the msvc deps parser rewrites every /showIncludes path on
# the build directory's drive as "../prefix/include/...". The boundary scanner
# only judges absolute paths, so anchor every relative dependency line (the
# indented lines under each "<target>: #deps" header) at <build_dir>.
function(_spark_sdk_template_anchor_deps text build_dir out_var)
    string(REPLACE "\\" "/" text "${text}")
    string(REPLACE "\r" "" text "${text}")
    # Keep the text list-safe; the scanner treats these as separators anyway.
    string(REPLACE ";" " " text "${text}")
    string(REPLACE "[" " " text "${text}")
    string(REPLACE "]" " " text "${text}")
    string(REPLACE "\n" ";" _lines "${text}")
    set(_anchored "")
    foreach(_line IN LISTS _lines)
        if(_line MATCHES "^[ \t]+([^ \t].*)$")
            string(REGEX REPLACE "[ \t]+$" "" _dep "${CMAKE_MATCH_1}")
            if(NOT _dep MATCHES "^/" AND NOT _dep MATCHES "^[A-Za-z]:/")
                cmake_path(SET _dep NORMALIZE "${build_dir}/${_dep}")
                set(_line "    ${_dep}")
            endif()
        endif()
        string(APPEND _anchored "${_line}\n")
    endforeach()
    set(${out_var} "${_anchored}" PARENT_SCOPE)
endfunction()

# <out> is TRUE when <text> names a path inside <prefix>.
function(_spark_sdk_template_mentions_prefix text prefix out)
    string(REPLACE "\\" "/" text "${text}")
    string(TOLOWER "${text}" text)
    string(TOLOWER "${prefix}/" prefix)
    string(FIND "${text}" "${prefix}" _at)
    if(_at EQUAL -1)
        set(${out} FALSE PARENT_SCOPE)
    else()
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

function(_spark_sdk_template_same_path out a b)
    cmake_path(SET _a NORMALIZE "${a}")
    cmake_path(SET _b NORMALIZE "${b}")
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${_a}" _a)
        string(TOLOWER "${_b}" _b)
    endif()
    if(_a STREQUAL _b)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# Scans <build_dir>. <out_error> is empty only when there are no violations and
# every file in <required_dumps> was among the scanned files.
function(_spark_sdk_template_check_boundary build_dir forbidden_roots allowed_roots required_dumps out_scanned out_error)
    spark_package_consumer_boundary_violations(
        OUT_VAR _violations
        SCANNED_VAR _scanned
        CONSUMER_BUILD_DIR "${build_dir}"
        FORBIDDEN_ROOTS ${forbidden_roots}
        ALLOWED_ROOTS ${allowed_roots})
    list(LENGTH _scanned _scanned_count)
    set(${out_scanned} "${_scanned_count}" PARENT_SCOPE)
    if(_violations)
        list(JOIN _violations "\n  " _listing)
        set(${out_error} "engine source/build-tree paths reached the consumer build:\n  ${_listing}" PARENT_SCOPE)
        return()
    endif()
    foreach(_dump IN LISTS required_dumps)
        set(_found FALSE)
        foreach(_file IN LISTS _scanned)
            _spark_sdk_template_same_path(_same "${_file}" "${_dump}")
            if(_same)
                set(_found TRUE)
                break()
            endif()
        endforeach()
        if(NOT _found)
            set(${out_error} "boundary scan did not read ${_dump}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_error} "" PARENT_SCOPE)
endfunction()

if(NOT DEFINED SPARK_TEST_ROOT OR SPARK_TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_TEST_ROOT is required for the installed SDK template test")
endif()
file(TO_CMAKE_PATH "${SPARK_TEST_ROOT}" SPARK_TEST_ROOT)

if(SPARK_SDK_TEMPLATE_SELF_TEST)
    string(CONCAT _good
        "format=1\nstruct_size=64\nmagic=1263685715\nsdk_version=5\nruntime_abi_version=1\ncompiler_family=1\n"
        "compiler_abi_version=1944\ncxx_language_level=202302\nruntime_library=1\niterator_debug_level=0\n"
        "pointer_size=8\nbinary_sha256=AB12\n")

    function(_spark_expect_sidecar_case name text reference image_sha256 expected_field)
        _spark_sdk_template_check_sidecar("${text}" 5 "${reference}" "${image_sha256}" _error)
        if(expected_field STREQUAL "")
            if(NOT _error STREQUAL "")
                message(FATAL_ERROR "SDK template sidecar case '${name}' unexpectedly failed: ${_error}")
            endif()
        elseif(_error STREQUAL "")
            message(FATAL_ERROR "SDK template sidecar case '${name}' unexpectedly passed")
        elseif(NOT _error MATCHES "${expected_field}")
            message(FATAL_ERROR
                "SDK template sidecar case '${name}' was rejected without naming '${expected_field}': ${_error}")
        endif()
    endfunction()

    string(REPLACE "sdk_version=5" "sdk_version=4" _old_sdk "${_good}")
    string(REPLACE "runtime_library=1\n" "" _missing_key "${_good}")
    string(REPLACE "compiler_abi_version=1944" "compiler_abi_version=1943" _drifted "${_good}")
    string(REPLACE "binary_sha256=AB12" "binary_sha256=CD34" _other_image "${_good}")
    string(REPLACE "cxx_language_level=202302" "cxx_language_level=202302\ncxx_language_level=202002" _duplicate
        "${_good}")

    _spark_expect_sidecar_case(valid "${_good}" "${_good}" "ab12" "")
    _spark_expect_sidecar_case(reference-differs-only-in-image-hash "${_good}" "${_other_image}" "" "")
    _spark_expect_sidecar_case(mismatched-sdk-version "${_old_sdk}" "" "" "sdk_version")
    _spark_expect_sidecar_case(missing-key "${_missing_key}" "" "" "runtime_library")
    _spark_expect_sidecar_case(duplicate-key "${_duplicate}" "" "" "cxx_language_level")
    _spark_expect_sidecar_case(reference-field-drift "${_drifted}" "${_good}" "" "compiler_abi_version")
    _spark_expect_sidecar_case(sidecar-for-another-image "${_good}" "" "ef56" "binary_sha256")

    # Boundary fixtures: an engine header recorded with MSVC backslashes, one
    # recorded relative to the build directory (as ninja's msvc deps parser
    # stores same-drive headers) and a flag-glued engine include must be
    # rejected; prefix-only paths, absolute or build-relative, accepted. The
    # fixture builds sit where the real one does, beside the prefix.
    set(_fixture "${SPARK_TEST_ROOT}/self-test")
    file(REMOVE_RECURSE "${_fixture}")
    set(_engine "${_fixture}/engine")
    set(_prefix "${_fixture}/root/prefix")
    file(MAKE_DIRECTORY "${_engine}/Source/Core" "${_prefix}/include/Spark")
    string(REPLACE "/" "\\" _engine_backslashed "${_engine}")
    string(REPLACE "/" "\\" _prefix_backslashed "${_prefix}")

    function(_spark_expect_boundary_case name deps_text commands_text expect_violation)
        set(_build "${_fixture}/root/${name}")
        file(MAKE_DIRECTORY "${_build}/spark-boundary")
        set(_dumps "${_build}/spark-boundary/ninja-deps.d" "${_build}/spark-boundary/ninja-commands.d")
        _spark_sdk_template_anchor_deps("${deps_text}" "${_build}" _anchored_deps)
        if(NOT expect_violation)
            foreach(_dump_text IN ITEMS _anchored_deps commands_text)
                _spark_sdk_template_mentions_prefix("${${_dump_text}}" "${_prefix}" _mentions_prefix)
                if(NOT _mentions_prefix)
                    message(FATAL_ERROR "SDK template boundary case '${name}' does not mention the prefix in its "
                        "${_dump_text}: ${${_dump_text}}")
                endif()
            endforeach()
        endif()
        _spark_sdk_template_write_boundary_dump("${_build}/spark-boundary/ninja-deps.d" "${_anchored_deps}")
        _spark_sdk_template_write_boundary_dump("${_build}/spark-boundary/ninja-commands.d" "${commands_text}")
        _spark_sdk_template_check_boundary("${_build}" "${_engine}" "${_fixture}/root" "${_dumps}" _scanned _error)
        if(expect_violation)
            if(_error STREQUAL "")
                message(FATAL_ERROR "SDK template boundary case '${name}' unexpectedly passed")
            elseif(NOT _error MATCHES "Source/Core/Engine[.]h")
                message(FATAL_ERROR
                    "SDK template boundary case '${name}' was rejected without naming the engine header: ${_error}")
            endif()
        elseif(NOT _error STREQUAL "")
            message(FATAL_ERROR "SDK template boundary case '${name}' unexpectedly failed: ${_error}")
        elseif(_scanned LESS 2)
            message(FATAL_ERROR "SDK template boundary case '${name}' scanned ${_scanned} files")
        endif()
    endfunction()

    set(_clean_deps "GameModule.cpp.obj: #deps 1\n    ${_prefix_backslashed}\\include\\Spark\\SparkSDK.h\n")
    set(_clean_commands "cl.exe -external:I${_prefix_backslashed}\\include /c GameModule.cpp\n")
    _spark_expect_boundary_case(clean "${_clean_deps}" "${_clean_commands}" FALSE)
    _spark_expect_boundary_case(backslash-engine-header
        "GameModule.cpp.obj: #deps 1\n    ${_engine_backslashed}\\Source\\Core\\Engine.h\n" "${_clean_commands}" TRUE)
    _spark_expect_boundary_case(build-relative-prefix-header
        "GameModule.cpp.obj: #deps 1, deps mtime 1 (VALID)\n    ..\\prefix\\include\\Spark\\SparkSDK.h\n\n"
        "${_clean_commands}" FALSE)
    string(CONCAT _relative_engine_deps
        "GameModule.cpp.obj: #deps 2, deps mtime 1 (VALID)\n    ..\\prefix\\include\\Spark\\SparkSDK.h\n"
        "    ..\\..\\engine\\Source\\Core\\Engine.h\n\n")
    _spark_expect_boundary_case(build-relative-engine-header "${_relative_engine_deps}" "${_clean_commands}" TRUE)
    # MSVC glues imported (external) include dirs to their flag; the scanner
    # splits on ':' so only the drive-path spacing keeps that path whole. The
    # flag only exists on Windows hosts; elsewhere the glued -I form is checked.
    if(CMAKE_HOST_WIN32)
        set(_glued_flag "-external:I")
    else()
        set(_glued_flag "-I")
    endif()
    _spark_expect_boundary_case(flag-glued-engine-include
        "${_clean_deps}" "cl.exe ${_glued_flag}${_engine_backslashed}\\Source\\Core\\Engine.h /c GameModule.cpp\n"
        TRUE)

    # A scan that reads nothing must not look clean.
    set(_empty "${_fixture}/unscanned")
    file(MAKE_DIRECTORY "${_empty}")
    _spark_sdk_template_check_boundary("${_empty}" "${_engine}" "${_fixture}/root"
        "${_empty}/spark-boundary/ninja-deps.d" _scanned _error)
    if(NOT _error MATCHES "did not read")
        message(FATAL_ERROR "SDK template boundary case 'unscanned-dump' was not rejected: ${_error}")
    endif()

    file(REMOVE_RECURSE "${_fixture}")
    message(STATUS "SPARK_SDK_TEMPLATE_SELF_TEST sidecar_cases=7 boundary_cases=6 passed")
    return()
endif()

foreach(_required IN ITEMS
        SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_CONSUMER_GENERATOR SPARK_CONSUMER_MAKE_PROGRAM
        SPARK_PYTHON_EXECUTABLE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the installed SDK template test")
    endif()
endforeach()
foreach(_path_var IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONSUMER_MAKE_PROGRAM)
    file(TO_CMAKE_PATH "${${_path_var}}" ${_path_var})
endforeach()
if(NOT IS_DIRECTORY "${SPARK_ENGINE_BUILD_DIR}" OR NOT EXISTS "${SPARK_ENGINE_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR "SPARK_ENGINE_BUILD_DIR is not a configured build tree: ${SPARK_ENGINE_BUILD_DIR}")
endif()
if(NOT EXISTS "${SPARK_SOURCE_ROOT}/Templates/EmptyProject/CMakeLists.txt")
    message(FATAL_ERROR "SPARK_SOURCE_ROOT is not the SparkEngine source tree: ${SPARK_SOURCE_ROOT}")
endif()
# The boundary proof reads Ninja's dependency log; another generator would
# leave nothing to scan and the check would silently pass.
if(NOT SPARK_CONSUMER_GENERATOR STREQUAL "Ninja Multi-Config")
    message(FATAL_ERROR
        "The installed SDK template test needs the 'Ninja Multi-Config' consumer generator, got "
        "'${SPARK_CONSUMER_GENERATOR}'")
endif()
if(NOT EXISTS "${SPARK_CONSUMER_MAKE_PROGRAM}" OR IS_DIRECTORY "${SPARK_CONSUMER_MAKE_PROGRAM}")
    message(FATAL_ERROR "SPARK_CONSUMER_MAKE_PROGRAM is not a ninja executable: ${SPARK_CONSUMER_MAKE_PROGRAM}")
endif()
set(_reference_text "")
if(DEFINED SPARK_REFERENCE_SIDECAR AND NOT SPARK_REFERENCE_SIDECAR STREQUAL "")
    if(NOT EXISTS "${SPARK_REFERENCE_SIDECAR}")
        message(FATAL_ERROR "SPARK_REFERENCE_SIDECAR does not exist: ${SPARK_REFERENCE_SIDECAR}")
    endif()
    file(READ "${SPARK_REFERENCE_SIDECAR}" _reference_text)
endif()

function(_run_checked _stage)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT 1500)
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "${_stage} failed (${_result}):\n${_output}\n${_error}")
    endif()
endfunction()

set(_template_name "EmptyProject")
set(_module_name "SparkGeneratedGame")

# Work under the canonical spelling of the test root. The root lives in %TEMP%,
# which on hosted runners is the 8.3 alias C:/Users/RUNNER~1/...; CMake expands
# that alias in the consumer's include and library paths, so the "dumps mention
# the prefix" guard below would never match a short-spelled prefix.
file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
file(MAKE_DIRECTORY "${SPARK_TEST_ROOT}")
spark_real_path(SPARK_TEST_ROOT "${SPARK_TEST_ROOT}")
set(_prefix "${SPARK_TEST_ROOT}/prefix")
set(_projects "${SPARK_TEST_ROOT}/projects")
set(_source "${_projects}/${_module_name}")
set(_build "${SPARK_TEST_ROOT}/b")

foreach(_component IN ITEMS runtime sdk tools)
    _run_checked("Install the ${_component} component"
        "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
        --config "${SPARK_CONFIG}" --prefix "${_prefix}" --component ${_component})
endforeach()

set(_example "${_prefix}/share/SparkEngine/sdk/examples/${_template_name}")
if(NOT EXISTS "${_example}/CMakeLists.txt")
    message(FATAL_ERROR "The sdk component did not install the ${_template_name} example at ${_example}")
endif()
set(_installed_cli "${_prefix}/tools/spark-cli/spark_cli.py")
if(NOT EXISTS "${_installed_cli}")
    message(FATAL_ERROR "The tools component did not install spark-cli at ${_installed_cli}")
endif()
set(_version_header "${_prefix}/include/Spark/Version.h")
if(NOT EXISTS "${_version_header}")
    message(FATAL_ERROR "The sdk component did not install ${_version_header}")
endif()
file(STRINGS "${_version_header}" _sdk_lines REGEX "^#[ \t]*define[ \t]+SPARK_SDK_VERSION[ \t]")
list(LENGTH _sdk_lines _sdk_line_count)
if(NOT _sdk_line_count EQUAL 1 OR NOT _sdk_lines MATCHES "SPARK_SDK_VERSION[ \t]+([0-9]+)[uU]?([ \t]|$)")
    message(FATAL_ERROR "${_version_header} must define SPARK_SDK_VERSION exactly once as a decimal integer")
endif()
set(_sdk_version "${CMAKE_MATCH_1}")

# Generate the game with the installed CLI. It must locate the prefix from its
# own install location (SPARK_ENGINE_DIR is unset) and write outside the prefix.
file(MAKE_DIRECTORY "${_projects}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=SPARK_ENGINE_DIR
        "${SPARK_PYTHON_EXECUTABLE}" -B "${_installed_cli}" new ${_module_name}
        --template ${_template_name} --output "${_projects}"
    WORKING_DIRECTORY "${_projects}"
    RESULT_VARIABLE _new_result
    OUTPUT_VARIABLE _new_output
    ERROR_VARIABLE _new_error
    TIMEOUT 120)
if(NOT "${_new_result}" STREQUAL "0")
    message(FATAL_ERROR "The installed spark-cli could not create ${_module_name} (${_new_result}):\n"
                        "${_new_output}\n${_new_error}")
endif()
if(NOT EXISTS "${_source}/CMakeLists.txt" OR NOT EXISTS "${_source}/${_module_name}.sparkproject")
    message(FATAL_ERROR "spark-cli new did not produce a renamed ${_module_name} project:\n${_new_output}")
endif()
file(REAL_PATH "${_prefix}/lib/cmake/SparkEngine" _package_dir_real)
if(NOT _new_output MATCHES "-DSparkEngine_DIR=([^\r\n]+)")
    message(FATAL_ERROR "spark-cli new printed no install-based SparkEngine_DIR:\n${_new_output}")
endif()
file(REAL_PATH "${CMAKE_MATCH_1}" _printed_package_dir)
if(NOT _printed_package_dir STREQUAL _package_dir_real)
    message(FATAL_ERROR "spark-cli new printed SparkEngine_DIR=${CMAKE_MATCH_1}, expected ${_package_dir_real}")
endif()

set(_configure
    "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}"
    -G "${SPARK_CONSUMER_GENERATOR}"
    "-DSparkEngine_DIR=${_prefix}/lib/cmake/SparkEngine"
    "-DCMAKE_MAKE_PROGRAM=${SPARK_CONSUMER_MAKE_PROGRAM}"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=")
if(DEFINED SPARK_CONSUMER_PLATFORM AND NOT "${SPARK_CONSUMER_PLATFORM}" STREQUAL "")
    list(APPEND _configure -A "${SPARK_CONSUMER_PLATFORM}")
endif()
if(DEFINED SPARK_CONSUMER_TOOLSET AND NOT "${SPARK_CONSUMER_TOOLSET}" STREQUAL "")
    list(APPEND _configure -T "${SPARK_CONSUMER_TOOLSET}")
endif()
if(DEFINED SPARK_CONSUMER_COMPILER AND NOT "${SPARK_CONSUMER_COMPILER}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_CXX_COMPILER=${SPARK_CONSUMER_COMPILER}")
endif()
if(DEFINED SPARK_CONSUMER_TOOLCHAIN AND NOT "${SPARK_CONSUMER_TOOLCHAIN}" STREQUAL "")
    list(APPEND _configure "-DCMAKE_TOOLCHAIN_FILE=${SPARK_CONSUMER_TOOLCHAIN}")
endif()
_run_checked("Configure the installed ${_module_name} example" ${_configure})
_run_checked("Build the installed ${_module_name} example"
    "${CMAKE_COMMAND}" --build "${_build}" --config "${SPARK_CONFIG}" --parallel 2)

# Exactly one module image with its pre-load sidecar.
file(GLOB_RECURSE _images LIST_DIRECTORIES FALSE
    "${_build}/${_module_name}.dll" "${_build}/lib${_module_name}.so" "${_build}/lib${_module_name}.dylib")
list(FILTER _images EXCLUDE REGEX "/CMakeFiles/")
list(LENGTH _images _image_count)
if(NOT _image_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one ${_module_name} module image under ${_build}, found ${_image_count}: ${_images}")
endif()
set(_sidecar "${_images}.sparkabi")
if(NOT EXISTS "${_sidecar}")
    message(FATAL_ERROR "The SDK-only build produced ${_images} without its pre-load sidecar ${_sidecar}")
endif()
file(READ "${_sidecar}" _sidecar_text)
file(SHA256 "${_images}" _image_sha256)
_spark_sdk_template_check_sidecar("${_sidecar_text}" "${_sdk_version}" "${_reference_text}" "${_image_sha256}"
    _sidecar_error)
if(NOT _sidecar_error STREQUAL "")
    message(FATAL_ERROR "The SDK-only ${_module_name} module would be rejected before load: ${_sidecar_error}")
endif()

# Boundary proof over what the build actually resolved.
set(_ninja_file "build-${SPARK_CONFIG}.ninja")
if(NOT EXISTS "${_build}/${_ninja_file}")
    message(FATAL_ERROR "Ninja Multi-Config did not write ${_build}/${_ninja_file}")
endif()
set(_dump_dir "${_build}/spark-boundary")
file(MAKE_DIRECTORY "${_dump_dir}")
set(_dumps)
foreach(_tool IN ITEMS deps commands)
    execute_process(
        COMMAND "${SPARK_CONSUMER_MAKE_PROGRAM}" -C "${_build}" -f "${_ninja_file}" -t ${_tool}
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error
        TIMEOUT 120)
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "ninja -t ${_tool} failed (${_result}):\n${_error}")
    endif()
    if(_tool STREQUAL "deps")
        _spark_sdk_template_anchor_deps("${_output}" "${_build}" _output)
    endif()
    # Fail closed on a dump that could not have seen the SDK at all.
    _spark_sdk_template_mentions_prefix("${_output}" "${_prefix}" _mentions_prefix)
    if(NOT _mentions_prefix)
        message(FATAL_ERROR "ninja -t ${_tool} never mentions the SDK prefix ${_prefix}; nothing to check")
    endif()
    set(_dump "${_dump_dir}/ninja-${_tool}.d")
    _spark_sdk_template_write_boundary_dump("${_dump}" "${_output}")
    list(APPEND _dumps "${_dump}")
endforeach()

_spark_sdk_template_check_boundary("${_build}" "${SPARK_SOURCE_ROOT};${SPARK_ENGINE_BUILD_DIR}" "${SPARK_TEST_ROOT}"
    "${_dumps}" _scanned _boundary_error)
if(NOT _boundary_error STREQUAL "")
    message(FATAL_ERROR "The SDK-only ${_module_name} build is not source-tree free: ${_boundary_error}")
endif()

# The SDK consumer must cross the real host/module ABI, not merely link.
# Reuse the production headless lifecycle parser: one initialized module,
# successful update/fixed/unload, no render callback, no guarded faults.
set(_engine "${_prefix}/bin/SparkEngine")
if(CMAKE_HOST_WIN32)
    string(APPEND _engine ".exe")
endif()
if(NOT EXISTS "${_engine}")
    message(FATAL_ERROR "Installed SDK runtime host is missing: ${_engine}")
endif()
set(_user_root "${SPARK_TEST_ROOT}/runtime-user")
set(_runtime_env "SPARK_RHI_BACKEND=null")
if(CMAKE_HOST_WIN32)
    list(APPEND _runtime_env "LOCALAPPDATA=${_user_root}/local" "APPDATA=${_user_root}/roaming")
else()
    foreach(_kind IN ITEMS DATA CONFIG CACHE STATE)
        string(TOLOWER "${_kind}" _directory)
        list(APPEND _runtime_env "XDG_${_kind}_HOME=${_user_root}/${_directory}")
    endforeach()
endif()
foreach(_directory IN ITEMS local roaming data config cache state)
    file(MAKE_DIRECTORY "${_user_root}/${_directory}")
endforeach()
set(SPARK_HEADLESS_NULLRHI_PARSER_ONLY ON)
include("${SPARK_SOURCE_ROOT}/cmake/RunSparkHeadlessNullRHILifecycle.cmake")
unset(SPARK_HEADLESS_NULLRHI_PARSER_ONLY)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=SPARK_ENGINE_DIR ${_runtime_env}
        "${_engine}" -headless -game "${_images}" -require-game
        -test-frames 30 -threads 2 -no-subprocess
    WORKING_DIRECTORY "${_source}"
    RESULT_VARIABLE _runtime_result
    OUTPUT_VARIABLE _runtime_stdout
    ERROR_VARIABLE _runtime_stderr
    TIMEOUT 120
    ENCODING UTF-8)
file(WRITE "${SPARK_TEST_ROOT}/runtime-stdout.log" "${_runtime_stdout}")
file(WRITE "${SPARK_TEST_ROOT}/runtime-stderr.log" "${_runtime_stderr}")
_spark_validate_headless_nullrhi_result("${_runtime_result}" "${_runtime_stdout}" "${_runtime_stderr}"
    _runtime_ok _runtime_reason)
if(NOT _runtime_ok)
    message(FATAL_ERROR "SDK template runtime lifecycle failed: ${_runtime_reason}\n"
        "${_runtime_stdout}\n${_runtime_stderr}")
endif()
file(SHA256 "${_images}" _runtime_image_sha256)
if(NOT _runtime_image_sha256 STREQUAL _image_sha256)
    message(FATAL_ERROR "SDK module image changed during runtime qualification")
endif()
message(STATUS "SPARK_SDK_TEMPLATE_RUNTIME module=${_module_name} sha256=${_image_sha256} lifecycle=passed")

message(STATUS
    "SPARK_SDK_TEMPLATE module=${_module_name} sdk_version=${_sdk_version} scanned=${_scanned} violations=0")
