#[=============================================================================[
  SparkGameModule.cmake - Helper function for creating game module DLLs

  Usage in a standalone game project:

    find_package(SparkEngine REQUIRED)

    file(GLOB_RECURSE GAME_SOURCES "Source/*.cpp" "Source/*.h")
    spark_add_game_module(MyGame ${GAME_SOURCES})

  This creates a SHARED library with the correct definitions and links
  against SparkEngineLib. The module will export CreateModule/DestroyModule
  when you use SPARK_IMPLEMENT_MODULE(YourModuleClass) in a .cpp file.
#]=============================================================================]

function(spark_add_game_module TARGET_NAME)
    add_library(${TARGET_NAME} SHARED ${ARGN})

    # Export macros
    target_compile_definitions(${TARGET_NAME} PRIVATE
        SPARK_MODULE_DLL
        SPARK_GAME_DLL
    )

    # Link against the engine static library
    target_link_libraries(${TARGET_NAME} PRIVATE Spark::SparkEngineLib)

    # Include SDK headers
    target_include_directories(${TARGET_NAME} PRIVATE
        ${SPARK_ENGINE_INCLUDE_DIR}
        ${SPARK_ENGINE_INCLUDE_DIR}/Spark
        ${SPARK_ENGINE_INCLUDE_DIR}/SparkEngine
    )

    # C++20
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)

    # Platform-specific settings
    if(MSVC)
        set_property(TARGET ${TARGET_NAME} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    message(STATUS "spark_add_game_module: ${TARGET_NAME} configured as game module DLL")
endfunction()
