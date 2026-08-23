if(NOT DEFINED MODULE_PATH OR NOT EXISTS "${MODULE_PATH}")
    message(FATAL_ERROR "WriteSparkModuleABI: MODULE_PATH is missing or does not exist: ${MODULE_PATH}")
endif()
if(NOT DEFINED SIDECAR_PATH OR SIDECAR_PATH STREQUAL "")
    message(FATAL_ERROR "WriteSparkModuleABI: SIDECAR_PATH is required")
endif()

file(SHA256 "${MODULE_PATH}" MODULE_SHA256)
file(WRITE "${SIDECAR_PATH}"
    "format=${DESCRIPTOR_VERSION}\n"
    "struct_size=${DESCRIPTOR_SIZE}\n"
    "magic=1263685715\n"
    "sdk_version=${SDK_VERSION}\n"
    "runtime_abi_version=${RUNTIME_ABI_VERSION}\n"
    "compiler_family=${COMPILER_FAMILY}\n"
    "compiler_abi_version=${COMPILER_ABI_VERSION}\n"
    "cxx_language_level=${CXX_LANGUAGE_LEVEL}\n"
    "runtime_library=${RUNTIME_LIBRARY}\n"
    "iterator_debug_level=${ITERATOR_DEBUG_LEVEL}\n"
    "pointer_size=${CMAKE_SIZEOF_VOID_P}\n"
    "binary_sha256=${MODULE_SHA256}\n")
