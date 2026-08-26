cmake_minimum_required(VERSION 3.25)

if(NOT TARGET SparkEngineLib)
    message(FATAL_ERROR "SparkGateway must be added after SparkEngineLib")
endif()

if(NOT ENABLE_NETWORKING)
    message(STATUS "SparkGateway: skipped because ENABLE_NETWORKING is OFF")
    return()
endif()

add_library(SparkGatewaySecurity STATIC
    src/GatewaySecurity.cpp
    src/GatewaySecurity.h
)
target_compile_features(SparkGatewaySecurity PUBLIC cxx_std_23)
target_include_directories(SparkGatewaySecurity PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(SparkGatewaySecurity PUBLIC SparkEngineLib)
if(WIN32)
    target_link_libraries(SparkGatewaySecurity PRIVATE advapi32)
endif()

add_library(SparkGatewayCore STATIC
    src/GatewayAreaControl.cpp
    src/GatewayAreaControl.h
    src/GatewayApplication.cpp
    src/GatewayApplication.h
    src/GatewayCoordinator.cpp
    src/GatewayCoordinator.h
)
target_compile_features(SparkGatewayCore PUBLIC cxx_std_23)
target_include_directories(SparkGatewayCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(SparkGatewayCore PUBLIC SparkEngineLib SparkGatewaySecurity)

add_executable(SparkGateway src/main.cpp)
target_compile_features(SparkGateway PRIVATE cxx_std_23)
target_link_libraries(SparkGateway PRIVATE SparkGatewayCore)

if(MSVC)
    target_compile_options(SparkGatewayCore PRIVATE /W4 /permissive- /Zc:preprocessor)
    target_compile_options(SparkGateway PRIVATE /W4 /permissive- /Zc:preprocessor)
else()
    target_compile_options(SparkGatewayCore PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(SparkGateway PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(SparkGateway PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

install(TARGETS SparkGateway RUNTIME DESTINATION bin COMPONENT runtime)
install(FILES config/gateway.example.ini DESTINATION share/SparkEngine/examples COMPONENT runtime)

if(BUILD_TESTS)
    enable_testing()
    add_test(NAME SparkGatewayHelp COMMAND $<TARGET_FILE:SparkGateway> --help)
endif()
