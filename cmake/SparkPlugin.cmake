#[=============================================================================[
  SparkPlugin.cmake - Build helper for stable-C-ABI Spark plugins

  Example:
    spark_add_plugin(MyImporter
        ID "org.example.my-importer"
        VERSION "1.2.0"
        TYPE "asset-importer"
        SOURCES MyImporter.cpp)

  Plugins intentionally do not link SparkEngineLib. They communicate only
  through Spark/PluginABI.h so host and plugin may use different C++ runtimes.
#]=============================================================================]

include_guard(GLOBAL)

function(spark_add_plugin TARGET_NAME)
    cmake_parse_arguments(SPARK_PLUGIN "" "ID;VERSION;TYPE" "SOURCES;LINK_LIBRARIES" ${ARGN})
    if(NOT SPARK_PLUGIN_ID)
        message(FATAL_ERROR "spark_add_plugin(${TARGET_NAME}): ID is required")
    endif()
    if(NOT SPARK_PLUGIN_VERSION)
        set(SPARK_PLUGIN_VERSION "0.1.0")
    endif()
    if(NOT SPARK_PLUGIN_TYPE)
        set(SPARK_PLUGIN_TYPE "runtime-extension")
    endif()
    if(NOT SPARK_PLUGIN_SOURCES)
        message(FATAL_ERROR "spark_add_plugin(${TARGET_NAME}): SOURCES is required")
    endif()

    add_library(${TARGET_NAME} SHARED ${SPARK_PLUGIN_SOURCES})
    target_compile_definitions(${TARGET_NAME} PRIVATE SPARK_PLUGIN_DLL=1)
    target_include_directories(${TARGET_NAME} PRIVATE
        "${SPARK_ENGINE_INCLUDE_DIR}"
        "${SPARK_ENGINE_INCLUDE_DIR}/Spark")
    target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)
    if(SPARK_PLUGIN_LINK_LIBRARIES)
        target_link_libraries(${TARGET_NAME} PRIVATE ${SPARK_PLUGIN_LINK_LIBRARIES})
    endif()

    set_target_properties(${TARGET_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES)

    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DPLUGIN_PATH=$<TARGET_FILE:${TARGET_NAME}>"
            "-DMETADATA_PATH=$<TARGET_FILE:${TARGET_NAME}>.sparkplugin.json"
            "-DPLUGIN_ID=${SPARK_PLUGIN_ID}"
            "-DPLUGIN_VERSION=${SPARK_PLUGIN_VERSION}"
            "-DPLUGIN_TYPE=${SPARK_PLUGIN_TYPE}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/WriteSparkPluginMetadata.cmake"
        COMMENT "Writing ${TARGET_NAME} deterministic plugin metadata"
        VERBATIM)
endfunction()
