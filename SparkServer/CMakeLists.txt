cmake_minimum_required(VERSION 3.25)

if(NOT TARGET SparkEngineLib)
    message(FATAL_ERROR "SparkServer must be added after SparkEngineLib")
endif()

if(NOT ENABLE_NETWORKING)
    message(STATUS "SparkServer: skipped because ENABLE_NETWORKING is OFF")
    return()
endif()

add_library(SparkServerCore STATIC
    src/ServerApplication.cpp
    src/ServerApplication.h
)

target_compile_features(SparkServerCore PUBLIC cxx_std_23)
target_include_directories(SparkServerCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_link_libraries(SparkServerCore PUBLIC SparkEngineLib)
else()
    # SparkGame modules resolve every engine call from their host process. A
    # normal static-library link discards archive members that SparkServer does
    # not call directly, even though a loaded module may need them later.
    # WHOLE_ARCHIVE maps to --whole-archive on ELF and -force_load on Apple so
    # the complete module-facing runtime is present for ENABLE_EXPORTS below.
    target_link_libraries(SparkServerCore PUBLIC
        "$<LINK_LIBRARY:WHOLE_ARCHIVE,SparkEngineLib>")
endif()
if(NOT TARGET SparkGatewayCore)
    message(FATAL_ERROR "SparkServer requires SparkGatewayCore; add SparkGateway before SparkServer")
endif()
target_link_libraries(SparkServerCore PUBLIC SparkGatewayCore)

add_executable(SparkServer src/main.cpp)
target_compile_features(SparkServer PRIVATE cxx_std_23)
target_link_libraries(SparkServer PRIVATE SparkServerCore)

# POSIX game modules intentionally leave engine symbols unresolved until
# dlopen(). SparkServer is a module host just like SparkEngine, so its dynamic
# symbol table must expose the SparkEngineLib symbols that those modules bind.
# Without ENABLE_EXPORTS, the dedicated host builds successfully but rejects
# libSparkGame.so/libSparkGame.dylib immediately at runtime.
if(NOT WIN32)
    set_target_properties(SparkServer PROPERTIES ENABLE_EXPORTS ON)
endif()

if(MSVC)
    target_compile_options(SparkServerCore PRIVATE /W4 /permissive- /Zc:preprocessor)
    target_compile_options(SparkServer PRIVATE /W4 /permissive- /Zc:preprocessor)
else()
    target_compile_options(SparkServerCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(SparkServer PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(SparkServer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

install(TARGETS SparkServer RUNTIME DESTINATION bin COMPONENT runtime)
install(FILES config/server.example.ini DESTINATION share/SparkEngine/examples COMPONENT runtime)

if(BUILD_TESTS)
    enable_testing()
    add_test(NAME SparkServerHelp COMMAND $<TARGET_FILE:SparkServer> --help)
endif()
