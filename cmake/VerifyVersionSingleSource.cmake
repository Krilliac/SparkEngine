cmake_minimum_required(VERSION 3.25)

# Assert that every project template declares the engine version the build is
# actually producing.
#
# SPARK_ENGINE_VERSION is a cache variable the release pipeline overrides from
# the git tag, but the nine Templates/*.sparkproject files hard-code
# "engineVersion" and are installed verbatim. Without this gate a 1.1.0 package
# shipped templates announcing 1.0.0 and nothing failed.

foreach(_required SPARK_TEMPLATE_ROOT SPARK_EXPECTED_VERSION)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "VerifyVersionSingleSource.cmake requires -D${_required}=<value>")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${SPARK_TEMPLATE_ROOT}")
    message(FATAL_ERROR "Template root does not exist: ${SPARK_TEMPLATE_ROOT}")
endif()

file(GLOB _spark_project_files "${SPARK_TEMPLATE_ROOT}/*/*.sparkproject")
list(LENGTH _spark_project_files _spark_project_count)
if(_spark_project_count EQUAL 0)
    message(FATAL_ERROR
        "No .sparkproject files found under ${SPARK_TEMPLATE_ROOT}; the version gate would pass vacuously")
endif()

set(_spark_mismatches "")
foreach(_spark_project IN LISTS _spark_project_files)
    file(READ "${_spark_project}" _spark_contents)
    if(NOT _spark_contents MATCHES "\"engineVersion\"[ \t]*:[ \t]*\"([^\"]*)\"")
        list(APPEND _spark_mismatches "${_spark_project}: no engineVersion field")
        continue()
    endif()
    set(_spark_declared "${CMAKE_MATCH_1}")
    if(NOT _spark_declared STREQUAL "${SPARK_EXPECTED_VERSION}")
        list(APPEND _spark_mismatches
            "${_spark_project}: engineVersion '${_spark_declared}' != project version '${SPARK_EXPECTED_VERSION}'")
    endif()
endforeach()

if(_spark_mismatches)
    string(REPLACE ";" "\n  " _spark_report "${_spark_mismatches}")
    message(FATAL_ERROR
        "Template engineVersion drifted from the engine version being built:\n  ${_spark_report}")
endif()

message(STATUS
    "Version single source: ${_spark_project_count} template(s) declare ${SPARK_EXPECTED_VERSION}")
