# SparkLibsodium.cmake -- builds the vendored libsodium submodule as the static
# target `spark_sodium` (NET-100, owner decision OD-06).
#
# Upstream ships autotools, MSVC solutions and a Zig build, but no CMake build.
# This wrapper compiles every C file under src/libsodium the way the upstream
# MSVC projects and build.zig do, with these deliberate differences:
#   * no assembly: HAVE_AMD64_ASM / HAVE_AVX_ASM stay undefined, so the portable
#     C and intrinsics implementations are used on every platform;
#   * no x86 implementation above the stable-v1 CPU floor (OD-04) is compiled on
#     any toolchain (AVX, AVX2, AVX-512, AES-NI/PCLMUL);
#   * feature probes (HAVE_*) use check_symbol_exists / check_c_source_compiles
#     with configure.ac's own probe programs instead of autoconf;
#   * include/sodium/version.h is generated from version.h.in into the build tree.
#
# There is no fallback: NetworkEncryption.cpp has no ENABLE_NETWORKING guard and
# compiles in every configuration (windows-shipping sets ENABLE_NETWORKING=OFF),
# so a missing submodule fails configuration instead of silently building an
# engine whose transport cannot encrypt.

get_filename_component(SPARK_LIBSODIUM_ROOT "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/Security/libsodium" ABSOLUTE)
set(_spark_sodium_src "${SPARK_LIBSODIUM_ROOT}/src/libsodium")

if(NOT EXISTS "${_spark_sodium_src}/include/sodium.h")
    message(FATAL_ERROR
        "SparkEngine requires libsodium (NET-100 / OD-06), but "
        "${_spark_sodium_src}/include/sodium.h is missing.\n"
        "Run: git submodule update --init ThirdParty/Security/libsodium")
endif()

# The library version comes from the pinned configure.ac, the same source the
# upstream autotools build substitutes into version.h.in. A function scope keeps
# the template's generic variable names (VERSION, ...) out of the caller.
function(_spark_sodium_write_version_header source_root output_dir)
    file(READ "${source_root}/configure.ac" _configure_ac)
    if(NOT _configure_ac MATCHES "AC_INIT\\(\\[libsodium\\],\\[([0-9.]+)\\]")
        message(FATAL_ERROR "Cannot read the libsodium version from ${source_root}/configure.ac")
    endif()
    set(VERSION "${CMAKE_MATCH_1}")
    if(NOT _configure_ac MATCHES "\nSODIUM_LIBRARY_VERSION_MAJOR=([0-9]+)")
        message(FATAL_ERROR "Cannot read SODIUM_LIBRARY_VERSION_MAJOR from ${source_root}/configure.ac")
    endif()
    set(SODIUM_LIBRARY_VERSION_MAJOR "${CMAKE_MATCH_1}")
    if(NOT _configure_ac MATCHES "\nSODIUM_LIBRARY_VERSION_MINOR=([0-9]+)")
        message(FATAL_ERROR "Cannot read SODIUM_LIBRARY_VERSION_MINOR from ${source_root}/configure.ac")
    endif()
    set(SODIUM_LIBRARY_VERSION_MINOR "${CMAKE_MATCH_1}")
    set(SODIUM_LIBRARY_MINIMAL_DEF "")
    configure_file("${source_root}/src/libsodium/include/sodium/version.h.in"
        "${output_dir}/sodium/version.h" @ONLY)
    # version.h includes "export.h" by quote, which resolves next to the
    # generated file; copying it avoids exposing include/sodium/ (core.h,
    # utils.h, ...) as a public include directory.
    configure_file("${source_root}/src/libsodium/include/sodium/export.h"
        "${output_dir}/sodium/export.h" COPYONLY)
    set(SPARK_LIBSODIUM_VERSION "${VERSION}" PARENT_SCOPE)
endfunction()

set(_spark_sodium_gen_include "${CMAKE_BINARY_DIR}/ThirdParty/libsodium/include")
_spark_sodium_write_version_header("${SPARK_LIBSODIUM_ROOT}" "${_spark_sodium_gen_include}")

