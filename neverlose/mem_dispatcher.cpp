#include "internal_fixes.h"
#include "HookFn.h"
#include "FindPattern.h"
#include <cstdio>
#include "detours.h"
#include <vector>

enum operation_t
{
    OPERATION_REGISTER_HOOK = 1,
    OPERATION_EMPLACE_HOOKS,
    OPERATION_ERASE_HOOKS,
    OPERATION_SIGSCAN = 6,
};

#pragma pack(push, 1)
struct sigscan_t
{
    PVOID64 Base;
    PVOID64 Signature;
    size_t Length;
    PVOID64 Result;
};

struct hook_t
{
    PVOID64 Address;
    PVOID64 Hook;
    PVOID64 pTrampoline;
};
#pragma pack(pop)

struct HookDesc
{
    bool IsActive;
    PVOID Address;
    PVOID Trampoline;
    PVOID Hook;
};

const char* optostr[] =
{
    NULL,
    "OPERATION_REGISTER_HOOK",
    "OPERATION_EMPLACE_HOOKS",
    "OPERATION_ERASE_HOOKS",
    NULL, NULL,
    "OPERATION_SIGSCAN",
};
static auto& g_HkDesc = *reinterpret_cast<std::vector<HookDesc>*>(0x42500C44);
static bool TransactionAlive = false;
// Diagnostic logging for hook registration - helps debug DT/resolver issues
static FILE* g_hooklog = nullptr;
static void hooklog(const char* fmt, ...)
{
    if (!g_hooklog)
    {
        g_hooklog = fopen("nl_hooks_debug.log", "a");
        if (!g_hooklog) return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(g_hooklog, fmt, args);
    va_end(args);
    fflush(g_hooklog);
}

BOOL __cdecl hkMemDispatcher(operation_t type, void* ptr)
{
    BOOL result = FALSE;

    switch (type)
    {
    case OPERATION_SIGSCAN:
    {
        auto* data = (sigscan_t*)ptr;
        data->Result = FindPattern(data->Base, 0x7FFFFFFF, (PBYTE)data->Signature, data->Length, 0xCC, 0);
        if (!data->Result)
        {
            hooklog("[SIGSCAN] FAILED: Base=0x%llX Len=%zu\n", (unsigned long long)data->Base, data->Length);
        }
        result = TRUE;
    };
    break;
    case OPERATION_REGISTER_HOOK:
    {
        if (!TransactionAlive)
        {
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            TransactionAlive = true;
        };

        auto* data = (hook_t*)ptr;
        PVOID pTramp = data->Address;

        // Skip known-problematic hook that causes instability
        if (data->Address == (PBYTE)GetModuleHandle(L"engine.dll") + 0xF0470) return TRUE;

        // Validate the hook target before attaching
        __try
        {
            BYTE firstByte = *(BYTE*)data->Address;
            if (firstByte == 0x00 || firstByte == 0xCC)
            {
                hooklog("[HOOK] REJECTED bad target 0x%llX (byte=0x%02X)\n",
                    (unsigned long long)data->Address, firstByte);
                result = FALSE;
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            hooklog("[HOOK] REJECTED unreadable target 0x%llX\n",
                (unsigned long long)data->Address);
            result = FALSE;
            break;
        }

        if (DetourAttachEx(&pTramp, data->Hook, (PDETOUR_TRAMPOLINE*)data->pTrampoline, NULL, NULL) == NO_ERROR)
        {
            hooklog("[HOOK] OK: 0x%llX -> tramp=0x%llX\n",
                (unsigned long long)data->Address, (unsigned long long)*(PVOID64*)data->pTrampoline);
            result = TRUE;
        }
        else
        {
            hooklog("[HOOK] FAILED DetourAttach: 0x%llX\n", (unsigned long long)data->Address);
            result = FALSE;
        }
    };
    break;
    case OPERATION_EMPLACE_HOOKS:
        if (TransactionAlive)
        {
            DetourTransactionCommit();
            TransactionAlive = false;
            result = TRUE;
        }
        else
            result = FALSE;
        break;
    case OPERATION_ERASE_HOOKS:
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        for (auto& hook : g_HkDesc)
        {
            if (hook.IsActive && hook.Trampoline)
            {
                DetourDetach(&hook.Trampoline, hook.Hook);
                hook.IsActive = false;
            };
        };
        DetourTransactionCommit();
        result = TRUE;
    };
    break;
    default:
        break;
    };
    //printf("\n");
    return result;
};


void fix_mem_dispatcher()
{
        HookFn((PVOID)0x41DA0BA0, hkMemDispatcher, 0);
};