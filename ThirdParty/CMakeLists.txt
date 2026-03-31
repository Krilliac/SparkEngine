# ThirdParty Dependencies CMakeLists.txt
cmake_minimum_required(VERSION 3.25)

# Match the root project C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 is built from the root CMakeLists.txt (section 7) via add_subdirectory.
# The SDL2 target is available here if the submodule is present.

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
        # SDL2 may already be set up from the submodule above, or try system
        if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2)
            find_package(SDL2 QUIET)
        endif()
        find_package(OpenGL QUIET)

        set(_imgui_has_sdl2 FALSE)
        if(TARGET SDL2::SDL2 OR TARGET SDL2 OR SDL2_FOUND)
            set(_imgui_has_sdl2 TRUE)
        endif()

        if(_imgui_has_sdl2 AND OpenGL_FOUND)
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

# =========================================================================
# stb libraries (stb_image, stb_image_write) - Public Domain image I/O
# =========================================================================
set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Utils/stb")
if(EXISTS "${STB_DIR}/stb_image.h")
    add_library(stb_image STATIC "${STB_DIR}/stb_impl.cpp")
    target_include_directories(stb_image PUBLIC "${STB_DIR}")
    if(MSVC)
        target_compile_options(stb_image PRIVATE /W0)
        set_property(TARGET stb_image PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(stb_image PRIVATE -w)
    endif()
    message(STATUS "ThirdParty: stb_image configured (PNG/JPG/BMP/TGA/HDR)")
else()
    message(STATUS "ThirdParty: stb_image not found")
endif()

# =========================================================================
# cgltf - single-file glTF 2.0 parser (MIT)
# =========================================================================
set(CGLTF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Utils/cgltf")
if(EXISTS "${CGLTF_DIR}/cgltf.h")
    add_library(cgltf STATIC "${CGLTF_DIR}/cgltf_impl.cpp")
    target_include_directories(cgltf PUBLIC "${CGLTF_DIR}")
    if(MSVC)
        target_compile_options(cgltf PRIVATE /W0)
        set_property(TARGET cgltf PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(cgltf PRIVATE -w)
    endif()
    message(STATUS "ThirdParty: cgltf configured (glTF 2.0 parser)")
else()
    message(STATUS "ThirdParty: cgltf not found")
endif()

# =========================================================================
# miniaudio - cross-platform audio (Public Domain / MIT-0)
# =========================================================================
set(MINIAUDIO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Audio/miniaudio")
if(EXISTS "${MINIAUDIO_DIR}/miniaudio.h")
    add_library(miniaudio STATIC "${MINIAUDIO_DIR}/miniaudio_impl.cpp")
    target_include_directories(miniaudio PUBLIC "${MINIAUDIO_DIR}")
    if(MSVC)
        target_compile_options(miniaudio PRIVATE /W0)
        set_property(TARGET miniaudio PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(miniaudio PRIVATE -w)
    endif()
    # miniaudio links to platform audio backends automatically
    if(NOT WIN32 AND NOT APPLE)
        find_package(Threads QUIET)
        if(Threads_FOUND)
            target_link_libraries(miniaudio PRIVATE Threads::Threads)
        endif()
        target_link_libraries(miniaudio PRIVATE ${CMAKE_DL_LIBS})
    endif()
    message(STATUS "ThirdParty: miniaudio configured (cross-platform audio)")
else()
    message(STATUS "ThirdParty: miniaudio not found")
endif()

# =========================================================================
# nlohmann/json - JSON for Modern C++ (MIT, header-only)
# =========================================================================
set(NLOHMANN_JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Utils/json")
if(EXISTS "${NLOHMANN_JSON_DIR}/nlohmann_json.h")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE "${NLOHMANN_JSON_DIR}")
    message(STATUS "ThirdParty: nlohmann/json configured (header-only)")
else()
    message(STATUS "ThirdParty: nlohmann/json not found")
endif()

# =========================================================================
# tinyexr - OpenEXR loader/saver (BSD 3-Clause)
# =========================================================================
set(TINYEXR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Utils/tinyexr")
if(EXISTS "${TINYEXR_DIR}/tinyexr.h")
    add_library(tinyexr STATIC "${TINYEXR_DIR}/tinyexr_impl.cpp")
    target_include_directories(tinyexr PUBLIC "${TINYEXR_DIR}")
    if(MSVC)
        target_compile_options(tinyexr PRIVATE /W0)
        set_property(TARGET tinyexr PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        target_compile_options(tinyexr PRIVATE -w)
    endif()
    message(STATUS "ThirdParty: tinyexr configured (OpenEXR loader)")
else()
    message(STATUS "ThirdParty: tinyexr not found")
endif()
