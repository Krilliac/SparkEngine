cmake_minimum_required(VERSION 3.16)

project(SparkGame LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------
# Collect SparkGame source files
# ---------------------------------------------------------------------
file(GLOB_RECURSE SPARK_GAME_SOURCES
    CONFIGURE_DEPENDS
    "Source/*.cpp"
    "Source/*.h"
    "Source/*.hpp"
)
list(FILTER SPARK_GAME_SOURCES EXCLUDE REGEX ".*[Tt]est.*")
list(FILTER SPARK_GAME_SOURCES EXCLUDE REGEX ".*[Ee]xample.*")

# ---------------------------------------------------------------------
# Create the game executable
# ---------------------------------------------------------------------
if(WIN32)
    add_executable(SparkGame WIN32 ${SPARK_GAME_SOURCES})
else()
    add_executable(SparkGame ${SPARK_GAME_SOURCES})
endif()

# ---------------------------------------------------------------------
# Link against SparkEngine static library
# ---------------------------------------------------------------------
target_link_libraries(SparkGame PRIVATE SparkEngine)

# ---------------------------------------------------------------------
# Include directories
# - SparkGame's own source for game-specific headers
# - SparkEngine source for engine headers (the engine is a library)
# ---------------------------------------------------------------------
target_include_directories(SparkGame PRIVATE
    "Source"
    "${CMAKE_SOURCE_DIR}/Spark Engine/Source"
    ${THIRDPARTY_INCLUDE_DIRS}
)

# ---------------------------------------------------------------------
# Platform-specific libraries
# ---------------------------------------------------------------------
if(WIN32)
    target_link_libraries(SparkGame PRIVATE
        d3d11 dxgi d3dcompiler dxguid
        kernel32 user32 gdi32 winspool
        shell32 comdlg32 advapi32
        ole32 oleaut32 uuid
        winmm
        $<$<BOOL:${ENABLE_NETWORKING}>:ws2_32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wsock32>
        $<$<BOOL:${ENABLE_NETWORKING}>:crypt32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wldap32>
        $<$<BOOL:${ENABLE_NETWORKING}>:normaliz>
    )

    target_compile_definitions(SparkGame PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(SparkGame PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(SparkGame PRIVATE SPARK_PLATFORM_LINUX)
endif()

if(MSVC)
    target_link_libraries(SparkGame PRIVATE legacy_stdio_definitions)
endif()

# Link Bullet Physics if available
if(TARGET BulletDynamics)
    target_link_libraries(SparkGame PRIVATE BulletDynamics BulletCollision LinearMath)
endif()

# Link third-party libraries
if(TARGET miniz)
    target_link_libraries(SparkGame PRIVATE miniz)
endif()
if(TARGET tinyobjloader)
    target_link_libraries(SparkGame PRIVATE tinyobjloader)
endif()

# Vulkan backend
if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkGame PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkGame PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkGame PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkGame PRIVATE ${Vulkan_LIBRARY})
    endif()
endif()

# OpenGL backend
if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkGame PRIVATE SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkGame PRIVATE OpenGL::GL)
    if(TARGET glad)
        target_link_libraries(SparkGame PRIVATE glad)
    endif()
endif()

# Apply feature definitions (inherited from root CMakeLists.txt via FEATURE_DEFINITIONS)
if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkGame PRIVATE ${FEATURE_DEFINITIONS})
endif()

# ---------------------------------------------------------------------
# Asset Management (game-specific assets)
# ---------------------------------------------------------------------
add_custom_command(TARGET SparkGame POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Assets"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Models"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Textures"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Audio"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Scripts"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Config"
    COMMENT "Creating SparkGame asset directory structure"
)

# Copy model assets
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/Models")
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets/Models"
            "$<TARGET_FILE_DIR:SparkGame>/Assets/Models"
        COMMENT "Copying model assets to SparkGame"
    )
endif()

# Copy scene files
if(EXISTS "${CMAKE_SOURCE_DIR}/Assets/Scenes")
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Assets/Scenes"
            "$<TARGET_FILE_DIR:SparkGame>/Assets/Scenes"
        COMMENT "Copying scene assets to SparkGame"
    )
endif()

# Copy HLSL shaders
file(GLOB_RECURSE SHADER_FILES "${CMAKE_SOURCE_DIR}/Spark Engine/Shaders/HLSL/*.hlsl")
foreach(SHADER_FILE ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_FILE}
            "$<TARGET_FILE_DIR:SparkGame>/Shaders/${SHADER_NAME}"
        COMMENT "Copying shader ${SHADER_NAME}"
    )
endforeach()

# Copy GLSL shaders
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders/GLSL")
    file(GLOB GLSL_SHADER_FILES "${CMAKE_SOURCE_DIR}/Shaders/GLSL/*.glsl")
    foreach(SHADER_FILE ${GLSL_SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Shaders/GLSL"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkGame>/Shaders/GLSL/${SHADER_NAME}"
            COMMENT "Copying GLSL shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# Copy SPIR-V shaders
if(EXISTS "${CMAKE_SOURCE_DIR}/Shaders/SPIRV")
    file(GLOB SPIRV_SHADER_FILES "${CMAKE_SOURCE_DIR}/Shaders/SPIRV/*.spv")
    foreach(SHADER_FILE ${SPIRV_SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        add_custom_command(TARGET SparkGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGame>/Shaders/SPIRV"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SHADER_FILE}
                "$<TARGET_FILE_DIR:SparkGame>/Shaders/SPIRV/${SHADER_NAME}"
            COMMENT "Copying SPIR-V shader ${SHADER_NAME}"
        )
    endforeach()
endif()

# Ensure SparkConsole is copied if available
if(TARGET SparkConsole)
    add_dependencies(SparkGame SparkConsole)
    add_custom_command(TARGET SparkGame POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SparkConsole>
            $<TARGET_FILE_DIR:SparkGame>
        COMMENT "Copying SparkConsole to SparkGame output directory"
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------
# Visual Studio settings
# ---------------------------------------------------------------------
if(MSVC)
    foreach(src ${SPARK_GAME_SOURCES})
        get_filename_component(dir "${src}" DIRECTORY)
        file(RELATIVE_PATH grp "${CMAKE_CURRENT_SOURCE_DIR}/Source" "${dir}")
        string(REPLACE "/" "\\\\" grp "${grp}")
        if(grp STREQUAL "")
            source_group("Source Files" FILES "${src}")
        else()
            source_group("Source Files\\\\${grp}" FILES "${src}")
        endif()
    endforeach()

    set_property(TARGET SparkGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

    # Exclude shader files from compilation
    file(GLOB_RECURSE ALL_SHADER_FILES "*.hlsl")
    if(ALL_SHADER_FILES)
        set_source_files_properties(${ALL_SHADER_FILES} PROPERTIES
            VS_TOOL_OVERRIDE "None"
        )
    endif()
endif()

message(STATUS "SparkGame configured - links against SparkEngine static library")