file(GLOB_RECURSE _spark_sodium_sources CONFIGURE_DEPENDS "${_spark_sodium_src}/*.c")
add_library(spark_sodium STATIC ${_spark_sodium_sources})
set_target_properties(spark_sodium PROPERTIES LINKER_LANGUAGE C)

# The generated version.h must win over any stale copy inside the submodule.
target_include_directories(spark_sodium
    PUBLIC
        $<BUILD_INTERFACE:${_spark_sodium_gen_include}>
        $<BUILD_INTERFACE:${_spark_sodium_src}/include>
    PRIVATE
        # Sources include their siblings bare ("version.h", "core.h", ...).
        "${_spark_sodium_gen_include}/sodium"
        "${_spark_sodium_src}/include/sodium")
target_compile_definitions(spark_sodium
    PUBLIC SODIUM_STATIC=1
    PRIVATE CONFIGURED=1 SODIUM_EXPORT=)

include(TestBigEndian)
test_big_endian(_spark_sodium_big_endian)
if(_spark_sodium_big_endian)
    target_compile_definitions(spark_sodium PRIVATE NATIVE_BIG_ENDIAN=1)
else()
    target_compile_definitions(spark_sodium PRIVATE NATIVE_LITTLE_ENDIAN=1)
endif()

if(MSVC)
    # private/common.h enables the MSVC intrinsics headers itself, and on x64 it
    # unconditionally defines HAVE_AVXINTRIN_H, HAVE_WMMINTRIN_H,
    # HAVE_AVX2INTRIN_H and HAVE_AVX512FINTRIN_H (common.h lines 240-260 at the
    # pinned revision), so a compile definition cannot turn them off. The impl
    # files cannot simply be left out either: the dispatchers reference them
    # under the same macros. Instead every source force-includes a header that
    # includes common.h first (its include guard makes each source's own include
    # a no-op) and then undefines those four macros. The AVX/AVX2/AVX-512 and
    # AES-NI/PCLMUL variants then compile empty, the dispatchers never select
    # them, and runtime.c reports none of those features. The remaining set is
    # MMX/SSE2/SSE3/SSSE3/SSE4.1, the same as the non-MSVC branch below, within
    # the stable-v1 x86-64 floor (OD-04: SSE4.2, no AVX). Under LTCG the variants'
    # VEX code was otherwise inlined into SecureChannel::Seal/Open and
    # PasswordHash, where no reviewed runtime-dispatch range could bound it.
    # CpuFloor_LibsodiumVariants fails on any AVX2/AES-NI host if one returns.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x86|X86|i[3-6]86)$")
        set(_spark_sodium_floor_header "${_spark_sodium_gen_include}/spark_sodium_cpu_floor.h")
        file(CONFIGURE OUTPUT "${_spark_sodium_floor_header}" CONTENT [[
/* Generated by cmake/SparkLibsodium.cmake: keep libsodium within the OD-04 CPU floor on MSVC. */
#include "private/common.h"
#undef HAVE_AVXINTRIN_H
#undef HAVE_WMMINTRIN_H
#undef HAVE_AVX2INTRIN_H
#undef HAVE_AVX512FINTRIN_H
]])
        target_compile_options(spark_sodium PRIVATE "/FI${_spark_sodium_floor_header}")
    endif()
    target_compile_definitions(spark_sodium PRIVATE _CRT_SECURE_NO_WARNINGS inline=__inline)
    target_compile_options(spark_sodium PRIVATE /W0)
    set_property(TARGET spark_sodium PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    include(CheckIncludeFile)
    include(CheckSymbolExists)
    include(CheckCSourceCompiles)

    set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE=1)
    set(_spark_sodium_probes
        "sys/mman.h|mmap|HAVE_MMAP"
        "sys/mman.h|mprotect|HAVE_MPROTECT"
        "sys/mman.h|mlock|HAVE_MLOCK"
        "sys/mman.h|madvise|HAVE_MADVISE"
        "stdlib.h|posix_memalign|HAVE_POSIX_MEMALIGN"
        "unistd.h|sysconf|HAVE_SYSCONF"
        "unistd.h|getpid|HAVE_GETPID"
        "signal.h|raise|HAVE_RAISE"
        "time.h|nanosleep|HAVE_NANOSLEEP"
        "string.h|explicit_bzero|HAVE_EXPLICIT_BZERO"
        "sys/random.h|getrandom|HAVE_GETRANDOM"
        "unistd.h|getentropy|HAVE_GETENTROPY")
    foreach(_probe IN LISTS _spark_sodium_probes)
        string(REPLACE "|" ";" _probe "${_probe}")
        list(GET _probe 0 _probe_header)
        list(GET _probe 1 _probe_symbol)
        list(GET _probe 2 _probe_macro)
        check_symbol_exists(${_probe_symbol} ${_probe_header} SPARK_SODIUM_${_probe_macro})
        if(SPARK_SODIUM_${_probe_macro})
            target_compile_definitions(spark_sodium PRIVATE ${_probe_macro}=1)
        endif()
    endforeach()
    foreach(_header sys/mman.h sys/param.h sys/random.h sys/auxv.h)
        string(TOUPPER "HAVE_${_header}" _header_macro)
        string(REGEX REPLACE "[/.]" "_" _header_macro "${_header_macro}")
        check_include_file(${_header} SPARK_SODIUM_${_header_macro})
        if(SPARK_SODIUM_${_header_macro})
            target_compile_definitions(spark_sodium PRIVATE ${_header_macro}=1)
        endif()
    endforeach()
    unset(CMAKE_REQUIRED_DEFINITIONS)

    check_c_source_compiles("
        #ifndef __SIZEOF_INT128__
        #error no int128
        #endif
        int main(void) { unsigned __int128 x = 1; return (int) (x >> 64); }"
        SPARK_SODIUM_HAVE_TI_MODE)
    if(SPARK_SODIUM_HAVE_TI_MODE)
        target_compile_definitions(spark_sodium PRIVATE HAVE_TI_MODE=1)
    endif()

    # Hardening and barrier macros. Upstream build.zig (initLibConfig) defines
    # HAVE_ATOMIC_OPS, HAVE_C11_MEMORY_FENCES, HAVE_GCC_MEMORY_FENCES and
    # HAVE_INLINE_ASM for every target, and HAVE_WEAK_SYMBOLS for Linux, macOS
    # and WASI; configure.ac probes each of them with AC_LINK_IFELSE ("whether
    # we can use inline asm code", "if weak symbols are supported", "if atomic
    # operations are supported", "if C11 memory fences are supported", "if gcc
    # memory fences are supported"). The probes below are those programs.
    # Without the fence macros private/common.h turns ACQUIRE_FENCE into
    # (void) 0, dropping the barrier between tag verification and decryption in
    # crypto_aead_chacha20poly1305_ietf_decrypt_detached (the AEAD open used by
    # NetworkEncryption.cpp). Without HAVE_INLINE_ASM the compiler barriers in
    # sodium_memzero, the softaes table lookups and the ML-KEM cmov disappear.
    # MSVC is unaffected: upstream's MSVC projects define none of these.
    check_c_source_compiles("
        #ifdef __FILC__
        # error inline assembly is not supported with FilC
        #endif
        int main(void) {
            int a = 42;
            int *pnt = &a;
            __asm__ __volatile__ (\"\" : : \"r\"(pnt) : \"memory\");
            return 0;
        }"
        SPARK_SODIUM_HAVE_INLINE_ASM)
    check_c_source_compiles("
        #if !defined(__ELF__) && !defined(__APPLE_CC__)
        # error Support for weak symbols may not be available
        #endif
        __attribute__((weak)) void __dummy(void *x) { (void) x; }
        void f(void *x) { __dummy(x); }
        int main(void) { f((void *) 0); return 0; }"
        SPARK_SODIUM_HAVE_WEAK_SYMBOLS)
    check_c_source_compiles("
        int main(void) {
            static volatile int _sodium_lock;
            __sync_lock_test_and_set(&_sodium_lock, 1);
            __sync_lock_release(&_sodium_lock);
            return 0;
        }"
        SPARK_SODIUM_HAVE_ATOMIC_OPS)
    check_c_source_compiles("
        #include <stdatomic.h>
        int main(void) { atomic_thread_fence(memory_order_acquire); return 0; }"
        SPARK_SODIUM_HAVE_C11_MEMORY_FENCES)
    check_c_source_compiles("
        int main(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); return 0; }"
        SPARK_SODIUM_HAVE_GCC_MEMORY_FENCES)
    foreach(_macro HAVE_INLINE_ASM HAVE_WEAK_SYMBOLS HAVE_ATOMIC_OPS HAVE_C11_MEMORY_FENCES HAVE_GCC_MEMORY_FENCES)
        if(SPARK_SODIUM_${_macro})
            target_compile_definitions(spark_sodium PRIVATE ${_macro}=1)
        endif()
    endforeach()
    # The AEAD open path depends on the acquire fence: refuse a GCC/Clang build
    # in which neither fence form is available rather than ship it silently.
    if(NOT SPARK_SODIUM_HAVE_C11_MEMORY_FENCES AND NOT SPARK_SODIUM_HAVE_GCC_MEMORY_FENCES)
        message(FATAL_ERROR "libsodium needs C11 or GCC memory fences (ACQUIRE_FENCE); neither probe compiled")
    endif()

    # sodium_init()'s critical section uses pthreads on POSIX (Win32 primitives
    # on MinGW, where SparkEngineConfig does not re-find Threads).
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_compile_definitions(spark_sodium PRIVATE HAVE_PTHREAD=1)
        target_link_libraries(spark_sodium PRIVATE Threads::Threads)
    endif()

    # Each SIMD implementation enables its own ISA through target pragmas and is
    # selected at run time from CPUID, as in upstream build.zig. Only the
    # implementations within the stable-v1 x86-64 floor (OD-04: SSE4.2, no AVX)
    # are compiled: CpuFloor_IsaBaseline rejects any above-floor instruction in a
    # shipped image, even behind a CPUID check. That drops the AVX/AVX2/AVX-512 and
    # AES-NI/PCLMUL (wmmintrin) variants; SparkEngine uses ChaCha20-Poly1305,
    # X25519, Ed25519 and SHA-256, whose SSSE3/SSE4.1 or portable paths remain.
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$")
        target_compile_definitions(spark_sodium PRIVATE
            HAVE_CPUID=1 HAVE_MMINTRIN_H=1 HAVE_EMMINTRIN_H=1 HAVE_PMMINTRIN_H=1
            HAVE_TMMINTRIN_H=1 HAVE_SMMINTRIN_H=1)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
        target_compile_definitions(spark_sodium PRIVATE HAVE_ARMCRYPTO=1)
    endif()

    # Upstream's recommended code-generation flags; warnings are upstream's to
    # fix, so the vendored target is compiled silently.
    # Keep the cryptographic dependency optimized in Debug, as upstream's
    # configure.ac does by default. At -O0 the 600,000-round password verifier
    # exceeds the production authenticator's 2 s deadline on CI runners.
    # Debug symbols and the engine's own Debug code remain enabled.
    target_compile_options(spark_sodium PRIVATE
        -w -fvisibility=hidden -fno-strict-aliasing -fno-strict-overflow -fwrapv
        -flax-vector-conversions $<$<CONFIG:Debug>:-O2>)
    set_property(TARGET spark_sodium PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

message(STATUS "Found libsodium ${SPARK_LIBSODIUM_VERSION} at: ${SPARK_LIBSODIUM_ROOT}")
