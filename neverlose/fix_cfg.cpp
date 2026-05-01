#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <filesystem>

#include "internal_fixes.h"
#include "HookFn.h"
#include "json.hpp"

// ============================================================================
// fix_cfg.cpp — Local config save/load system (v2)
// ============================================================================
//
// v2 improvements:
// - Fixed memory safety: proper string assignment instead of placement new
// - NL response format compatibility: returns responses in the exact format
//   that NL's frontend expects (with proper "id", "name", "date" fields)
// - Config import/export support
// - Better error handling with descriptive error messages
// - Thread-safe config operations with SRWLOCK
// - Proper handling of all NL request types (0-7+)
// - Config data is stored as raw JSON exactly as NL sends it
// - Script handling returns proper format with "code" field
// ============================================================================

namespace
{
    constexpr uintptr_t requestor_instance_ptr = 0x42518C58;
    constexpr int FN3_VTABLE_INDEX = 3;

    static std::string g_config_dir;
    static SRWLOCK g_cfg_lock = SRWLOCK_INIT;

    // Original fn3 function pointer
    typedef void(__thiscall* fn3_t)(void* ecx, void* edx, std::string& out, nlohmann::json& request);
    static fn3_t g_orig_fn3 = nullptr;

    // ========================================================================
    // Config directory management
    // ========================================================================

    static const std::string& get_config_dir()
    {
        if (!g_config_dir.empty())
            return g_config_dir;

        char module_path[MAX_PATH]{};
        GetModuleFileNameA(nullptr, module_path, MAX_PATH);

        char* last_slash = strrchr(module_path, '\\');
        if (last_slash)
            *(last_slash + 1) = '\0';

        g_config_dir = module_path;
        g_config_dir += "nl_configs\\";

        CreateDirectoryA(g_config_dir.c_str(), nullptr);

        return g_config_dir;
    }

    static std::string get_scripts_dir()
    {
        std::string dir = get_config_dir() + "scripts\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }

    // ========================================================================
    // Filename sanitization
    // ========================================================================

    static std::string sanitize_name(const std::string& name)
    {
        std::string result;
        result.reserve(name.size());
        for (char c : name)
        {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' ')
            {
                result += c;
            }
            else
            {
                result += '_';
            }
        }
        if (result.empty())
            result = "unnamed";
        return result;
    }

    // ========================================================================
    // Config operations
    // ========================================================================

    // NL sends config save requests with the config data as a JSON object.
    // The "params" field contains:
    //   "name": config name
    //   "content" or "data": the actual config data (JSON string or object)
    // We save the entire request JSON to disk so we can return it exactly as-is.
    static void handle_config_save(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockExclusive(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "default");

        std::string filename = sanitize_name(name) + ".json";
        std::string filepath = get_config_dir() + filename;

        // Store the entire request for perfect round-trip compatibility
        // Add metadata that NL's frontend expects
        nlohmann::json save_data = request;
        save_data["_saved_at"] = (int64_t)time(nullptr);

        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
        if (file.is_open())
        {
            file << save_data.dump(2);
            file.close();
            printf("[fix_cfg] saved config: %s\n", name.c_str());
        }
        else
        {
            printf("[fix_cfg] failed to save config: %s\n", name.c_str());
        }

        ReleaseSRWLockExclusive(&g_cfg_lock);

        // Return success in the format NL expects
        nlohmann::json response;
        response["status"] = "ok";
        response["id"] = sanitize_name(name);
        response["name"] = name;
        out = response.dump();
    }

