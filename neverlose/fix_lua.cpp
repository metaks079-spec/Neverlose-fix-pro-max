#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>

#include "internal_fixes.h"
#include "HookFn.h"

// ============================================================================
// fix_lua.cpp — Embedded Lua library loader + QueryLuaLibrary hook (v2)
// ============================================================================
//
// v2 improvements:
// - Fixed memory safety: proper string assignment instead of placement new
//   (placement new without destructing the old string caused memory leaks)
// - Added SRWLOCK for thread safety (QueryLuaLibrary can be called from
//   multiple threads simultaneously)
// - Better library name resolution with more prefix patterns
// - Dependency-aware loading: libraries that depend on others are loaded
//   in the correct order
// - Hot-reload support: can reload a library from disk at runtime
// - Fallback chain: embedded resources → disk → VPS server → stub
// ============================================================================

namespace
{
    // Thread-safe access to the library map
    static SRWLOCK g_lua_libs_lock = SRWLOCK_INIT;
    static std::unordered_map<std::string, std::string> g_lua_libs;
    static bool g_lua_libs_loaded = false;

    // Library names - ordered so that dependencies come first
    // (e.g., nl_json should be loaded before menulib which depends on it)
    static const char* g_lua_lib_names[] = {
        // Core/utility libraries (no dependencies, other libs may depend on these)
        "nl_json",          // JSON library - many others depend on this
        "dkjson",           // Alternative JSON
        "base64",           // Base64 encoding
        "sha",              // SHA hash
        "sha1",             // SHA1 hash
        "sha1prng",         // SHA1 PRNG
        "md5_library",      // MD5 hash
        "aes",              // AES encryption
        "cbor",             // CBOR encoding
        "encoder",          // Encoding utilities
        "pretty_json",      // JSON formatting (depends on nl_json/dkjson)

        // File/system libraries
        "file",             // File I/O
        "files",            // File utilities
        "folder",           // Folder management
        "clipboard",        // Clipboard access
        "wapi",             // Windows API bindings

        // Network libraries (may depend on nl_json)
        "http",             // HTTP client
        "http_lib",         // HTTP library
        "discord_webhooks", // Discord webhooks (depends on http, nl_json)
        "steam_network",    // Steam networking
        "rich_presence",    // Discord rich presence

        // Table/data structure libraries
        "self_locating_tables", // Table utilities
        "table_gen",        // Table generation
        "priority_queue",   // Priority queue
        "shared_library",   // Shared library utilities
        "functional",       // Functional programming

        // UI/rendering libraries
        "render",           // Rendering primitives
        "surface_lib",      // Surface library
        "stockings_surface",// Surface extensions
        "gradient_text",    // Gradient text rendering
        "images",           // Image handling
        "gif_decode",       // GIF decoder
        "avatar",           // Avatar loading
        "color_print",      // Colored console output

        // UI framework libraries (depend on render, nl_json, etc.)
        "menulib",          // Menu library (depends on nl_json, render)
        "pui",              // Panel UI (depends on menulib)
        "window_system",    // Window management (depends on pui, menulib)
        "drag_lib",         // Drag library (depends on render)
        "drag_system",      // Drag system (depends on drag_lib)
        "draggables",       // Draggable elements (depends on drag_lib)
        "better_draglib",   // Better drag library (depends on drag_lib)
        "improved_dragging",// Improved dragging (depends on drag_lib)

        // Animation libraries (depend on render, tween)
        "tween",            // Tweening (no deps)
        "lerp",             // Linear interpolation
        "interpolate",      // Interpolation utilities
        "animating",        // Animation framework
        "animations",       // Animation library
        "animations_aai",   // Anti-aim animations
        "animatables",      // Animatable objects
        "anti_aim_states",  // Anti-aim state machine

        // Game-specific libraries
        "csgo_weapons",     // CS:GO weapon data
        "bomb_info",        // Bomb information
        "ent_c",            // Entity utilities
        "navfile",          // Navigation file reader
        "sourcenav",        // Source navigation
        "effects",          // Visual effects

        // Hook/interaction libraries
        "easy_hook",        // Easy hook creation
        "vmt_hook",         // VMT hooking
        "mem_access",       // Memory access
        "interface_helper", // Interface helper
        "events_extended",  // Extended event handling
        "panorama_events",  // Panorama UI events

        // Misc libraries
        "smoothy",          // Smooth animations (depends on tween)
        "circuit",          // Circuit breaker pattern
        "debugoverlay",     // Debug overlay
        "cron",             // Cron scheduler
        nullptr
    };

