# Each scenario gets a fresh process because the production root is cached.
# Copy the executable so fixtures never affect the real installed/build tree.
file(REMOVE_RECURSE "${FIXTURE}")
file(MAKE_DIRECTORY "${FIXTURE}/bin" "${FIXTURE}/work/deep")
file(COPY "${CONSUMER}" DESTINATION "${FIXTURE}/bin")
get_filename_component(_executable "${CONSUMER}" NAME)
if(SCENARIO STREQUAL "executable")
    file(MAKE_DIRECTORY "${FIXTURE}/bin/Assets/Models" "${FIXTURE}/Assets/Models"
        "${FIXTURE}/work/deep/Assets/Models")
elseif(SCENARIO STREQUAL "parent")
    file(MAKE_DIRECTORY "${FIXTURE}/Assets/Models" "${FIXTURE}/work/deep/Assets/Models")
elseif(SCENARIO STREQUAL "working")
    file(MAKE_DIRECTORY "${FIXTURE}/work/deep/Assets/Models")
elseif(NOT SCENARIO STREQUAL "fallback")
    message(FATAL_ERROR "Unknown asset scenario: ${SCENARIO}")
endif()
execute_process(COMMAND "${FIXTURE}/bin/${_executable}" "${SCENARIO}"
    WORKING_DIRECTORY "${FIXTURE}/work/deep"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr TIMEOUT 30)
file(REMOVE_RECURSE "${FIXTURE}")
if(NOT "${_result}" STREQUAL "0")
    message(FATAL_ERROR "FPS asset consumer ${SCENARIO} failed (${_result}):\n${_stdout}\n${_stderr}")
endif()
