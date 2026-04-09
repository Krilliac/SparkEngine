# =============================================================================
# SparkCPack.cmake (compatibility shim)
# =============================================================================
#
# CPack metadata now lives in the top-level CMakeLists.txt so package
# configuration is visible next to install/component definitions.
# This file is intentionally kept as a lightweight include target for
# downstream scripts that still include(cmake/SparkCPack.cmake).

if(NOT DEFINED CPACK_PACKAGE_NAME)
    message(FATAL_ERROR
        "SparkCPack.cmake is a compatibility shim. Include the project root "
        "CMakeLists.txt (which calls include(CPack)) to configure packaging.")
endif()