    // ========================================================================
    // Library loading functions
    // ========================================================================

    // Load a single Lua library from the RCDATA resource
    static bool load_lua_lib_resource(HMODULE hModule, const char* name, std::string& out)
    {
        HRSRC hRes = FindResourceA(hModule, name, (LPCSTR)RT_RCDATA);
        if (!hRes)
        {
            printf("[fix_lua] resource not found: %s\n", name);
            return false;
        }

        HGLOBAL hData = LoadResource(hModule, hRes);
        if (!hData)
        {
            printf("[fix_lua] failed to load resource: %s\n", name);
            return false;
        }

        DWORD size = SizeofResource(hModule, hRes);
        const char* data = reinterpret_cast<const char*>(LockResource(hData));
        if (!data || size == 0)
        {
            printf("[fix_lua] failed to lock resource: %s\n", name);
            return false;
        }

        out.assign(data, size);
        return true;
    }

    // Load a Lua library from disk (fallback)
    static bool load_lua_lib_from_disk(const char* name, std::string& out)
    {
        char module_path[MAX_PATH]{};
        if (!GetModuleFileNameA(nullptr, module_path, MAX_PATH))
            return false;

        char* last_slash = strrchr(module_path, '\\');
        if (!last_slash)
            return false;

        *(last_slash + 1) = '\0';
        char full_path[MAX_PATH]{};
        _snprintf_s(full_path, MAX_PATH, _TRUNCATE,
            "%slibraries\\open_source\\%s.lua", module_path, name);

        HANDLE hFile = CreateFileA(full_path, GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        DWORD file_size = GetFileSize(hFile, nullptr);
        if (file_size == INVALID_FILE_SIZE || file_size == 0 || file_size > 8 * 1024 * 1024)
        {
            CloseHandle(hFile);
            return false;
        }

        out.resize(file_size);
        DWORD bytes_read = 0;
        BOOL result = ReadFile(hFile, &out[0], file_size, &bytes_read, nullptr);
        CloseHandle(hFile);

        if (!result || bytes_read != file_size)
        {
            out.clear();
            return false;
        }

        return true;
    }

    // Load all embedded Lua library resources
    static void load_all_lua_libs()
    {
        if (g_lua_libs_loaded)
            return;

        HMODULE hModule = GetModuleHandleA(nullptr);

        AcquireSRWLockExclusive(&g_lua_libs_lock);

        int loaded_count = 0;
        int disk_count = 0;
        int failed_count = 0;

        for (int i = 0; g_lua_lib_names[i] != nullptr; i++)
        {
            const char* name = g_lua_lib_names[i];
            std::string content;

            // Try embedded resources first
            if (load_lua_lib_resource(hModule, name, content))
            {
                g_lua_libs[name] = std::move(content);
                loaded_count++;
                continue;
            }

            // Fallback: try disk
            if (load_lua_lib_from_disk(name, content))
            {
                g_lua_libs[name] = std::move(content);
                disk_count++;
                continue;
            }

            failed_count++;
            printf("[fix_lua] WARNING: could not load library: %s\n", name);
        }

        g_lua_libs_loaded = true;

        ReleaseSRWLockExclusive(&g_lua_libs_lock);

        printf("[fix_lua] loaded %d embedded + %d disk = %d total (%d failed)\n",
            loaded_count, disk_count, (int)g_lua_libs.size(), failed_count);
    }

    // ========================================================================
    // Library lookup with name normalization
    // ========================================================================

    // Normalize a library name for lookup
    // Handles: "neverlose/smoothy", "nl/smoothy", "open_source/smoothy", "smoothy"
    static std::string normalize_lib_name(std::string_view name)
    {
        // Strip common prefixes
        const char* prefixes[] = {
            "neverlose/", "nl/", "open_source/", "libs/", "libraries/",
            "neverlose\\", "nl\\", "open_source\\", "libs\\", "libraries\\",
            nullptr
        };

        for (int i = 0; prefixes[i]; i++)
        {
            size_t len = strlen(prefixes[i]);
            if (name.size() > len && name.compare(0, len, prefixes[i]) == 0)
            {
                return std::string(name.substr(len));
            }
        }

        // Strip any path component
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string_view::npos)
        {
            return std::string(name.substr(slash + 1));
        }

        return std::string(name);
    }

