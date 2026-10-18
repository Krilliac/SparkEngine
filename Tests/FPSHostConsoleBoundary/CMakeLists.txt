# Run in a separate process: graphics commands borrow the GraphicsEngine used
# below, and the singleton console must shut down before that engine is destroyed.
if(WIN32 AND NOT TARGET SparkGameFPS)
    return()
endif()
add_executable(SparkFPSHostConsoleBoundaryTests
    main.cpp
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source/Console/AdvancedConsoleCommands.cpp")
target_compile_features(SparkFPSHostConsoleBoundaryTests PRIVATE cxx_std_23)
target_include_directories(SparkFPSHostConsoleBoundaryTests PRIVATE
    "${CMAKE_SOURCE_DIR}/GameModules/SparkGameFPS/Source")
target_link_libraries(SparkFPSHostConsoleBoundaryTests PRIVATE SparkEngineLib)
if(WIN32)
    # AdvancedConsoleCommands uses the exported Game::GetEngineContext accessor.
    # Keep real consumer import semantics, including non-inlined Debug calls.
    target_link_libraries(SparkFPSHostConsoleBoundaryTests PRIVATE SparkGameFPS)
endif()
target_compile_definitions(SparkFPSHostConsoleBoundaryTests PRIVATE
    SPARK_TEST_REGISTRY_LIFECYCLE_MODULE_PATH="$<TARGET_FILE:SparkRegistryLifecycleModuleFixture>")
add_dependencies(SparkFPSHostConsoleBoundaryTests SparkRegistryLifecycleModuleFixture)
if(NOT WIN32)
    # The production registry fixture resolves host engine symbols on POSIX.
    set_target_properties(SparkFPSHostConsoleBoundaryTests PROPERTIES ENABLE_EXPORTS ON)
    if(APPLE)
        target_link_options(SparkFPSHostConsoleBoundaryTests PRIVATE "LINKER:-export_dynamic")
    endif()
endif()
add_test(NAME SparkFPSHostConsoleBoundary COMMAND SparkFPSHostConsoleBoundaryTests)
set_tests_properties(SparkFPSHostConsoleBoundary PROPERTIES
    TIMEOUT 90 LABELS "console;sdk;fps;module;integration")
