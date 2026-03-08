# ThirdParty Dependencies CMakeLists.txt
cmake_minimum_required(VERSION 3.16)

# Match the root project C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EnTT - Header only
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ECS/entt/single_include")
    add_library(entt INTERFACE)
    target_include_directories(entt INTERFACE ECS/entt/single_include)
    message(STATUS "ThirdParty: EnTT configured (header-only)")
else()
    message(STATUS "ThirdParty: EnTT not found (submodule not initialized)")
endif()

# Dear ImGui (Windows only - requires DX11/Win32 backends)
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/UI/imgui")
if(WIN32 AND EXISTS "${IMGUI_DIR}/imgui.cpp")
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
    if(MSVC)
        set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
    message(STATUS "ThirdParty: Dear ImGui configured (Win32+DX11)")
elseif(EXISTS "${IMGUI_DIR}/imgui.cpp")
    # Non-Windows: build core ImGui without platform backends
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
    )
    target_include_directories(imgui PUBLIC ${IMGUI_DIR})
    message(STATUS "ThirdParty: Dear ImGui configured (core only, no platform backend)")
else()
    message(STATUS "ThirdParty: Dear ImGui not found (submodule not initialized)")
endif()

# AngelScript - path matches the submodule name in .gitmodules (angelscript-mirror)
set(ANGELSCRIPT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Scripting/angelscript-mirror/angelscript/projects/cmake")
if(EXISTS "${ANGELSCRIPT_CMAKE_DIR}/CMakeLists.txt")
    add_subdirectory("${ANGELSCRIPT_CMAKE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/angelscript")
    message(STATUS "ThirdParty: AngelScript configured")
else()
    message(STATUS "ThirdParty: AngelScript not found (submodule not initialized)")
endif()
