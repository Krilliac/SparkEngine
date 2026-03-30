cmake_minimum_required(VERSION 3.25)

    project(SparkConsole)

#Set C++ standard
        set(CMAKE_CXX_STANDARD 23) set(CMAKE_CXX_STANDARD_REQUIRED ON)

#Use the same output directories as parent project
            if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY) set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR} /
                                                        bin) endif()

#Ensure all configurations use the same output directory
                foreach (config Debug Release RelWithDebInfo MinSizeRel) string(TOUPPER ${config} CONFIG_UPPER) if (
                    NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}) set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${
                    CONFIG_UPPER} ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}) endif() endforeach()

#Find all source files in SparkConsole directory
                    file(GLOB_RECURSE CONSOLE_SOURCES CONFIGURE_DEPENDS "src/*.cpp"
                                                                        "src/*.h"
                                                                        "src/*.hpp")

#Pull in Logger and its dependency(DebugHookManager) from SparkEngine so
#the SPARK_LOG_* macros resolve at link time.
                        set(SPARK_LOGGER_SOURCES ${CMAKE_SOURCE_DIR} / SparkEngine / Source / Utils /
                            Logger.cpp ${CMAKE_SOURCE_DIR} / SparkEngine / Source / Utils / DebugHookManager.cpp)

#Only create the target if we have source files
                            if (CONSOLE_SOURCES)
#Create the console executable
                                add_executable(SparkConsole ${CONSOLE_SOURCES} ${SPARK_LOGGER_SOURCES})

#Include directories
                                    target_include_directories(SparkConsole PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} /
                                                               src ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR} /
                                                               SparkEngine / Source)

#Compiler definitions
                                        target_compile_definitions(SparkConsole PRIVATE CONSOLE_ENABLED) if (
                                            WIN32) target_compile_definitions(SparkConsole PRIVATE
                                                                                  _CRT_SECURE_NO_WARNINGS
                                                                                      WIN32_LEAN_AND_MEAN NOMINMAX
                                                                                          SPARK_PLATFORM_WINDOWS) endif()

#Per - configuration definitions so console compiles in both Debug and Release
                                            target_compile_definitions(
                                                SparkConsole PRIVATE $<$<CONFIG : Debug> : SPARK_DEBUG = 1>
                                                    $<$<CONFIG : Debug> : SPARK_CONSOLE_DEBUG = 1>
                                                        $<$<CONFIG : RelWithDebInfo> : SPARK_DEVELOPMENT = 1>
                                                            $<$<CONFIG : Release> : SPARK_RELEASE = 1>
                                                                $<$<CONFIG : Release> : NDEBUG>
                                                                    $<$<CONFIG : MinSizeRel> : SPARK_SHIPPING = 1>
                                                                        $<$<CONFIG : MinSizeRel> : NDEBUG>)

#System libraries - platform - specific
                                                if (WIN32) target_link_libraries(
                                                    SparkConsole PRIVATE kernel32 user32 gdi32 shell32 comdlg32 advapi32
                                                        winmm) else() find_package(Threads REQUIRED)
                                                    target_link_libraries(SparkConsole PRIVATE Threads::Threads) if (
                                                        APPLE) target_compile_definitions(SparkConsole PRIVATE
                                                                                              SPARK_PLATFORM_MACOS) else()
                                                        target_compile_definitions(
                                                            SparkConsole PRIVATE SPARK_PLATFORM_LINUX) endif() endif()

#MSVC specific settings - works for both Debug and Release
                                                            if (MSVC) target_compile_options(
                                                                SparkConsole PRIVATE / W3 / MP /
                                                                        wd4996
#Debug : full debug info; Release : optimize for speed
                                                                            $ <
                                                                    $ < CONFIG
                                                                : Debug >
                                                                : / Zi / Od / RTC1 > $ < $ < CONFIG
                                                                : Release >
                                                                : / O2 / Oi / GL > $ < $ < CONFIG
                                                                : RelWithDebInfo >
                                                                : / Zi / O2 > $ < $ < CONFIG
                                                                : MinSizeRel >
                                                                : / O1 / Os >)
