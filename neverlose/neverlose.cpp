#include "neverlose.h"

// Forward declarations for IDE
#ifdef __INTELLISENSE__
extern "C" HRSRC WINAPI FindResourceW(HMODULE, LPCWSTR, LPCWSTR);
#endif

void neverlose::panic(const char* fmt, ...)
{
        char buffer[1024];

        va_list va;
        va_start(va, fmt);
        vsprintf(buffer, fmt, va);
        va_end(va);

        MessageBoxA(0, buffer, 0, MB_ICONERROR);
        NtTerminateProcess(NtCurrentProcess(), STATUS_UNSUCCESSFUL);
};

void neverlose::map(HMODULE hModule)
{
        	hThis = hModule;
        	HRSRC hRes = FindResourceW(hThis, MAKEINTRESOURCEW(IDR_BINARY), L"BINARY");

        if (!hRes)
                panic("Failed to locate cheat binary! (FindResourceW returned NULL - is nl.bin embedded?)");

        HGLOBAL hResData = LoadResource(hThis, hRes);

        if (!hResData)
                panic("Failed to load cheat binary! (LoadResource returned NULL)");

        LPVOID pData = LockResource(hResData);

        if (!pData)
                panic("Failed to lock cheat binary! (LockResource returned NULL)");

        DWORD Size = SizeofResource(hThis, hRes);
        	printf("[map] Resource size: %lu bytes (0x%lX)\n", (unsigned long)Size, (unsigned long)Size);

        // Stupid fallback?
        if (Size)
                imageSize = Size;

        printf("[map] Target baseAddr: 0x%p, imageSize: 0x%zX (%zu MB)\n", baseAddr, imageSize, imageSize / (1024*1024));

        // ===== Strategy 1: Try direct RWX allocation at preferred base =====
        NTSTATUS status = NtAllocateVirtualMemory(NtCurrentProcess(), &baseAddr, NULL, &imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (NT_SUCCESS(status))
        {
                printf("[map] Strategy 1 (direct RWX) succeeded at 0x%p\n", baseAddr);
                goto alloc_success;
        }
        printf("[map] Strategy 1 failed: 0x%08X\n", (unsigned)status);

        // ===== Diagnose what's at the target address =====
        {
                MEMORY_BASIC_INFORMATION mbi = {};
                PVOID queryAddr = (PVOID)0x412A0000;
                SIZE_T querySize = 0x3501000;
                NTSTATUS qs = NtQueryVirtualMemory(NtCurrentProcess(), queryAddr, MemoryBasicInformation, &mbi, sizeof(mbi), NULL);
                if (NT_SUCCESS(qs))
                {
                        			printf("[map] VirtualQuery at 0x%p: State=0x%lX Protect=0x%lX Type=0x%lX Size=0x%llX\n",
                        				mbi.BaseAddress, (unsigned long)mbi.State, (unsigned long)mbi.Protect, 
                        				(unsigned long)mbi.Type, (unsigned long long)mbi.RegionSize);
                }
                else
                {
                        printf("[map] VirtualQuery failed: 0x%08X\n", (unsigned)qs);
                }
        }

        // ===== Strategy 2: Free conflicting memory, then retry =====
        {
                PVOID freeAddr = (PVOID)0x412A0000;
                SIZE_T freeSize = 0x3501000;
                NTSTATUS fs = NtFreeVirtualMemory(NtCurrentProcess(), &freeAddr, &freeSize, MEM_RELEASE);
                printf("[map] Strategy 2: NtFreeVirtualMemory at 0x412A0000: 0x%08X\n", (unsigned)fs);
                if (NT_SUCCESS(fs))
                {
                        // Reset and retry allocation
                        baseAddr = (PVOID)0x412A0000;
                        imageSize = Size ? (SIZE_T)Size : (SIZE_T)0x3501000;
                        status = NtAllocateVirtualMemory(NtCurrentProcess(), &baseAddr, NULL, &imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                        if (NT_SUCCESS(status))
                        {
                                printf("[map] Strategy 2 (free+RWX) succeeded at 0x%p\n", baseAddr);
                                goto alloc_success;
                        }
                        printf("[map] Strategy 2 retry failed: 0x%08X\n", (unsigned)status);
                }
        }

        // ===== Strategy 3: PAGE_READWRITE first, then upgrade to RWX =====
        {
                baseAddr = (PVOID)0x412A0000;
                imageSize = Size ? (SIZE_T)Size : (SIZE_T)0x3501000;
                status = NtAllocateVirtualMemory(NtCurrentProcess(), &baseAddr, NULL, &imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (NT_SUCCESS(status))
                {
                        printf("[map] Strategy 3: RW allocation succeeded at 0x%p, upgrading to RWX...\n", baseAddr);
                        ULONG oldProtect;
                        NTSTATUS ps = NtProtectVirtualMemory(NtCurrentProcess(), &baseAddr, &imageSize, PAGE_EXECUTE_READWRITE, &oldProtect);
                        if (NT_SUCCESS(ps))
                        {
                                printf("[map] Strategy 3 (RW->RWX) succeeded at 0x%p\n", baseAddr);
                                goto alloc_success;
                        }
                        printf("[map] Strategy 3 NtProtectVirtualMemory failed: 0x%08X\n", (unsigned)ps);
                        // Free and continue
                        NtFreeVirtualMemory(NtCurrentProcess(), &baseAddr, &imageSize, MEM_RELEASE);
                }
                else
                {
                        printf("[map] Strategy 3 RW allocation failed: 0x%08X\n", (unsigned)status);
                }
        }

        // ===== Strategy 4: Decommit + recommit at target address =====
        {
                baseAddr = (PVOID)0x412A0000;
                imageSize = Size ? (SIZE_T)Size : (SIZE_T)0x3501000;
                // Try MEM_RESERVE first, then MEM_COMMIT separately
                status = NtAllocateVirtualMemory(NtCurrentProcess(), &baseAddr, NULL, &imageSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (NT_SUCCESS(status))
                {
                        printf("[map] Strategy 4: MEM_RESERVE succeeded at 0x%p, committing...\n", baseAddr);
                        status = NtAllocateVirtualMemory(NtCurrentProcess(), &baseAddr, NULL, &imageSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
                        if (NT_SUCCESS(status))
                        {
                                printf("[map] Strategy 4 (reserve+commit) succeeded at 0x%p\n", baseAddr);
                                goto alloc_success;
                        }
                        printf("[map] Strategy 4 MEM_COMMIT failed: 0x%08X\n", (unsigned)status);
                }
                else
                {
                        printf("[map] Strategy 4 MEM_RESERVE failed: 0x%08X\n", (unsigned)status);
                }
        }

        // ===== Strategy 5: VirtualAlloc fallback =====
        {
                baseAddr = (PVOID)0x412A0000;
                imageSize = Size ? (SIZE_T)Size : (SIZE_T)0x3501000;
                LPVOID va = VirtualAlloc(baseAddr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                if (va != NULL)
                {
                        baseAddr = va;
                        printf("[map] Strategy 5 (VirtualAlloc RWX) succeeded at 0x%p\n", baseAddr);
                        goto alloc_success;
                }
                // Try RW then upgrade
                va = VirtualAlloc(baseAddr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (va != NULL)
                {
                        baseAddr = va;
                        DWORD oldProtect;
                        if (VirtualProtect(baseAddr, imageSize, PAGE_EXECUTE_READWRITE, &oldProtect))
                        {
                                printf("[map] Strategy 5 (VirtualAlloc RW->RWX) succeeded at 0x%p\n", baseAddr);
                                goto alloc_success;
                        }
                        			printf("[map] Strategy 5 VirtualProtect failed: %lu\n", (unsigned long)GetLastError());
                        VirtualFree(baseAddr, 0, MEM_RELEASE);
                }
                		printf("[map] Strategy 5 VirtualAlloc failed: %lu\n", (unsigned long)GetLastError());
        }

        // All strategies failed
        {
                // Final diagnostic: scan nearby addresses for free regions
                printf("[map] === ALL ALLOCATION STRATEGIES FAILED ===\n");
                printf("[map] Scanning for free regions near 0x412A0000...\n");
                MEMORY_BASIC_INFORMATION mbi2;
                PVOID scan = (PVOID)0x40000000;
                while (VirtualQuery(scan, &mbi2, sizeof(mbi2)))
                {
                        if (mbi2.State == MEM_FREE && mbi2.RegionSize >= 0x3501000)
                                printf("[map]   FREE region at 0x%p size 0x%zX (%zu MB)\n", mbi2.BaseAddress, mbi2.RegionSize, mbi2.RegionSize/(1024*1024));
                        scan = (PVOID)((ULONG_PTR)mbi2.BaseAddress + mbi2.RegionSize);
                        if ((ULONG_PTR)scan > 0x80000000) break; // stop at 2GB
                }
        }

        panic("Failed to allocate cheat base at 0x412A0000! (last NTSTATUS: 0x%08X)\n"
              "The address range is occupied. Try:\n"
              "1. Close other injectors/overlays\n"
              "2. Restart CS:GO\n"
              "3. Inject earlier before other DLLs load", (unsigned)status);

alloc_success:
	printf("[map] Allocated cheat base at 0x%p\n", baseAddr);

        if (!NT_SUCCESS(NtWriteVirtualMemory(NtCurrentProcess(), baseAddr, pData, imageSize, NULL)))
                panic("Failed to write cheat image!");
};

PVOID neverlose::load_res_to_mem(int idr, const char* rcname) const
{
	HRSRC hRes = FindResourceW(hThis, MAKEINTRESOURCEW(idr), L"BINARY");

        if (!hRes)
                panic("Failed to find %s binary!", rcname);

        HGLOBAL hResData = LoadResource(hThis, hRes);

        if (!hResData)
                panic("Failed to load %s binary!", rcname);

        LPVOID pData = LockResource(hResData);

        if (!pData)
                panic("Failed to lock %s binary!", rcname);

        DWORD Size = SizeofResource(hThis, hRes);

        PVOID addr = NULL;
        SIZE_T size = Size;
        if (!NT_SUCCESS(NtAllocateVirtualMemory(NtCurrentProcess(), &addr, 0, &size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)))
                panic("Failed to allocate %s image!", rcname);

        if (!NT_SUCCESS(NtWriteVirtualMemory(NtCurrentProcess(), addr, pData, Size, NULL)))
                panic("Failed to write %s image!", rcname);

        return addr;
};