    // Load a config from disk and return it in the format NL's frontend expects
    static void handle_config_load(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockShared(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "default");

        std::string filename = sanitize_name(name) + ".json";
        std::string filepath = get_config_dir() + filename;

        std::ifstream file(filepath, std::ios::binary);
        if (file.is_open())
        {
            std::string content((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            file.close();

            // Try to parse the saved data and return the original request
            try
            {
                auto saved = nlohmann::json::parse(content);
                // Return the original config data, not the metadata
                if (saved.contains("params") && saved["params"].contains("content"))
                {
                    out = saved["params"]["content"].get<std::string>();
                }
                else if (saved.contains("params") && saved["params"].contains("data"))
                {
                    out = saved["params"]["data"].dump();
                }
                else
                {
                    // Return the whole saved data as-is
                    out = content;
                }
            }
            catch (...)
            {
                // If parsing fails, return raw content
                out = content;
            }

            printf("[fix_cfg] loaded config: %s (%zu bytes)\n", name.c_str(), out.size());
        }
        else
        {
            out = "{}";
            printf("[fix_cfg] config not found: %s\n", name.c_str());
        }

        ReleaseSRWLockShared(&g_cfg_lock);
    }

    // List all saved configs
    static void handle_config_list(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockShared(&g_cfg_lock);

        nlohmann::json configs = nlohmann::json::array();

        const std::string& dir = get_config_dir();
        std::string search_pattern = dir + "*.json";

        WIN32_FIND_DATAA find_data;
        HANDLE hFind = FindFirstFileA(search_pattern.c_str(), &find_data);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    std::string filename = find_data.cFileName;
                    if (filename.size() > 5 && filename.substr(filename.size() - 5) == ".json")
                        filename = filename.substr(0, filename.size() - 5);

                    // Build the config entry in the format NL's frontend expects
                    nlohmann::json entry;
                    entry["id"] = filename;
                    entry["name"] = filename;

                    // Try to read the saved date
                    std::string filepath = dir + find_data.cFileName;
                    std::ifstream file(filepath, std::ios::binary);
                    if (file.is_open())
                    {
                        try
                        {
                            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
                            auto saved = nlohmann::json::parse(content);
                            if (saved.contains("_saved_at"))
                            {
                                entry["date"] = saved["_saved_at"].get<int64_t>();
                            }
                            else
                            {
                                // Use file modification time
                                FILETIME ft = find_data.ftLastWriteTime;
                                LONGLONG ll = (LONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
                                entry["date"] = (int64_t)(ll / 10000000 - 11644473600); // Windows FT to Unix
                            }
                        }
                        catch (...)
                        {
                            entry["date"] = 0;
                        }
                        file.close();
                    }

                    configs.push_back(entry);
                }
            } while (FindNextFileA(hFind, &find_data));
            FindClose(hFind);
        }

        ReleaseSRWLockShared(&g_cfg_lock);

        // NL's frontend expects a specific format for config lists
        nlohmann::json response;
        response["configs"] = configs;
        response["status"] = "ok";
        out = response.dump();

        printf("[fix_cfg] listed %zu configs\n", configs.size());
    }

    // Delete a config
    static void handle_config_delete(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockExclusive(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "");

        std::string filename = sanitize_name(name) + ".json";
        std::string filepath = get_config_dir() + filename;

        bool deleted = DeleteFileA(filepath.c_str()) != 0;

        ReleaseSRWLockExclusive(&g_cfg_lock);

        nlohmann::json response;
        response["status"] = deleted ? "ok" : "error";
        if (!deleted)
            response["message"] = "Failed to delete config";
        out = response.dump();

        printf("[fix_cfg] %s config: %s\n", deleted ? "deleted" : "failed to delete", name.c_str());
    }

    // ========================================================================
    // Script operations
    // ========================================================================

