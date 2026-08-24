cmake_minimum_required(VERSION 3.25)
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
endif()
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

project(Blank3D LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SparkEngine CONFIG REQUIRED)

set(GAME_SOURCES
    Source/GameModule.cpp
    Source/GameModule.h
)

spark_add_game_module(Blank3D ${GAME_SOURCES})
target_include_directories(Blank3D PRIVATE "Source")
