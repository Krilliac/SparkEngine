cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")
project(SparkFPSGameplayEventsTests LANGUAGES CXX)

set(_engine_root "${CMAKE_CURRENT_LIST_DIR}/../..")
# Standalone use exercises an installed SDK. The engine's test build stages the
# same canonical headers directly, without inheriting private engine includes.
if(NOT DEFINED SPARK_ENGINE_INCLUDE_DIR)
    if(PROJECT_IS_TOP_LEVEL)
        find_package(SparkEngine REQUIRED)
    else()
        set(SPARK_ENGINE_INCLUDE_DIR "${_engine_root}/SparkSDK/Include")
    endif()
endif()
set(_public_sdk "${CMAKE_CURRENT_BINARY_DIR}/public-sdk")
file(REMOVE_RECURSE "${_public_sdk}")
file(MAKE_DIRECTORY "${_public_sdk}/Spark")
foreach(_header IN ITEMS EventBus.h EventSystem.h EventTypes.h)
    if(NOT EXISTS "${SPARK_ENGINE_INCLUDE_DIR}/Spark/${_header}")
        message(FATAL_ERROR "Missing canonical SDK event header: Spark/${_header}")
    endif()
    configure_file("${SPARK_ENGINE_INCLUDE_DIR}/Spark/${_header}"
        "${_public_sdk}/Spark/${_header}" COPYONLY)
endforeach()

find_package(Threads REQUIRED)
function(spark_fps_event_consumer target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE
        "${_engine_root}/GameModules/SparkGameFPS/Source"
        "${_public_sdk}")
    target_link_libraries(${target} PRIVATE Threads::Threads)
endfunction()

add_executable(spark_fps_gameplay_events_tests
    main.cpp
    "${_engine_root}/GameModules/SparkGameFPS/Source/Game/FPSGameplayEvents.cpp"
)
spark_fps_event_consumer(spark_fps_gameplay_events_tests)

add_executable(spark_public_event_contract_tests event_contract.cpp)
spark_fps_event_consumer(spark_public_event_contract_tests)

enable_testing()
add_test(NAME SparkFPSGameplayEvents COMMAND spark_fps_gameplay_events_tests)
add_test(NAME SparkPublicEventContract COMMAND spark_public_event_contract_tests)
set_tests_properties(SparkFPSGameplayEvents SparkPublicEventContract PROPERTIES
    TIMEOUT 30 LABELS "sdk;module;fps;integration")

if(WIN32 OR UNIX)
    add_library(spark_fps_event_module MODULE
        event_module.cpp
        "${_engine_root}/GameModules/SparkGameFPS/Source/Game/FPSGameplayEvents.cpp")
    spark_fps_event_consumer(spark_fps_event_module)
    set_target_properties(spark_fps_event_module PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    # GCC's GNU-unique statics otherwise prevent dlclose from unmapping an image.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(spark_fps_event_module PRIVATE -fno-gnu-unique)
    endif()
    add_executable(spark_fps_event_module_host event_module_host.cpp)
    spark_fps_event_consumer(spark_fps_event_module_host)
    target_link_libraries(spark_fps_event_module_host PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(spark_fps_event_module_host spark_fps_event_module)
    add_test(NAME SparkFPSPublicEventsModuleLifecycle
        COMMAND spark_fps_event_module_host "$<TARGET_FILE:spark_fps_event_module>")
    set_tests_properties(SparkFPSPublicEventsModuleLifecycle PROPERTIES
        TIMEOUT 30 LABELS "sdk;module;fps;integration;lifecycle")
endif()
