cmake_minimum_required(VERSION 3.25)
project(FPSStarter LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../cmake")
include(SparkEnginePreflight)

find_package(SparkEngine REQUIRED)

file(GLOB_RECURSE GAME_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)

spark_add_game_module(FPSStarter ${GAME_SOURCES})
target_include_directories(FPSStarter PRIVATE "Source")

add_custom_command(TARGET FPSStarter POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FPSStarter>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:FPSStarter>/Assets"
    COMMENT "Creating asset directories"
)

if(EXISTS "${CMAKE_SOURCE_DIR}/Assets")
    add_custom_command(TARGET FPSStarter POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets"
            "$<TARGET_FILE_DIR:FPSStarter>/Assets"
        COMMENT "Copying game assets"
    )
endif()

message(STATUS "FPSStarter FPS project configured")
