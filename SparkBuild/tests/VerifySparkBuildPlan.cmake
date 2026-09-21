foreach(_spark_required IN ITEMS SPARK_PLAN_EXECUTABLE SPARK_PLAN_SOURCE SPARK_PLAN_BUILD)
    if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
        message(FATAL_ERROR "${_spark_required} is required")
    endif()
endforeach()

if(EXISTS "${SPARK_PLAN_BUILD}")
    file(REMOVE_RECURSE "${SPARK_PLAN_BUILD}")
endif()

execute_process(
    COMMAND "${SPARK_PLAN_EXECUTABLE}"
        --plan
        --engine-path "${SPARK_PLAN_SOURCE}"
        --build-path "${SPARK_PLAN_BUILD}"
        --generator Ninja
        --build-type Release
    RESULT_VARIABLE _spark_plan_result
    OUTPUT_VARIABLE _spark_plan_output
    ERROR_VARIABLE _spark_plan_error
    TIMEOUT 5)
if(NOT _spark_plan_result EQUAL 0)
    message(FATAL_ERROR
        "SparkBuild --plan failed (${_spark_plan_result}):\n"
        "stdout: ${_spark_plan_output}\n"
        "stderr: ${_spark_plan_error}")
endif()

foreach(_spark_marker IN ITEMS
        "SparkBuild plan v"
        "Generator=Ninja"
        "BuildType=Release"
        "ConfigureCommand="
        "BuildCommand=cmake --build")
    string(FIND "${_spark_plan_output}" "${_spark_marker}" _spark_marker_offset)
    if(_spark_marker_offset LESS 0)
        message(FATAL_ERROR "SparkBuild --plan output is missing '${_spark_marker}': ${_spark_plan_output}")
    endif()
endforeach()

string(FIND "${_spark_plan_output}" "${SPARK_PLAN_SOURCE}" _spark_source_offset)
if(_spark_source_offset LESS 0)
    message(FATAL_ERROR "SparkBuild --plan output omitted the owned engine path: ${_spark_plan_output}")
endif()
string(FIND "${_spark_plan_output}" "${SPARK_PLAN_BUILD}" _spark_build_offset)
if(_spark_build_offset LESS 0)
    message(FATAL_ERROR "SparkBuild --plan output omitted the fresh build path: ${_spark_plan_output}")
endif()

if(EXISTS "${SPARK_PLAN_BUILD}")
    message(FATAL_ERROR "SparkBuild --plan modified the build tree: ${SPARK_PLAN_BUILD}")
endif()

execute_process(
    COMMAND "${SPARK_PLAN_EXECUTABLE}" --plan --engine-path "${SPARK_PLAN_SOURCE}" --unknown
    RESULT_VARIABLE _spark_invalid_result
    OUTPUT_VARIABLE _spark_invalid_output
    ERROR_VARIABLE _spark_invalid_error
    TIMEOUT 5)
if(_spark_invalid_result EQUAL 0)
    message(FATAL_ERROR "SparkBuild --plan accepted an unknown option")
endif()
string(FIND "${_spark_invalid_error}" "Unknown --plan option" _spark_invalid_marker)
if(_spark_invalid_marker LESS 0)
    message(FATAL_ERROR "SparkBuild --plan did not explain the rejected option: ${_spark_invalid_error}")
endif()

function(assert_plan_rejected _expected_error)
    execute_process(
        COMMAND "${SPARK_PLAN_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE _spark_rejected_result
        OUTPUT_VARIABLE _spark_rejected_output
        ERROR_VARIABLE _spark_rejected_error
        TIMEOUT 5)
    if(_spark_rejected_result EQUAL 0)
        message(FATAL_ERROR "SparkBuild --plan unexpectedly accepted invalid input: ${ARGN}")
    endif()
    string(FIND "${_spark_rejected_error}" "${_expected_error}" _spark_rejected_marker)
    if(_spark_rejected_marker LESS 0)
        message(FATAL_ERROR
            "SparkBuild --plan rejection omitted '${_expected_error}': ${_spark_rejected_error}")
    endif()
endfunction()

assert_plan_rejected("Missing value for --engine-path" --plan --engine-path)
assert_plan_rejected("Unsupported generator: NotAGenerator"
    --plan --engine-path "${SPARK_PLAN_SOURCE}" --build-path "${SPARK_PLAN_BUILD}"
    --generator NotAGenerator)
assert_plan_rejected("Unsupported build type: NotABuildType"
    --plan --engine-path "${SPARK_PLAN_SOURCE}" --build-path "${SPARK_PLAN_BUILD}"
    --build-type NotABuildType)
assert_plan_rejected("engine path must contain CMakeLists.txt"
    --plan --engine-path "${SPARK_PLAN_SOURCE}/does-not-exist"
    --build-path "${SPARK_PLAN_BUILD}")

if(WIN32)
    set(_spark_unsafe_build_path "${SPARK_PLAN_BUILD}%unsafe")
else()
    set(_spark_unsafe_build_path "${SPARK_PLAN_BUILD}$unsafe")
endif()
assert_plan_rejected("Unsafe character in SparkBuild BuildPath"
    --plan --engine-path "${SPARK_PLAN_SOURCE}" --build-path "${_spark_unsafe_build_path}")
