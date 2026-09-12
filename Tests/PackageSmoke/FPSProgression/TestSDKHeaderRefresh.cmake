# Reconfigure the actual consumer against an incomplete current installation.
# Its previous staged headers must never turn the second build into a pass.
function(require_success stage)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "${stage} failed (${result}):\n${output}\n${error}")
    endif()
endfunction()

file(REMOVE_RECURSE "${FIXTURE}")
file(MAKE_DIRECTORY "${FIXTURE}/include")
file(COPY "${INSTALLED_INCLUDE_DIR}/Spark" DESTINATION "${FIXTURE}/include")
set(configure "${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE}" -B "${FIXTURE}/build"
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
set(build "${CMAKE_COMMAND}" --build "${FIXTURE}/build"
    --target spark_fps_weapons_consumer --parallel 2)
if(CONSUMER_CONFIG)
    list(APPEND build --config "${CONSUMER_CONFIG}")
endif()

require_success("Complete SDK configure" ${configure})
require_success("Complete SDK consumer build" ${build})
file(REMOVE "${FIXTURE}/include/Spark/WeaponTypes.h")
require_success("Incomplete SDK reconfigure" ${configure})
# Force compilation so the test also works with generators that do not rebuild
# an existing object merely because one of its header dependencies vanished.
execute_process(COMMAND ${build} --clean-first
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
if("${result}" STREQUAL "0")
    message(FATAL_ERROR "Consumer accepted an SDK missing its current WeaponTypes.h after reconfiguration")
endif()
if(NOT "${output}\n${error}" MATCHES "WeaponTypes[.]h")
    message(FATAL_ERROR "Consumer failed for an unrelated reason (${result}):\n${output}\n${error}")
endif()

# The package consumer must also reject an installation missing the canonical
# SDK umbrella header; accepting only a surviving leaf header would make the
# public SDK contract depend on stale or partial package contents.
file(COPY "${INSTALLED_INCLUDE_DIR}/Spark/SparkSDK.h"
    DESTINATION "${FIXTURE}/include/Spark")
file(REMOVE "${FIXTURE}/include/Spark/SparkSDK.h")
execute_process(COMMAND ${configure}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 120)
if("${result}" STREQUAL "0")
    message(FATAL_ERROR
        "Consumer accepted an SDK missing its canonical SparkSDK.h after reconfiguration")
endif()
if(NOT "${output}\n${error}" MATCHES "SparkSDK[.]h")
    message(FATAL_ERROR
        "Consumer failed for an unrelated reason while SparkSDK.h was missing (${result}):\n"
        "${output}\n${error}")
endif()
file(REMOVE_RECURSE "${FIXTURE}")
