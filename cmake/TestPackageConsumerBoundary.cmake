# Fixture tests for cmake/SparkPackageConsumerBoundary.cmake, the source-tree
# leak detector behind the opt-in PackageConsumer_LinuxInstalledSDK lane.
# The slow lane only ever sees a clean consumer, so this proves the detector
# itself rejects each leak shape and accepts the allowed consumer layout.
#
# Required: SPARK_TEST_ROOT (a scratch directory this script owns)

if(NOT DEFINED SPARK_TEST_ROOT OR SPARK_TEST_ROOT STREQUAL "")
    message(FATAL_ERROR "SPARK_TEST_ROOT is required")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/SparkPackageConsumerBoundary.cmake")

file(REMOVE_RECURSE "${SPARK_TEST_ROOT}")
# Hermetic layout mirroring a real run: the engine build and the test root both
# live inside the source checkout, and the install prefix inside the test root.
set(_src "${SPARK_TEST_ROOT}/checkout")
set(_engine_build "${_src}/build/linux-gcc-release")
set(_test_root "${_engine_build}/package-consumer-linux/Release")
set(_prefix "${_test_root}/prefix")
set(_consumer "${_test_root}/consumer-build")
file(MAKE_DIRECTORY "${_src}/SparkEngine/Source/Core" "${_src}/Tests/PackageSmoke")
file(WRITE "${_src}/SparkEngine/Source/Core/X.h" "")

# Writes one consumer build containing a single scanned file (its content is
# the concatenated trailing arguments), runs the detector, and checks that it
# reported exactly the expected path, or nothing when that is empty.
function(_expect _name _relative_file _expected_violation)
    string(CONCAT _content ${ARGN})
    file(REMOVE_RECURSE "${_consumer}")
    file(WRITE "${_consumer}/${_relative_file}" "${_content}")
    spark_package_consumer_boundary_violations(
        OUT_VAR _violations
        SCANNED_VAR _scanned
        CONSUMER_BUILD_DIR "${_consumer}"
        FORBIDDEN_ROOTS "${_src}" "${_engine_build}"
        ALLOWED_ROOTS "${_test_root}" "${_src}/Tests/PackageSmoke" "${_src}/GameModules/SparkGameFPS/Source")
    list(LENGTH _scanned _scanned_count)
    set(_ok TRUE)
    if(NOT _scanned_count EQUAL 1)
        set(_ok FALSE)
    elseif(_expected_violation STREQUAL "")
        if(_violations)
            set(_ok FALSE)
        endif()
    else()
        if(NOT _violations STREQUAL "${_consumer}/${_relative_file}: ${_expected_violation}")
            set(_ok FALSE)
        endif()
    endif()
    if(_ok)
        message(STATUS "PASS ${_name}")
    else()
        message(SEND_ERROR
            "FAIL ${_name}: scanned ${_scanned_count} file(s), expected violation "
            "'${_expected_violation}', got '${_violations}'")
    endif()
endfunction()

_expect("clean consumer depfile"

    "CMakeFiles/app.dir/main.cpp.o.d"

    ""
    "CMakeFiles/app.dir/main.cpp.o: ${_src}/Tests/PackageSmoke/main.cpp \\\n"
    " ${_prefix}/include/Spark/SparkSDK.h /usr/include/c++/13/string \\\n"
    " ${_src}/GameModules/SparkGameFPS/Source/Game/ProgressionSystem.h\n")

_expect("header from engine source"

    "CMakeFiles/app.dir/main.cpp.o.d"

    "${_src}/SparkEngine/Source/Core/EngineContext.h"
    "CMakeFiles/app.dir/main.cpp.o: ${_src}/Tests/PackageSmoke/main.cpp \\\n"
    " ${_src}/SparkEngine/Source/Core/EngineContext.h\n")

_expect("dot-dot escape from an allowed root"

    "CMakeFiles/app.dir/main.cpp.o.d"

    "${_src}/SparkEngine/Source/Core/Platform.h"
    "CMakeFiles/app.dir/main.cpp.o: "
    "${_src}/Tests/PackageSmoke/FPSProgression/../../../SparkEngine/Source/Core/Platform.h\n")

_expect("static library from engine build"

    "CMakeFiles/app.dir/link.txt"

    "${_engine_build}/lib/libSparkEngineLib.a"
    "/usr/bin/c++ -O3 CMakeFiles/app.dir/main.cpp.o -o app ${_prefix}/lib/libSDL2.so "
    "${_engine_build}/lib/libSparkEngineLib.a -ldl\n")

_expect("rpath into engine build"

    "CMakeFiles/app.dir/link.txt"

    "${_engine_build}/lib"
    "/usr/bin/c++ CMakeFiles/app.dir/main.cpp.o -o app -Wl,-rpath,${_engine_build}/lib "
    "${_prefix}/lib/libSparkEngineLib.a\n")

_expect("library search path into source"

    "CMakeFiles/app.dir/link.txt"

    "${_src}/ThirdParty/SDL2/lib"
    "/usr/bin/c++ CMakeFiles/app.dir/main.cpp.o -o app -L${_src}/ThirdParty/SDL2/lib -lSDL2\n")

_expect("include directory declared into source"

    "compile_commands.json"

    "${_src}/ThirdParty/OpenGL"
    "[{\"directory\": \"${_consumer}\", \"command\": \"/usr/bin/c++ -isystem ${_prefix}/include "
    "-I${_src}/ThirdParty/OpenGL -o x.o -c ${_src}/Tests/PackageSmoke/main.cpp\", "
    "\"file\": \"${_src}/Tests/PackageSmoke/main.cpp\"}]\n")

_expect("separate -isystem include into engine build"

    "compile_commands.json"

    "${_engine_build}/generated"
    "[{\"command\": \"/usr/bin/c++ -isystem ${_engine_build}/generated -c ${_src}/Tests/PackageSmoke/main.cpp\"}]\n")

_expect("make-escaped space keeps one path"

    "CMakeFiles/app.dir/main.cpp.o.d"

    "${_src}/Sibling Dir/leak.h"
    "CMakeFiles/app.dir/main.cpp.o: ${_src}/Sibling\\ Dir/leak.h\n")

_expect("prefix sibling is not an allowed root"

    "CMakeFiles/app.dir/main.cpp.o.d"

    "${_src}/Tests/PackageSmokeEvil/leak.h"
    "CMakeFiles/app.dir/main.cpp.o: ${_src}/Tests/PackageSmokeEvil/leak.h\n")

_expect("install prefix inside the build tree is allowed"

    "CMakeFiles/app.dir/link.txt"

    ""
    "/usr/bin/c++ app.o -o app -Wl,-rpath,${_prefix}/lib ${_prefix}/lib/libSparkEngineLib.a\n")

# A checkout reached through a symlink must not hide a leak through the real path.
file(CREATE_LINK "${_src}" "${SPARK_TEST_ROOT}/checkout-link" RESULT _link_result SYMBOLIC)
if(_link_result STREQUAL "0")
    _expect("leak spelled through the real path of a symlinked root"
        "CMakeFiles/app.dir/main.cpp.o.d"
        "${SPARK_TEST_ROOT}/checkout-link/SparkEngine/Source/Core/X.h"
        "CMakeFiles/app.dir/main.cpp.o: ${SPARK_TEST_ROOT}/checkout-link/SparkEngine/Source/Core/X.h\n")
else()
    message(SEND_ERROR "FAIL could not create the symlink fixture: ${_link_result}")
endif()
