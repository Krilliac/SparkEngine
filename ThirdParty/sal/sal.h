/**
 * @file sal.h
 * @brief Minimal SAL (Source Annotation Language) stubs for non-MSVC compilers
 *
 * Microsoft's DirectXMath headers use SAL annotations for static analysis.
 * These are no-ops on GCC/Clang — this shim provides empty macros so the
 * headers compile without modification.
 *
 * Located in ThirdParty/sal/ so that DirectXMath.h (which does
 * #include "sal.h") can find it via the compiler include path without
 * modifying the DirectXMath submodule.
 */

#pragma once

#if !defined(_MSC_VER)

#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _In_reads_
#define _In_reads_(s)
#endif
#ifndef _In_reads_opt_
#define _In_reads_opt_(s)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(s)
#endif
#ifndef _In_reads_bytes_opt_
#define _In_reads_bytes_opt_(s)
#endif
#ifndef _Out_writes_
#define _Out_writes_(s)
#endif
#ifndef _Out_writes_opt_
#define _Out_writes_opt_(s)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(s)
#endif
#ifndef _Out_writes_bytes_opt_
#define _Out_writes_bytes_opt_(s)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(s)
#endif
#ifndef _Out_writes_to_
#define _Out_writes_to_(s, c)
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Outptr_opt_
#define _Outptr_opt_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#ifndef _Analysis_assume_
#define _Analysis_assume_(x)
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Post_writable_byte_size_
#define _Post_writable_byte_size_(s)
#endif
#ifndef _Post_equal_to_
#define _Post_equal_to_(x)
#endif
#ifndef _Pre_satisfies_
#define _Pre_satisfies_(x)
#endif

#endif // !_MSC_VER
