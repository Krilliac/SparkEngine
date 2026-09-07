# Build the package consumer from a tree that deliberately omits SparkEngine/.
# The copied module sources must compile using only the staged public SDK.
function(require_success stage)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "${stage} failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

foreach(required IN ITEMS CONSUMER_SOURCE FPS_SOURCE FIXTURE INSTALLED_INCLUDE_DIR
        CONSUMER_GENERATOR CONSUMER_COMPILER CONSUMER_CTEST)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required for the isolated consumer boundary test")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
set(project_root "${FIXTURE}/project")
set(consumer_copy "${project_root}/Tests/PackageSmoke/FPSProgression")
file(MAKE_DIRECTORY "${project_root}/Tests/PackageSmoke")
file(COPY "${CONSUMER_SOURCE}" DESTINATION "${project_root}/Tests/PackageSmoke")
file(MAKE_DIRECTORY "${project_root}/GameModules/SparkGameFPS")
file(COPY "${FPS_SOURCE}" DESTINATION "${project_root}/GameModules/SparkGameFPS")
file(MAKE_DIRECTORY "${FIXTURE}/include")
file(COPY "${INSTALLED_INCLUDE_DIR}/Spark" DESTINATION "${FIXTURE}/include")

set(configure "${CMAKE_COMMAND}" -S "${consumer_copy}" -B "${FIXTURE}/build"
    -G "${CONSUMER_GENERATOR}" "-DSPARK_ENGINE_INCLUDE_DIR=${FIXTURE}/include"
    "-DCMAKE_CXX_COMPILER=${CONSUMER_COMPILER}")
if(CONSUMER_PLATFORM)
    list(APPEND configure -A "${CONSUMER_PLATFORM}")
endif()
if(CONSUMER_TOOLSET)
    list(APPEND configure -T "${CONSUMER_TOOLSET}")
endif()
if(CONSUMER_MAKE_PROGRAM)
    list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${CONSUMER_MAKE_PROGRAM}")
endif()
if(CONSUMER_TOOLCHAIN)
    list(APPEND configure "-DCMAKE_TOOLCHAIN_FILE=${CONSUMER_TOOLCHAIN}")
endif()
set(build "${CMAKE_COMMAND}" --build "${FIXTURE}/build" --parallel 2)
if(CONSUMER_CONFIG)
    list(APPEND build --config "${CONSUMER_CONFIG}")
endif()
set(test "${CONSUMER_CTEST}" --test-dir "${FIXTURE}/build" --output-on-failure --no-tests=error
    -E "^SparkFPSProgressionConsumerSourceBoundary$")
if(CONSUMER_CONFIG)
    list(APPEND test -C "${CONSUMER_CONFIG}")
endif()

require_success("Isolated consumer configure" ${configure})
require_success("Isolated consumer build" ${build})
require_success("Isolated consumer tests" ${test})
file(REMOVE_RECURSE "${FIXTURE}")
