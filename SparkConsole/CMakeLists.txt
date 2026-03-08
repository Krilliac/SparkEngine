cmake_minimum_required(VERSION 3.16)

project(SparkConsole)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use the same output directories as parent project
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# Ensure all configurations use the same output directory
foreach(config Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${config} CONFIG_UPPER)
    if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER})
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    endif()
endforeach()

# Find all source files in SparkConsole directory
file(GLOB_RECURSE CONSOLE_SOURCES
    CONFIGURE_DEPENDS
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)

# Only create the target if we have source files
if(CONSOLE_SOURCES)
    # Create the console executable
    add_executable(SparkConsole ${CONSOLE_SOURCES})

    # Include directories
    target_include_directories(SparkConsole PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Compiler definitions
    target_compile_definitions(SparkConsole PRIVATE
        CONSOLE_ENABLED
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    # System libraries - platform-specific
    if(WIN32)
        target_link_libraries(SparkConsole PRIVATE
            kernel32 user32 gdi32
            shell32 comdlg32 advapi32
            winmm
        )
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(SparkConsole PRIVATE Threads::Threads)
        target_compile_definitions(SparkConsole PRIVATE SPARK_PLATFORM_LINUX)
    endif()

    # MSVC specific settings
    if(MSVC)
        target_compile_options(SparkConsole PRIVATE
            /W3 /MP
            /wd4996  # Deprecated functions
        )
        set_property(TARGET SparkConsole PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

        # Organize source files in Visual Studio
        foreach(src ${CONSOLE_SOURCES})
            get_filename_component(dir "${src}" DIRECTORY)
            file(RELATIVE_PATH grp "${CMAKE_CURRENT_SOURCE_DIR}/src" "${dir}")
            string(REPLACE "/" "\\\\" grp "${grp}")
            if(grp STREQUAL "")
                source_group("Source Files" FILES "${src}")
            else()
                source_group("Source Files\\\\${grp}" FILES "${src}")
            endif()
        endforeach()
    endif()

    message(STATUS "SparkConsole configured successfully")
    message(STATUS "SparkConsole output directory: ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
else()
    message(STATUS "No SparkConsole source files found - skipping SparkConsole build")
endif()