    static void handle_script_save(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockExclusive(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "unnamed");
        std::string content = params.value("content", params.value("code", ""));

        std::string scripts_dir = get_scripts_dir();
        std::string filename = sanitize_name(name) + ".lua";
        std::string filepath = scripts_dir + filename;

        // Save script metadata alongside the script
        nlohmann::json meta;
        meta["name"] = name;
        meta["saved_at"] = (int64_t)time(nullptr);
        meta["code"] = content;

        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
        if (file.is_open())
        {
            // Save just the code part (NL expects to read code, not JSON)
            file << content;
            file.close();

            // Save metadata separately
            std::string meta_path = scripts_dir + sanitize_name(name) + ".meta";
            std::ofstream meta_file(meta_path, std::ios::binary | std::ios::trunc);
            if (meta_file.is_open())
            {
                meta_file << meta.dump(2);
                meta_file.close();
            }

            printf("[fix_cfg] saved script: %s (%zu bytes)\n", name.c_str(), content.size());
        }

        ReleaseSRWLockExclusive(&g_cfg_lock);

        nlohmann::json response;
        response["status"] = "ok";
        response["id"] = sanitize_name(name);
        out = response.dump();
    }

    static void handle_script_load(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockShared(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "");

        std::string scripts_dir = get_scripts_dir();
        std::string filename = sanitize_name(name) + ".lua";
        std::string filepath = scripts_dir + filename;

        std::ifstream file(filepath, std::ios::binary);
        if (file.is_open())
        {
            std::string content((std::istreambuf_iterator<char>(file)),
                std::istreambuf_iterator<char>());
            file.close();

            // NL's frontend expects the script code in a "code" field
            nlohmann::json response;
            response["code"] = content;
            response["name"] = name;
            response["status"] = "ok";
            out = response.dump();

            printf("[fix_cfg] loaded script: %s (%zu bytes)\n", name.c_str(), content.size());
        }
        else
        {
            nlohmann::json response;
            response["status"] = "not_found";
            response["code"] = "";
            out = response.dump();

            printf("[fix_cfg] script not found: %s\n", name.c_str());
        }

        ReleaseSRWLockShared(&g_cfg_lock);
    }

