#include <windows.h>

#include "neverlose.h"
#include "internal_fixes.h"
#include "token.h"

// Global crash handler
static volatile bool g_crash_detected = false;
static LONG WINAPI CrashHandler(PEXCEPTION_POINTERS ex)
{
    if (ex->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
    {
        void* faultAddr = (void*)ex->ExceptionRecord->ExceptionInformation[1];
        void* exceptionAddr = ex->ExceptionRecord->ExceptionAddress;
        
        printf("\n[CRASH] Access Violation caught!\n");
        printf("[CRASH] Exception at: 0x%p\n", exceptionAddr);
        printf("[CRASH] Tried to access: 0x%p\n", faultAddr);
        
        // Если это попытка чтения из NULL или около-нулевого адреса
        if ((uintptr_t)faultAddr < 0x10000)
        {
            printf("[CRASH] NULL pointer dereference detected!\n");
            printf("[CRASH] This is likely due to uninitialized NL structure\n");
            
            // Проверяем адрес краша - если это в NL binary, значит entry thread крашится
            if ((uintptr_t)exceptionAddr >= 0x412A0000 && (uintptr_t)exceptionAddr < 0x447A1000)
            {
                printf("[CRASH] Crash is inside NL binary (entry thread)\n");
                printf("[CRASH] Terminating entry thread gracefully...\n");
                g_crash_detected = true;
                
                #ifdef _M_IX86
                // Вместо пропуска инструкции, завершаем поток
                ex->ContextRecord->Eip = (DWORD)&RtlExitUserThread;
                ex->ContextRecord->Eax = 0; // Exit code
                return EXCEPTION_CONTINUE_EXECUTION;
                #endif
            }
            else
            {
                printf("[CRASH] Crash is outside NL binary, cannot safely recover\n");
            }
        }
    }
    
    return EXCEPTION_CONTINUE_SEARCH;
}

// ============================================================================
// main.cpp — DLL entry point with fix initialization
// ============================================================================

// ============================================================================
// Auth Bypass Watchdog — Force NL to skip auth if server is unreachable
// ============================================================================
//
// NL's internal init has 10 steps. Step 6 is "Connecting to server" which
// hangs if the WebSocket proxy at 162.19.230.28:30030 is down.
// This watchdog thread waits 15 seconds after entry(), then patches NL
// to force-authenticate so initialization can proceed.
//

// Known addresses for NL network::Client instance fields
// The Client struct (from neverlosesdk.hpp):
//   +0: vtable ptr
//   +4: IsConnected (int)
//   +8: endpoint (websocketpp ptr)
//   +C: reserved[2]
//  +14: SomeKey (char*)
//  +1C: reserved2[6]
//  +34: SomeKey1 (char*)

static volatile bool g_auth_force_done = false;

// Scan for the Client instance by looking near the Requestor pointer.
// The Client is likely allocated near other NL globals.
static void force_nl_authenticated()
{
    if (g_auth_force_done) return;
    g_auth_force_done = true;

    printf("[watchdog] === Forcing NL authentication bypass ===\n");

    __try
    {
        // The Requestor instance pointer is at 0x42518C58
        // The Client instance pointer is likely at a nearby global address.
        // Common NL memory layout: Client ptr is often at 0x42518C5C or nearby.
        // We scan a range of addresses to find it.

        // Known addresses from fix_dump.cpp's last_resort_fixes:
        // 0x42518C44 = stdout handle
        // 0x42518C54 = requestor state (0x80000004)
        // 0x42518C58 = requestor instance ptr

        // Try to find Client instance by scanning near requestor globals
        // The client ptr might be at 0x42518C48, 0x42518C4C, 0x42518C50, etc.
        PVOID clientPtr = nullptr;
        int* clientIsConnected = nullptr;

        // Scan addresses near the requestor for valid Client pointers
        for (uintptr_t addr = 0x42518C40; addr < 0x42518D00; addr += 4)
        {
            PVOID ptr = *(PVOID*)addr;
            if (ptr == nullptr) continue;

            // Check if this pointer points to allocated memory in NL range
            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) continue;
            if (mbi.State != MEM_COMMIT) continue;
            if (!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY))) continue;

            // Check if this looks like a Client object:
            // +4 should be 0 (IsConnected = false, since we're stuck on auth)
            // +0 should be a vtable pointer in NL range
            PVOID vtable = *(PVOID*)ptr;
            int isConnected = *(int*)((char*)ptr + 4);

            if (vtable && (uintptr_t)vtable >= 0x412A0000 && (uintptr_t)vtable < 0x447A1000 && isConnected == 0)
            {
                printf("[watchdog] Found potential Client at 0x%p (stored at 0x%p, vtable=0x%p, IsConnected=%d)\n",
                    ptr, (PVOID)addr, vtable, isConnected);
                clientPtr = ptr;
                clientIsConnected = (int*)((char*)ptr + 4);
                break;
            }
        }

        if (clientIsConnected)
        {
            // Force IsConnected = 1 to make NL think auth succeeded
            *clientIsConnected = 1;
            printf("[watchdog] Set Client.IsConnected = 1 at 0x%p\n", clientIsConnected);
        }
        else
        {
            printf("[watchdog] Could not find Client instance via scan, trying hardcoded patches...\n");
        }

        // Additional patches: force flow past auth wait
        // These are conditional jumps that gate on auth/connection status.
        // Patching them to JMP (0xEB) forces the flow to proceed.
        constexpr DWORD auth_flow_fixes[] = {
            // Common NL auth wait locations - these are conditional jumps that
            // check IsConnected or auth state. Patching to unconditional JMP
            // bypasses the wait.
            0x415CD9EB,  // fix_signatures.cpp lists this as a client.dll pattern
            0x415D0057,  // Another auth-related flow gate
        };

        for (DWORD addr : auth_flow_fixes)
        {
            __try
            {
                BYTE current = *(PBYTE)addr;
                if (current != 0xEB) // Not already patched
                {
                    *(PBYTE)addr = 0xEB; // Force unconditional JMP
                    				printf("[watchdog] Patched auth flow at 0x%08lX (was 0x%02X -> 0xEB)\n", (unsigned long)addr, current);
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                				printf("[watchdog] Exception patching 0x%08lX\n", (unsigned long)addr);
            }
        }

        // Also try to set up dummy SomeKey/SomeKey1 so NL doesn't crash on null deref
        if (clientPtr)
        {
            // SomeKey at +0x14, SomeKey1 at +0x34
            char** someKey = (char**)((char*)clientPtr + 0x14);
            char** someKey1 = (char**)((char*)clientPtr + 0x34);

            // Allocate persistent strings for the keys
            static char dummyKey[] = "cracked";
            static char dummyKey1[] = "cracked";

            if (*someKey == nullptr)
            {
                *someKey = dummyKey;
                printf("[watchdog] Set SomeKey = \"cracked\"\n");
            }
            if (*someKey1 == nullptr)
            {
                *someKey1 = dummyKey1;
                printf("[watchdog] Set SomeKey1 = \"cracked\"\n");
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[watchdog] Exception during auth bypass: 0x%08X\n", GetExceptionCode());
    }

    printf("[watchdog] === Auth bypass complete ===\n");
}

static DWORD WINAPI AuthWatchdogThread(LPVOID)
{
    printf("[watchdog] Started, will force auth bypass in 15 seconds if stuck...\n");

    for (int i = 0; i < 150; i++) // 15 seconds (100ms * 150)
    {
        Sleep(100);
        if (g_auth_force_done)
            return 0; // Already done by another trigger
    }

    // 15 seconds elapsed, NL is probably stuck on step 6
    printf("[watchdog] 15s timeout reached, forcing auth bypass...\n");
    force_nl_authenticated();

    // Additional: after another 10 seconds, try even more aggressive patches
    Sleep(10000);

    printf("[watchdog] Applying aggressive secondary patches...\n");
    __try
    {
        // Scan a wider range of addresses near the Client globals
        // and force-set any zero IsConnected-like fields to 1
        for (uintptr_t addr = 0x42518C00; addr < 0x42518E00; addr += 4)
        {
            PVOID ptr = *(PVOID*)addr;
            if (ptr == nullptr) continue;

            MEMORY_BASIC_INFORMATION mbi = {};
            if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) continue;
            if (mbi.State != MEM_COMMIT) continue;

            // If this points to something in NL range and has IsConnected=0 at +4
            if ((uintptr_t)ptr >= 0x412A0000 && (uintptr_t)ptr < 0x447A1000)
            {
                __try
                {
                    int* connField = (int*)((char*)ptr + 4);
                    PVOID vtable = *(PVOID*)ptr;
                    if (vtable && (uintptr_t)vtable >= 0x412A0000 && *connField == 0)
                    {
                        *connField = 1;
                        printf("[watchdog] Aggressive: Set IsConnected=1 at 0x%p+4 (stored at 0x%p)\n", ptr, (PVOID)addr);
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[watchdog] Exception in aggressive scan: 0x%08X\n", GetExceptionCode());
    }

    return 0;
}

NTSTATUS NTAPI MainThread(LPVOID lpThreadParameter)
{
    // Allocate console for debug output so we can see what's happening
    AllocConsole();
    FILE* fp = nullptr;
    freopen_s(&fp, "CONOUT$", "w", stdout);

    printf("[main] === Neverlose Crack v5 starting ===\n");
    printf("[main] DLL base: 0x%p\n", (void*)lpThreadParameter);
    
    // Register crash handler to catch NULL pointer dereferences
    printf("[main] Installing crash handler...\n");
    AddVectoredExceptionHandler(1, CrashHandler);
    printf("[main] Crash handler installed\n");

    // Step 1: Auth token - skip prompt, use empty (crack bypasses auth anyway)
    printf("[main] loading auth token...\n");
    ensure_auth_token_loaded();
    printf("[main] auth token loaded (len=%zu)\n", strlen(auth_token));

    // Step 2: Map the NL binary into memory
    printf("[main] mapping NL binary...\n");
    __try
    {
        g_neverlose.map((HMODULE)lpThreadParameter);
        printf("[main] NL binary mapped OK at 0x%p\n", g_neverlose.base());
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] FATAL: exception in map() - code 0x%08X\n", GetExceptionCode());
        return STATUS_UNSUCCESSFUL;
    }

    // Step 3: Wait for serverbrowser.dll (indicates game is loaded enough)
    printf("[main] waiting for serverbrowser.dll...\n");
    while (!GetModuleHandleW(L"serverbrowser.dll"))
        Sleep(100);
    printf("[main] serverbrowser.dll found, game loaded\n");

    // Step 4: Setup sequence
    printf("[main] running fix_dump()...\n");
    __try
    {
        g_neverlose.fix_dump();
        printf("[main] fix_dump OK\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in fix_dump: 0x%08X\n", GetExceptionCode());
    }

    printf("[main] running set_veh()...\n");
    __try
    {
        g_neverlose.set_veh();
        printf("[main] set_veh OK\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in set_veh: 0x%08X\n", GetExceptionCode());
    }

    printf("[main] running setup_hooks()...\n");
    __try
    {
        g_neverlose.setup_hooks();
        printf("[main] setup_hooks OK\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in setup_hooks: 0x%08X\n", GetExceptionCode());
    }

    printf("[main] running spoof()...\n");
    __try
    {
        g_neverlose.spoof();
        printf("[main] spoof OK\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in spoof: 0x%08X\n", GetExceptionCode());
    }

    // Step 5: Pre-entry fixes
    printf("[main] running hijack_requestor()...\n");
    __try
    {
        hijack_requestor();
        printf("[main] hijack_requestor OK\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in hijack_requestor: 0x%08X\n", GetExceptionCode());
    }

    printf("[main] applying pre-entry fixes...\n");
    __try { fix_defensive();  printf("[main] fix_defensive OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_defensive\n"); }

    __try { fix_doubletap();  printf("[main] fix_doubletap OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_doubletap\n"); }

    __try { fix_resolver();   printf("[main] fix_resolver OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_resolver\n"); }

    // Step 6: Wait for game to fully initialize before running NL's entry
    printf("[main] waiting 10 seconds before entry()...\n");
    Sleep(10000);

    // Step 7: Run NL's main entry point
    printf("[main] running entry()...\n");

    // Start the auth watchdog BEFORE entry() so it can kick in if step 6 hangs
    CreateThread(NULL, 0, AuthWatchdogThread, NULL, 0, NULL);
    printf("[main] Auth watchdog started\n");

    __try
    {
        g_neverlose.entry();
        printf("[main] entry() returned\n");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[main] EXCEPTION in entry(): 0x%08X\n", GetExceptionCode());
    }

    // Step 8: Post-entry fixes
    printf("[main] re-applying fixes after entry()...\n");
    Sleep(2000);

    __try { fix_defensive();  printf("[main] fix_defensive (post) OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_defensive (post)\n"); }

    __try { fix_doubletap();  printf("[main] fix_doubletap (post) OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_doubletap (post)\n"); }

    __try { fix_resolver();   printf("[main] fix_resolver (post) OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_resolver (post)\n"); }

    __try { fix_lua();        printf("[main] fix_lua OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_lua\n"); }

    __try { fix_cfg();        printf("[main] fix_cfg OK\n"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("[main] EXCEPTION in fix_cfg\n"); }

    printf("[main] === All fixes applied ===\n");

    return STATUS_SUCCESS;
}

// DLL entry point
BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    if (fdwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hinstDLL);

        HANDLE hThread = NULL;

        NTSTATUS status = NtCreateThreadEx(
            &hThread,
            THREAD_ALL_ACCESS,
            NULL,
            NtCurrentProcess(),
            MainThread,
            hinstDLL,
            THREAD_CREATE_FLAGS_NONE,
            0, 0, 0,
            NULL
        );

        if (NT_SUCCESS(status) && hThread)
        {
            NtClose(hThread);
        }
        else
        {
            return FALSE;
        }
    }

    return TRUE;
}