#Enable whole - program optimization for Release
                                                                target_link_options(SparkConsole PRIVATE
                                                                                        $<$<CONFIG : Release> : / LTCG>)
#Generate PDB for RelWithDebInfo and Debug
                                                                    set_target_properties(
                                                                        SparkConsole PROPERTIES COMPILE_PDB_NAME_DEBUG
                                                                        "SparkConsole" COMPILE_PDB_NAME_RELWITHDEBINFO
                                                                        "SparkConsole") set_property(TARGET SparkConsole
                                                                                                         PROPERTY VS_DEBUGGER_WORKING_DIRECTORY
                                                                                                     "${CMAKE_SOURCE_"
                                                                                                     "DIR}")

#Organize source files in Visual Studio
                                                                        foreach (src ${CONSOLE_SOURCES}) get_filename_component(
                                                                            dir
                                                                            "${src}" DIRECTORY) file(RELATIVE_PATH grp
                                                                                                     "${CMAKE_CURRENT_"
                                                                                                     "SOURCE_DIR}/src"
                                                                                                     "${dir}")
                                                                            string(
                                                                                REPLACE
                                                                                "/"
                                                                                "\\\\" grp
                                                                                "${grp}") if (grp STREQUAL
                                                                                              "") source_group("Source "
                                                                                                               "File"
                                                                                                               "s" FILES
                                                                                                               "${"
                                                                                                               "src"
                                                                                                               "}") else()
                                                                                source_group(
                                                                                    "Source Files\\\\${grp}" FILES
                                                                                    "${src}") endif() endforeach()
                                                                                    endif()

#GCC / Clang per - configuration flags
                                                                                        if (NOT MSVC)
                                                                                            target_compile_options(
                                                                                                SparkConsole PRIVATE -
                                                                                                        Wall - Wextra -
                                                                                                        Wno - unused -
                                                                                                        parameter $ <
                                                                                                    $ <
                                                                                                    CONFIG
                                                                                                : Debug >
                                                                                                : -g - O0 > $ < $ <
                                                                                                      CONFIG
                                                                                                : Release >
                                                                                                : -O2 - DNDEBUG > $ <
                                                                                                      $ <
                                                                                                      CONFIG
                                                                                                : RelWithDebInfo >
                                                                                                : -g - O2 > $ < $ <
                                                                                                      CONFIG
                                                                                                : MinSizeRel >
                                                                                                : -Os - DNDEBUG >)
                                                                                                endif()

                                                                                                    message(
                                                                                                        STATUS "SparkCo"
                                                                                                               "nsole "
                                                                                                               "configu"
                                                                                                               "red "
                                                                                                               "success"
                                                                                                               "fully "
                                                                                                               "(all "
                                                                                                               "configu"
                                                                                                               "rations"
                                                                                                               ": "
                                                                                                               "Debug, "
                                                                                                               "Release"
                                                                                                               ", "
                                                                                                               "RelWith"
                                                                                                               "DebInfo"
                                                                                                               ", "
                                                                                                               "MinSize"
                                                                                                               "Rel)")
                                                                                                        message(
                                                                                                            STATUS
                                                                                                            "SparkConso"
                                                                                                            "le output "
                                                                                                            "directory:"
                                                                                                            " ${CMAKE_"
                                                                                                            "RUNTIME_"
                                                                                                            "OUTPUT_"
                                                                                                            "DIRECTORY"
                                                                                                            "}") else()
                                                                                                            message(
                                                                                                                STATUS
                                                                                                                "No "
                                                                                                                "SparkC"
                                                                                                                "onsole"
                                                                                                                " sourc"
                                                                                                                "e "
                                                                                                                "files "
                                                                                                                "found "
                                                                                                                "- "
                                                                                                                "skippi"
                                                                                                                "ng "
                                                                                                                "SparkC"
                                                                                                                "onsole"
                                                                                                                " buil"
                                                                                                                "d")
                                                                                                                endif()
