# SparkUI — the Ember Signal design system shared by every first-party UI surface.
#
#   SparkUITokens  ImGui-free tokens, colour science and the accessibility audit. Always built;
#                  headless code (SparkTests, editor preferences) may link it.
#   SparkUIKit     Dear ImGui style mapping, fonts, DPI, per-context runtime, widgets and brand
#                  drawing. Created only when the shared `imgui` target exists (cmake/BuildImGui.cmake
#                  is included by the root CMakeLists before this directory).
#
# SparkUIKit never links SparkEngineLib or SparkEditor: diagnostics are returned as values.
# Fuzz targets (clang + libc++) must not link either archive; they may include the header-only
# SparkUI/Enums.h.

add_library(SparkUITokens STATIC
    Source/Color.cpp
    Source/Tokens.cpp
    Source/TokenAudit.cpp
)
target_include_directories(SparkUITokens PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/Include")
target_compile_features(SparkUITokens PUBLIC cxx_std_20)
if(MSVC)
    set_property(TARGET SparkUITokens PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

if(TARGET imgui)
    add_library(SparkUIKit STATIC
        Source/Style.cpp
        Source/Fonts.cpp
        Source/Widgets.cpp
        Source/Brand.cpp
        Source/Runtime.cpp
    )
    # The imgui target's non-Windows backend is SDL2 by construction (cmake/BuildImGui.cmake), so
    # every non-Windows SparkUIKit has SDL2 available for the DPI query.
    if(WIN32)
        target_sources(SparkUIKit PRIVATE Source/DpiWin32.cpp)
    else()
        target_sources(SparkUIKit PRIVATE Source/DpiSdl.cpp)
    endif()
    target_link_libraries(SparkUIKit PUBLIC imgui SparkUITokens)
    # Runtime.cpp fills the SDK's SparkUiRuntimeBlock, which Spark/UiTheme.h only declares when
    # SPARK_HAS_IMGUI is defined; nothing else in the kit depends on the SDK.
    target_include_directories(SparkUIKit PRIVATE "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
    set_source_files_properties(Source/Runtime.cpp PROPERTIES COMPILE_DEFINITIONS "SPARK_HAS_IMGUI=1")
    if(MSVC)
        set_property(TARGET SparkUIKit PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    message(STATUS "SparkUI: SparkUITokens + SparkUIKit")
else()
    message(STATUS "SparkUI: SparkUITokens only (no imgui target)")
endif()
