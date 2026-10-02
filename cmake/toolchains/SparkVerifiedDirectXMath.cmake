# SparkVerifiedDirectXMath.cmake — fetch the pinned DirectXMath archive for the MinGW toolchain.
#
# Every byte that reaches the include path is tied to the pinned SHA-256, on every configure:
#   - the download goes to "<zip>.part" with TLS verification on, and only a completed download is renamed to
#     the cache path; a failed or mismatched download never becomes the cached archive;
#   - the cached archive is re-hashed before each use, and a mismatch deletes it and its extraction;
#   - the extraction is trusted only while its stamp names the pinned hash, otherwise it is wiped and redone.
# Failures are fatal: a configure that cannot prove the headers stops rather than building against them.
#
# Script-mode safe (no targets, no include_directories) so Tests/Tools/test_mingw_dxmath_fetch.py can drive
# it with `cmake -P`.

# Sets <out_inc_dir> in the caller to the verified DirectXMath Inc/ directory.
#   cache_dir        directory that holds "<archive_name>" and "extract/"
#   url              archive URL (the toolchain passes the pinned GitHub tag archive)
#   expected_sha256  lowercase hex SHA-256 of the archive
#   archive_root     top-level directory inside the archive (e.g. DirectXMath-oct2024)
function(spark_fetch_verified_directxmath cache_dir url expected_sha256 archive_root out_inc_dir)
    string(TOLOWER "${expected_sha256}" _expected)
    if(NOT _expected MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "DirectXMath fetch: pinned hash '${expected_sha256}' is not a SHA-256")
    endif()
    string(LENGTH "${_expected}" _expected_length)
    if(NOT _expected_length EQUAL 64)
        message(FATAL_ERROR "DirectXMath fetch: pinned hash '${expected_sha256}' is not a SHA-256")
    endif()

    set(_zip "${cache_dir}/${archive_root}.zip")
    set(_partial "${_zip}.part")
    set(_extract_dir "${cache_dir}/extract")
    set(_stamp "${_extract_dir}/.verified-sha256")
    set(_inc "${_extract_dir}/${archive_root}/Inc")

    # A cached archive earns no trust from existing: re-hash it and discard it on mismatch.
    if(EXISTS "${_zip}")
        file(SHA256 "${_zip}" _cached_hash)
        if(NOT _cached_hash STREQUAL _expected)
            message(STATUS "DirectXMath fetch: cached archive ${_zip} does not match the pinned hash; discarding it")
            file(REMOVE "${_zip}")
            file(REMOVE_RECURSE "${_extract_dir}")
        endif()
    endif()

    if(NOT EXISTS "${_zip}")
        message(STATUS "DirectXMath fetch: downloading ${url}")
        file(MAKE_DIRECTORY "${cache_dir}")
        file(REMOVE "${_partial}")
        file(DOWNLOAD "${url}" "${_partial}" TLS_VERIFY ON STATUS _status)
        list(GET _status 0 _rc)
        if(NOT _rc EQUAL 0)
            file(REMOVE "${_partial}")
            message(FATAL_ERROR "DirectXMath fetch: download failed (${_status}). Drop the headers into "
                                "cmake/mingw-shims/ manually, see its README.md.")
        endif()
        file(SHA256 "${_partial}" _downloaded_hash)
        if(NOT _downloaded_hash STREQUAL _expected)
            file(REMOVE "${_partial}")
            message(FATAL_ERROR "DirectXMath fetch: downloaded archive hash ${_downloaded_hash} does not match "
                                "the pinned ${_expected}; the archive was discarded")
        endif()
        file(RENAME "${_partial}" "${_zip}")
        file(REMOVE_RECURSE "${_extract_dir}")
    endif()

    # Re-extract unless the stamp proves this extraction came from the pinned archive.
    set(_stamped_hash "")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _stamped_hash)
        string(STRIP "${_stamped_hash}" _stamped_hash)
    endif()
    if(NOT _stamped_hash STREQUAL _expected OR NOT EXISTS "${_inc}/DirectXMath.h")
        file(REMOVE_RECURSE "${_extract_dir}")
        file(MAKE_DIRECTORY "${_extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${_extract_dir}")
        if(NOT EXISTS "${_inc}/DirectXMath.h")
            file(REMOVE_RECURSE "${_extract_dir}")
            message(FATAL_ERROR "DirectXMath fetch: verified archive has no ${archive_root}/Inc/DirectXMath.h")
        endif()
        file(WRITE "${_stamp}" "${_expected}\n")
    endif()

    set(${out_inc_dir} "${_inc}" PARENT_SCOPE)
endfunction()
