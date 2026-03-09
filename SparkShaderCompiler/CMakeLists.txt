# ============================================================================
# SparkShaderCompiler - Standalone Offline Shader Compilation Tool
# ============================================================================
#
# Uses the RHI cross-compilation pipeline to compile shaders for any
# supported backend (D3D11, Vulkan, OpenGL) from the command line.
#
# Build:
#   cmake --build . --target SparkShaderCompiler
#
# ============================================================================

set(COMPILER_SOURCES
    src/main.cpp
)

# RHI sources needed for shader compilation
set(RHI_SOURCES
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/RHIFactory.cpp"
)

# Backend implementation files (needed by RHIFactory::CreateDevice)
if(WIN32)
    list(APPEND RHI_SOURCES
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.cpp"
    )
endif()

if(SPARK_VULKAN_AVAILABLE)
    list(APPEND RHI_SOURCES
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/Vulkan/VulkanDevice.cpp"
    )
endif()

add_executable(SparkShaderCompiler
    ${COMPILER_SOURCES}
    ${RHI_SOURCES}
)

target_include_directories(SparkShaderCompiler PRIVATE
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source"
)

target_compile_features(SparkShaderCompiler PRIVATE cxx_std_20)

# Platform-specific link libraries and definitions
if(WIN32)
    target_link_libraries(SparkShaderCompiler PRIVATE d3dcompiler)
    target_compile_definitions(SparkShaderCompiler PRIVATE SPARK_PLATFORM_WINDOWS)
else()
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(SparkShaderCompiler PRIVATE Threads::Threads)
    endif()
    if(APPLE)
        target_compile_definitions(SparkShaderCompiler PRIVATE SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkShaderCompiler PRIVATE SPARK_PLATFORM_LINUX)
    endif()
endif()

# Vulkan backend for shader compilation
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkShaderCompiler PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkShaderCompiler PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkShaderCompiler PRIVATE "${Vulkan_INCLUDE_DIR}")
        target_link_libraries(SparkShaderCompiler PRIVATE "${Vulkan_LIBRARY}")
    endif()
endif()

# OpenGL backend for shader compilation
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkShaderCompiler PRIVATE SPARK_OPENGL_SUPPORT)
endif()

set_target_properties(SparkShaderCompiler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Tools"
)
