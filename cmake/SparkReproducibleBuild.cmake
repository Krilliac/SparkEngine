# SparkReproducibleBuild.cmake -- reproducible optimized outputs (BLD-100).
#
# Included from the root CMakeLists.txt after the per-configuration compiler
# flags and before any target is declared, so every first-party and
# third-party target created afterwards inherits these directory options.
#
# Two clean builds of identical inputs must yield comparable Shipping images.
# Without these flags every optimized image and archive carries a link-time
# timestamp, and every __FILE__ (ASSERT, logging, crash context) bakes the
# build machine's absolute checkout path into the shipped binary.
#
#   /Brepro           replace PE/COFF timestamps with a content hash in
#                     compiler objects, linker images, and lib.exe archives.
#   /d1trimfile:<dir> strip the source-root prefix from __FILE__.
#   -ffile-prefix-map GCC/Clang equivalent for __FILE__ and debug info, for the
#                     source root and the build root (DWARF comp_dir, generated
#                     sources, precompiled headers).
#   -frandom-seed     GCC with LTO: deterministic LTO IR section names.
#
# Debug keeps absolute paths and /INCREMENTAL for local debugging. clang-cl is
# intentionally not configured here: it is not a supported stable-v1 toolchain.
# Tests/Tools/test_build_shipping_contract.py pins these flags.
# tools/compare_build_outputs.py compares two builds: the
# ReproducibleBuild_LinuxToolTargets CTest builds SparkCooker in two source and
# build trees on Linux; Windows equivalence is proven only by the
# reproducibility-windows CI job.

include_guard(GLOBAL)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" SPARK_REPRO_SOURCE_ROOT)
    add_compile_options(
        $<$<NOT:$<CONFIG:Debug>>:/Brepro>
        "$<$<NOT:$<CONFIG:Debug>>:/d1trimfile:${SPARK_REPRO_SOURCE_ROOT}>"
    )
    add_link_options($<$<NOT:$<CONFIG:Debug>>:/Brepro>)
    foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND CMAKE_STATIC_LINKER_FLAGS_${config} " /Brepro")
    endforeach()
    message(STATUS "Build    : reproducible optimized outputs (/Brepro, /d1trimfile source root)")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang|IntelLLVM" AND NOT MSVC)
    # The build-directory map comes second: GCC and Clang apply the last
    # matching map, so it wins when the build tree sits inside the source tree.
    # Every compile runs in its target's binary directory, which the compilers
    # record as the DWARF comp_dir. Under GCC LTO the LTRANS units are compiled
    # at link time in the linking target's binary directory, so GCC also gets
    # the maps as link options. Clang records its compile units in the bitcode
    # at compile time, and would warn about the unused link-time flag.
    add_compile_options(
        "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>"
        "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.>"
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND ENABLE_LTO)
        # GCC names the LTO IR sections of every object with a seed drawn from
        # the clock and pid, so static-library members differed between two
        # builds of one tree. Seeding with the object's build-relative path
        # (<OBJECT>) makes the IR deterministic and still unique per object.
        foreach(_spark_repro_lang C CXX)
            if(DEFINED CMAKE_${_spark_repro_lang}_COMPILE_OBJECT)
                string(APPEND CMAKE_${_spark_repro_lang}_COMPILE_OBJECT " -frandom-seed=<OBJECT>")
            endif()
        endforeach()
        add_link_options(
            "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>"
            "$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.>"
        )
    endif()
    message(STATUS "Build    : reproducible optimized outputs (-ffile-prefix-map source and build roots)")
endif()
