# PLT-200: stage the Windows MSI component set into a fresh prefix and walk its
# PE import and delay-import tables against each Windows row plan's declared
# dependencyClosure and firstPartyImages (Tools/platform-cert/pe_imports.py).
#
# The package checker requires app-local CRT DLLs; the certification checker
# compares the measured graph with the row declaration. Both must pass. The
# staged set is the one cmake/SparkCPackOptions.cmake
# puts in the MSI (runtime, redist, tools, samples), so the walk sees what an
# installed product holds rather than the build tree.

foreach(_required IN ITEMS SPARK_ENGINE_BUILD_DIR SPARK_SOURCE_ROOT SPARK_CONFIG SPARK_TEST_ROOT
                           SPARK_PYTHON_EXECUTABLE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required for the Windows package closure test")
    endif()
endforeach()
if(NOT SPARK_CONFIG STREQUAL "MinSizeRel")
    message(FATAL_ERROR "The Windows Shipping closure requires SPARK_CONFIG=MinSizeRel")
endif()
if(NOT IS_ABSOLUTE "${SPARK_TEST_ROOT}" OR "${SPARK_TEST_ROOT}" MATCHES "[\r\n;]")
    message(FATAL_ERROR "SPARK_TEST_ROOT must be a safe absolute path")
endif()

# A fresh prefix per run, so a DLL left behind by an earlier stage can never
# satisfy an import the current package no longer ships.
cmake_path(SET _test_root NORMALIZE "${SPARK_TEST_ROOT}")
string(TIMESTAMP _run_timestamp "%Y%m%dT%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_nonce)
set(_run_root "${_test_root}/run-${_run_timestamp}-${_run_nonce}")
if(EXISTS "${_run_root}" OR IS_SYMLINK "${_run_root}")
    message(FATAL_ERROR "Generated package run already exists: ${_run_root}")
endif()
set(_install_root "${_run_root}/install")
file(MAKE_DIRECTORY "${_install_root}")
message(STATUS "Windows Shipping closure reports: ${_run_root}")

# Consume the native installer's component definition, so a packaging change
# cannot silently leave this measurement checking the old component set.
set(CPACK_GENERATOR WIX)
include("${SPARK_SOURCE_ROOT}/cmake/SparkCPackOptions.cmake")
if(NOT CPACK_COMPONENTS_ALL)
    message(FATAL_ERROR "The Windows installer component set is empty")
endif()
foreach(_component IN LISTS CPACK_COMPONENTS_ALL)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${SPARK_ENGINE_BUILD_DIR}"
            --config "${SPARK_CONFIG}" --prefix "${_install_root}" --component ${_component}
        RESULT_VARIABLE _install_result
        OUTPUT_VARIABLE _install_output
        ERROR_VARIABLE _install_error
        TIMEOUT 300)
    file(WRITE "${_run_root}/install-${_component}.log" "${_install_output}\n${_install_error}")
    if(NOT "${_install_result}" STREQUAL "0")
        message(FATAL_ERROR
            "Installing the ${_component} component failed (${_install_result}):\n"
            "${_install_output}\n${_install_error}")
    endif()
endforeach()

# The general certification authority permits a host-installed VC runtime.
# Shipping requires the redistributable beside the executable, even on a
# developer/CI host that already has it. This existing ENG-220 check also
# requires OS imports to exist in System32 on Windows and never consults PATH.
execute_process(
    COMMAND "${SPARK_PYTHON_EXECUTABLE}" -B "${SPARK_SOURCE_ROOT}/tools/pe_import_closure.py"
        --list "${_install_root}/bin"
    RESULT_VARIABLE _package_result
    OUTPUT_VARIABLE _package_output
    ERROR_VARIABLE _package_error
    TIMEOUT 300)
file(WRITE "${_run_root}/package-imports.log" "${_package_output}\n${_package_error}")
if(NOT "${_package_result}" STREQUAL "0")
    message(FATAL_ERROR
        "The Shipping package has unresolved runtime dependencies (${_package_result}):\n"
        "${_package_output}\n${_package_error}")
endif()

foreach(_row IN ITEMS win11-x64-msvc143-d3d11 win11-x64-msvc143-nullrhi)
    execute_process(
        COMMAND "${SPARK_PYTHON_EXECUTABLE}" -B "${SPARK_SOURCE_ROOT}/Tools/platform-cert/pe_imports.py"
            --package-root "${_install_root}/bin"
            --plan "${SPARK_SOURCE_ROOT}/docs/certification/plans/${_row}.json"
        RESULT_VARIABLE _closure_result
        OUTPUT_FILE "${_run_root}/${_row}.import-graph.json"
        ERROR_VARIABLE _closure_report
        TIMEOUT 300)
    file(WRITE "${_run_root}/${_row}.log" "${_closure_report}")
    if(NOT "${_closure_result}" STREQUAL "0")
        message(FATAL_ERROR
            "The staged package's import graph disagrees with the ${_row} plan (${_closure_result}):\n"
            "${_closure_report}")
    endif()
    string(STRIP "${_closure_report}" _closure_report)
    message(STATUS "${_row}: ${_closure_report}")
endforeach()

# Retain the hashed import graphs and logs on success; retain the stage too on
# failure. Resolve and check the owned child immediately before deleting it.
file(REAL_PATH "${_run_root}" _real_run_root)
file(REAL_PATH "${_install_root}" _real_install_root)
cmake_path(IS_PREFIX _real_run_root "${_real_install_root}" NORMALIZE _owned_install)
if(NOT _owned_install OR _real_run_root STREQUAL _real_install_root OR IS_SYMLINK "${_install_root}")
    message(FATAL_ERROR "Refusing to remove a package stage outside its run directory")
endif()
file(REMOVE_RECURSE "${_install_root}")
