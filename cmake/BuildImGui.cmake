# Builds the `imgui` static library target shared by SparkEditor and SparkLauncher.
# Idempotent: safe to include from multiple subdirs — the target is created at most once.

if(NOT DEFINED IMGUI_INCLUDE_DIR OR NOT IMGUI_INCLUDE_DIR)
    find_path(IMGUI_INCLUDE_DIR
        NAMES imgui.h
        PATHS
            "${CMAKE_SOURCE_DIR}/ThirdParty/imgui"
            "${CMAKE_SOURCE_DIR}/ThirdParty/UI/imgui"
            "${CMAKE_SOURCE_DIR}/External/imgui"
            "${CMAKE_SOURCE_DIR}/Vendor/imgui"
        DOC "Dear ImGui include directory"
        NO_CMAKE_FIND_ROOT_PATH
    )
endif()

if(IMGUI_INCLUDE_DIR)
    if(NOT TARGET imgui)
        message(STATUS "Found Dear ImGui at: ${IMGUI_INCLUDE_DIR}")

        set(_IMGUI_CORE_SOURCES
            "${IMGUI_INCLUDE_DIR}/imgui.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_demo.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_draw.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_tables.cpp"
            "${IMGUI_INCLUDE_DIR}/imgui_widgets.cpp"
        )

        if(WIN32)
            add_library(imgui STATIC
                ${_IMGUI_CORE_SOURCES}
                "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_win32.cpp"
                "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_dx11.cpp"
            )

            target_compile_definitions(imgui PUBLIC
                IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
                IMGUI_IMPL_WIN32_DISABLE_LINKING_XINPUT
            )

            target_link_libraries(imgui PUBLIC
                d3d11 dxgi user32 gdi32 shell32 ole32 oleaut32 uuid comdlg32 advapi32
            )
        else()
            if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2)
                find_package(SDL2 REQUIRED)
            endif()
            find_package(OpenGL REQUIRED)

            add_library(imgui STATIC
                ${_IMGUI_CORE_SOURCES}
                "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_sdl2.cpp"
                "${IMGUI_INCLUDE_DIR}/backends/imgui_impl_opengl3.cpp"
            )

            if(TARGET SDL2::SDL2)
                target_link_libraries(imgui PUBLIC SDL2::SDL2)
            elseif(TARGET SDL2)
                target_link_libraries(imgui PUBLIC SDL2)
            else()
                target_include_directories(imgui PUBLIC ${SDL2_INCLUDE_DIRS})
                target_link_libraries(imgui PUBLIC ${SDL2_LIBRARIES})
            endif()

            target_link_libraries(imgui PUBLIC OpenGL::GL)
        endif()

        target_include_directories(imgui PUBLIC
            "${IMGUI_INCLUDE_DIR}"
            "${IMGUI_INCLUDE_DIR}/backends"
        )

        if(MSVC)
            target_compile_options(imgui PRIVATE /W2)
            target_compile_definitions(imgui PRIVATE
                WIN32_LEAN_AND_MEAN
                NOMINMAX
                _CRT_SECURE_NO_WARNINGS
            )
            set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endif()

    set(IMGUI_FOUND TRUE)
else()
    set(IMGUI_FOUND FALSE)
endif()
