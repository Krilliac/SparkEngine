include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/SparkTrackedInstall.cmake")

# Install the first-party runtime asset tree and its integrity manifest for one
# package profile.
#
#   spark_install_runtime_assets(
#       ROOT <source-relative asset root>          # e.g. Assets
#       DESTINATION <install-relative directory>   # e.g. bin/Assets
#       COMPONENT <install component>
#       PROFILE <default|stable-v1>
#       VERIFIER <absolute path to verify_asset_integrity.py>
#       DIRECTORIES <asset subdirectories to install>...
#       [ROOT_FILES <root-level files to install>...])
#
# PROFILE default installs every tracked file in DIRECTORIES plus
# <ROOT>/assets.integrity.json unchanged.
#
# PROFILE stable-v1 ships only the runtime asset closure of its in-profile game
# modules (RDY-020): the assets their sources and the reviewed engine source
# directories name, the scenes/materials those reference, and the reviewed
# seeds in tools/asset-integrity/package-profiles.json (see
# package_closure.py). It also
# applies OD-09: a closure entry whose license is NOASSERTION fails configure.
# At configure time the verifier derives the stable-v1 package manifest from
# <ROOT>/assets.integrity.json and lists the excluded files/directories. The
# install rules skip exactly those paths and ship the derived manifest as
# <DESTINATION>/assets.integrity.json, so `verify --profile stable-v1` on the
# installed tree proves both completeness and the exclusion. The excluded
# files stay in the repository.
#
# Configure re-runs when the manifest, the derivation scripts, the profile
# definition, the module inventory, a followed scene/material, or the set of
# C/C++ files in a scanned source directory changes. Editing the contents of
# an existing source file does not reconfigure (that would make every engine
# edit a full configure); a stale closure is instead rejected by
# `verify --profile stable-v1`, which re-derives the closure from the current
# sources and fails on any profile-incomplete or profile-outside-closure file.
function(spark_install_runtime_assets)
    cmake_parse_arguments(PARSE_ARGV 0 SPARK_ASSETS
        ""
        "ROOT;DESTINATION;COMPONENT;PROFILE;VERIFIER"
        "DIRECTORIES;ROOT_FILES")
    if(SPARK_ASSETS_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "spark_install_runtime_assets received unknown arguments: "
            "${SPARK_ASSETS_UNPARSED_ARGUMENTS}")
    endif()
    foreach(_spark_required IN ITEMS ROOT DESTINATION COMPONENT PROFILE VERIFIER DIRECTORIES)
        if(NOT DEFINED SPARK_ASSETS_${_spark_required} OR
           "${SPARK_ASSETS_${_spark_required}}" STREQUAL "")
            message(FATAL_ERROR "spark_install_runtime_assets requires ${_spark_required}")
        endif()
    endforeach()
    if(NOT SPARK_ASSETS_PROFILE STREQUAL "default" AND
       NOT SPARK_ASSETS_PROFILE STREQUAL "stable-v1")
        message(FATAL_ERROR "Unknown runtime asset package profile: ${SPARK_ASSETS_PROFILE}")
    endif()

    set(_spark_asset_root "${CMAKE_SOURCE_DIR}/${SPARK_ASSETS_ROOT}")
    set(_spark_source_manifest "${_spark_asset_root}/assets.integrity.json")
    set(_spark_installed_manifest "${_spark_source_manifest}")
    set(_spark_exclusions "")

    if(SPARK_ASSETS_PROFILE STREQUAL "stable-v1")
        if(NOT IS_ABSOLUTE "${SPARK_ASSETS_VERIFIER}" OR NOT EXISTS "${SPARK_ASSETS_VERIFIER}")
            message(FATAL_ERROR
                "stable-v1 runtime assets need the asset verifier: ${SPARK_ASSETS_VERIFIER}")
        endif()
        if(NOT EXISTS "${_spark_source_manifest}")
            message(FATAL_ERROR
                "stable-v1 runtime assets need the source manifest: ${_spark_source_manifest}")
        endif()
        find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
        set(_spark_profile_dir "${CMAKE_CURRENT_BINARY_DIR}/spark-asset-profile/${SPARK_ASSETS_PROFILE}")
        file(REMOVE_RECURSE "${_spark_profile_dir}")
        file(MAKE_DIRECTORY "${_spark_profile_dir}")
        set(_spark_installed_manifest "${_spark_profile_dir}/assets.integrity.json")
        set(_spark_exclusion_file "${_spark_profile_dir}/excluded-paths.txt")
        set(_spark_inputs_file "${_spark_profile_dir}/closure-inputs.txt")
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -B "${SPARK_ASSETS_VERIFIER}"
                package-profile "${_spark_source_manifest}"
                --profile "${SPARK_ASSETS_PROFILE}"
                --output "${_spark_installed_manifest}"
                --exclusions "${_spark_exclusion_file}"
                --repo-root "${CMAKE_SOURCE_DIR}"
                --inputs "${_spark_inputs_file}"
            RESULT_VARIABLE _spark_profile_result
            OUTPUT_VARIABLE _spark_profile_output
            ERROR_VARIABLE _spark_profile_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _spark_profile_result EQUAL 0)
            message(FATAL_ERROR
                "Could not derive the ${SPARK_ASSETS_PROFILE} runtime asset manifest "
                "(${_spark_profile_result}):\n${_spark_profile_output}\n${_spark_profile_error}")
        endif()
        message(STATUS "${_spark_profile_output}")
        # The inputs file lists the data files read plus each scanned source
        # directory; a directory contributes a CONFIGURE_DEPENDS glob so that
        # adding or removing a source file re-derives the closure.
        file(STRINGS "${_spark_inputs_file}" _spark_closure_inputs)
        set(_spark_closure_files "")
        foreach(_spark_closure_input IN LISTS _spark_closure_inputs)
            if(IS_DIRECTORY "${_spark_closure_input}")
                set(_spark_source_patterns "")
                foreach(_spark_suffix IN ITEMS c cc cpp cxx h hh hpp hxx inl)
                    list(APPEND _spark_source_patterns "${_spark_closure_input}/*.${_spark_suffix}")
                endforeach()
                file(GLOB_RECURSE _spark_scanned_sources CONFIGURE_DEPENDS ${_spark_source_patterns})
            else()
                list(APPEND _spark_closure_files "${_spark_closure_input}")
            endif()
        endforeach()
        get_filename_component(_spark_verifier_dir "${SPARK_ASSETS_VERIFIER}" DIRECTORY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${_spark_source_manifest}" "${SPARK_ASSETS_VERIFIER}"
            "${_spark_verifier_dir}/package_closure.py" ${_spark_closure_files})
        file(STRINGS "${_spark_exclusion_file}" _spark_exclusions)
    endif()

    foreach(_spark_directory IN LISTS SPARK_ASSETS_DIRECTORIES)
        if(NOT EXISTS "${_spark_asset_root}/${_spark_directory}")
            continue()
        endif()
        # The tracked-install helper matches "/<path below SOURCE>".
        set(_spark_directory_regexes "")
        set(_spark_directory_excluded FALSE)
        foreach(_spark_exclusion IN LISTS _spark_exclusions)
            string(FIND "${_spark_exclusion}" "${_spark_directory}/" _spark_prefix_index)
            if(NOT _spark_prefix_index EQUAL 0)
                continue()
            endif()
            string(LENGTH "${_spark_directory}/" _spark_prefix_length)
            string(SUBSTRING "${_spark_exclusion}" ${_spark_prefix_length} -1 _spark_below)
            if(_spark_below STREQUAL "")
                set(_spark_directory_excluded TRUE)
                break()
            endif()
            string(REGEX REPLACE "([][.*+?^$()|\\\\])" "\\\\\\1" _spark_escaped "${_spark_below}")
            if(_spark_below MATCHES "/$")
                list(APPEND _spark_directory_regexes "^/${_spark_escaped}")
            else()
                list(APPEND _spark_directory_regexes "^/${_spark_escaped}$")
            endif()
        endforeach()
        if(_spark_directory_excluded)
            continue()
        endif()
        if(_spark_directory_regexes)
            spark_install_tracked_directory(
                SOURCE "${SPARK_ASSETS_ROOT}/${_spark_directory}"
                DESTINATION "${SPARK_ASSETS_DESTINATION}/${_spark_directory}"
                COMPONENT ${SPARK_ASSETS_COMPONENT}
                EXCLUDE_REGEXES ${_spark_directory_regexes})
        else()
            spark_install_tracked_directory(
                SOURCE "${SPARK_ASSETS_ROOT}/${_spark_directory}"
                DESTINATION "${SPARK_ASSETS_DESTINATION}/${_spark_directory}"
                COMPONENT ${SPARK_ASSETS_COMPONENT})
        endif()
    endforeach()

    foreach(_spark_root_file IN LISTS SPARK_ASSETS_ROOT_FILES)
        if(_spark_root_file IN_LIST _spark_exclusions OR
           NOT EXISTS "${_spark_asset_root}/${_spark_root_file}")
            continue()
        endif()
        install(FILES "${_spark_asset_root}/${_spark_root_file}"
            DESTINATION "${SPARK_ASSETS_DESTINATION}"
            COMPONENT ${SPARK_ASSETS_COMPONENT})
    endforeach()
    if(EXISTS "${_spark_installed_manifest}")
        install(FILES "${_spark_installed_manifest}"
            DESTINATION "${SPARK_ASSETS_DESTINATION}"
            COMPONENT ${SPARK_ASSETS_COMPONENT})
    endif()
endfunction()
