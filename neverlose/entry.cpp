#include "neverlose.h"

constexpr uintptr_t winver_entry_point = 0x412A0A00;
constexpr SIZE_T entry_stack_size = 0x100000;

#ifdef _M_IX86
NTSTATUS __declspec(naked) NTAPI _fictive_(LPVOID lpThreadParameter)
{
	__asm
	{
		push 0
		call RtlExitUserProcess
	};
};
#else
// Fallback for non-x86
NTSTATUS NTAPI _fictive_(LPVOID lpThreadParameter)
{
	(void)lpThreadParameter;
	return 0;
}
#endif

void neverlose::entry()
{
	printf("[entry] Starting entry function\n");

	HANDLE hThread;

	if (!NT_SUCCESS(NtCreateThreadEx(&hThread, THREAD_ALL_ACCESS, NULL, NtCurrentProcess(), (PUSER_THREAD_START_ROUTINE)winver_entry_point, 0, THREAD_CREATE_FLAGS_CREATE_SUSPENDED, 0, entry_stack_size, entry_stack_size, NULL))) panic("Failed to create thread!\n");

	printf("[entry] Created thread\n");

	THREAD_BASIC_INFORMATION tbi{0};
	if (!NT_SUCCESS(NtQueryInformationThread(hThread, (THREADINFOCLASS)ThreadBasicInformation, &tbi, sizeof(tbi), NULL))) panic("Failed to get TIB!\n");
	
	printf("[entry] Entry thread: 0x%p\n", (void*)tbi.ClientId.UniqueThread);

        
        CONTEXT tctx = { 0 };
        //tctx.ContextFlags = CONTEXT_FULL;
        //
        //if (!NT_SUCCESS(NtGetContextThread(hThread, &tctx))) panic("Failed to get thread context!\n");
        //
        //logger << "Extracted thread context.\n";
        //
        //tctx.Esp = (DWORD)VirtualAlloc(nullptr, 0x80000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) + (0x40000-4);
        //*(PPVOID)tctx.Esp = &RtlExitUserThread;
        //
        //if (!NT_SUCCESS(NtSetContextThread(hThread, &tctx))) panic("Failed to set thread context!\n");
        //
        //logger << "Applied thread context.\n";

        	NtResumeThread(hThread, NULL);

        	printf("[entry] Resumed thread\n");
        	tbi = { 0 };

        	// Wait for entry thread with a 5-second timeout.
        	// If it crashes, crash handler will terminate it
        	LARGE_INTEGER timeout;
        	timeout.QuadPart = -50000000LL; // 5 seconds (negative = relative)
        	NTSTATUS waitStatus = NtWaitForSingleObject(hThread, FALSE, &timeout);

	if (waitStatus == STATUS_SUCCESS)
	{
		if (NT_SUCCESS(NtQueryInformationThread(hThread, (THREADINFOCLASS)ThreadBasicInformation, &tbi, sizeof(tbi), NULL)))
			printf("[entry] Entry returned 0x%X\n", (unsigned)tbi.ExitStatus);
	}
	else if (waitStatus == STATUS_TIMEOUT)
	{
		printf("[entry] NL entry thread did not finish in 5s\n");
		printf("[entry] This is OK - entry thread may have crashed but fixes are already applied\n");
		printf("[entry] Continuing without entry thread...\n");
		// Don't close the handle - let the NL thread keep running or crash
		return;
	}
	else
	{
		printf("[entry] NtWaitForSingleObject returned 0x%08X\n", (unsigned)waitStatus);
	}

        NtClose(hThread);
};