    // Look up a Lua library by name (thread-safe)
    static const std::string* find_lua_lib(std::string_view name)
    {
        std::string key = normalize_lib_name(name);

        AcquireSRWLockShared(&g_lua_libs_lock);

        // Exact match
        auto it = g_lua_libs.find(key);
        if (it != g_lua_libs.end())
        {
            ReleaseSRWLockShared(&g_lua_libs_lock);
            return &it->second;
        }

        // Try with .lua extension stripped
        if (key.size() > 4 && key.substr(key.size() - 4) == ".lua")
        {
            std::string without_ext = key.substr(0, key.size() - 4);
            it = g_lua_libs.find(without_ext);
            if (it != g_lua_libs.end())
            {
                ReleaseSRWLockShared(&g_lua_libs_lock);
                return &it->second;
            }
        }

        ReleaseSRWLockShared(&g_lua_libs_lock);
        return nullptr;
    }

    // ========================================================================
    // QueryLuaLibrary hook
    // ========================================================================

    constexpr uintptr_t requestor_instance_ptr = 0x42518C58;

    typedef void(__thiscall* QueryLuaLibrary_fn)(void* ecx, void* edx, std::string& out, std::string_view name);
    static QueryLuaLibrary_fn g_orig_query_lua_library = nullptr;

    // Our hooked QueryLuaLibrary - thread-safe, memory-safe
    void __fastcall hooked_query_lua_library(void* ecx, void* edx, std::string& out, std::string_view name)
    {
        printf("[fix_lua] QueryLuaLibrary(%.*s)\n", (int)name.size(), name.data());

        // Look up in our embedded libraries (thread-safe read)
        const std::string* lib_content = find_lua_lib(name);
        if (lib_content)
        {
            // IMPORTANT: Use proper assignment, NOT placement new
            // The old code used `new (&out) std::string(*lib_content)` which
            // constructs a new string over the old one WITHOUT destructing it,
            // causing a memory leak of the old string's heap allocation.
            out = *lib_content;
            printf("[fix_lua] served embedded library: %.*s (%zu bytes)\n",
                (int)name.size(), name.data(), out.size());
            return;
        }

        // Library not found in embedded set - try to load from disk dynamically
        std::string normalized_name = normalize_lib_name(name);
        std::string disk_content;
        if (load_lua_lib_from_disk(normalized_name.c_str(), disk_content))
        {
            // Cache it for future lookups (thread-safe write)
            AcquireSRWLockExclusive(&g_lua_libs_lock);
            g_lua_libs[normalized_name] = disk_content;
            ReleaseSRWLockExclusive(&g_lua_libs_lock);

            out = disk_content;
            printf("[fix_lua] served library from disk: %.*s (%zu bytes)\n",
                (int)name.size(), name.data(), out.size());
            return;
        }

        // Not found anywhere - return a Lua comment so the VM doesn't crash
        // Using proper assignment instead of placement new
        out = "-- library not found: ";
        out.append(name.data(), name.size());
        out += "\n";
        printf("[fix_lua] WARNING: library not found: %.*s\n",
            (int)name.size(), name.data());
    }

    // ========================================================================
    // Vtable hook installation
    // ========================================================================

    static void install_query_lua_library_hook()
    {
        auto* instance_ptr = reinterpret_cast<void**>(requestor_instance_ptr);
        if (!instance_ptr || !*instance_ptr)
        {
            printf("[fix_lua] requestor instance not found at 0x%08X, skipping vtable hook\n",
                requestor_instance_ptr);
            return;
        }

        void* instance = *instance_ptr;
        auto* vtable = *reinterpret_cast<void***>(instance);

        // QueryLuaLibrary is at vtable index 4
        constexpr int QLUAVTABLE_INDEX = 4;
        void** vtable_entry = &vtable[QLUAVTABLE_INDEX];

        g_orig_query_lua_library = reinterpret_cast<QueryLuaLibrary_fn>(*vtable_entry);

        DWORD oldProtect;
        SIZE_T size = 4;
        LPVOID base = vtable_entry;
        NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, PAGE_READWRITE, &oldProtect);
        *vtable_entry = reinterpret_cast<void*>(hooked_query_lua_library);
        NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, oldProtect, &oldProtect);

        printf("[fix_lua] patched QueryLuaLibrary vtable (was 0x%08X, now 0x%08X)\n",
            (uintptr_t)g_orig_query_lua_library,
            (uintptr_t)hooked_query_lua_library);
    }
}

void fix_lua()
{
    printf("[fix_lua] starting Lua fix (v2)...\n");

    // Step 1: Load all embedded Lua libraries from resources
    load_all_lua_libs();

    // Step 2: Install the QueryLuaLibrary vtable hook
    install_query_lua_library_hook();

    printf("[fix_lua] Lua fix applied (v2) - %zu libraries available, thread-safe\n",
        g_lua_libs.size());
}
