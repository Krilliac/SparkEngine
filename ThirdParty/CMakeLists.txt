# ThirdParty Dependencies CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

# Match the root project C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# EnTT - Header only
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ECS/entt/single_include")
    add_library(entt INTERFACE)
    target_include_directories(entt INTERFACE ECS/entt/single_include)
    message(STATUS "ThirdParty: EnTT configured (header-only)")
else()
    message(STATUS "ThirdParty: EnTT not found (submodule not initialized)")
endif()

# Dear ImGui - Cross-platform with platform-specific backends
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/UI/imgui")
if(EXISTS "${IMGUI_DIR}/imgui.cpp")
    set(IMGUI_CORE_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
    )

    if(WIN32)
        # Windows: Win32 + DX11 backends
        add_library(imgui STATIC
            ${IMGUI_CORE_SOURCES}
            ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
            ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
        )
        target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
        if(MSVC)
            set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        message(STATUS "ThirdParty: Dear ImGui configured (Win32+DX11)")
    else()
        # Linux: SDL2 + OpenGL3 backends (ImGui ships its own GL loader)
        find_package(SDL2 QUIET)
        find_package(OpenGL QUIET)

        if(SDL2_FOUND AND OpenGL_FOUND)
            add_library(imgui STATIC
                ${IMGUI_CORE_SOURCES}
                ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
                ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
            )
            target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)

            if(TARGET SDL2::SDL2)
                target_link_libraries(imgui PUBLIC SDL2::SDL2)
            else()
                target_include_directories(imgui PUBLIC ${SDL2_INCLUDE_DIRS})
                target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES})
            endif()
            target_link_libraries(imgui PUBLIC OpenGL::GL)
            message(STATUS "ThirdParty: Dear ImGui configured (SDL2+OpenGL3)")
        else()
            # Fallback: core only without platform backends
            add_library(imgui STATIC ${IMGUI_CORE_SOURCES})
            target_include_directories(imgui PUBLIC ${IMGUI_DIR})
            message(STATUS "ThirdParty: Dear ImGui configured (core only, install SDL2 for full editor support)")
        endif()
    endif()
else()
    message(STATUS "ThirdParty: Dear ImGui not found (submodule not initialized)")
endif()

# Recast Navigation - NavMesh generation library
set(RECAST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/AI/recastnavigation")
if(EXISTS "${RECAST_DIR}/Recast/Source/Recast.cpp")
    # Build Recast (voxelization + navmesh generation)
    file(GLOB RECAST_SOURCES "${RECAST_DIR}/Recast/Source/*.cpp")
    add_library(Recast STATIC ${RECAST_SOURCES})
    target_include_directories(Recast PUBLIC "${RECAST_DIR}/Recast/Include")
    if(MSVC)
        target_compile_options(Recast PRIVATE /W0)
    else()
        target_compile_options(Recast PRIVATE -w)
    endif()

    # Build Detour (runtime query + pathfinding)
    file(GLOB DETOUR_SOURCES "${RECAST_DIR}/Detour/Source/*.cpp")
    add_library(Detour STATIC ${DETOUR_SOURCES})
    target_include_directories(Detour PUBLIC "${RECAST_DIR}/Detour/Include")
    if(MSVC)
        target_compile_options(Detour PRIVATE /W0)
    else()
        target_compile_options(Detour PRIVATE -w)
    endif()

    message(STATUS "ThirdParty: Recast/Detour configured")
else()
    message(STATUS "ThirdParty: Recast/Detour not found (submodule not initialized)")
endif()

# AngelScript - path matches the submodule name in .gitmodules (angelscript-mirror)
set(ANGELSCRIPT_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Scripting/angelscript-mirror/angelscript/projects/cmake")
if(EXISTS "${ANGELSCRIPT_CMAKE_DIR}/CMakeLists.txt")
    add_subdirectory("${ANGELSCRIPT_CMAKE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/angelscript")
    message(STATUS "ThirdParty: AngelScript configured")
else()
    message(STATUS "ThirdParty: AngelScript not found (submodule not initialized)")
endif()
