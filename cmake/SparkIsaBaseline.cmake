# Register the BLD-100 ISA floor scan for real MSVC PE images.
#
# The image list is supplied by the root SPARK_SHIPPED_IMAGE_TARGETS inventory.
# Each image carries its own TARGET_PDB_FILE generator expression so a scan can
# never accidentally pair an image with a PDB from another configuration.

include("${CMAKE_CURRENT_LIST_DIR}/SparkCpuFloor.cmake")

function(spark_register_isa_baseline_scan)
    spark_cpu_floor_enforced(_spark_isa_floor_enforced)
    if(NOT WIN32 OR NOT MSVC OR NOT _spark_isa_floor_enforced)
        return()
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(_spark_isa_images)
    set(_spark_isa_pdb_pairs)
    set(_spark_isa_dependencies)
    foreach(_spark_isa_target IN LISTS ARGN)
        if(NOT TARGET ${_spark_isa_target})
            continue()
        endif()
        get_target_property(_spark_isa_type ${_spark_isa_target} TYPE)
        get_target_property(_spark_isa_imported ${_spark_isa_target} IMPORTED)
        if(_spark_isa_imported OR
           NOT _spark_isa_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
            continue()
        endif()
        list(APPEND _spark_isa_images "$<TARGET_FILE:${_spark_isa_target}>")
        list(APPEND _spark_isa_pdb_pairs
            "$<TARGET_FILE:${_spark_isa_target}>=$<TARGET_PDB_FILE:${_spark_isa_target}>")
        list(APPEND _spark_isa_dependencies ${_spark_isa_target})
    endforeach()

    if(NOT _spark_isa_images)
        message(FATAL_ERROR
            "BLD-100 ISA scan has no configured shipped PE image targets; refusing a vacuous pass")
    endif()

    set(_spark_isa_command
        ${Python3_EXECUTABLE} -B "${CMAKE_SOURCE_DIR}/tools/check_isa_baseline.py"
        --objdump llvm-objdump --pdbutil llvm-pdbutil)
    foreach(_spark_isa_pdb_pair IN LISTS _spark_isa_pdb_pairs)
        list(APPEND _spark_isa_command --pdb "${_spark_isa_pdb_pair}")
    endforeach()
    list(APPEND _spark_isa_command ${_spark_isa_images})

    add_custom_target(CpuFloor_IsaBaseline
        COMMAND ${_spark_isa_command}
        DEPENDS ${_spark_isa_dependencies}
        VERBATIM
        COMMENT "Checking shipped Windows PE images against the SSE4.2 + POPCNT CPU floor")

    if(BUILD_TESTS)
        add_test(NAME CpuFloor_IsaBaseline COMMAND ${_spark_isa_command})
        # "static", not "integration": the scan disassembles every shipped image
        # without running it. The CTest policy counts an integration-labelled test
        # that names a binary as that binary's behavioural lane, so an
        # integration label here would hide every missing runtime lane.
        set_tests_properties(CpuFloor_IsaBaseline PROPERTIES
            LABELS "build;shipping;static"
            RUN_SERIAL TRUE
            TIMEOUT 600)
    endif()
endfunction()
