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
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Utils/DebugHookManager.cpp"
    "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Utils/Logger.cpp"
)

# Backend implementation files (needed by RHIFactory::CreateDevice)
if(WIN32)
    list(APPEND RHI_SOURCES
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/D3D11/D3D11Device.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/D3D12/D3D12Device.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/D3D12/D3D12CommandList.cpp"
    )
endif()

if(SPARK_VULKAN_AVAILABLE)
    list(APPEND RHI_SOURCES
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/Vulkan/VulkanDevice.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/Vulkan/VulkanCommandList.cpp"
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/Vulkan/VulkanFormatHelpers.cpp"
    )
endif()

if(SPARK_OPENGL_AVAILABLE)
    list(APPEND RHI_SOURCES
        "${CMAKE_SOURCE_DIR}/SparkEngine/Source/Graphics/RHI/OpenGL/OpenGLDevice.cpp"
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

target_compile_features(SparkShaderCompiler PRIVATE cxx_std_23)

# Platform-specific link libraries and definitions
if(WIN32)
    target_link_libraries(SparkShaderCompiler PRIVATE d3d11 d3d12 dxgi dxguid d3dcompiler opengl32)
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
    if(GLAD_FOUND)
        target_link_libraries(SparkShaderCompiler PRIVATE glad)
    endif()
    if(NOT WIN32)
        find_package(X11 QUIET)
        if(X11_FOUND)
            target_link_libraries(SparkShaderCompiler PRIVATE X11::X11)
        endif()
        target_link_libraries(SparkShaderCompiler PRIVATE OpenGL::GL)
    endif()
endif()

set_target_properties(SparkShaderCompiler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Tools"
)
