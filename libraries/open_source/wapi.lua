local ffi = require("ffi")
local bit = require("bit")

ffi.cdef[[
    typedef void* HWND;
    typedef const char* LPCSTR;
    typedef unsigned long DWORD;
    typedef uintptr_t ULONG_PTR;
    typedef unsigned short WORD;
    typedef unsigned char BYTE;
    typedef void* HANDLE;

    typedef struct {
        DWORD cb;
        HWND hwnd;
        LPCSTR lpReserved;
        LPCSTR lpDesktop;
        LPCSTR lpTitle;
        DWORD dwX;
        DWORD dwY;
        DWORD dwXSize;
        DWORD dwYSize;
        DWORD dwXCountChars;
        DWORD dwYCountChars;
        DWORD dwFillAttribute;
        DWORD dwFlags;
        WORD wShowWindow;
        WORD cbReserved2;
        BYTE* lpReserved2;
        DWORD dwStdInput;
        DWORD dwStdOutput;
        DWORD dwStdError;
    } STARTUPINFOA;

    typedef struct {
        DWORD dwProcessId;
        DWORD dwThreadId;
        DWORD dwPriorityClass;
        HANDLE hProcess;
        HANDLE hThread;
        DWORD dwCreationFlags;
        DWORD dwExitCode;
        DWORD dwExitProcess;
    } PROCESS_INFORMATION;

    void* ShellExecuteA(HWND hwnd, LPCSTR lpOperation, LPCSTR lpFile, LPCSTR lpParameters, LPCSTR lpDirectory, int nShowCmd);
    int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, unsigned int uType);
    HWND GetForegroundWindow();
    int SetWindowTextA(HWND hWnd, LPCSTR lpString);
    int GetWindowTextA(HWND hWnd, LPCSTR lpString, int nMaxCount);
    int PostMessageA(HWND hWnd, unsigned int Msg, ULONG_PTR wParam, intptr_t lParam);
    int ShowWindow(HWND hWnd, int nCmdShow);
    HWND FindWindowA(LPCSTR lpClassName, LPCSTR lpWindowName);
    int GetComputerNameA(LPCSTR lpBuffer, DWORD* nSize);
    int GetFileAttributesA(const char* lpFileName);
    DWORD GetLastError();
    int CreateProcessA(LPCSTR lpApplicationName, LPCSTR lpCommandLine, void* lpProcessAttributes, void* lpThreadAttributes, int bInheritHandles, DWORD dwCreationFlags, void* lpEnvironment, LPCSTR lpCurrentDirectory, STARTUPINFOA* lpStartupInfo, PROCESS_INFORMATION* lpProcessInformation);
    DWORD GetCurrentProcessId();
    int OpenProcess(DWORD dwDesiredAccess, int bInheritHandle, DWORD dwProcessId);
    void* GlobalAlloc(DWORD dwFlags, size_t dwBytes);
    void* GlobalLock(void* hMem);
    void GlobalUnlock(void* hMem);
    void* GlobalFree(void* hMem);
    int SetClipboardData(int uFormat, HANDLE hMem);
    int OpenClipboard(HWND hWnd);
    int EmptyClipboard();
    int CloseClipboard();
    int GetVersionExA(void* lpVersionInformation);
    int SetForegroundWindow(HWND hWnd);
]]

local shell32 = ffi.load("shell32")
local user32 = ffi.load("user32")
local kernel32 = ffi.load("kernel32")
local comdlg32 = ffi.load("Comdlg32")

local WM_CLOSE = 0x0010
local SW_MINIMIZE = 6
local HWND_TOP = ffi.cast("HWND", 0)
local SWP_SHOWWINDOW = 0x0040
local INVALID_FILE_ATTRIBUTES = -1
local CF_UNICODETEXT = 13

local function open_folder(folder_path)
    local result = shell32.ShellExecuteA(nil, "explore", folder_path, nil, nil, 1)
    if result == nil or tonumber(ffi.cast("intptr_t", result)) <= 32 then
        error("Failed to open folder, error code: " .. tonumber(ffi.cast("intptr_t", result)))
    end
end

local function run_application(file_path, parameters)
    local result = shell32.ShellExecuteA(nil, "open", file_path, parameters, nil, 1)
    if result == nil or tonumber(ffi.cast("intptr_t", result)) <= 32 then
        error("Failed to run application, error code: " .. tonumber(ffi.cast("intptr_t", result)))
    end
end

local function show_message(message, title, msg_type)
    msg_type = msg_type or 0
    user32.MessageBoxA(nil, message, title or "Message", msg_type)
end

local function set_window_title(new_title)
    local hwnd = user32.GetForegroundWindow()
    if hwnd == nil then
        error("Failed to get foreground window")
    end
    if user32.SetWindowTextA(hwnd, new_title) == 0 then
        error("Failed to set window title")
    end
