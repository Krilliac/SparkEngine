# Initial cache (cmake -C) for the MSan-instrumented libc++/libc++abi that
# build-linux-msan builds from the LLVM release tarball (build.yml, LLVM_TAG).
# hashFiles() of this file is part of the prefix cache key: any edit here
# rebuilds and re-caches the runtime. Mirrors upstream
# libcxx/cmake/caches/Generic-msan.cmake (LLVM_USE_SANITIZER=MemoryWithOrigins,
# LIBCXXABI_USE_LLVM_UNWINDER=OFF) plus:
#   - -fsanitize-recover=memory so halt_on_error is honoured for checks
#     whose site is out-of-line libc++ code (otherwise those Die() regardless);
#   - shared-only, no tests/benchmarks, flat include/c++/v1 + lib layout.
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "")
set(CMAKE_C_FLAGS "-fsanitize-recover=memory" CACHE STRING "")
set(CMAKE_CXX_FLAGS "-fsanitize-recover=memory" CACHE STRING "")
set(LLVM_ENABLE_RUNTIMES "libcxx;libcxxabi" CACHE STRING "")
set(LLVM_USE_SANITIZER MemoryWithOrigins CACHE STRING "")
set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "")
set(LLVM_ENABLE_PER_TARGET_RUNTIME_DIR OFF CACHE BOOL "")
set(LIBCXX_CXX_ABI libcxxabi CACHE STRING "")
set(LIBCXX_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXX_INCLUDE_BENCHMARKS OFF CACHE BOOL "")
set(LIBCXX_ENABLE_STATIC OFF CACHE BOOL "")
set(LIBCXXABI_ENABLE_STATIC OFF CACHE BOOL "")
set(LIBCXXABI_INCLUDE_TESTS OFF CACHE BOOL "")
set(LIBCXXABI_USE_LLVM_UNWINDER OFF CACHE BOOL "")
