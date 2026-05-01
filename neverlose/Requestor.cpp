#include <intrin.h>

#include "internal_fixes.h"
#include "neverlosesdk.hpp"
#include "HookFn.h"
#include "token.h"
#include <cstdarg>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ============================================================================
// Requestor.cpp — Replaces NL's network requestor with our own implementation
// ============================================================================
//
// We use the NLR_Requestor class approach (previously behind GHETTO_FIX ifdef)
// because it provides proper vtable-based dispatch for all requestor methods.
// The GHETTO_FIX approach only hooked GetSerial and MakeRequest via direct
// address hooks, leaving QueryLuaLibrary broken.
//
// QueryLuaLibrary now actually fetches from the VPS if the library is not
// found locally (fix_lua.cpp vtable hook serves embedded libs first).
//
// fn3 now delegates to fix_cfg.cpp's vtable hook for config/script handling.
// As a fallback, it returns appropriate responses for each type.
// ============================================================================

static void requestor_log(const char* fmt, ...)
{
    char path[MAX_PATH]{};
    if (!GetTempPathA(MAX_PATH, path))
        return;
    strcat_s(path, "nl_requestor_debug.log");

    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    char line[2048]{};
    va_list args;
    va_start(args, fmt);
    int len = vsprintf_s(line, fmt, args);
    va_end(args);

    if (len > 0)
    {
        DWORD written = 0;
        WriteFile(file, line, len, &written, nullptr);
    }

    CloseHandle(file);
}

void* reqtram = 0;
static int reqinst_call_count = 0;

void* hkReqInst()
{
    // Защита от бесконечного цикла
    reqinst_call_count++;
    if (reqinst_call_count > 100)
    {
        printf("[ERROR] hkReqInst called more than 100 times! Infinite loop detected!\n");
        printf("[ERROR] reqtram = 0x%p, returning nullptr\n", reqtram);
        reqinst_call_count = 0; // Reset counter
        return nullptr;
    }
    
    // Проверка что reqtram валидный
    if (!reqtram)
    {
        printf("[ERROR] reqtram is NULL in hkReqInst! Returning nullptr\n");
        return nullptr;
    }
    
    printf("[0x%lX] 0x%p Requestor::Instance (count=%d)\n", (unsigned long)NtCurrentThreadId(), _ReturnAddress(), reqinst_call_count);
    requestor_log("[0x%lX] 0x%p Requestor::Instance\n", (unsigned long)NtCurrentThreadId(), _ReturnAddress());
    
    void* result = reinterpret_cast<decltype(&hkReqInst)>(reqtram)();
    reqinst_call_count--; // Decrement after successful call
    return result;
};


class NLR_Requestor : public neverlosesdk::network::Requestor
{
        HINTERNET hSession;
        HINTERNET hConnection;

    static std::string with_token(std::string_view route)
    {
        std::string resolved(route.data(), route.size());
        ensure_auth_token_loaded();

        if (!auth_token || !auth_token[0])
            return resolved;

        if (resolved.find("token=") != std::string::npos)
            return resolved;

        if (!resolved.empty() && resolved[0] == '/')
            resolved += (resolved.find('?') == std::string::npos) ? '?' : '&';
        else
            return resolved;

        resolved += "token=";
        resolved += auth_token;
        return resolved;
    }

