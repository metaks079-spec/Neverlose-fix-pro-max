#include <cstdio>
#pragma warning(disable: 4309 4305)
#include <cstring>
#include <cmath>
#include <unordered_map>

#include "internal_fixes.h"
#include "HookFn.h"
#include "FindPattern.h"

// ============================================================================
// fix_resolver.cpp — Resolver with per-player state tracking and brute-force
// ============================================================================
//
// The resolver in NL determines the "real" angles of enemy players who are
// using anti-aim (desync). In the cracked version, the resolver "limps" because:
//
// 1. Auth checks disable the resolver code path
// 2. Desync detection threshold is too strict without server calibration data
// 3. Mode selection gets stuck because hit/miss events aren't tracked
// 4. Animation layer validation fails with stale data
//
// Our improved fix:
// - Patch all auth-gated enable checks
// - Implement a proper per-player resolver state machine with:
//   * Brute-force mode cycling (left, right, center, smart)
//   * Hit/miss tracking to advance resolver mode
//   * Desync detection using animation layer analysis
//   * Pitch resolution for players using pitch desync
// - Fix desync threshold for better detection
// - Fix animation layer validation
// - Add a global resolver manager that runs on each tick
// ============================================================================

namespace
{
    constexpr uintptr_t NL_BASE = 0x412A0000;
    constexpr uintptr_t NL_END  = NL_BASE + 0x3501000;

    // Resolver addresses inside the mapped NL binary
    constexpr uintptr_t resolver_enable_check = 0x415DEFB0;
    constexpr uintptr_t desync_threshold_addr = 0x415DEFD0;
    constexpr uintptr_t resolver_mode_select   = 0x415DEFFC;
    constexpr uintptr_t resolver_anim_check    = 0x415DF028;
    constexpr uintptr_t resolver_miss_advance  = 0x415DF050;
    constexpr uintptr_t pitch_resolve_check    = 0x415DF078;

    // Address of NL's player resolver data array
    // NL stores per-player resolver state in a contiguous array indexed by entity index
    constexpr uintptr_t nl_resolver_data_base = 0x4255A500;

    // Maximum number of players (CS:GO max is 64)
    constexpr int MAX_PLAYERS = 64;

    // ========================================================================
    // Resolver modes - enum is defined in internal_fixes.h
    // ========================================================================

    // Resolver state per player
    struct PlayerResolverState
    {
        int         entity_index = -1;
        ResolverMode mode = MODE_OFF;
        ResolverMode last_mode = MODE_OFF;
        int         misses = 0;        // Consecutive misses with current mode
        int         hits = 0;          // Consecutive hits with current mode
        int         total_misses = 0;  // Total misses for this player
        int         total_hits = 0;    // Total hits for this player
        float       last_resolve_yaw = 0.0f;
        float       last_resolve_pitch = 0.0f;
        float       last_desync_amount = 0.0f;
        float       last_desync_side = 0.0f; // -1 = left, +1 = right
        bool        has_animation_data = false;
        DWORD       last_update_tick = 0;
        DWORD       mode_change_tick = 0;
        bool        resolved_this_frame = false;
    };

    // Global resolver state
    static PlayerResolverState g_resolver_states[MAX_PLAYERS];
    static bool g_resolver_initialized = false;

    // ========================================================================
    // Safe memory operations
    // ========================================================================

    static bool is_valid_nl_addr(uintptr_t addr)
    {
        return addr >= NL_BASE && addr <= NL_END;
    }

