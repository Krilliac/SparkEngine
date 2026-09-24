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
#   -ffile-prefix-map GCC/Clang equivalent for __FILE__ and debug info.
#
# Debug keeps absolute paths and /INCREMENTAL for local debugging. clang-cl is
# intentionally not configured here: it is not a supported stable-v1 toolchain.
# Tests/Tools/test_build_shipping_contract.py pins these flags; equivalence of
# two clean builds is proven only by the reproducibility-windows CI job.

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
    add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-ffile-prefix-map=${CMAKE_SOURCE_DIR}/=>")
    message(STATUS "Build    : reproducible optimized outputs (-ffile-prefix-map source root)")
endif()