    void MakeRequest(std::string& out, std::string_view route, int _, int __) override
    {
        const std::string resolved_route = with_token(route);
        printf("[0x%p] 0x%p MakeRequest(%.*s, 0x%X, 0x%X)\n",
            NtCurrentThreadId(),
            _ReturnAddress(),
            (int)resolved_route.size(),
            resolved_route.data(),
            _,
            __);
        fflush(stdout);
        requestor_log("[0x%p] 0x%p MakeRequest(%.*s, 0x%X, 0x%X)\n",
            NtCurrentThreadId(),
            _ReturnAddress(),
            (int)resolved_route.size(),
            resolved_route.data(),
            _,
            __);

        new (&out) std::string("");

        int size_needed = MultiByteToWideChar(CP_UTF8, 0, resolved_route.data(), (int)resolved_route.size(), NULL, 0);
        wchar_t* wroute = (wchar_t*)malloc((size_needed + 1) * sizeof(wchar_t));

        if (wroute)
        {
            MultiByteToWideChar(CP_UTF8, 0, resolved_route.data(), (int)resolved_route.size(), wroute, size_needed);
            wroute[size_needed] = L'\0';
            HINTERNET hRequest = WinHttpOpenRequest(hConnection, L"GET", wroute, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            free(wroute);
            if (hRequest && WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            {
                if (WinHttpReceiveResponse(hRequest, NULL))
                {
                    DWORD status = 0;
                    DWORD status_size = sizeof(status);
                    WinHttpQueryHeaders(
                        hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status,
                        &status_size,
                        WINHTTP_NO_HEADER_INDEX
                    );
                    printf("[0x%lX] MakeRequest status=%lu\n", (unsigned long)NtCurrentThreadId(), status);
                    fflush(stdout);
                    requestor_log("[0x%p] MakeRequest status=%lu\n", NtCurrentThreadId(), status);

                    DWORD dwSize = 0;
                    DWORD dwDownloaded = 0;

                    do
                    {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;

                        size_t oldSize = out.size();
                        out.resize(oldSize + dwSize);

                        if (!WinHttpReadData(hRequest, &out[oldSize], dwSize, &dwDownloaded))
                        {
                            out.resize(oldSize);
                            break;
                        };

                        if (dwDownloaded < dwSize)
                            out.resize(oldSize + dwDownloaded);

                    } while (dwSize > 0);
                };
            };
            WinHttpCloseHandle(hRequest);
        };
        printf("[0x%lX] MakeRequest returned %zu bytes\n", (unsigned long)NtCurrentThreadId(), out.size());
        fflush(stdout);
        requestor_log("[0x%p] MakeRequest returned %zu bytes\n", NtCurrentThreadId(), out.size());
    };

    void GetSerial(std::string& out, nlohmann::json& request) override
    {
        ensure_auth_token_loaded();
        printf("[GetSerial] called from 0x%p\n", _ReturnAddress()); fflush(stdout);

        __try
        {
            printf("[GetSerial] request: %s\n", request.dump().c_str()); fflush(stdout);
            if (request.contains("params"))
            {
                auto& p = request["params"];
                if (p.contains("hash"))
                    printf("[GetSerial] hash:  %s\n", p["hash"].get<std::string>().c_str());
                if (p.contains("hash2"))
                    printf("[GetSerial] hash2: %s\n", p["hash2"].get<std::string>().c_str());
                fflush(stdout);
            }
            if (auth_token && auth_token[0])
                printf("[GetSerial] using website token (%zu bytes)\n", strlen(auth_token));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            printf("[GetSerial] WARNING: crashed reading request (debug/release STL mismatch?)\n"); fflush(stdout);
        }
        new (&out) std::string("g6w/cgN2AuDsLw3xrzboM1kbkLy+osvg0Y/j0LJnQf04GHbV8s5V4yReEk1mh3ZA2G72fHG3oOh7zlGEfR1nKw717WiwRwsrgSDfJtaTQz14VDDkayLBNV1DaT/qSyx8Frg1nXU0crRu1P/G+EPvH6nWNPYLZdUMIeqVCToEFhJnqiuRoAyypjFNiKnLEMiy5j2YvBcLCOC8yC3FPt/GGsvUldBqkmQGkBjIsXsSkut05txVxq7VDx1i9adKE4zalTzNHr0Vtd6DTr8aeH8NYHWPGWAsnTBkZlkNuRuhBTtgRTcIKxzGATTN4k8/JaXCpxri7IqsylvZgXQw+5zldLjAHqcAWw3OD5iQn8DtOoon+DrHm3k3FY6wIrCM1FzTdjAIcTvXSiWOURHiwA4sJ8ExR4dyBZMydo8aBAYjrRxcD9oDa/VVJT4cZfDkyWvRjI3WMyEajF2JhiGcjpjztmD8fyt9C16VXwLfoYuJnrX1/Dv8SZfCU6U2UhwJlxO5mkg+/IctveCdxy8IIiXTKwA5vmiEpXRuUu17SCdmJhFLZ+Jr6cTmrob4exSEggGRk6BTaVomOq4I6IpkVUBIUVup+4JvWFseL5UkPOQqHIO5Rxnj1jY+PjAWFPeeXSZsP8/ceEnX8J13tfb7PAqRSrpQ1Wv/y+OjaqMoPg9PiRE=");
        printf("[GetSerial] returned serial (%zu bytes)\n", out.size()); fflush(stdout);
    };

    void fn2() override
    {
        printf("[0x%lX] 0x%p fn2() called (ignored)\n", (unsigned long)NtCurrentThreadId(), _ReturnAddress());
        fflush(stdout);
    };

    // fn3 is the WebSocket data method - called for config, lua scripts, auth, etc.
    // Note: fix_cfg.cpp installs a vtable hook that overrides this method.
    // This implementation serves as a fallback if the vtable hook is not installed.
    void fn3(std::string& out, nlohmann::json& request) override
    {
        ensure_auth_token_loaded();
        int type = -1;
        if (request.contains("type"))
            type = request["type"].get<int>();

        printf("[0x%lX] 0x%p fn3(type=%d) called\n", (unsigned long)NtCurrentThreadId(), _ReturnAddress(), type);
        fflush(stdout);

        switch (type)
        {
        case 0: // Config request
            // Config handling is overridden by fix_cfg.cpp vtable hook
            // Fallback: return empty config list
            new (&out) std::string("{}");
            break;
        case 1: // Heartbeat
            new (&out) std::string("{\"status\":\"ok\"}");
            break;
        case 4: // Auth
            new (&out) std::string("{\"status\":\"ok\",\"authenticated\":true}");
            break;
        case 5: // Lua scripts
            // Script handling is overridden by fix_cfg.cpp vtable hook
            new (&out) std::string("{}");
            break;
        default:
            new (&out) std::string("{}");
            break;
        }
    };

    // QueryLuaLibrary: fetch Lua script source.
    // Note: fix_lua.cpp installs a vtable hook that overrides this method
    // to serve embedded libraries. This implementation serves as a fallback
    // that tries the VPS server, then falls back to a disk search.
    void QueryLuaLibrary(std::string& out, std::string_view name) override
    {
        ensure_auth_token_loaded();
        printf("[0x%p] QueryLuaLibrary(%.*s) fetching...\n",
            NtCurrentThreadId(), (int)name.size(), name.data());
        fflush(stdout);
        requestor_log("[0x%p] QueryLuaLibrary(%.*s) fetching\n",
            NtCurrentThreadId(), (int)name.size(), name.data());

        // Try fetching from the VPS server first
        std::string route = "/lua/";
        route.append(name.data(), name.size());
        route += "?cheat=csgo";
        route = with_token(route);

        int size_needed = MultiByteToWideChar(CP_UTF8, 0, route.data(), (int)route.size(), NULL, 0);
        wchar_t* wroute = (wchar_t*)malloc((size_needed + 1) * sizeof(wchar_t));

        new (&out) std::string("");

        if (wroute)
        {
            MultiByteToWideChar(CP_UTF8, 0, route.data(), (int)route.size(), wroute, size_needed);
            wroute[size_needed] = L'\0';
            HINTERNET hRequest = WinHttpOpenRequest(hConnection, L"GET", wroute, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            free(wroute);
            if (hRequest && WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            {
                if (WinHttpReceiveResponse(hRequest, NULL))
                {
                    DWORD dwSize = 0;
                    DWORD dwDownloaded = 0;
                    do
                    {
                        dwSize = 0;
                        if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) break;
                        size_t oldSize = out.size();
                        out.resize(oldSize + dwSize);
                        if (!WinHttpReadData(hRequest, &out[oldSize], dwSize, &dwDownloaded))
                        {
                            out.resize(oldSize);
                            break;
                        };
                        if (dwDownloaded < dwSize)
                            out.resize(oldSize + dwDownloaded);
                    } while (dwSize > 0);
                };
            };
            WinHttpCloseHandle(hRequest);
        };

        // If server didn't return anything, try reading from disk
        if (out.empty())
        {
            char module_path[MAX_PATH]{};
            if (GetModuleFileNameA(nullptr, module_path, MAX_PATH))
            {
                char* last_slash = strrchr(module_path, '\\');
                if (last_slash)
                {
                    *(last_slash + 1) = '\0';
                    // Try libraries/open_source/<name>.lua
                    char full_path[MAX_PATH]{};
                    std::string name_str(name.data(), name.size());
                    _snprintf_s(full_path, MAX_PATH, _TRUNCATE,
                        "%slibraries\\open_source\\%s.lua", module_path, name_str.c_str());

                    HANDLE hFile = CreateFileA(full_path, GENERIC_READ, FILE_SHARE_READ,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (hFile != INVALID_HANDLE_VALUE)
                    {
                        DWORD file_size = GetFileSize(hFile, nullptr);
                        if (file_size > 0 && file_size < 8 * 1024 * 1024)
                        {
                            out.resize(file_size);
                            DWORD bytes_read = 0;
                            if (ReadFile(hFile, &out[0], file_size, &bytes_read, nullptr) && bytes_read == file_size)
                            {
                                printf("[fix_lua] loaded library from disk: %.*s (%zu bytes)\n",
                                    (int)name.size(), name.data(), out.size());
                            }
                            else
                            {
                                out.clear();
                            }
                        }
                        CloseHandle(hFile);
                    }
                }
            }
        }

        // If still empty, return a Lua comment so the VM doesn't crash
        if (out.empty())
        {
            out = "-- library not found: ";
            out.append(name.data(), name.size());
            out += "\n";
        }

        printf("[0x%p] QueryLuaLibrary(%.*s) -> %zu bytes\n",
            NtCurrentThreadId(),
            (int)name.size(),
            name.data(),
            out.size());
        fflush(stdout);
        requestor_log("[0x%p] QueryLuaLibrary(%.*s) -> %zu bytes: %.*s\n",
            NtCurrentThreadId(),
            (int)name.size(),
            name.data(),
            out.size(),
            (int)(out.size() < 80 ? out.size() : 80),
            out.data());
    };

public:
    NLR_Requestor() : hSession(NULL), hConnection(NULL)
    {
        ensure_auth_token_loaded();
        requestor_log("[0x%p] NLR_Requestor ctor token_present=%d\n", NtCurrentThreadId(), auth_token && auth_token[0]);
        hSession = WinHttpOpen(L"NLR/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession)
        {
            // Set aggressive timeouts: 5s connect, 5s send, 5s recv
            WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);
            hConnection = WinHttpConnect(hSession, L"162.19.230.28", 30031, 0); // public VPS
        }
        if (!hConnection)
            printf("[NLR_Requestor] WARNING: WinHttpConnect to VPS failed! Error=%u\n", GetLastError());
        requestor_log("[0x%p] NLR_Requestor hSession=0x%p hConnection=0x%p\n", NtCurrentThreadId(), hSession, hConnection);
    };
};

void hijack_requestor()
{
    requestor_log("[0x%p] hijack_requestor installing\n", NtCurrentThreadId());
    *(neverlosesdk::network::Requestor**)0x42518C58 = new NLR_Requestor;
    *(PDWORD)0x42518C54 = 0x80000004;
    HookFn((PVOID)0x41BC9450, hkReqInst, 0, &reqtram);
    requestor_log("[0x%p] hijack_requestor installed reqtram=0x%p\n", NtCurrentThreadId(), reqtram);
}
