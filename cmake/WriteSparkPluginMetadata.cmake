if(NOT DEFINED PLUGIN_PATH OR NOT EXISTS "${PLUGIN_PATH}")
    message(FATAL_ERROR "WriteSparkPluginMetadata: PLUGIN_PATH is missing or does not exist")
endif()
if(NOT DEFINED METADATA_PATH OR METADATA_PATH STREQUAL "")
    message(FATAL_ERROR "WriteSparkPluginMetadata: METADATA_PATH is required")
endif()
foreach(_required IN ITEMS PLUGIN_ID PLUGIN_VERSION PLUGIN_TYPE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "WriteSparkPluginMetadata: ${_required} is required")
    endif()
endforeach()

function(_spark_json_escape INPUT OUTPUT)
    set(_value "${INPUT}")
    string(REPLACE "\\" "\\\\" _value "${_value}")
    string(REPLACE "\"" "\\\"" _value "${_value}")
    string(REPLACE "\n" "\\n" _value "${_value}")
    string(REPLACE "\r" "\\r" _value "${_value}")
    set(${OUTPUT} "${_value}" PARENT_SCOPE)
endfunction()

file(SHA256 "${PLUGIN_PATH}" _sha256)
get_filename_component(_binary_name "${PLUGIN_PATH}" NAME)
_spark_json_escape("${PLUGIN_ID}" _id)
_spark_json_escape("${PLUGIN_VERSION}" _version)
_spark_json_escape("${PLUGIN_TYPE}" _type)
_spark_json_escape("${_binary_name}" _binary)

# Stable key order and LF endings keep packages byte-for-byte reproducible.
file(WRITE "${METADATA_PATH}"
    "{\n"
    "  \"schema\": 1,\n"
    "  \"id\": \"${_id}\",\n"
    "  \"version\": \"${_version}\",\n"
    "  \"type\": \"${_type}\",\n"
    "  \"abi_major\": 1,\n"
    "  \"abi_minor\": 0,\n"
    "  \"entry_point\": \"SparkGetPluginDescriptor\",\n"
    "  \"binary\": \"${_binary}\",\n"
    "  \"sha256\": \"${_sha256}\"\n"
    "}\n")
