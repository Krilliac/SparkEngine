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
if(CPACK_GENERATOR STREQUAL "ZIP" OR CPACK_GENERATOR STREQUAL "TGZ")
    # Component archives otherwise flatten bin/, lib/, and share/ at the ZIP
    # root. The extracted-package gate requires one enclosing package root so
    # sibling files cannot be confused with package members. Native MSI/NSIS
    # layouts are not changed by this archive-only setting.
    set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY ON)
endif()

if(CPACK_GENERATOR STREQUAL "NSIS" OR CPACK_GENERATOR STREQUAL "WIX")
    # redist: the app-local Visual C++ runtime the binaries import (ENG-220).
    set(CPACK_COMPONENTS_ALL runtime redist tools samples)
    if(NOT CPACK_PACKAGE_FILE_NAME MATCHES "-Runtime$")
        string(APPEND CPACK_PACKAGE_FILE_NAME "-Runtime")
    endif()
endif()

if(CPACK_GENERATOR STREQUAL "NSIS")
    if(NOT DEFINED CPACK_PACKAGE_DIRECTORY OR CPACK_PACKAGE_DIRECTORY STREQUAL "")
        message(FATAL_ERROR "NSIS version metadata requires CPACK_PACKAGE_DIRECTORY")
    endif()
    # CPack's default NSIS template does not create a VERSIONINFO resource.
    # The installer is a shipped PE product and must carry the same fixed and
    # string versions as the runtime binaries, not just a versioned filename.
    # Component generation overwrites the internal CPACK_NSIS_DEFINES variable.
    # Derive a template from this CMake installation and preserve its component
    # placeholder. CPack searches CMAKE_MODULE_PATH for custom templates.
    file(READ "${CMAKE_ROOT}/Modules/Internal/CPack/NSIS.template.in" _spark_nsis_template)
    string(REGEX MATCHALL "@CPACK_NSIS_DEFINES@" _spark_nsis_anchors "${_spark_nsis_template}")
    list(LENGTH _spark_nsis_anchors _spark_nsis_anchor_count)
    if(NOT _spark_nsis_anchor_count EQUAL 1)
        message(FATAL_ERROR "The NSIS template must have exactly one version-metadata insertion anchor")
    endif()
    set(_spark_nsis_version_metadata [=[
VIProductVersion "@CPACK_PACKAGE_VERSION@.0"
VIFileVersion "@CPACK_PACKAGE_VERSION@.0"
VIAddVersionKey /LANG=1033 "ProductName" "SparkEngine"
VIAddVersionKey /LANG=1033 "CompanyName" "Spark Engine Team"
VIAddVersionKey /LANG=1033 "FileDescription" "SparkEngine Runtime Installer"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (C) SparkEngine contributors"
VIAddVersionKey /LANG=1033 "FileVersion" "@CPACK_PACKAGE_VERSION@.0"
VIAddVersionKey /LANG=1033 "ProductVersion" "@CPACK_PACKAGE_VERSION@.0"
]=])
    string(REPLACE "@CPACK_NSIS_DEFINES@"
        "@CPACK_NSIS_DEFINES@\n${_spark_nsis_version_metadata}"
        _spark_nsis_template "${_spark_nsis_template}")
    set(_spark_nsis_template_dir "${CPACK_PACKAGE_DIRECTORY}/spark-nsis-template")
    file(MAKE_DIRECTORY "${_spark_nsis_template_dir}")
    file(WRITE "${_spark_nsis_template_dir}/NSIS.template.in" "${_spark_nsis_template}")
    list(PREPEND CMAKE_MODULE_PATH "${_spark_nsis_template_dir}")
    unset(_spark_nsis_template)
    unset(_spark_nsis_anchors)
    unset(_spark_nsis_anchor_count)
    unset(_spark_nsis_version_metadata)
    unset(_spark_nsis_template_dir)
endif()

if(CPACK_GENERATOR STREQUAL "WIX")
    # Qualification and installation are per-user and must never request UAC
    # or default to Program Files. CMake added this WiX switch in 3.29; older
    # versions can still build the engine and ZIP, but cannot create this MSI.
    # The closure driver also includes this file in cmake -P mode only to
    # obtain the component set; that operation does not generate an MSI.
    get_property(_spark_packaging_role GLOBAL PROPERTY CMAKE_ROLE)
    if(_spark_packaging_role STREQUAL "CPACK" AND CMAKE_VERSION VERSION_LESS "3.29")
        message(FATAL_ERROR "Per-user Windows MSI packaging requires CMake 3.29 or newer")
    endif()
    unset(_spark_packaging_role)
    set(CPACK_WIX_INSTALL_SCOPE "perUser")
    # Project-owned identity for the Windows Runtime installer family. Keep it
    # across versions so a separately built predecessor can be upgraded. Leave
    # CPACK_WIX_PRODUCT_GUID unset: each MSI needs its own ProductCode.
    set(CPACK_WIX_UPGRADE_GUID "74E90DE5-B7E2-442C-9696-8CA63782F55A")
endif()