    static void handle_script_list(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockShared(&g_cfg_lock);

        nlohmann::json scripts = nlohmann::json::array();

        std::string scripts_dir = get_scripts_dir();
        std::string search_pattern = scripts_dir + "*.lua";

        WIN32_FIND_DATAA find_data;
        HANDLE hFind = FindFirstFileA(search_pattern.c_str(), &find_data);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                {
                    std::string filename = find_data.cFileName;
                    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".lua")
                        filename = filename.substr(0, filename.size() - 4);

                    nlohmann::json entry;
                    entry["id"] = filename;
                    entry["name"] = filename;

                    // Try to read metadata
                    std::string meta_path = scripts_dir + filename + ".meta";
                    std::ifstream meta_file(meta_path, std::ios::binary);
                    if (meta_file.is_open())
                    {
                        try
                        {
                            std::string meta_content((std::istreambuf_iterator<char>(meta_file)),
                                std::istreambuf_iterator<char>());
                            auto meta = nlohmann::json::parse(meta_content);
                            if (meta.contains("saved_at"))
                                entry["date"] = meta["saved_at"].get<int64_t>();
                        }
                        catch (...) {}
                        meta_file.close();
                    }

                    scripts.push_back(entry);
                }
            } while (FindNextFileA(hFind, &find_data));
            FindClose(hFind);
        }

        ReleaseSRWLockShared(&g_cfg_lock);

        nlohmann::json response;
        response["scripts"] = scripts;
        response["status"] = "ok";
        out = response.dump();

        printf("[fix_cfg] listed %zu scripts\n", scripts.size());
    }

    static void handle_script_delete(std::string& out, nlohmann::json& request)
    {
        AcquireSRWLockExclusive(&g_cfg_lock);

        auto& params = request["params"];
        std::string name = params.value("name", "");

        std::string scripts_dir = get_scripts_dir();
        std::string filename = sanitize_name(name) + ".lua";
        std::string filepath = scripts_dir + filename;
        std::string meta_path = scripts_dir + sanitize_name(name) + ".meta";

        bool deleted = DeleteFileA(filepath.c_str()) != 0;
        DeleteFileA(meta_path.c_str()); // Also delete metadata

        ReleaseSRWLockExclusive(&g_cfg_lock);

        nlohmann::json response;
        response["status"] = deleted ? "ok" : "error";
        out = response.dump();
    }

    // ========================================================================
    // fn3 hook - handles all request types
    // ========================================================================

    void __fastcall hooked_fn3(void* ecx, void* edx, std::string& out, nlohmann::json& request)
    {
        int type = -1;
        if (request.contains("type"))
            type = request["type"].get<int>();

        std::string action;
        if (request.contains("params") && request["params"].contains("action"))
            action = request["params"]["action"].get<std::string>();

        printf("[fix_cfg] fn3(type=%d, action=%s)\n", type, action.c_str());

        switch (type)
        {
        case 0: // Config
            if (action == "save" || action == "set" || action == "update")
                handle_config_save(out, request);
            else if (action == "load" || action == "get")
                handle_config_load(out, request);
            else if (action == "list" || action == "get_all")
                handle_config_list(out, request);
            else if (action == "delete" || action == "remove")
                handle_config_delete(out, request);
            else if (request.contains("params") && request["params"].contains("content"))
                handle_config_save(out, request);
            else if (request.contains("params") && request["params"].contains("name"))
                handle_config_load(out, request);
            else
                handle_config_list(out, request);
            return;

        case 1: // Heartbeat
            out = "{\"status\":\"ok\",\"type\":\"heartbeat\"}";
            return;

        case 2: // Skin/data request
            out = "{\"status\":\"ok\",\"data\":{}}";
            return;

        case 3: // Entity/netvar data
            out = "{\"status\":\"ok\"}";
            return;

        case 4: // Auth
            out = "{\"status\":\"ok\",\"authenticated\":true,\"type\":\"premium\"}";
            return;

        case 5: // Lua scripts
            if (action == "save" || action == "set" || action == "update")
                handle_script_save(out, request);
            else if (action == "load" || action == "get")
                handle_script_load(out, request);
            else if (action == "list" || action == "get_all")
                handle_script_list(out, request);
            else if (action == "delete" || action == "remove")
                handle_script_delete(out, request);
            else
                handle_script_list(out, request);
            return;

        case 6: // Style/theme data
            out = "{\"status\":\"ok\"}";
            return;

        case 7: // Subscription/license info
            out = "{\"status\":\"ok\",\"subscription\":{\"type\":\"premium\",\"expires\":9999999999}}";
            return;

        default:
            out = "{\"status\":\"ok\"}";
            return;
        }
    }

    // ========================================================================
    // Vtable hook installation
    // ========================================================================

    static void install_fn3_hook()
    {
        auto* instance_ptr = reinterpret_cast<void**>(requestor_instance_ptr);
        if (!instance_ptr || !*instance_ptr)
        {
            printf("[fix_cfg] requestor instance not found at 0x%08X, skipping fn3 hook\n",
                requestor_instance_ptr);
            return;
        }

        void* instance = *instance_ptr;
        auto* vtable = *reinterpret_cast<void***>(instance);
        void** vtable_entry = &vtable[FN3_VTABLE_INDEX];

        g_orig_fn3 = reinterpret_cast<fn3_t>(*vtable_entry);

        DWORD oldProtect;
        SIZE_T size = 4;
        LPVOID base = vtable_entry;
        NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, PAGE_READWRITE, &oldProtect);
        *vtable_entry = reinterpret_cast<void*>(hooked_fn3);
        NtProtectVirtualMemory(NtCurrentProcess(), &base, &size, oldProtect, &oldProtect);

        printf("[fix_cfg] patched fn3 vtable (was 0x%08X, now 0x%08X)\n",
            (uintptr_t)g_orig_fn3,
            (uintptr_t)hooked_fn3);
    }
}

void fix_cfg()
{
    printf("[fix_cfg] starting config fix (v2)...\n");

    // Create the config directory
    get_config_dir();
    get_scripts_dir();

    // Install the fn3 vtable hook
    install_fn3_hook();

    printf("[fix_cfg] config fix applied (v2) - configs in %s\n", g_config_dir.c_str());
}
