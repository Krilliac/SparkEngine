# =============================================================================
# SparkCPack.cmake — CPack packaging configuration for SparkEngine
# =============================================================================
#
# Generates distributable packages (ZIP, TGZ, NSIS on Windows).
# Include this file from the root CMakeLists.txt to enable `cpack` commands.

# Package metadata
set(CPACK_PACKAGE_NAME "SparkEngine")
set(CPACK_PACKAGE_VENDOR "Spark Engine Team")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "C++23 3D Game Engine")
set(CPACK_PACKAGE_VERSION_MAJOR 0)
set(CPACK_PACKAGE_VERSION_MINOR 1)
set(CPACK_PACKAGE_VERSION_PATCH 0)
set(CPACK_PACKAGE_CONTACT "https://github.com/SparkEngine")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/SparkEngine")

if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
endif()

# Generator configuration — ZIP and TGZ everywhere, NSIS on Windows
set(CPACK_GENERATOR "ZIP;TGZ")
if(WIN32)
    list(APPEND CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "SparkEngine SDK")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_PACKAGE_NAME "SparkEngine ${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
    set(CPACK_NSIS_HELP_LINK "https://github.com/SparkEngine")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()

# Component-based install
set(CPACK_COMPONENTS_ALL Runtime Development Headers Shaders Templates)

set(CPACK_COMPONENT_RUNTIME_DISPLAY_NAME "Engine Runtime")
set(CPACK_COMPONENT_RUNTIME_DESCRIPTION "Core engine binaries and shared libraries")
set(CPACK_COMPONENT_RUNTIME_REQUIRED ON)

set(CPACK_COMPONENT_DEVELOPMENT_DISPLAY_NAME "Development Libraries")
set(CPACK_COMPONENT_DEVELOPMENT_DESCRIPTION "Static libraries and import libraries for linking")
set(CPACK_COMPONENT_DEVELOPMENT_DEPENDS Runtime)

set(CPACK_COMPONENT_HEADERS_DISPLAY_NAME "SDK Headers")
set(CPACK_COMPONENT_HEADERS_DESCRIPTION "Public header files for engine development")
set(CPACK_COMPONENT_HEADERS_DEPENDS Development)

set(CPACK_COMPONENT_SHADERS_DISPLAY_NAME "Shader Files")
set(CPACK_COMPONENT_SHADERS_DESCRIPTION "Built-in shader source files")

set(CPACK_COMPONENT_TEMPLATES_DISPLAY_NAME "Project Templates")
set(CPACK_COMPONENT_TEMPLATES_DESCRIPTION "Starter project templates for new game modules")

# Component grouping
set(CPACK_COMPONENT_RUNTIME_GROUP "Core")
set(CPACK_COMPONENT_DEVELOPMENT_GROUP "SDK")
set(CPACK_COMPONENT_HEADERS_GROUP "SDK")
set(CPACK_COMPONENT_SHADERS_GROUP "Content")
set(CPACK_COMPONENT_TEMPLATES_GROUP "Content")

# Source package configuration
set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
set(CPACK_SOURCE_IGNORE_PATTERN
    "/build/"
    "/.git/"
    "/.claude/"
    "/.vs/"
    "/.vscode/"
    "/out/"
    "\\\\.user$"
    "\\\\.suo$"
)

# Package naming
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}-${CMAKE_SYSTEM_NAME}")

include(CPack)
