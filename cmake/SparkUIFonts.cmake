# SparkUIFonts.cmake - stage the Spark UI fonts next to an executable.
#
# Included mode (from the root CMakeLists.txt) defines:
#
#   spark_stage_ui_fonts(<target>)
#
# which adds a POST_BUILD step copying every tracked font under SparkEditor/Fonts (the faces
# and LICENSES/) to $<TARGET_FILE_DIR:<target>>/EditorAssets/Fonts, where
# Spark::Ui::FindFontDirectory looks first, and
#
#   spark_install_ui_fonts(<component>)
#
# which installs the same tree to bin/EditorAssets/Fonts under <component>. Every executable that
# loads the fonts (SparkEngine's game overlay, SparkEditor, SparkLauncher, SparkInstaller's GUI)
# stages them; the ones in the required runtime component also install them, so every package
# ships the faces and their licences. The install rule is registered once per install tree, and
# a request for a second component is a configure error: two components installing the same
# file break the MSI (WiX ICE30, one target file owned by two components).
#
# Script mode (the POST_BUILD step itself) performs the copy:
#
#   cmake -DSPARK_UI_FONTS_SOURCE=<dir> -DSPARK_UI_FONTS_DEST=<dir> -P cmake/SparkUIFonts.cmake
#
# Several executables share one output directory (SparkEngine, SparkEditor, SparkLauncher), so
# their POST_BUILD steps can run concurrently against the same files. The copy holds a file lock
# on the destination and copies only files whose contents differ, so concurrent and repeated
# builds are safe and cheap.

if(CMAKE_SCRIPT_MODE_FILE)
    foreach(_spark_required IN ITEMS SPARK_UI_FONTS_SOURCE SPARK_UI_FONTS_DEST)
        if(NOT DEFINED ${_spark_required} OR "${${_spark_required}}" STREQUAL "")
            message(FATAL_ERROR "SparkUIFonts: -D${_spark_required}=... is required")
        endif()
    endforeach()
    if(NOT IS_DIRECTORY "${SPARK_UI_FONTS_SOURCE}")
        message(FATAL_ERROR "SparkUIFonts: font source '${SPARK_UI_FONTS_SOURCE}' is not a directory")
    endif()

    file(MAKE_DIRECTORY "${SPARK_UI_FONTS_DEST}")
    file(LOCK "${SPARK_UI_FONTS_DEST}/.spark-ui-fonts.lock" GUARD PROCESS TIMEOUT 120)
    file(GLOB_RECURSE _spark_font_files LIST_DIRECTORIES false RELATIVE "${SPARK_UI_FONTS_SOURCE}"
        "${SPARK_UI_FONTS_SOURCE}/*.ttf"
        "${SPARK_UI_FONTS_SOURCE}/*.otf"
        "${SPARK_UI_FONTS_SOURCE}/LICENSES/*")
    if(NOT _spark_font_files)
        message(FATAL_ERROR "SparkUIFonts: no fonts found under '${SPARK_UI_FONTS_SOURCE}'")
    endif()
    foreach(_spark_font IN LISTS _spark_font_files)
        get_filename_component(_spark_font_dir "${SPARK_UI_FONTS_DEST}/${_spark_font}" DIRECTORY)
        file(MAKE_DIRECTORY "${_spark_font_dir}")
        file(COPY_FILE "${SPARK_UI_FONTS_SOURCE}/${_spark_font}" "${SPARK_UI_FONTS_DEST}/${_spark_font}"
            ONLY_IF_DIFFERENT)
    endforeach()
    return()
endif()

function(spark_stage_ui_fonts target)
    if(NOT TARGET ${target})
        message(FATAL_ERROR "spark_stage_ui_fonts: '${target}' is not a target")
    endif()
    set(_spark_font_source "${CMAKE_SOURCE_DIR}/SparkEditor/Fonts")
    if(NOT IS_DIRECTORY "${_spark_font_source}")
        message(WARNING "spark_stage_ui_fonts(${target}): ${_spark_font_source} is missing; "
                        "the UI falls back to the embedded default font")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DSPARK_UI_FONTS_SOURCE=${_spark_font_source}"
                "-DSPARK_UI_FONTS_DEST=$<TARGET_FILE_DIR:${target}>/EditorAssets/Fonts"
                -P "${CMAKE_SOURCE_DIR}/cmake/SparkUIFonts.cmake"
        COMMENT "Staging Spark UI fonts for ${target}"
        VERBATIM)
endfunction()

function(spark_install_ui_fonts component)
    if("${component}" STREQUAL "")
        message(FATAL_ERROR "spark_install_ui_fonts: an install component is required")
    endif()
    set(_spark_font_source "${CMAKE_SOURCE_DIR}/SparkEditor/Fonts")
    if(NOT IS_DIRECTORY "${_spark_font_source}")
        message(WARNING "spark_install_ui_fonts(${component}): ${_spark_font_source} is missing; "
                        "installed UIs fall back to the embedded default font")
        return()
    endif()
    get_property(_spark_installed GLOBAL PROPERTY SPARK_UI_FONTS_INSTALL_COMPONENT)
    if(_spark_installed)
        if(NOT "${component}" STREQUAL "${_spark_installed}")
            message(FATAL_ERROR "spark_install_ui_fonts(${component}): the fonts are already installed "
                                "by component '${_spark_installed}'; a second component installing the same "
                                "files fails MSI validation (ICE30)")
        endif()
        return()
    endif()
    set_property(GLOBAL PROPERTY SPARK_UI_FONTS_INSTALL_COMPONENT "${component}")
    install(DIRECTORY "${_spark_font_source}/"
        DESTINATION bin/EditorAssets/Fonts
        COMPONENT ${component})
endfunction()