end

local function get_window_title()
    local hwnd = user32.GetForegroundWindow()
    if hwnd == nil then
        error("Failed to get foreground window")
    end
    local buffer = ffi.new("char[256]")
    local length = user32.GetWindowTextA(hwnd, buffer, ffi.sizeof(buffer))
    if length == 0 then
        error("Failed to get window title")
    end
    return ffi.string(buffer, length)
end

local function close_window(hwnd)
    local result = user32.PostMessageA(hwnd, WM_CLOSE, 0, 0)
    if result == 0 then
        error("Failed to send WM_CLOSE message")
    end
end

local function file_exists(path)
    local attributes = kernel32.GetFileAttributesA(path)
    if attributes == INVALID_FILE_ATTRIBUTES then
        return false
    end
    return true
end

local function get_computer_name()
    local buffer = ffi.new("char[256]")
    local size = ffi.new("DWORD[1]", 256)
    if kernel32.GetComputerNameA(buffer, size) == 0 then
        error("Failed to get computer name")
    end
    return ffi.string(buffer)
end

local function get_current_process_id()
    return kernel32.GetCurrentProcessId()
end

local function hide_window()
    local hwnd = user32.GetForegroundWindow()
    if hwnd == nil then
        error("Failed to get foreground window")
    end
    if user32.ShowWindow(hwnd, SW_MINIMIZE) == 0 then
        error("Failed to minimize window")
    end
end

local function get_last_error_message()
    local error_code = kernel32.GetLastError()
    return string.format("Error code: %d", error_code)
end

local function find_window_by_title(title)
    local hwnd = user32.FindWindowA(nil, title)
    if hwnd == nil then
        error("Window not found")
    end
    return hwnd
end

local function get_process_info_by_pid(pid)
    local result = kernel32.OpenProcess(0x1F0FFF, false, pid)
    if result == nil then
        error("Failed to get process info, error code: " .. kernel32.GetLastError())
    end
    return result
end

local function kill_process_by_name(process_name)
    local find_process_cmd = "taskkill /F /IM " .. process_name
    run_application("cmd.exe", "/C " .. find_process_cmd)
end

local function xor_encrypt(input, key)
    local encrypted = {}
    for i = 1, #input do
        local char = string.byte(input, i)
        local enc_char = bit.bxor(char, key)
        table.insert(encrypted, string.char(enc_char))
    end
    return table.concat(encrypted)
end

local function xor_decrypt(input, key)
    return xor_encrypt(input, key)
end

local function add_to_autostart(program_path)
    local reg_key = "HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\MyProgram"
    local result = run_application("cmd.exe", "/C reg add \"" .. reg_key .. "\" /v MyProgram /t REG_SZ /d \"" .. program_path .. "\" /f")
    if result ~= 0 then
        error("Failed to add program to autostart")
    end
end

local function create_process_as_admin(command)
    local result = shell32.ShellExecuteA(nil, "runas", "cmd.exe", "/C " .. command, nil, 1)
    if result == nil or tonumber(ffi.cast("intptr_t", result)) <= 32 then
        error("Failed to create process as admin, error code: " .. tonumber(ffi.cast("intptr_t", result)))
    end
end

local SW_RESTORE = 9
local SW_SHOWNORMAL = 1

local function restore_game_window(window_name)
    local hwnd = user32.FindWindowA(nil, window_name)
    if hwnd == nil then
        error("Failed to find window with name: " .. window_name)
    end

    if user32.SetForegroundWindow(hwnd) == 0 then
        error("Failed to bring window to foreground")
    end

    if user32.ShowWindow(hwnd, SW_RESTORE) == 0 then
        error("Failed to restore window")
    end

    if user32.ShowWindow(hwnd, SW_SHOWNORMAL) == 0 then
        error("Failed to show window normally")
    end
end

return {
    open_folder = open_folder,
    run_application = run_application,
    show_message = show_message,
    set_window_title = set_window_title,
    get_window_title = get_window_title,
    close_window = close_window,
    file_exists = file_exists,
    get_computer_name = get_computer_name,
    get_current_process_id = get_current_process_id,
    hide_window = hide_window,
    get_last_error_message = get_last_error_message,
    find_window_by_title = find_window_by_title,
    get_process_info_by_pid = get_process_info_by_pid,
    kill_process_by_name = kill_process_by_name,
    xor_encrypt = xor_encrypt,
    xor_decrypt = xor_decrypt,
    add_to_autostart = add_to_autostart,
    create_process_as_admin = create_process_as_admin,
    restore_and_foreground_window = restore_and_foreground_window,
}