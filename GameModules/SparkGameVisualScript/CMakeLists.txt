cmake_minimum_required(VERSION 3.25)

# ================================================================
# SparkGameVisualScript — Zero C++ Game Logic Module
#
# ALL game logic lives in visual script graphs (.vscript/.as files):
#   - PlayerController: WASD movement, jump, sprint, health regen
#   - Collectible: spinning coins, pickup scoring, sound effects
#   - EnemyPatrol: waypoint patrol, player detection, chase, attack
#   - GameManager: score tracking, win/lose conditions, HUD
#   - HealthPickup: healing items with respawn cooldown
#
# The C++ code is ONLY the IModule shell that loads scripts and
# spawns entities. No gameplay logic exists in C++.
# ================================================================

if(NOT CMAKE_PROJECT_NAME OR CMAKE_PROJECT_NAME STREQUAL "SparkGameVisualScript")
    project(SparkGameVisualScript LANGUAGES CXX)
    set(SPARK_GAME_VS_STANDALONE TRUE)
else()
    set(SPARK_GAME_VS_STANDALONE FALSE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(SPARK_GAME_VS_STANDALONE)
    if(NOT SPARK_ENGINE_DIR)
        set(SPARK_ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH
            "Path to SparkEngine root directory")
    endif()

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER ${config} CONFIG_UPPER)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib)
    endforeach()

    if(MSVC)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        add_compile_options(/W3 /MP /bigobj /wd4005 /wd4996 /wd4244 /wd4267)
        add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wno-unused-parameter -fPIC)
    endif()

    set(THIRDPARTY_INCLUDE_DIRS "")
    if(EXISTS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
        list(APPEND THIRDPARTY_INCLUDE_DIRS "${SPARK_ENGINE_DIR}/ThirdParty/ECS/entt/single_include")
    endif()

    set(ENGINE_SOURCE_DIR "${SPARK_ENGINE_DIR}/SparkEngine/Source")
else()
    set(ENGINE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/SparkEngine/Source")
endif()

# Only two source files — the IModule shell
file(GLOB_RECURSE SPARK_GAME_VS_SOURCES CONFIGURE_DEPENDS "Source/*.cpp" "Source/*.h")

add_library(SparkGameVisualScript SHARED ${SPARK_GAME_VS_SOURCES})
target_compile_definitions(SparkGameVisualScript PRIVATE SPARK_GAME_DLL SPARK_MODULE_DLL)

if(NOT SPARK_GAME_VS_STANDALONE AND TARGET SparkEngineLib)
    if(WIN32)
        target_link_libraries(SparkGameVisualScript PRIVATE SparkEngineLib)
    elseif(TARGET SparkEngineInterface)
        target_link_libraries(SparkGameVisualScript PRIVATE SparkEngineInterface)
    endif()
endif()

if(SPARK_GAME_VS_STANDALONE)
    set(SPARK_SDK_INCLUDE_DIR "${SPARK_ENGINE_DIR}/SparkSDK/Include")
else()
    set(SPARK_SDK_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/SparkSDK/Include")
endif()

target_include_directories(SparkGameVisualScript PRIVATE
    "Source"
    "${ENGINE_SOURCE_DIR}"
    "${SPARK_SDK_INCLUDE_DIR}"
    ${THIRDPARTY_INCLUDE_DIRS}
)

if(WIN32)
    target_link_libraries(SparkGameVisualScript PRIVATE
        d3d11 dxgi d3dcompiler dxguid
        kernel32 user32 gdi32 winspool shell32 comdlg32 advapi32
        ole32 oleaut32 uuid winmm
        $<$<BOOL:${ENABLE_NETWORKING}>:ws2_32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wsock32>
        $<$<BOOL:${ENABLE_NETWORKING}>:crypt32>
        $<$<BOOL:${ENABLE_NETWORKING}>:wldap32>
        $<$<BOOL:${ENABLE_NETWORKING}>:normaliz>
    )
    target_compile_definitions(SparkGameVisualScript PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS SPARK_PLATFORM_WINDOWS)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(SparkGameVisualScript PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(APPLE)
        target_compile_definitions(SparkGameVisualScript PRIVATE SPARK_PLATFORM_MACOS)
    else()
        target_compile_definitions(SparkGameVisualScript PRIVATE SPARK_PLATFORM_LINUX)
    endif()
endif()

if(MSVC)
    target_link_libraries(SparkGameVisualScript PRIVATE legacy_stdio_definitions)
endif()

if(WIN32)
    if(TARGET Jolt)
        target_link_libraries(SparkGameVisualScript PRIVATE Jolt)
    endif()
    if(TARGET miniz)
        target_link_libraries(SparkGameVisualScript PRIVATE miniz)
    endif()
    if(TARGET tinyobjloader)
        target_link_libraries(SparkGameVisualScript PRIVATE tinyobjloader)
    endif()
endif()

if(SPARK_VULKAN_AVAILABLE)
    target_compile_definitions(SparkGameVisualScript PRIVATE SPARK_VULKAN_SUPPORT)
    if(Vulkan_FOUND)
        target_link_libraries(SparkGameVisualScript PRIVATE Vulkan::Vulkan)
    else()
        target_include_directories(SparkGameVisualScript PRIVATE ${Vulkan_INCLUDE_DIR})
        target_link_libraries(SparkGameVisualScript PRIVATE ${Vulkan_LIBRARY})
    endif()
endif()

if(SPARK_OPENGL_AVAILABLE)
    target_compile_definitions(SparkGameVisualScript PRIVATE SPARK_OPENGL_SUPPORT)
    target_link_libraries(SparkGameVisualScript PRIVATE OpenGL::GL)
    if(TARGET glad)
        target_link_libraries(SparkGameVisualScript PRIVATE glad)
    endif()
endif()

if(FEATURE_DEFINITIONS)
    target_compile_definitions(SparkGameVisualScript PRIVATE ${FEATURE_DEFINITIONS})
endif()

# Copy visual script assets alongside the built DLL
add_custom_command(TARGET SparkGameVisualScript POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:SparkGameVisualScript>/Assets/Scripts/Generated"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/Assets/Scripts/Generated"
        "$<TARGET_FILE_DIR:SparkGameVisualScript>/Assets/Scripts/Generated"
    COMMENT "Copying visual script assets for SparkGameVisualScript"
)

message(STATUS "SparkGameVisualScript configured — zero C++ game logic, all visual scripts")
