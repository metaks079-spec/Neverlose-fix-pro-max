#ifndef PHNT_COMPAT_H
#define PHNT_COMPAT_H

// Suppress phnt vs Windows SDK macro redefinition warnings (C4005)
#pragma warning(push)
#pragma warning(disable: 4005)

#define PHNT_VERSION PHNT_WINDOWS_10

// Try to include PHNT, if not available provide fallback
#if __has_include(<phnt_windows.h>)
#include <phnt_windows.h>
#include <phnt.h>
#else
// Fallback for IDE - include standard Windows headers
#include <windows.h>
#include <winternl.h>

// Define missing PHNT types for IDE
#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif

#ifndef STATUS_TIMEOUT
#define STATUS_TIMEOUT ((NTSTATUS)0x00000102L)
#endif

#ifndef THREAD_CREATE_FLAGS_CREATE_SUSPENDED
#define THREAD_CREATE_FLAGS_CREATE_SUSPENDED 0x00000001
#endif

#ifndef THREAD_CREATE_FLAGS_NONE
#define THREAD_CREATE_FLAGS_NONE 0
#endif

// Forward declare PHNT functions
#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS NTAPI NtCreateThreadEx(
    PHANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes,
    HANDLE ProcessHandle,
    PVOID StartRoutine,
    PVOID Argument,
    ULONG CreateFlags,
    SIZE_T ZeroBits,
    SIZE_T StackSize,
    SIZE_T MaximumStackSize,
    PVOID AttributeList
);

NTSTATUS NTAPI NtClose(HANDLE Handle);
NTSTATUS NTAPI NtAllocateVirtualMemory(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
NTSTATUS NTAPI NtFreeVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG);
NTSTATUS NTAPI NtWriteVirtualMemory(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE, PVOID, DWORD, PVOID, SIZE_T, PSIZE_T);
NTSTATUS NTAPI NtQueryInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG, PULONG);
NTSTATUS NTAPI NtGetContextThread(HANDLE, PCONTEXT);
NTSTATUS NTAPI NtSetContextThread(HANDLE, PCONTEXT);
NTSTATUS NTAPI NtResumeThread(HANDLE, PULONG);
NTSTATUS NTAPI NtWaitForSingleObject(HANDLE, BOOLEAN, PLARGE_INTEGER);
NTSTATUS NTAPI NtTerminateProcess(HANDLE, NTSTATUS);

#define NtCurrentProcess() ((HANDLE)(LONG_PTR)-1)
#define NtCurrentThreadId() GetCurrentThreadId()

typedef struct _THREAD_BASIC_INFORMATION {
    NTSTATUS ExitStatus;
    PVOID TebBaseAddress;
    CLIENT_ID ClientId;
    KAFFINITY AffinityMask;
    LONG Priority;
    LONG BasePriority;
} THREAD_BASIC_INFORMATION, *PTHREAD_BASIC_INFORMATION;

#define ThreadBasicInformation 0

typedef NTSTATUS (NTAPI *PUSER_THREAD_START_ROUTINE)(PVOID);

// Memory information class
#ifndef MemoryBasicInformation
#define MemoryBasicInformation 0
#endif

// Additional missing function
NTSTATUS NTAPI NtProtectVirtualMemory(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);

// TEXT macro if not defined
#ifndef TEXT
#ifdef UNICODE
#define TEXT(x) L##x
#else
#define TEXT(x) x
#endif
#endif

// FALSE if not defined
#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#ifdef __cplusplus
}
#endif

#endif // __has_include

#pragma warning(pop)

#endif // PHNT_COMPAT_H
