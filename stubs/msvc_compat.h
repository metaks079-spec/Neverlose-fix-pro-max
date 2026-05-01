#ifndef MSVC_COMPAT_H
#define MSVC_COMPAT_H

// MSVC compatibility layer for MinGW cross-compilation

// Include Windows headers first for basic types
#include <windows.h>

// EXTERN_C_START / EXTERN_C_END
#ifndef EXTERN_C_START
#define EXTERN_C_START extern "C" {
#endif
#ifndef EXTERN_C_END  
#define EXTERN_C_END }
#endif

// QUAD type (MSVC-specific) - use long long
#ifndef QUAD
typedef long long QUAD;
#endif

// SAL annotations - stub them out
#ifndef _Maybenull_
#define _Maybenull_
#endif
#ifndef _Analysis_noreturn_
#define _Analysis_noreturn_
#endif
#ifndef _In_
#define _In_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _In_reads_
#define _In_reads_(x)
#endif
#ifndef _Out_writes_
#define _Out_writes_(x)
#endif
#ifndef _In_reads_bytes_
#define _In_reads_bytes_(x)
#endif
#ifndef _Out_writes_bytes_
#define _Out_writes_bytes_(x)
#endif
#ifndef _Success_
#define _Success_(x)
#endif
#ifndef _When_
#define _When_(x,y)
#endif
#ifndef _At_
#define _At_(x,y)
#endif
#ifndef _Post_satisfies_
#define _Post_satisfies_(x)
#endif
#ifndef _IRQL_requires_min_
#define _IRQL_requires_min_(x)
#endif
#ifndef _IRQL_requires_max_
#define _IRQL_requires_max_(x)
#endif
#ifndef _IRQL_requires_
#define _IRQL_requires_(x)
#endif
#ifndef _IRQL_raises_
#define _IRQL_raises_(x)
#endif
#ifndef _Acquires_lock_
#define _Acquires_lock_(x)
#endif
#ifndef _Releases_lock_
#define _Releases_lock_(x)
#endif
#ifndef _Requires_lock_held_
#define _Requires_lock_held_(x)
#endif
#ifndef _Use_decl_annotations_
#define _Use_decl_annotations_
#endif
#ifndef _Field_size_
#define _Field_size_(x)
#endif
#ifndef _Field_size_bytes_
#define _Field_size_bytes_(x)
#endif
#ifndef _Struct_size_bytes_
#define _Struct_size_bytes_(x)
#endif
#ifndef _Notnull_
#define _Notnull_
#endif
#ifndef _Null_terminated_
#define _Null_terminated_
#endif
#ifndef _Post_writable_byte_size_
#define _Post_writable_byte_size_(x)
#endif
#ifndef _Pre_notnull_
#define _Pre_notnull_
#endif
#ifndef _Printf_format_string_
#define _Printf_format_string_
#endif

// UFIELD_OFFSET - use __builtin_offsetof to avoid static_assert issues
#ifndef UFIELD_OFFSET
#define UFIELD_OFFSET(type, field) ((ULONG)__builtin_offsetof(type, field))
#endif

#ifndef FIELD_OFFSET
#define FIELD_OFFSET(type, field) ((LONG)__builtin_offsetof(type, field))
#endif

// SEH compatibility for MinGW
#ifdef __MINGW32__
  #undef __try
  #undef __except
  // Use a simple if/else pattern - not real SEH but allows compilation
  #define __try if (1)
  #define __except(x) else
#endif

// NTAPI calling convention
#ifndef NTAPI
#define NTAPI __stdcall
#endif

// NT_SUCCESS macro
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS)(status) >= 0)
#endif

// EXCEPTION_EXECUTE_HANDLER
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif

// GetExceptionCode stub
inline DWORD GetExceptionCodeCompat() { return 0; }
#ifndef GetExceptionCode
#define GetExceptionCode() GetExceptionCodeCompat()
#endif

// FORCEINLINE
#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif

// __analysis_assume
#ifndef __analysis_assume
#define __analysis_assume(x)
#endif

// __specstrings - common MSVC spec string stubs
#ifndef __in
#define __in
#endif
#ifndef __out
#define __out
#endif
#ifndef __inout
#define __inout
#endif
#ifndef __in_opt
#define __in_opt
#endif
#ifndef __out_opt
#define __out_opt
#endif
#ifndef __inout_opt
#define __inout_opt
#endif
#ifndef __in_ecount
#define __in_ecount(x)
#endif
#ifndef __out_ecount
#define __out_ecount(x)
#endif
#ifndef __in_bcount
#define __in_bcount(x)
#endif
#ifndef __out_bcount
#define __out_bcount(x)
#endif
#ifndef __deref_out_ecount
#define __deref_out_ecount(x)
#endif
#ifndef __out_ecount_part
#define __out_ecount_part(x,y)
#endif

// _ReturnAddress and _AddressOfReturnAddress  
#ifndef _ReturnAddress
#define _ReturnAddress() __builtin_return_address(0)
#endif

// InterlockedExchange / InterlockedCompareExchange already in mingw headers

#endif // MSVC_COMPAT_H
