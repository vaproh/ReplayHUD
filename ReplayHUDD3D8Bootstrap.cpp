#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "resource.h"
#include "ReplayHUDBuildConfig.h"

namespace {

bool EnsureDirectoryExists(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ResourceFileMatches(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER length{};
    bool matches = GetFileSizeEx(file, &length) != FALSE &&
        length.QuadPart == static_cast<LONGLONG>(size);
    if (matches) {
        std::vector<BYTE> contents(size);
        DWORD read = 0;
        matches = ReadFile(file, contents.data(), size, &read, nullptr) != FALSE &&
            read == size && std::memcmp(contents.data(), data, size) == 0;
    }

    CloseHandle(file);
    return matches;
}

std::wstring ExtractEmbeddedHook(HMODULE proxyModule) {
    HRSRC resource = FindResourceW(proxyModule,
        MAKEINTRESOURCEW(IDR_D3D8_REPLAYHUD_HOOK), MAKEINTRESOURCEW(10));
    if (!resource) return {};

    HGLOBAL loaded = LoadResource(proxyModule, resource);
    const BYTE* data = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    const DWORD size = SizeofResource(proxyModule, resource);
    if (!data || size < 2 || data[0] != 'M' || data[1] != 'Z') return {};

    wchar_t localAppData[32768]{};
    const DWORD localAppDataLength = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData, static_cast<DWORD>(_countof(localAppData)));
    if (localAppDataLength == 0 || localAppDataLength >= _countof(localAppData)) return {};

    const std::wstring appDirectory = std::wstring(localAppData, localAppDataLength) + L"\\ReplayHUD";
    const std::wstring runtimeDirectory = appDirectory + L"\\Runtime";
    const std::wstring d3d8Directory = runtimeDirectory + L"\\D3D8";
    const std::wstring buildDirectory = d3d8Directory + L"\\" REPLAYHUD_BUILD_ID;
    if (!EnsureDirectoryExists(appDirectory) ||
        !EnsureDirectoryExists(runtimeDirectory) ||
        !EnsureDirectoryExists(d3d8Directory) ||
        !EnsureDirectoryExists(buildDirectory)) {
        return {};
    }

    const std::wstring hookPath = buildDirectory + L"\\ReplayHUDHook.dll";
    if (ResourceFileMatches(hookPath, data, size)) return hookPath;

    const std::wstring temporaryPath = hookPath + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    DWORD written = 0;
    const bool saved = WriteFile(file, data, size, &written, nullptr) != FALSE &&
        written == size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);

    if (!saved) {
        DeleteFileW(temporaryPath.c_str());
        return {};
    }
    if (!MoveFileExW(temporaryPath.c_str(), hookPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        // Another game process may have extracted the same build concurrently.
        return ResourceFileMatches(hookPath, data, size) ? hookPath : std::wstring{};
    }
    return hookPath;
}

DWORD WINAPI LoadReplayHUDHook(LPVOID parameter) {
    const HMODULE proxyModule = static_cast<HMODULE>(parameter);
    const std::wstring hookPath = ExtractEmbeddedHook(proxyModule);
    if (hookPath.empty()) {
        OutputDebugStringW(L"ReplayHUD D3D8 proxy: could not extract its embedded ReplayHUDHook.dll.\r\n");
        return 1;
    }

    HMODULE hook = LoadLibraryW(hookPath.c_str());
    if (!hook) {
        wchar_t message[512]{};
        swprintf_s(message, L"ReplayHUD D3D8 proxy: failed to load %s (error %lu).\r\n",
            hookPath.c_str(), GetLastError());
        OutputDebugStringW(message);
        return 1;
    }

    OutputDebugStringW(L"ReplayHUD D3D8 proxy: embedded multi-API overlay hook loaded.\r\n");
    return 0;
}

} // namespace

// The proxy translates D3D8 calls into D3D9. Load the embedded architecture-
// matching ReplayHUD hook off the loader-lock thread.
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