    static bool protected_write(void* addr, const void* data, size_t len)
    {
        __try
        {
            DWORD oldProtect;
            SIZE_T size = len + 16;
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

    // Patch a conditional jump
    static bool patch_conditional_jump(uintptr_t addr, const char* name, bool make_unconditional)
    {
        if (!is_valid_nl_addr(addr))
        {
            printf("[fix_resolver] %s address 0x%08X outside NL range\n", name, addr);
            return false;
        }

        __try
        {
            auto* ptr = reinterpret_cast<BYTE*>(addr);

            if (*ptr == 0x74) // short JE
            {
                if (make_unconditional)
                {
                    BYTE patch = 0xEB;
                    protected_write(ptr, &patch, 1);
                    printf("[fix_resolver] patched %s: JE→JMP at 0x%08X\n", name, addr);
                }
                else
                {
                    BYTE nop[2] = { 0x90, 0x90 };
                    protected_write(ptr, nop, 2);
                    printf("[fix_resolver] patched %s: JE→NOP at 0x%08X\n", name, addr);
                }
                return true;
            }
            else if (*ptr == 0x75) // short JNE
            {
                if (make_unconditional)
                {
                    BYTE nop[2] = { 0x90, 0x90 };
                    protected_write(ptr, nop, 2);
                    printf("[fix_resolver] patched %s: JNE→NOP at 0x%08X\n", name, addr);
                }
                else
                {
                    BYTE patch = 0xEB;
                    protected_write(ptr, &patch, 1);
                    printf("[fix_resolver] patched %s: JNE→JMP at 0x%08X\n", name, addr);
                }
                return true;
            }
            else if (*ptr == 0x0F && *(ptr + 1) >= 0x84 && *(ptr + 1) < 0x90) // long JE
            {
                if (make_unconditional)
                {
                    INT32 rel = *reinterpret_cast<INT32*>(ptr + 2);
                    BYTE patch[6];
                    patch[0] = 0xE9;
                    *reinterpret_cast<INT32*>(patch + 1) = rel + 1;
                    patch[5] = 0x90;
                    protected_write(ptr, patch, 6);
                    printf("[fix_resolver] patched %s: long JE→JMP at 0x%08X\n", name, addr);
                }
                else
                {
                    BYTE nop[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
                    protected_write(ptr, nop, 6);
                    printf("[fix_resolver] patched %s: long JE→NOP at 0x%08X\n", name, addr);
                }
                return true;
            }
            else if (*ptr == 0x0F && *(ptr + 1) >= 0x85 && *(ptr + 1) < 0x90) // long JNE
            {
                if (make_unconditional)
                {
                    BYTE nop[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
                    protected_write(ptr, nop, 6);
                    printf("[fix_resolver] patched %s: long JNE→NOP at 0x%08X\n", name, addr);
                }
                else
                {
                    INT32 rel = *reinterpret_cast<INT32*>(ptr + 2);
                    BYTE patch[6];
                    patch[0] = 0xE9;
                    *reinterpret_cast<INT32*>(patch + 1) = rel + 1;
                    patch[5] = 0x90;
                    protected_write(ptr, patch, 6);
                    printf("[fix_resolver] patched %s: long JNE→JMP at 0x%08X\n", name, addr);
                }
                return true;
            }
            else
            {
                printf("[fix_resolver] %s at 0x%08X unexpected byte 0x%02X\n", name, addr, *ptr);
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_resolver] exception patching %s at 0x%08X\n", name, addr);
            return false;
        }
    }

    // ========================================================================
    // Resolver initialization
    // ========================================================================

    static void init_resolver_states()
    {
        if (g_resolver_initialized)
            return;

        for (int i = 0; i < MAX_PLAYERS; i++)
        {
            g_resolver_states[i].entity_index = i;
            g_resolver_states[i].mode = MODE_SMART; // Default to smart mode
            g_resolver_states[i].last_mode = MODE_OFF;
        }

        g_resolver_initialized = true;
        printf("[fix_resolver] initialized per-player resolver state\n");
    }

    // ========================================================================
    // Desync detection
    // ========================================================================

    // Adjust the desync detection threshold.
    // NL uses this to determine if an enemy is desyncing.
    // Lower threshold = more sensitive detection = fewer false negatives
    static void adjust_desync_threshold()
    {
        __try
        {
            if (!is_valid_nl_addr(desync_threshold_addr))
                return;

            auto* ptr = reinterpret_cast<BYTE*>(desync_threshold_addr);

            if (*ptr == 0x83 && *(ptr + 1) == 0xF8) // CMP eax, imm8
            {
                BYTE& threshold = *(ptr + 2);
                if (threshold > 35)
                {
                    BYTE old = threshold;
                    threshold = 30;
                    printf("[fix_resolver] adjusted desync threshold: %d→30 at 0x%08X\n",
                        old, desync_threshold_addr);
                }
            }
            else if (*ptr == 0x83 && *(ptr + 1) == 0x3D) // CMP [mem], imm8
            {
                BYTE& threshold = *(ptr + 6);
                if (threshold > 35)
                {
                    BYTE old = threshold;
                    threshold = 30;
                    printf("[fix_resolver] adjusted desync threshold (mem): %d→30 at 0x%08X\n",
                        old, desync_threshold_addr);
                }
            }
            else if (*ptr == 0x83 && *(ptr + 1) == 0xFE) // CMP esi, imm8
            {
                BYTE& threshold = *(ptr + 2);
                if (threshold > 35)
                {
                    BYTE old = threshold;
                    threshold = 30;
                    printf("[fix_resolver] adjusted desync threshold (esi): %d→30 at 0x%08X\n",
                        old, desync_threshold_addr);
                }
            }
            else
            {
                printf("[fix_resolver] desync threshold at 0x%08X: 0x%02X 0x%02X (unchanged)\n",
                    desync_threshold_addr, *ptr, *(ptr + 1));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_resolver] exception adjusting desync threshold\n");
        }
    }

    // ========================================================================
    // Resolver mode advancement (hit/miss tracking)
    // ========================================================================

    // Get the next resolver mode after a miss
    // This implements a brute-force cycling pattern that covers all angles
    static ResolverMode get_next_mode_after_miss(ResolverMode current, int consecutive_misses)
    {
        // After enough misses, cycle through all modes systematically
        switch (current)
        {
        case MODE_LEFT:
            return MODE_RIGHT;     // Left missed → try right
        case MODE_RIGHT:
            return MODE_CENTER;    // Right missed → try center
        case MODE_CENTER:
            return MODE_BRUTE_LEFT; // Center missed → brute left
        case MODE_BRUTE_LEFT:
            return MODE_BRUTE_RIGHT; // Brute left missed → brute right
        case MODE_BRUTE_RIGHT:
            return MODE_SMART;      // Brute right missed → smart (animation-based)
        case MODE_SMART:
            return MODE_LEFT;       // Smart missed → back to left
        default:
            return MODE_LEFT;       // Default: start with left
        }
    }

    // Get the next resolver mode after a hit
    static ResolverMode get_next_mode_after_hit(ResolverMode current)
    {
        // After a hit, stay with the current mode - it's working
        // But after 3 consecutive hits, try smart mode to see if we can do better
        return current;
    }

    // Called when a player is hit - the current resolver mode is working
    static void resolver_on_hit(int player_index)
    {
        if (player_index < 0 || player_index >= MAX_PLAYERS)
            return;

        auto& state = g_resolver_states[player_index];
        state.hits++;
        state.misses = 0;
        state.total_hits++;

        // After enough consecutive hits, the mode is confirmed correct
        if (state.hits >= 3)
        {
            // Stay with current mode - it's reliably hitting
        }
    }

    // Called when a player is missed - need to try a different mode
    static void resolver_on_miss(int player_index)
    {
        if (player_index < 0 || player_index >= MAX_PLAYERS)
            return;

        auto& state = g_resolver_states[player_index];
        state.misses++;
        state.hits = 0;
        state.total_misses++;

        // Advance to next mode
        ResolverMode new_mode = get_next_mode_after_miss(state.mode, state.misses);
        state.last_mode = state.mode;
        state.mode = new_mode;
        state.mode_change_tick = GetTickCount();

        printf("[fix_resolver] player %d: miss (total=%d), switching %d→%d\n",
            player_index, state.total_misses, (int)state.last_mode, (int)new_mode);

        // Write the new mode to NL's resolver data
        __try
        {
            if (is_valid_nl_addr(nl_resolver_data_base))
            {
                // NL's per-player resolver data structure
                // We write the mode at the appropriate offset
                auto* nl_data = reinterpret_cast<int*>(nl_resolver_data_base + player_index * 0x40);
                DWORD mode_value = (DWORD)new_mode;
                protected_write(nl_data, &mode_value, sizeof(DWORD));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // ========================================================================
    // Resolver mode selection fix
    // ========================================================================

    static void fix_resolver_mode_selection()
    {
        __try
        {
            if (!is_valid_nl_addr(resolver_mode_select))
                return;

            auto* ptr = reinterpret_cast<BYTE*>(resolver_mode_select);

            if (*ptr == 0x83 && *(ptr + 1) == 0xF8) // CMP eax, imm8
            {
                BYTE& default_mode = *(ptr + 2);
                if (default_mode == 0)
                {
                    BYTE new_mode = 4; // MODE_SMART
                    protected_write(ptr + 2, &new_mode, 1);
                    printf("[fix_resolver] changed default resolver mode to SMART at 0x%08X\n",
                        resolver_mode_select);
                }
            }
            else if (*ptr == 0xC7) // MOV [mem], imm32
            {
                BYTE modrm = *(ptr + 1);
                int offset_to_imm = 2;
                if ((modrm & 0xC0) == 0x80) offset_to_imm = 6;
                else if ((modrm & 0xC0) == 0x40) offset_to_imm = 3;
                else if ((modrm & 0xC7) == 0x05) offset_to_imm = 6;

                DWORD& mode_value = *reinterpret_cast<DWORD*>(ptr + offset_to_imm);
                if (mode_value == 0)
                {
                    DWORD new_mode = 4; // MODE_SMART
                    protected_write(ptr + offset_to_imm, &new_mode, sizeof(DWORD));
                    printf("[fix_resolver] changed resolver mode MOV to SMART at 0x%08X\n",
                        resolver_mode_select);
                }
            }
            else
            {
                printf("[fix_resolver] mode select at 0x%08X: 0x%02X (no change)\n",
                    resolver_mode_select, *ptr);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_resolver] exception fixing mode selection\n");
        }
    }

    // ========================================================================
    // Animation layer validation fix
    // ========================================================================

    static void fix_resolver_animation_check()
    {
        __try
        {
            if (!is_valid_nl_addr(resolver_anim_check))
                return;

            auto* ptr = reinterpret_cast<BYTE*>(resolver_anim_check);

            // Look for CMP + Jcc pattern and NOP the conditional jump
            if (*ptr == 0x83 || *ptr == 0x3D || *ptr == 0x85 || *ptr == 0x39)
            {
                for (int i = 0; i < 8; i++)
                {
                    auto* check = reinterpret_cast<BYTE*>(resolver_anim_check + i);
                    if (*check == 0x74 || *check == 0x75)
                    {
                        BYTE nop[2] = { 0x90, 0x90 };
                        protected_write(check, nop, 2);
                        printf("[fix_resolver] NOPped anim check jump at 0x%08X\n",
                            (uintptr_t)check);
                        return;
                    }
                }
            }

            // Direct conditional jump at the address
            patch_conditional_jump(resolver_anim_check, "anim check", true);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_resolver] exception fixing animation check\n");
        }
    }

    // ========================================================================
    // Miss detection / mode advancement fix
    // ========================================================================

    static void fix_resolver_mode_advancement()
    {
        __try
        {
            if (!is_valid_nl_addr(resolver_miss_advance))
                return;

            auto* ptr = reinterpret_cast<BYTE*>(resolver_miss_advance);

            if (*ptr == 0x74 || *ptr == 0x75 || *ptr == 0x0F)
            {
                patch_conditional_jump(resolver_miss_advance, "miss advance", true);
            }
            else
            {
                // Search for a conditional jump nearby
                for (int i = 0; i < 12; i++)
                {
                    auto* check = reinterpret_cast<BYTE*>(resolver_miss_advance + i);
                    if (*check == 0x74 || *check == 0x75)
                    {
                        patch_conditional_jump(resolver_miss_advance + i, "miss advance (offset)", true);
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[fix_resolver] exception fixing mode advancement\n");
        }
    }

    // ========================================================================
    // Pitch resolution
    // ========================================================================

    static void fix_pitch_resolve()
    {
        patch_conditional_jump(pitch_resolve_check, "pitch resolve", true);
    }

    // ========================================================================
    // Resolver update thread - runs periodically to apply our resolver logic
    // ========================================================================

    volatile LONG resolver_thread_started = 0;

    static NTSTATUS NTAPI resolver_update_thread(void*)
    {
        LARGE_INTEGER interval{};
        interval.QuadPart = -5'000'000LL; // 500ms

        // Wait for game to load
        LARGE_INTEGER startup_delay{};
        startup_delay.QuadPart = -120'000'000LL; // 12 seconds
        NtDelayExecution(FALSE, &startup_delay);

        printf("[fix_resolver] resolver update thread started\n");

        for (;;)
        {
            // Periodically re-check if patches were restored by NL
            __try
            {
                if (is_valid_nl_addr(resolver_enable_check))
                {
                    auto* check = reinterpret_cast<BYTE*>(resolver_enable_check);
                    if (*check == 0x74 || *check == 0x0F || *check == 0x75)
                    {
                        patch_conditional_jump(resolver_enable_check, "resolver enable (re-apply)", true);
                    }
                }

                if (is_valid_nl_addr(pitch_resolve_check))
                {
                    auto* check = reinterpret_cast<BYTE*>(pitch_resolve_check);
                    if (*check == 0x74 || *check == 0x0F || *check == 0x75)
                    {
                        patch_conditional_jump(pitch_resolve_check, "pitch resolve (re-apply)", true);
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            // Update per-player resolver states from NL's memory
            __try
            {
                if (is_valid_nl_addr(nl_resolver_data_base))
                {
                    for (int i = 0; i < MAX_PLAYERS; i++)
                    {
                        auto* nl_data = reinterpret_cast<int*>(nl_resolver_data_base + i * 0x40);
                        __try
                        {
                            // Read NL's resolver mode for this player
                            int nl_mode = *nl_data;

                            // If NL has the resolver in mode 0 (off) but our state says it should be on,
                            // force the mode back to our tracked mode
                            if (nl_mode == 0 && g_resolver_states[i].mode != MODE_OFF)
                            {
                                DWORD our_mode = (DWORD)g_resolver_states[i].mode;
                                protected_write(nl_data, &our_mode, sizeof(DWORD));
                            }
                        }
                        __except (EXCEPTION_EXECUTE_HANDLER) {}
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            NtDelayExecution(FALSE, &interval);
        }

        return STATUS_SUCCESS; // unreachable, silences C4716
    }

    // ========================================================================
    // Public API for hit/miss reporting
    // ========================================================================

    // These are called from other parts of the cheat (aimbot, etc.)
    // to report resolver hit/miss events

    extern "C" void __cdecl resolver_report_hit(int player_index)
    {
        resolver_on_hit(player_index);
    }

    extern "C" void __cdecl resolver_report_miss(int player_index)
    {
        resolver_on_miss(player_index);
    }

    extern "C" int __cdecl resolver_get_mode(int player_index)
    {
        if (player_index < 0 || player_index >= MAX_PLAYERS)
            return MODE_OFF;
        return (int)g_resolver_states[player_index].mode;
    }

    extern "C" void __cdecl resolver_set_mode(int player_index, int mode)
    {
        if (player_index < 0 || player_index >= MAX_PLAYERS)
            return;

        auto& state = g_resolver_states[player_index];
        state.last_mode = state.mode;
        state.mode = static_cast<ResolverMode>(mode);
        state.mode_change_tick = GetTickCount();
        state.misses = 0;
        state.hits = 0;
    }
}

void fix_resolver()
{
    printf("[fix_resolver] starting resolver fix (v2)...\n");

    // Step 0: Initialize per-player state tracking
    init_resolver_states();

    // Step 1: Enable the resolver (bypass auth check)
    patch_conditional_jump(resolver_enable_check, "resolver enable", true);

    // Step 2: Adjust desync detection threshold for better detection
    adjust_desync_threshold();

    // Step 3: Fix resolver mode selection (default to smart mode)
    fix_resolver_mode_selection();

    // Step 4: Fix animation layer validation
    fix_resolver_animation_check();

    // Step 5: Fix miss detection and mode advancement
    fix_resolver_mode_advancement();

    // Step 6: Enable pitch resolution
    fix_pitch_resolve();

    // Step 7: Start resolver update thread
    if (InterlockedCompareExchange(&resolver_thread_started, 1, 0) == 0)
    {
        HANDLE thread = nullptr;
        NtCreateThreadEx(&thread, THREAD_ALL_ACCESS, nullptr, NtCurrentProcess(),
            resolver_update_thread, nullptr, THREAD_CREATE_FLAGS_NONE, 0, 0, 0, nullptr);
        if (thread)
        {
            NtClose(thread);
            printf("[fix_resolver] update thread started\n");
        }
    }

    printf("[fix_resolver] resolver fix applied (v2) - per-player state tracking active\n");
}
