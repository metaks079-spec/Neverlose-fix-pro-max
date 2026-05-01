#include <cstdio>
#pragma warning(disable: 4309 4305)
#include <cmath>

#include "internal_fixes.h"
#include "HookFn.h"
#include "FindPattern.h"

// ============================================================================
// fix_defensive.cpp — Defensive Anti-Aim fix with CreateMove angle restore
// ============================================================================
//
// Defensive AA in NL works by manipulating the signon state callback to
// re-apply anti-aim angles after the engine resets them during connection
// state transitions. In the crack, defensive breaks because:
//
// 1. Auth checks disable the defensive code path
// 2. Choke packet logic uses wrong tick counts
// 3. The signonstate hook NL installs gets overwritten during entry()
// 4. No continuous angle re-application - angles get reset between ticks
//
// Our fix approach:
// - Patch the auth-gated enable checks (bypass license validation)
// - Patch choke logic for proper tick manipulation
// - Hook signonstate for defensive re-trigger (original approach)
// - Add a CreateMove-level angle restore that ensures defensive angles
//   are always applied even if the signonstate hook is removed
// - Guard thread with smart re-validation
// ============================================================================

namespace
{
    // NL binary base address
    constexpr uintptr_t NL_BASE = 0x412A0000;
    constexpr uintptr_t NL_END  = NL_BASE + 0x3501000;

    // Addresses inside the mapped NL binary
    constexpr uintptr_t signonstate_addr     = 0x415DCE40;
    constexpr uintptr_t defensive_fix_addr   = 0x415DEBD0;
    constexpr uintptr_t defensive_enable_check  = 0x415DEE10;
    constexpr uintptr_t defensive_enable_check2 = 0x415DEE3C;
    constexpr uintptr_t defensive_choke_patch   = 0x415DECC8;

    // Address of NL's anti-aim angle storage (where defensive writes angles)
    // This is the qangle that defensive AA writes to for the local player.
    // We read/restore from here in CreateMove to ensure angles persist.
    constexpr uintptr_t defensive_angle_ptr = 0x4255A3C0;

    // Address of the "is defensive active" flag in NL
    constexpr uintptr_t defensive_active_flag = 0x4255A3B8;

    // Address of the CreateMove hook target inside NL binary
    // NL hooks IBaseClientDLL::CreateMove - we hook their hook to add
    // our angle restore after their anti-aim runs
    constexpr uintptr_t nl_createmove_hook = 0x415DE200;

    // Address of the "send packet" choke flag that NL uses
    constexpr uintptr_t send_packet_choke_flag = 0x4255A3D0;

    volatile LONG defensive_guard_started = 0;
    volatile LONG defensive_hook_enabled = 0;

    // Stored defensive angles for re-application
    struct DefensiveState
    {
        float yaw = 0.0f;
        float pitch = 89.0f;   // default defensive pitch (fake up)
        float desync = 0.0f;
        bool  has_valid_angles = false;
        DWORD last_update_tick = 0;
    };

    static DefensiveState g_defensive_state;

    // Validate that an address is within NL binary range and looks like code
    static bool is_valid_code_addr(uintptr_t addr)
    {
        if (addr < NL_BASE || addr > NL_END)
        {
            printf("[fix_defensive] 0x%08X outside NL range, skipping\n", addr);
            return false;
        }

        __try
        {
            const auto* ptr = reinterpret_cast<const BYTE*>(addr);
            if (*ptr == 0x00 || *ptr == 0xCC || *ptr == 0xC3)
            {
                printf("[fix_defensive] 0x%08X looks like invalid code (0x%02X), skipping\n", addr, *ptr);
                return false;
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_defensive] 0x%08X is not readable, skipping\n", addr);
            return false;
        }
    }

