cmake_minimum_required(VERSION 3.25)
project(EmptyProject LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pre-flight validation for incomplete installations
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

# Find SparkEngine (install SparkEngine first, or set SparkEngine_DIR)
find_package(SparkEngine REQUIRED)

# Collect source files
file(GLOB_RECURSE GAME_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)

# Create the game module DLL
spark_add_game_module(EmptyProject ${GAME_SOURCES})

# Include the project's own source directory
target_include_directories(EmptyProject PRIVATE "Source")

# Post-build: create asset directories
add_custom_command(TARGET EmptyProject POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:EmptyProject>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:EmptyProject>/Assets"
    COMMENT "Creating asset directories"
)

# Copy assets to output
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets")
    add_custom_command(TARGET EmptyProject POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets"
            "$<TARGET_FILE_DIR:EmptyProject>/Assets"
        COMMENT "Copying game assets"
    )
endif()

message(STATUS "EmptyProject configured as SparkEngine game module")
