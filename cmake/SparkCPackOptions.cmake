# CPack evaluates this file once per requested build configuration. Keep
# multi-config packages distinct so Debug and Release artifacts cannot overwrite
# one another when a release workflow collects them into a flat directory.
if(CPACK_BUILD_CONFIG AND
   NOT CPACK_PACKAGE_FILE_NAME MATCHES "-${CPACK_BUILD_CONFIG}$")
    string(APPEND CPACK_PACKAGE_FILE_NAME "-${CPACK_BUILD_CONFIG}")
endif()
