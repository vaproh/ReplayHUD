#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

namespace {

DWORD WINAPI LoadReplayHUDHook(LPVOID parameter) {
    const HMODULE proxyModule = static_cast<HMODULE>(parameter);
    wchar_t modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(proxyModule, modulePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        OutputDebugStringW(L"ReplayHUD D3D8 proxy: could not resolve its module path.\r\n");
        return 1;
    }

    std::wstring path(modulePath, length);
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return 1;
    path.resize(separator + 1);
    path += L"ReplayHUDHook.dll";

    HMODULE hook = LoadLibraryW(path.c_str());
    if (!hook) {
        wchar_t message[512]{};
        swprintf_s(message, L"ReplayHUD D3D8 proxy: failed to load %s (error %lu).\r\n",
            path.c_str(), GetLastError());
        OutputDebugStringW(message);
        return 1;
    }

    OutputDebugStringW(L"ReplayHUD D3D8 proxy: multi-API overlay hook loaded.\r\n");
    return 0;
}

} // namespace

// The d3d8to9 proxy translates the game's D3D8 calls into D3D9. Loading
// ReplayHUDHook from the same directory lets its D3D9 backend draw into that
// translated swap chain. Do the LoadLibrary work off the loader-lock thread.
BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, LoadReplayHUDHook, module, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            OutputDebugStringW(L"ReplayHUD D3D8 proxy: failed to create bootstrap thread.\r\n");
        }
    }
    return TRUE;
}