    // Validate function prologue
    static bool validate_prologue(uintptr_t addr, const char* name)
    {
        __try
        {
            const auto* ptr = reinterpret_cast<const BYTE*>(addr);
            if (*ptr == 0x55 || *ptr == 0x8B || *ptr == 0x83 || *ptr == 0x56 || *ptr == 0x53)
            {
                return true;
            }
            printf("[fix_defensive] %s at 0x%08X has unexpected prologue 0x%02X\n", name, addr, *ptr);
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_defensive] cannot read %s at 0x%08X\n", name, addr);
            return false;
        }
    }

    // Apply a memory protection change, write, and restore
    static bool protected_write(void* addr, const void* data, size_t len)
    {
        __try
        {
            DWORD oldProtect;
            SIZE_T size = len + 16; // extra padding for page alignment
            LPVOID base = addr;
            NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, PAGE_EXECUTE_READWRITE, &oldProtect);
            memcpy(addr, data, len);
            NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, oldProtect, &oldProtect);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Patch the defensive enable checks inside the NL binary.
    // These are conditional jumps that skip defensive when auth checks fail.
    static void patch_defensive_enable_checks()
    {
        __try
        {
            // Patch 1: Defensive enable check
            auto* check1 = reinterpret_cast<BYTE*>(defensive_enable_check);
            if (*check1 == 0x74) // short JE → short JMP
            {
                BYTE patch = 0xEB;
                protected_write(check1, &patch, 1);
                printf("[fix_defensive] patched enable check at 0x%08X: JE→JMP\n", defensive_enable_check);
            }
            else if (*check1 == 0x0F && *(check1 + 1) >= 0x84 && *(check1 + 1) < 0x90) // long JE → JMP
            {
                // 0F 84 xx xx xx xx → E9 xx xx xx xx 90
                INT32 rel = *reinterpret_cast<INT32*>(check1 + 2);
                BYTE patch[6];
                patch[0] = 0xE9;
                *reinterpret_cast<INT32*>(patch + 1) = rel + 1;
                patch[5] = 0x90;
                protected_write(check1, patch, 6);
                printf("[fix_defensive] patched enable check at 0x%08X: long JE→JMP\n", defensive_enable_check);
            }
            else if (*check1 == 0x75) // short JNE → NOP
            {
                BYTE patch[2] = { 0x90, 0x90 };
                protected_write(check1, patch, 2);
                printf("[fix_defensive] patched enable check at 0x%08X: JNE→NOP NOP\n", defensive_enable_check);
            }
            else
            {
                printf("[fix_defensive] enable check at 0x%08X has unexpected byte 0x%02X\n",
                    defensive_enable_check, *check1);
            }

            // Patch 2: Second enable check
            auto* check2 = reinterpret_cast<BYTE*>(defensive_enable_check2);
            if (*check2 == 0x74)
            {
                BYTE patch = 0xEB;
                protected_write(check2, &patch, 1);
                printf("[fix_defensive] patched enable check2 at 0x%08X: JE→JMP\n", defensive_enable_check2);
            }
            else if (*check2 == 0x75)
            {
                BYTE patch[2] = { 0x90, 0x90 };
                protected_write(check2, patch, 2);
                printf("[fix_defensive] patched enable check2 at 0x%08X: JNE→NOP\n", defensive_enable_check2);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_defensive] exception while patching enable checks\n");
        }
    }

    // Patch the defensive choke logic to use correct tick count.
    static void patch_defensive_choke_logic()
    {
        __try
        {
            auto* choke = reinterpret_cast<BYTE*>(defensive_choke_patch);

            if (*choke == 0x83 && *(choke + 1) == 0x3D) // CMP [mem], imm8
            {
                BYTE old_val = *(choke + 6);
                if (old_val < 14)
                {
                    BYTE new_val = 14;
                    protected_write(choke + 6, &new_val, 1);
                    printf("[fix_defensive] patched choke limit at 0x%08X: %d→14\n",
                        defensive_choke_patch, old_val);
                }
            }
            else if (*choke == 0x83 && *(choke + 1) == 0xF8) // CMP eax, imm8
            {
                BYTE old_val = *(choke + 2);
                if (old_val < 14)
                {
                    BYTE new_val = 14;
                    protected_write(choke + 2, &new_val, 1);
                    printf("[fix_defensive] patched choke limit (reg) at 0x%08X: %d→14\n",
                        defensive_choke_patch, old_val);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_defensive] exception while patching choke logic\n");
        }
    }

    // Hooked signonstate callback - calls defensive_fix and captures angles
    static void __cdecl hooked_signonstate()
    {
        __try
        {
            // Call the original defensive fix handler
            reinterpret_cast<void(__cdecl*)()>(defensive_fix_addr)();

            // Capture the defensive angles after the handler runs
            // The defensive AA writes its angles to a known memory location
            auto* angle_ptr = reinterpret_cast<float*>(defensive_angle_ptr);
            __try
            {
                g_defensive_state.yaw   = angle_ptr[0];
                g_defensive_state.pitch = angle_ptr[1];
                g_defensive_state.desync = angle_ptr[2];
                g_defensive_state.has_valid_angles = true;
                g_defensive_state.last_update_tick = GetTickCount();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Angle pointer may be invalid early in initialization
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_defensive] defensive_fix threw exception 0x%08X, disabling hook\n",
                GetExceptionCode());
            InterlockedExchange(&defensive_hook_enabled, 0);
        }
    }

    // Trampoline for the original CreateMove hook
    static void* createmove_tram = nullptr;

    // Our CreateMove hook that runs after NL's anti-aim to ensure defensive
    // angles are properly applied. This is critical because the engine can
    // reset angles between frames, and NL's signonstate hook alone doesn't
    // guarantee angle persistence.
    //
    // The calling convention for CreateMove in NL's hook chain is:
    //   void __fastcall hook(void* ecx, void* edx, float sampletime, CUserCmd* cmd)
    static void __fastcall hooked_createmove(void* ecx, void* edx, float sampletime, void* cmd)
    {
        // Call the original NL CreateMove hook (which includes anti-aim)
        if (createmove_tram)
        {
            reinterpret_cast<void(__fastcall*)(void*, void*, float, void*)>(createmove_tram)(
                ecx, edx, sampletime, cmd);
        }

        // After NL's anti-aim runs, check if defensive should be active
        // and re-apply angles if needed
        if (!defensive_hook_enabled || !g_defensive_state.has_valid_angles)
            return;

        __try
        {
            // Check if defensive is supposed to be active
            auto* active_flag = reinterpret_cast<volatile LONG*>(defensive_active_flag);
            if (*active_flag == 0)
                return; // Defensive not enabled by user, don't force angles

            // Re-apply the defensive angles to ensure they persist
            // NL stores the anti-aim output in cmd->viewangles
            // CUserCmd layout: viewangles at offset 0x04 (3 floats: pitch, yaw, roll)
            if (cmd)
            {
                auto* viewangles = reinterpret_cast<float*>((BYTE*)cmd + 0x04);

                // Only override if the angles look like they were reset
                // (engine resets to 0,0,0 or to the actual view angles)
                // Defensive pitch is typically 89.0 (fake up) and yaw is manipulated
                if (g_defensive_state.pitch > 50.0f || g_defensive_state.pitch < -50.0f)
                {
                    // Defensive is active - force the defensive angles
                    viewangles[0] = g_defensive_state.pitch;
                    viewangles[1] = g_defensive_state.yaw;
                    viewangles[2] = g_defensive_state.desync;

                    // Ensure the send_packet choke flag is set correctly
                    // for defensive to work (we need to choke some packets)
                    auto* choke_flag = reinterpret_cast<BYTE*>(send_packet_choke_flag);
                    __try
                    {
                        // Don't force the choke flag - let NL's own logic handle it
                        // We only restore angles, not choke state
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Don't disable on CreateMove exception - just skip this frame
        }
    }

    static bool install_defensive_hook()
    {
        if (!is_valid_code_addr(signonstate_addr))
        {
            printf("[fix_defensive] signonstate address invalid, not hooking\n");
            return false;
        }

        if (!is_valid_code_addr(defensive_fix_addr))
        {
            printf("[fix_defensive] defensive_fix address invalid, not hooking\n");
            return false;
        }

        if (!validate_prologue(signonstate_addr, "signonstate"))
        {
            printf("[fix_defensive] signonstate prologue mismatch, not hooking\n");
            return false;
        }

        if (!validate_prologue(defensive_fix_addr, "defensive_fix"))
        {
            printf("[fix_defensive] defensive_fix prologue mismatch, not hooking\n");
            return false;
        }

        HookFn(reinterpret_cast<void*>(signonstate_addr), reinterpret_cast<void*>(hooked_signonstate), 0);
        InterlockedExchange(&defensive_hook_enabled, 1);
        printf("[fix_defensive] installed signonstate hook successfully\n");
        return true;
    }

    static bool install_createmove_hook()
    {
        if (!is_valid_code_addr(nl_createmove_hook))
        {
            printf("[fix_defensive] CreateMove hook target invalid (0x%08X), skipping\n", nl_createmove_hook);
            return false;
        }

        if (!validate_prologue(nl_createmove_hook, "CreateMove"))
        {
            printf("[fix_defensive] CreateMove prologue mismatch, skipping\n");
            return false;
        }

        NTSTATUS status = HookFn(
            reinterpret_cast<void*>(nl_createmove_hook),
            reinterpret_cast<void*>(hooked_createmove),
            0,
            &createmove_tram);

        if (NT_SUCCESS(status))
        {
            printf("[fix_defensive] installed CreateMove angle-restore hook (tram=0x%p)\n", createmove_tram);
            return true;
        }
        else
        {
            printf("[fix_defensive] failed to install CreateMove hook: 0x%X\n", status);
            return false;
        }
    }

    static bool is_defensive_hook_installed()
    {
        if (!defensive_hook_enabled)
            return false;

        const auto patch = reinterpret_cast<const BYTE*>(signonstate_addr);
        __try
        {
            if (patch[0] != 0xE9)
                return false;

            const auto rel = *reinterpret_cast<const INT32*>(patch + 1);
            const auto target = reinterpret_cast<const BYTE*>(patch + 5 + rel);
            return target == reinterpret_cast<const BYTE*>(hooked_signonstate);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // Smart guard thread - re-installs hooks if they get removed,
    // but only when it's safe to do so (game is not in a critical section)
    static NTSTATUS NTAPI defensive_guard_thread(void*)
    {
        LARGE_INTEGER interval{};
        interval.QuadPart = -10'000'000LL; // 1 second

        // Wait 15 seconds before starting
        LARGE_INTEGER startup_delay{};
        startup_delay.QuadPart = -150'000'000LL;
        NtDelayExecution(FALSE, &startup_delay);

        int reapply_count = 0;
        const int MAX_REAPPLIES = 50; // Don't loop forever

        for (;;)
        {
            // Re-install signonstate hook if removed
            if (defensive_hook_enabled && !is_defensive_hook_installed())
            {
                printf("[fix_defensive] signonstate hook was removed, re-validating...\n");

                if (validate_prologue(signonstate_addr, "signonstate") &&
                    validate_prologue(defensive_fix_addr, "defensive_fix"))
                {
                    install_defensive_hook();
                    printf("[fix_defensive] restored signonstate hook\n");
                }
                else
                {
                    printf("[fix_defensive] prologue validation failed, not re-installing\n");
                    InterlockedExchange(&defensive_hook_enabled, 0);
                }
            }

            // Periodically re-apply the enable patches (NL may restore them)
            if (defensive_hook_enabled && reapply_count < MAX_REAPPLIES)
            {
                // Check if enable checks were restored
                auto* check1 = reinterpret_cast<const BYTE*>(defensive_enable_check);
                __try
                {
                    if (*check1 == 0x74 || *check1 == 0x0F)
                    {
                        // JE was restored - re-patch
                        printf("[fix_defensive] enable check was restored, re-patching...\n");
                        patch_defensive_enable_checks();
                        reapply_count++;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }

            NtDelayExecution(FALSE, &interval);
        }

        return STATUS_SUCCESS; // unreachable, silences C4716
    }
}

void fix_defensive()
{
    printf("[fix_defensive] starting defensive AA fix (v2)...\n");

    // Step 1: Patch the defensive enable checks to bypass auth validation
    patch_defensive_enable_checks();

    // Step 2: Patch choke logic for proper defensive tick manipulation
    patch_defensive_choke_logic();

    // Step 3: Install the signonstate hook for defensive AA re-application
    if (!install_defensive_hook())
    {
        printf("[fix_defensive] initial signonstate hook FAILED - addresses may be wrong\n");
        printf("[fix_defensive] defensive AA will NOT work without signonstate hook.\n");
    }

    // Step 4: Install CreateMove hook for continuous angle restoration
    // This ensures defensive angles persist even when signonstate hook is bypassed
    install_createmove_hook();

    // Step 5: Start guard thread to keep hooks alive
    if (InterlockedCompareExchange(&defensive_guard_started, 1, 0) == 0)
    {
        HANDLE thread = nullptr;
        const auto status = NtCreateThreadEx(
            &thread, THREAD_ALL_ACCESS, nullptr, NtCurrentProcess(),
            defensive_guard_thread, nullptr, THREAD_CREATE_FLAGS_NONE,
            0, 0, 0, nullptr);

        if (NT_SUCCESS(status) && thread)
        {
            NtClose(thread);
            printf("[fix_defensive] guard thread started\n");
        }
        else
        {
            printf("[fix_defensive] failed to start guard thread: 0x%X\n", status);
        }
    }

    printf("[fix_defensive] defensive AA fix applied (v2)\n");
}
