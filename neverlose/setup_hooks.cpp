#include <intrin.h>
#include <winsock2.h>
#include <vector>
#include "json.hpp"
#include "neverlose.h"
#include "HookFn.h"
#include "neverlosesdk.hpp"

static void set_nl_logo(const char* name)
{
    constexpr size_t MAXLEN = 16;

    char buffer[MAXLEN] = { 0 };

    size_t len = strlen(name);
    if (len > MAXLEN)
        len = MAXLEN;

    memcpy(buffer, name, len);

    uint32_t* buff = reinterpret_cast<uint32_t*>(buffer);

    *(uint32_t*)0x4160555E = buff[0] ^ 0xD7E76FF9;
    *(uint32_t*)0x41605558 = buff[1] ^ 0xBA5A7287;
    *(uint32_t*)0x41605576 = buff[2] ^ 0x2D725D76;
    *(uint32_t*)0x41605570 = buff[3] ^ 0x4066CCAE;
}

HMODULE WaitForSingleModule(const char* module_name)
{
    HMODULE mod = nullptr;
    while (!mod)
    {
        mod = GetModuleHandleA(module_name);
        Sleep(0);
    }
    return mod;
}

void WSAAPI ProceedGetAddrInfo(PVOID retaddr, PCSTR* ppNodeName, PCSTR* ppServiceName)
{
    PVOID pBase = NULL;
    if (RtlPcToFileHeader(retaddr, &pBase) == (PVOID)0x412A0000)
    {
        printf("[0x%p] getaddrinfo(%s, %s)\n", NtCurrentThreadId(), *ppNodeName, *ppServiceName);
        *ppNodeName = "162.19.230.28";
        *ppServiceName = "30030";
    }
}

void* getaddr_tram = nullptr;
INT __declspec(naked) WSAAPI hkgetaddrinfo(PCSTR pNodeName, PCSTR pServiceName, const ADDRINFOA* pHints, PADDRINFOA* ppResult)
{
    __asm
    {
        push ebp
        mov ebp, esp
        lea eax, [ebp + 12]
        push eax
        lea eax, [ebp + 8]
        push eax
        push[ebp + 4]
        call ProceedGetAddrInfo
        mov esp, ebp
        pop ebp

        push ebp
        mov ebp, esp
        jmp getaddr_tram
    }
}

NTSTATUS hkterm(HANDLE, NTSTATUS)
{
    printf("Terminated from 0x%p\n", _ReturnAddress());
    RtlExitUserThread(STATUS_SUCCESS);
    return STATUS_SUCCESS;
}

void hkexit(int)
{
    printf("exit from 0x%p\n", _ReturnAddress());
    RtlExitUserThread(STATUS_SUCCESS);
}

void* quer_tram = 0;
NTSTATUS NTAPI hkNtQueryValueKey(
    HANDLE KeyHandle,
    PCUNICODE_STRING ValueName,
    KEY_VALUE_INFORMATION_CLASS KeyValueInformationClass,
    PVOID KeyValueInformation,
    ULONG Length,
    PULONG ResultLength
)
{
    ULONG size = 0;
    NtQueryKey(KeyHandle, KeyNameInformation, NULL, 0, &size);
    if (size)
    {
        PKEY_NAME_INFORMATION pkni = (PKEY_NAME_INFORMATION)malloc(size);
        if (pkni && NT_SUCCESS(NtQueryKey(KeyHandle, KeyNameInformation, pkni, size, &size)))
        {
            printf("[0x%p] 0x%p NtQueryValueKey(%.*ls)\n",
                NtCurrentThreadId(),
                _ReturnAddress(),
                pkni->NameLength / sizeof(*pkni->Name),
                pkni->Name);
        }
    }
    return reinterpret_cast<decltype(&NtQueryValueKey)>(quer_tram)(
        KeyHandle, ValueName, KeyValueInformationClass, KeyValueInformation, Length, ResultLength);
}

struct WMProtectDate
{
    unsigned short wYear;
    unsigned char bMonth;
    unsigned char bDay;
};

struct VMProtectSerialNumberData
{
    int nState;
    wchar_t wUserName[256];
    wchar_t wEMail[256];
    WMProtectDate dtExpire;
    WMProtectDate dtMaxBuild;
    int bRunningTime;
    unsigned char nUserDataLength;
    unsigned char bUserData[255];
};

void __stdcall errhandl(std::exception& ec, PVOID a2)
{
    UNREFERENCED_PARAMETER(ec);

    printf("[0x%p] 0x%p Throwed(0x%p)\n",
        NtCurrentThreadId(),
        _ReturnAddress(),
        a2);
}

void __fastcall performmenu(neverlosesdk::gui::Menu& menu)
{
    menu.IsOpen = !menu.IsOpen;
}

void* sndtram = 0;
void __fastcall hksend(void* hdl, void* edx, void* a1, void* const payload, size_t size)
{
    UNREFERENCED_PARAMETER(edx);
    UNREFERENCED_PARAMETER(payload);
    UNREFERENCED_PARAMETER(size);

    reinterpret_cast<void(__thiscall*)(void*, void*, void* const, size_t)>(sndtram)(
        hdl, a1, payload, size);
}

void neverlose::setup_hooks()
{
    HMODULE WS2 = WaitForSingleModule("ws2_32.dll");
    FARPROC getaddrinfo = GetProcAddress(WS2, "getaddrinfo");
    getaddr_tram = (PBYTE)getaddrinfo + 5;
    HookFn(getaddrinfo, hkgetaddrinfo, 0);

    HMODULE ntdll = GetModuleHandle(L"ntdll.dll");

    FARPROC ntterm = GetProcAddress(ntdll, "NtTerminateProcess");
    HookFn(ntterm, hkterm, 0);
    HookFn((PVOID)0x42026080, hkexit, 0);

    // This callback fires during script/runtime exceptions.
    // Suspending the whole process here turns recoverable failures into a hard hang.
    HookFn((PVOID)0x4200A118, errhandl, 0);
    HookFn((PVOID)0x415E9086, performmenu, 0);
    HookFn((PVOID)0x41609C80, performmenu, 0);

    // send_wrap is extremely hot during connect/map transitions. When the
    // target dump shifts even slightly, this hook becomes a crash magnet right
    // at "loading resources", while the proxy itself adds no functional logic.
    // Leave the send path untouched for stability.

    set_nl_logo("NEVERLOSE");
}
