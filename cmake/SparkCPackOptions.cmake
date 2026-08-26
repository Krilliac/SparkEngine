# CPack evaluates this file once per requested build configuration. Keep
# multi-config packages distinct so Debug and Release artifacts cannot overwrite
# one another when a release workflow collects them into a flat directory.
if(CPACK_BUILD_CONFIG AND
   NOT CPACK_PACKAGE_FILE_NAME MATCHES "-${CPACK_BUILD_CONFIG}$")
    string(APPEND CPACK_PACKAGE_FILE_NAME "-${CPACK_BUILD_CONFIG}")
endif()

# NSIS represents an input file's mapped length with a signed 32-bit integer.
# The full Release SDK contains the monolithic SparkEngineLib.lib, which can
# exceed 2 GiB when MSVC whole-program optimization is enabled. Keep the full
# SDK and project templates in the ZIP distribution, while the native Windows
# installers carry the runnable engine, tools, and sample modules.
if(CPACK_GENERATOR STREQUAL "NSIS" OR CPACK_GENERATOR STREQUAL "WIX")
    set(CPACK_COMPONENTS_ALL runtime tools samples)
    if(NOT CPACK_PACKAGE_FILE_NAME MATCHES "-Runtime$")
        string(APPEND CPACK_PACKAGE_FILE_NAME "-Runtime")
    endif()
endif()
