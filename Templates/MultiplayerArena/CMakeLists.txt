cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

project(MultiplayerArena LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SparkEngine CONFIG REQUIRED)

set(GAME_SOURCES
    Source/GameModule.cpp
    Source/GameModule.h
)

spark_add_game_module(MultiplayerArena ${GAME_SOURCES})
target_include_directories(MultiplayerArena PRIVATE "Source")

add_custom_command(TARGET MultiplayerArena POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MultiplayerArena>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:MultiplayerArena>/Assets"
    COMMENT "Creating asset directories"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Assets")
    add_custom_command(TARGET MultiplayerArena POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/Assets"
            "$<TARGET_FILE_DIR:MultiplayerArena>/Assets"
        COMMENT "Copying game assets"
    )
endif()
