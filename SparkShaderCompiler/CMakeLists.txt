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
    "${CMAKE_SOURCE_DIR}/Spark Engine/Source/Graphics/RHI/RHIFactory.cpp"
)

add_executable(SparkShaderCompiler
    ${COMPILER_SOURCES}
    ${RHI_SOURCES}
)

target_include_directories(SparkShaderCompiler PRIVATE
    "${CMAKE_SOURCE_DIR}/Spark Engine/Source/Graphics/RHI"
    "${CMAKE_SOURCE_DIR}/Spark Engine/Source"
)

target_compile_features(SparkShaderCompiler PRIVATE cxx_std_17)

# Platform-specific link libraries
if(WIN32)
    target_link_libraries(SparkShaderCompiler PRIVATE d3dcompiler)
endif()

set_target_properties(SparkShaderCompiler PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    FOLDER "Tools"
)
