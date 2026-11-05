#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ReplayHUDOverlayBridge.h"
#include "resource.h"
#include "ReplayHUDBuildConfig.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")

namespace {

constexpr wchar_t kMainClass[] = L"ReplayHUD.MainWindow";
constexpr wchar_t kOverlayClass[] = L"ReplayHUD.OverlayWindow";
constexpr wchar_t kSettingsClass[] = L"ReplayHUD.SettingsWindow";
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kReplaySavedMessage = WM_APP + 11;
constexpr UINT kStatusMessage = WM_APP + 12;
constexpr UINT kReplayBufferStateMessage = WM_APP + 13;
constexpr UINT_PTR kToastTimer = 1;
constexpr UINT_PTR kAnimationTimer = 2;
constexpr UINT kTrayId = 1;
constexpr UINT kEventSubscriptionOutputs = 1u << 6;
constexpr int kToastWidth = 440;
constexpr int kToastHeight = 112;
constexpr int kToastCornerDiameter = 22;
constexpr int kToastBorderThickness = 1;
constexpr int kMenuSettings = 2001;
constexpr int kMenuTest = 2002;
constexpr int kMenuOpenLast = 2003;
constexpr int kMenuLog = 2004;
constexpr int kMenuExit = 2005;

constexpr int IDC_PORT = 3001;
constexpr int IDC_PASSWORD = 3002;
constexpr int IDC_SHOW_PASSWORD = 3003;
constexpr int IDC_DURATION = 3005;
constexpr int IDC_MARGIN = 3006;
constexpr int IDC_POSITION = 3007;
constexpr int IDC_STARTUP = 3008;
constexpr int IDC_SETTINGS_TEST = 3009;
constexpr int IDC_SAVE = 3010;
constexpr int IDC_CANCEL = 3011;
constexpr int IDC_SETTINGS_STATUS = 3012;

constexpr COLORREF kBackground = RGB(20, 23, 29);
constexpr COLORREF kPanel = RGB(29, 33, 41);
constexpr COLORREF kBorder = RGB(54, 61, 72);
constexpr COLORREF kText = RGB(240, 243, 247);
constexpr COLORREF kMuted = RGB(157, 166, 180);
constexpr COLORREF kGreen = RGB(118, 185, 0);

struct Settings {
    INTERNET_PORT port = 4455;
    std::wstring password;
    UINT durationMs = 2400;
    int marginPx = 24;
    int position = 0; // 0 TR, 1 TL, 2 BR, 3 BL
    bool startWithWindows = false;
};

enum class ConnectionStatus : WPARAM {
    Connecting = 0,
    Connected = 1,
    Disconnected = 2,
    AuthenticationFailed = 3,
    Error = 4,
    SessionInvalidated = 5
};

Settings g_settings;
std::mutex g_settingsMutex;
std::mutex g_socketMutex;
std::wstring g_configPath;
std::wstring g_logPath;
std::wstring g_lastReplayPath;
std::wstring g_toastPath;
std::wstring g_toastTitle = L"REPLAY SAVED";
std::wstring g_toastSubtitle = L"Your clip is ready";

HWND g_mainWindow = nullptr;
HWND g_overlayWindow = nullptr;
HWND g_settingsWindow = nullptr;
NOTIFYICONDATAW g_trayData{};
HICON g_appIcon = nullptr;
HICON g_smallIcon = nullptr;
HBRUSH g_settingsBackgroundBrush = nullptr;
bool g_ownsAppIcon = false;
bool g_ownsSmallIcon = false;
std::thread g_worker;
std::atomic_bool g_running{ true };
std::atomic_bool g_reconfigureRequested{ false };
std::atomic<int> g_lastReplayBufferState{ -1 };
HINTERNET g_activeWebSocket = nullptr; // guarded by g_socketMutex; worker owns closing it
HANDLE g_shutdownEvent = nullptr;
HANDLE g_reconnectEvent = nullptr;
UINT g_taskbarCreatedMessage = 0;
ConnectionStatus g_status = ConnectionStatus::Connecting;
HFONT g_uiFont = nullptr;
HFONT g_uiBoldFont = nullptr;
HANDLE g_overlayMapping = nullptr;
HANDLE g_overlayMutex = nullptr;
replayhud::OverlayPayload* g_overlayPayload = nullptr;

std::wstring g_hookDllPath;
std::wstring g_x86HookDllPath;
std::wstring g_x86InjectorPath;
std::wstring g_x86RuntimeDirectory;

void Log(const std::wstring& message);
std::wstring GetDataDirectory();

void PublishOverlayPayload(const std::wstring& title, const std::wstring& subtitle, UINT durationMs, int position, int marginPx) {
    if (!g_overlayPayload || !g_overlayMutex) return;
    const DWORD wait = WaitForSingleObject(g_overlayMutex, 100);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        g_overlayPayload->version = replayhud::kOverlayPayloadVersion;
        g_overlayPayload->active = 1;
        g_overlayPayload->expiresAtTick = GetTickCount64() + durationMs;
        ++g_overlayPayload->sequence;
        if (g_overlayPayload->sequence == 0) ++g_overlayPayload->sequence;
        g_overlayPayload->position = std::clamp(position, 0, 3);
        g_overlayPayload->marginPx = std::clamp(marginPx, 8, 200);
        wcsncpy_s(g_overlayPayload->title, title.c_str(), _TRUNCATE);
        wcsncpy_s(g_overlayPayload->subtitle, subtitle.c_str(), _TRUNCATE);
        ReleaseMutex(g_overlayMutex);
    }
}

void ClearOverlayPayload() {
    if (!g_overlayPayload || !g_overlayMutex) return;
    const DWORD wait = WaitForSingleObject(g_overlayMutex, 100);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        g_overlayPayload->version = replayhud::kOverlayPayloadVersion;
        g_overlayPayload->active = 0;
        g_overlayPayload->expiresAtTick = 0;
        g_overlayPayload->position = 0;
        g_overlayPayload->marginPx = 24;
        g_overlayPayload->title[0] = L'\0';
        g_overlayPayload->subtitle[0] = L'\0';
        ReleaseMutex(g_overlayMutex);
    }
}

bool InitializeOverlayBridge() {
    g_overlayMutex = CreateMutexW(nullptr, FALSE, replayhud::kOverlayMutexName);
    if (!g_overlayMutex) return false;

    g_overlayMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        static_cast<DWORD>(sizeof(replayhud::OverlayPayload)), replayhud::kOverlayMappingName);
    if (!g_overlayMapping) {
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
        return false;
    }

    g_overlayPayload = static_cast<replayhud::OverlayPayload*>(
        MapViewOfFile(g_overlayMapping, FILE_MAP_WRITE, 0, 0, sizeof(replayhud::OverlayPayload)));
    if (!g_overlayPayload) {
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
        return false;
    }

    const DWORD wait = WaitForSingleObject(g_overlayMutex, 100);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        UnmapViewOfFile(g_overlayPayload);
        g_overlayPayload = nullptr;
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
        return false;
    }

    g_overlayPayload->version = replayhud::kOverlayPayloadVersion;
    g_overlayPayload->active = 0;
    g_overlayPayload->expiresAtTick = 0;
    g_overlayPayload->sequence = GetTickCount64();
    g_overlayPayload->position = 0;
    g_overlayPayload->marginPx = 24;
    g_overlayPayload->title[0] = L'\0';
    g_overlayPayload->subtitle[0] = L'\0';
    ReleaseMutex(g_overlayMutex);
    return true;
}

void ShutdownOverlayBridge() {
    ClearOverlayPayload();
    if (g_overlayPayload) {
        UnmapViewOfFile(g_overlayPayload);
        g_overlayPayload = nullptr;
    }
    if (g_overlayMapping) {
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
    }
    if (g_overlayMutex) {
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
    }
}

struct EmbeddedResource {
    const BYTE* data = nullptr;
    DWORD size = 0;
};

bool LoadEmbeddedResource(HINSTANCE module, int resourceId, EmbeddedResource& result) {
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return false;

    HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return false;

    result.data = static_cast<const BYTE*>(LockResource(loaded));
    result.size = SizeofResource(module, resource);
    return result.data != nullptr && result.size > 0;
}

bool EnsureDirectoryExists(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ResourceFileMatches(const std::wstring& path, const EmbeddedResource& resource) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER length{};
    bool matches = GetFileSizeEx(file, &length) != FALSE &&
        length.QuadPart == static_cast<LONGLONG>(resource.size);
    if (matches) {
        std::vector<BYTE> contents(resource.size);
        DWORD read = 0;
        matches = ReadFile(file, contents.data(), resource.size, &read, nullptr) != FALSE &&
            read == resource.size &&
            std::memcmp(contents.data(), resource.data, resource.size) == 0;
    }

    CloseHandle(file);
    return matches;
}

bool WriteEmbeddedResourceFile(const std::wstring& path, const EmbeddedResource& resource) {
    if (ResourceFileMatches(path, resource)) return true;

    const std::wstring temporaryPath = path + L".tmp-" +
        std::to_wstring(GetCurrentProcessId());
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool saved = WriteFile(file, resource.data, resource.size, &written, nullptr) != FALSE &&
        written == resource.size && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);

    if (!saved) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }
    if (!MoveFileExW(temporaryPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        return ResourceFileMatches(path, resource);
    }
    return true;
}

bool InitializeRuntimeFiles(HINSTANCE module) {
    EmbeddedResource hookX64{};
    EmbeddedResource hookX86{};
    EmbeddedResource injectorX86{};
    if (!LoadEmbeddedResource(module, IDR_REPLAYHUD_HOOK_X64, hookX64) ||
        !LoadEmbeddedResource(module, IDR_REPLAYHUD_HOOK_X86, hookX86) ||
        !LoadEmbeddedResource(module, IDR_REPLAYHUD_INJECTOR_X86, injectorX86)) {
        Log(L"One or more embedded game overlay components are missing from this executable.");
        return false;
    }

    const std::wstring runtimeRoot = GetDataDirectory() +
        L"\\Runtime\\" + REPLAYHUD_BUILD_ID;
    const std::wstring x64Directory = runtimeRoot + L"\\x64";
    g_x86RuntimeDirectory = runtimeRoot + L"\\x86";
    g_hookDllPath = x64Directory + L"\\ReplayHUDHook.dll";
    g_x86HookDllPath = g_x86RuntimeDirectory + L"\\ReplayHUDHook.dll";
    g_x86InjectorPath = g_x86RuntimeDirectory + L"\\ReplayHUDInjector32.exe";

    const std::wstring runtimeDirectory = GetDataDirectory() + L"\\Runtime";
    if (!EnsureDirectoryExists(runtimeDirectory) ||
        !EnsureDirectoryExists(runtimeRoot) ||
        !EnsureDirectoryExists(x64Directory) ||
        !EnsureDirectoryExists(g_x86RuntimeDirectory)) {
        Log(L"Could not create the ReplayHUD runtime directory under Local AppData.");
        return false;
    }

    if (!WriteEmbeddedResourceFile(g_hookDllPath, hookX64) ||
        !WriteEmbeddedResourceFile(g_x86HookDllPath, hookX86) ||
        !WriteEmbeddedResourceFile(g_x86InjectorPath, injectorX86)) {
        Log(L"Could not extract the embedded game overlay components.");
        return false;
    }

    Log(L"Embedded x64/x86 overlay components are ready.");
    return true;
}

bool InjectHookIntoProcess(DWORD processId, const std::wstring& hookDllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (!process) return false;

    BOOL targetIsWow64 = FALSE;
    if (!IsWow64Process(process, &targetIsWow64)) {
        CloseHandle(process);
        return false;
    }

    if (targetIsWow64) {
        // A 64-bit process cannot safely use its LoadLibraryW address in a
        // WOW64 target. Delegate the injection to the packaged 32-bit helper.
        CloseHandle(process);

        const std::wstring helperPath = g_x86InjectorPath;
        const std::wstring x86HookPath = g_x86HookDllPath;
        if (helperPath.empty() || x86HookPath.empty() ||
            GetFileAttributesW(helperPath.c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(x86HookPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log(L"32-bit target detected, but the embedded x86 overlay components are unavailable.");
            return false;
        }

        std::wstring command = L"\"" + helperPath + L"\" --pid " +
            std::to_wstring(processId) + L" --dll \"" + x86HookPath + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(helperPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, g_x86RuntimeDirectory.c_str(), &startup, &child)) {
            Log(L"Failed to start the 32-bit ReplayHUD injection helper.");
            return false;
        }

        const DWORD wait = WaitForSingleObject(child.hProcess, 30000);
        DWORD exitCode = 1;
        const bool completed = wait == WAIT_OBJECT_0 &&
            GetExitCodeProcess(child.hProcess, &exitCode) != FALSE;
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        if (!completed) Log(L"32-bit ReplayHUD injection helper timed out or could not report its result.");
        return completed && exitCode == 0;
    }

    const size_t bytes = (hookDllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) {
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, hookDllPath.c_str(), bytes, &written) ||
        written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel ? GetProcAddress(kernel, "LoadLibraryW") : nullptr);
    if (!loadLibrary) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE remoteThread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!remoteThread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    // Keep the remote string alive until LoadLibraryW has definitely returned.
    const DWORD waitResult = WaitForSingleObject(remoteThread, INFINITE);
    DWORD remoteResult = 0;
    const bool completed = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeThread(remoteThread, &remoteResult) != FALSE;
    CloseHandle(remoteThread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return completed && remoteResult != 0;
}

bool InspectOverlayTargetModules(DWORD processId, bool& hookLoaded) {
    hookLoaded = false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    wchar_t imagePath[32768]{};
    DWORD imagePathLength = static_cast<DWORD>(std::size(imagePath));
    const BOOL gotImagePath = QueryFullProcessImageNameW(process, 0, imagePath, &imagePathLength);
    CloseHandle(process);
    if (!gotImagePath || imagePathLength == 0) return false;

    std::wstring imageName(imagePath, imagePathLength);
    const size_t imageSeparator = imageName.find_last_of(L"\\/");
    if (imageSeparator != std::wstring::npos) imageName.erase(0, imageSeparator + 1);
    static constexpr const wchar_t* excludedProcesses[] = {
        L"ReplayHUD.exe", L"obs64.exe", L"obs32.exe", L"steam.exe",
        L"steamwebhelper.exe", L"explorer.exe", L"chrome.exe",
        L"msedge.exe", L"msedgewebview2.exe", L"firefox.exe",
        L"brave.exe", L"opera.exe", L"discord.exe", L"telegram.exe",
        L"vlc.exe", L"mpv.exe", L"spotify.exe", L"taskmgr.exe"
    };
    for (const wchar_t* name : excludedProcesses) {
        if (_wcsicmp(imageName.c_str(), name) == 0) return false;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    static constexpr const wchar_t* graphicsModules[] = {
        L"d3d8.dll", L"d3d9.dll", L"d3d10.dll", L"d3d10_1.dll",
        L"d3d11.dll", L"d3d12.dll", L"vulkan-1.dll", L"opengl32.dll"
    };
    static constexpr const wchar_t* protectedModules[] = {
        L"EasyAntiCheat_EOS.dll", L"EasyAntiCheat_x64.dll", L"EasyAntiCheat.dll",
        L"BEClient_x64.dll", L"BEService.dll", L"BattlEye.dll",
        L"FACEIT.dll", L"FACEITClient.dll"
    };

    bool graphicsRuntimeLoaded = false;
    bool protectedRuntimeLoaded = false;
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (Module32FirstW(snapshot, &module)) {
        do {
            if (_wcsicmp(module.szModule, L"ReplayHUDHook.dll") == 0)
                hookLoaded = true;
            for (const wchar_t* name : graphicsModules) {
                if (_wcsicmp(module.szModule, name) == 0) {
                    graphicsRuntimeLoaded = true;
                    break;
                }
            }
            for (const wchar_t* name : protectedModules) {
                if (_wcsicmp(module.szModule, name) == 0) {
                    protectedRuntimeLoaded = true;
                    break;
                }
            }
        } while (Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
    return graphicsRuntimeLoaded && !protectedRuntimeLoaded;
}

bool EnsureForegroundHookInjected(HANDLE readyEvent) {
    if (!readyEvent) return false;

    const std::wstring hookDllPath = g_hookDllPath;
    if (hookDllPath.empty() || GetFileAttributesW(hookDllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (!processId || processId == GetCurrentProcessId()) return false;

    bool hookAlreadyLoaded = false;
    if (!InspectOverlayTargetModules(processId, hookAlreadyLoaded)) return false;

    if (!hookAlreadyLoaded && !InjectHookIntoProcess(processId, hookDllPath)) return false;

    // Only suppress the desktop toast once the injected renderer has actually
    // drawn this notification. If no backend presents, keep the desktop fallback.
    const DWORD wait = WaitForSingleObject(readyEvent, 1000);
    return wait == WAIT_OBJECT_0;
}

int DpiScale(HWND hwnd, int value) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
    if (!dpi) dpi = 96;
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void Log(const std::wstring& message) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u] ",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::wstring line = prefix + message + L"\r\n";
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
            static_cast<int>(line.size()), utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

std::wstring GetDataDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring base = (count > 0 && count < MAX_PATH) ? localAppData : L".";
    base += L"\\ReplayHUD";
    CreateDirectoryW(base.c_str(), nullptr);
    return base;
}

std::wstring GetLocalConfigPath() {
    return GetDataDirectory() + L"\\ReplayHUD.ini";
}

std::wstring GetLogPath() {
    return GetDataDirectory() + L"\\ReplayHUD.log";
}

std::wstring ReadIniString(const wchar_t* section, const wchar_t* key,
    const wchar_t* fallback) {
    wchar_t value[8192]{};
    GetPrivateProfileStringW(section, key, fallback, value,
        static_cast<DWORD>(std::size(value)), g_configPath.c_str());
    return value;
}

bool EncryptPassword(const std::wstring& plain, std::wstring& encoded) {
    encoded.clear();
    if (plain.empty()) return true;

    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(plain.data()));
    input.cbData = static_cast<DWORD>(plain.size() * sizeof(wchar_t));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"ReplayHUD OBS WebSocket password",
            nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return false;
    }

    DWORD chars = 0;
    BOOL ok = CryptBinaryToStringW(output.pbData, output.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars);
    if (ok) {
        encoded.assign(chars, L'\0');
        ok = CryptBinaryToStringW(output.pbData, output.cbData,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded.data(), &chars);
        while (!encoded.empty() && encoded.back() == L'\0') encoded.pop_back();
    }
    LocalFree(output.pbData);
    return ok == TRUE;
}

bool DecryptPassword(const std::wstring& encoded, std::wstring& plain) {
    plain.clear();
    if (encoded.empty()) return true;

    DWORD bytes = 0;
    if (!CryptStringToBinaryW(encoded.c_str(), 0, CRYPT_STRING_BASE64,
            nullptr, &bytes, nullptr, nullptr)) return false;
    std::vector<BYTE> encrypted(bytes);
    if (!CryptStringToBinaryW(encoded.c_str(), 0, CRYPT_STRING_BASE64,
            encrypted.data(), &bytes, nullptr, nullptr)) return false;

    DATA_BLOB input{ bytes, encrypted.data() };
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &output)) return false;

    if ((output.cbData % sizeof(wchar_t)) == 0 && output.pbData) {
        const wchar_t* chars = reinterpret_cast<const wchar_t*>(output.pbData);
        plain.assign(chars, chars + output.cbData / sizeof(wchar_t));
    }
    if (output.pbData) LocalFree(output.pbData);
    return true;
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

    DWORD type = 0, size = 0;
    LONG result = RegQueryValueExW(key, L"ReplayHUD", nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_SZ;
}

bool SetStartupEnabled(bool enabled) {
    constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    LONG result = ERROR_SUCCESS;

    if (enabled) {
        DWORD disposition = 0;
        result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, &disposition);
    } else {
        result = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
        if (result == ERROR_FILE_NOT_FOUND) return true;
    }
    if (result != ERROR_SUCCESS) return false;

    if (!enabled) {
        result = RegDeleteValueW(key, L"ReplayHUD");
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    } else {
        wchar_t exe[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            RegCloseKey(key);
            return false;
        }
        const std::wstring command = L"\"" + std::wstring(exe) + L"\"";
        result = RegSetValueExW(key, L"ReplayHUD", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void EnsureDefaultConfig() {
    HANDLE file = CreateFileW(g_configPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    static constexpr char defaults[] =
        "; ReplayHUD configuration. The OBS password is DPAPI-protected.\r\n"
        "[OBS]\r\n"
        "port=4455\r\n"
        "password_dpapi=\r\n"
        "\r\n"
        "[HUD]\r\n"
        "duration_ms=2400\r\n"
        "margin_px=24\r\n"
        "position=0\r\n";
    DWORD written = 0;
    WriteFile(file, defaults, static_cast<DWORD>(sizeof(defaults) - 1), &written, nullptr);
    CloseHandle(file);
}

Settings SnapshotSettings() {
    std::lock_guard<std::mutex> lock(g_settingsMutex);
    return g_settings;
}

void LoadSettings() {
    g_configPath = GetLocalConfigPath();
    g_logPath = GetLogPath();
    EnsureDefaultConfig();

    Settings loaded;
    UINT port = GetPrivateProfileIntW(L"OBS", L"port", 4455, g_configPath.c_str());
    loaded.port = static_cast<INTERNET_PORT>(std::clamp<UINT>(port, 1u, 65535u));

    std::wstring protectedPassword = ReadIniString(L"OBS", L"password_dpapi", L"");
    if (!protectedPassword.empty()) {
        if (!DecryptPassword(protectedPassword, loaded.password)) {
            Log(L"Could not decrypt the saved OBS password. Re-enter it in Settings.");
            loaded.password.clear();
        }
    } else {
        // Import the legacy plaintext setting and immediately migrate it to DPAPI.
        loaded.password = ReadIniString(L"OBS", L"password", L"");
        if (!loaded.password.empty()) {
            std::wstring migrated;
            if (EncryptPassword(loaded.password, migrated)) {
                WritePrivateProfileStringW(L"OBS", L"password_dpapi", migrated.c_str(), g_configPath.c_str());
                WritePrivateProfileStringW(L"OBS", L"password", L"", g_configPath.c_str());
                Log(L"Migrated the legacy OBS password to DPAPI-protected storage.");
            }
        }
    }

    loaded.durationMs = std::clamp<UINT>(
        GetPrivateProfileIntW(L"HUD", L"duration_ms", 2400, g_configPath.c_str()),
        800u, 10000u);
    loaded.marginPx = static_cast<int>(std::clamp<UINT>(
        GetPrivateProfileIntW(L"HUD", L"margin_px", 24, g_configPath.c_str()),
        8u, 200u));
    loaded.position = static_cast<int>(std::clamp<UINT>(
        GetPrivateProfileIntW(L"HUD", L"position", 0, g_configPath.c_str()),
        0u, 3u));
    loaded.startWithWindows = IsStartupEnabled();

    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = std::move(loaded);
    }
}

bool SaveSettings(const Settings& settings) {
    std::wstring encrypted;
    if (!EncryptPassword(settings.password, encrypted)) {
        Log(L"DPAPI failed while encrypting the OBS password.");
        return false;
    }

    const bool previousStartupState = IsStartupEnabled();
    const bool startupChanged = settings.startWithWindows != previousStartupState;
    if (startupChanged && !SetStartupEnabled(settings.startWithWindows)) {
        Log(L"Failed to update the Windows startup registry entry.");
        return false;
    }

    auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(section, key, value.c_str(), g_configPath.c_str()) != FALSE;
    };
    const bool written =
        write(L"OBS", L"port", std::to_wstring(settings.port)) &&
        write(L"OBS", L"password_dpapi", encrypted) &&
        write(L"OBS", L"password", L"") &&
        write(L"HUD", L"duration_ms", std::to_wstring(settings.durationMs)) &&
        write(L"HUD", L"margin_px", std::to_wstring(settings.marginPx)) &&
        write(L"HUD", L"position", std::to_wstring(settings.position));

    if (!written) {
        if (startupChanged) SetStartupEnabled(previousStartupState);
        Log(L"Failed to write one or more settings to the INI file.");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_settingsMutex);
        g_settings = settings;
    }
    Log(L"Settings saved.");
    return true;
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), output.data(), count,
        nullptr, nullptr)) return {};
    return output;
}

std::wstring Utf8ToWide(const std::string& input) {
    if (input.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring output(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), output.data(), count)) return L"";
    return output;
}

void AppendUtf8(std::string& output, uint32_t cp) {
    if (cp <= 0x7F) output.push_back(static_cast<char>(cp));
    else if (cp <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Object, Array } type = Type::Null;
    bool boolean = false;
    int64_t number = 0;
    std::string string;
    std::map<std::string, JsonValue> object;
    std::vector<JsonValue> array;

    const JsonValue* Get(const char* key) const {
        if (type != Type::Object) return nullptr;
        auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
    bool AsInt(int& out) const {
        if (type != Type::Number) return false;
        out = static_cast<int>(number);
        return true;
    }
    bool AsString(std::string& out) const {
        if (type != Type::String) return false;
        out = string;
        return true;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}
    bool Parse(JsonValue& value) {
        position_ = 0;
        SkipWhitespace();
        if (!ParseValue(value, 0)) return false;
        SkipWhitespace();
        return position_ == text_.size();
    }
private:
    const std::string& text_;
    size_t position_ = 0;

    void SkipWhitespace() {
        while (position_ < text_.size() &&
            (text_[position_] == ' ' || text_[position_] == '\t' ||
             text_[position_] == '\r' || text_[position_] == '\n')) ++position_;
    }
    bool Consume(char c) {
        if (position_ < text_.size() && text_[position_] == c) {
            ++position_;
            return true;
        }
        return false;
    }
    bool ParseHex4(uint32_t& value) {
        if (position_ + 4 > text_.size()) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= c - '0';
            else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    bool ParseString(std::string& out) {
        if (!Consume('"')) return false;
        out.clear();
        while (position_ < text_.size()) {
            unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= text_.size()) return false;
            char esc = text_[position_++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!ParseHex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    uint32_t low = 0;
                    if (!ParseHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) return false;
                AppendUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool ParseValue(JsonValue& out, int depth) {
        if (depth > 64 || position_ >= text_.size()) return false;
        SkipWhitespace();
        if (position_ >= text_.size()) return false;
        char c = text_[position_];
        if (c == '{') {
            ++position_;
            out = JsonValue{};
            out.type = JsonValue::Type::Object;
            SkipWhitespace();
            if (Consume('}')) return true;
            for (;;) {
                SkipWhitespace();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWhitespace();
                if (!Consume(':')) return false;
                JsonValue value;
                if (!ParseValue(value, depth + 1)) return false;
                out.object[std::move(key)] = std::move(value);
                SkipWhitespace();
                if (Consume('}')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (c == '[') {
            ++position_;
            out = JsonValue{};
            out.type = JsonValue::Type::Array;
            SkipWhitespace();
            if (Consume(']')) return true;
            for (;;) {
                JsonValue value;
                if (!ParseValue(value, depth + 1)) return false;
                out.array.push_back(std::move(value));
                SkipWhitespace();
                if (Consume(']')) return true;
                if (!Consume(',')) return false;
            }
        }
        if (c == '"') {
            out = JsonValue{};
            out.type = JsonValue::Type::String;
            return ParseString(out.string);
        }
        if (text_.compare(position_, 4, "true") == 0) {
            position_ += 4; out = JsonValue{}; out.type = JsonValue::Type::Boolean; out.boolean = true; return true;
        }
        if (text_.compare(position_, 5, "false") == 0) {
            position_ += 5; out = JsonValue{}; out.type = JsonValue::Type::Boolean; return true;
        }
        if (text_.compare(position_, 4, "null") == 0) {
            position_ += 4; out = JsonValue{}; return true;
        }
        size_t start = position_;
        if (c == '-') ++position_;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        if (position_ == start || (position_ == start + 1 && c == '-')) return false;
        // Protocol fields we consume are integers; reject decimal/exponent forms here.
        if (position_ < text_.size() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) return false;
        try { out.number = std::stoll(text_.substr(start, position_ - start)); }
        catch (...) { return false; }
        out.type = JsonValue::Type::Number;
        return true;
    }
};

bool Sha256Base64(const std::string& input, std::string& output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, hashLength = 0, copied = 0;
    std::vector<UCHAR> object, digest;
    bool success = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &copied, 0) < 0) goto cleanup;
    if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
        reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength), &copied, 0) < 0) goto cleanup;
    object.resize(objectLength);
    digest.resize(hashLength);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) < 0) goto cleanup;
    if (!input.empty() && BCryptHashData(hash,
        reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
        static_cast<ULONG>(input.size()), 0) < 0) goto cleanup;
    if (BCryptFinishHash(hash, digest.data(), hashLength, 0) < 0) goto cleanup;

    {
        DWORD chars = 0;
        if (!CryptBinaryToStringA(digest.data(), hashLength,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &chars)) goto cleanup;
        output.assign(chars, '\0');
        if (!CryptBinaryToStringA(digest.data(), hashLength,
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, output.data(), &chars)) goto cleanup;
        while (!output.empty() && output.back() == '\0') output.pop_back();
    }
    success = true;
cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

bool MakeObsAuthentication(const std::wstring& password,
    const std::string& salt, const std::string& challenge, std::string& result) {
    std::string secret;
    return Sha256Base64(WideToUtf8(password) + salt, secret) &&
        Sha256Base64(secret + challenge, result);
}

bool ReceiveWebSocketMessage(HINTERNET socket, std::string& message, USHORT* closeStatus = nullptr) {
    char buffer[8192];
    message.clear();
    if (closeStatus) *closeStatus = 0;
    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        DWORD error = WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &bytesRead, &type);
        if (error != NO_ERROR) return false;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            if (closeStatus) {
                char reason[123]{};
                DWORD reasonLength = 0;
                WinHttpWebSocketQueryCloseStatus(socket, closeStatus,
                    reason, sizeof(reason), &reasonLength);
            }
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return false;
        message.append(buffer, bytesRead);
        if (message.size() > 1024 * 1024) return false;
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) return true;
        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) return false;
    }
}

bool SendWebSocketText(HINTERNET socket, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (g_activeWebSocket != socket) return false;
    return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()), static_cast<DWORD>(message.size())) == NO_ERROR;
}

void SetTrayStatus(ConnectionStatus status) {
    if (g_mainWindow) PostMessageW(g_mainWindow, kStatusMessage,
        static_cast<WPARAM>(status), 0);
}

void ShutdownActiveWebSocket() {
    std::lock_guard<std::mutex> lock(g_socketMutex);
    if (g_activeWebSocket) {
        // Shutdown uses the send side; WinHTTP allows it alongside a receive operation.
        DWORD error = WinHttpWebSocketShutdown(g_activeWebSocket,
            WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        if (error != NO_ERROR && error != ERROR_IO_PENDING) {
            Log(L"WinHttpWebSocketShutdown failed with error " + std::to_wstring(error));
        }
    }
}

bool WaitForReconnectOrShutdown(DWORD timeoutMs) {
    HANDLE events[] = { g_shutdownEvent, g_reconnectEvent };
    DWORD result = WaitForMultipleObjects(2, events, FALSE, timeoutMs);
    if (result == WAIT_OBJECT_0) return false;
    if (result == WAIT_OBJECT_0 + 1) {
        g_reconfigureRequested.store(false);
        return g_running.load();
    }
    return g_running.load();
}

bool ParseProtocolOp(const JsonValue& root, int& op) {
    const JsonValue* value = root.Get("op");
    return value && value->AsInt(op);
}

bool IdentifyObsWebSocket(HINTERNET socket, const Settings& settings, ConnectionStatus& failureStatus) {
    failureStatus = ConnectionStatus::Error;
    std::string helloText;
    USHORT closeStatus = 0;
    if (!ReceiveWebSocketMessage(socket, helloText, &closeStatus)) {
        if (closeStatus == 4009) failureStatus = ConnectionStatus::AuthenticationFailed;
        return false;
    }

    JsonValue hello;
    JsonParser helloParser(helloText);
    int helloOp = -1;
    if (!helloParser.Parse(hello) || !ParseProtocolOp(hello, helloOp) || helloOp != 0) {
        Log(L"OBS WebSocket did not send a valid Hello message.");
        return false;
    }

    const JsonValue* d = hello.Get("d");
    if (!d || d->type != JsonValue::Type::Object) return false;

    std::string identify = "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":" +
        std::to_string(kEventSubscriptionOutputs);

    const JsonValue* auth = d->Get("authentication");
    if (auth && auth->type == JsonValue::Type::Object) {
        std::string challenge, salt;
        const JsonValue* challengeValue = auth->Get("challenge");
        const JsonValue* saltValue = auth->Get("salt");
        if (!challengeValue || !challengeValue->AsString(challenge) ||
            !saltValue || !saltValue->AsString(salt)) return false;
        std::string authentication;
        if (!MakeObsAuthentication(settings.password, salt, challenge, authentication)) return false;
        identify += ",\"authentication\":\"" + authentication + "\"";
    }
    identify += "}}";
    if (!SendWebSocketText(socket, identify)) return false;

    std::string identifiedText;
    closeStatus = 0;
    if (!ReceiveWebSocketMessage(socket, identifiedText, &closeStatus)) {
        if (closeStatus == 4009) failureStatus = ConnectionStatus::AuthenticationFailed;
        return false;
    }
    JsonValue identified;
    JsonParser identifiedParser(identifiedText);
    int op = -1;
    if (!identifiedParser.Parse(identified) || !ParseProtocolOp(identified, op)) return false;
    if (op != 2) {
        const JsonValue* d2 = identified.Get("d");
        const JsonValue* code = d2 ? d2->Get("code") : nullptr;
        if (code && code->type == JsonValue::Type::Number && code->number == 4009)
            failureStatus = ConnectionStatus::AuthenticationFailed;
        else
            failureStatus = ConnectionStatus::Error;
        Log(L"OBS rejected the WebSocket Identify request.");
        return false;
    }
    return true;
}

void PostReplaySaved(const std::wstring& path) {
    if (!g_mainWindow) return;
    auto* payload = new (std::nothrow) std::wstring(path);
    if (!payload) return;
    if (!PostMessageW(g_mainWindow, kReplaySavedMessage,
        reinterpret_cast<WPARAM>(payload), 0)) delete payload;
}

void PostReplayBufferState(bool active) {
    if (!g_mainWindow) return;
    const int nextState = active ? 1 : 0;
    // OBS can emit both ReplayBufferStarted/Stopped and ReplayBufferStateChanged
    // for the same transition. Only publish an actual state change.
    const int previousState = g_lastReplayBufferState.exchange(nextState, std::memory_order_acq_rel);
    if (previousState == nextState) return;
    PostMessageW(g_mainWindow, kReplayBufferStateMessage, nextState, 0);
}

void ObsWorker() {
    while (g_running.load()) {
        Settings settings = SnapshotSettings();
        SetTrayStatus(ConnectionStatus::Connecting);

        HINTERNET session = WinHttpOpen(L"ReplayHUD/1.1",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            Log(L"WinHttpOpen failed.");
            SetTrayStatus(ConnectionStatus::Disconnected);
            if (!WaitForReconnectOrShutdown(4000)) break;
            continue;
        }
        WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);

        HINTERNET connection = WinHttpConnect(session, L"127.0.0.1", settings.port, 0);
        HINTERNET request = nullptr;
        HINTERNET socket = nullptr;
        if (connection) {
            request = WinHttpOpenRequest(connection, L"GET", L"/", nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        }
        if (request && WinHttpSetOption(request,
            WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);
                if (WinHttpQueryHeaders(request,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode,
                    &statusSize, WINHTTP_NO_HEADER_INDEX) && statusCode == 101) {
                    socket = WinHttpWebSocketCompleteUpgrade(request, 0);
                }
            }
        }
        if (request) WinHttpCloseHandle(request);

        if (!socket) {
            if (connection) WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            Log(L"Could not connect to OBS at 127.0.0.1. Check OBS WebSocket settings and port.");
            SetTrayStatus(ConnectionStatus::Disconnected);
            if (!WaitForReconnectOrShutdown(4000)) break;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            g_activeWebSocket = socket;
        }

        // A settings change can arrive while the connect handshake is in progress.
        if (g_reconfigureRequested.load()) {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            WinHttpWebSocketShutdown(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
        ConnectionStatus identifyFailure = ConnectionStatus::Error;
        bool sessionInvalidated = false;
        bool identified = IdentifyObsWebSocket(socket, settings, identifyFailure);
        if (identified && g_running.load()) {
            SetTrayStatus(ConnectionStatus::Connected);
            Log(L"Connected to OBS WebSocket.");
            while (g_running.load()) {
                std::string message;
                USHORT closeStatus = 0;
                if (!ReceiveWebSocketMessage(socket, message, &closeStatus)) {
                    if (closeStatus == 4011) {
                        sessionInvalidated = true;
                        Log(L"OBS invalidated this WebSocket session; automatic reconnect is disabled.");
                        SetTrayStatus(ConnectionStatus::SessionInvalidated);
                    }
                    break;
                }

                JsonValue root;
                JsonParser parser(message);
                int op = -1;
                if (!parser.Parse(root) || !ParseProtocolOp(root, op) || op != 5) continue;
                const JsonValue* data = root.Get("d");
                const JsonValue* eventType = data ? data->Get("eventType") : nullptr;
                if (!eventType || eventType->type != JsonValue::Type::String) continue;

                if (eventType->string == "ReplayBufferSaved") {
                    const JsonValue* eventData = data->Get("eventData");
                    const JsonValue* savedPath = eventData ? eventData->Get("savedReplayPath") : nullptr;
                    std::string utf8Path;
                    if (savedPath && savedPath->AsString(utf8Path)) {
                        std::wstring path = Utf8ToWide(utf8Path);
                        if (!path.empty()) PostReplaySaved(path);
                        Log(L"OBS reported a saved replay: " + path);
                    }
                    continue;
                }

                if (eventType->string == "ReplayBufferStarted") {
                    PostReplayBufferState(true);
                    Log(L"OBS replay buffer started.");
                    continue;
                }

                if (eventType->string == "ReplayBufferStopped") {
                    PostReplayBufferState(false);
                    Log(L"OBS replay buffer stopped.");
                    continue;
                }

                if (eventType->string == "ReplayBufferStateChanged") {
                    const JsonValue* eventData = data->Get("eventData");
                    const JsonValue* outputActive = eventData ? eventData->Get("outputActive") : nullptr;
                    if (outputActive && outputActive->type == JsonValue::Type::Boolean) {
                        PostReplayBufferState(outputActive->boolean);
                        Log(outputActive->boolean ? L"OBS replay buffer started."
                                                  : L"OBS replay buffer stopped.");
                    }
                }
            }
        } else if (g_running.load()) {
            SetTrayStatus(identifyFailure);
        }

        {
            std::lock_guard<std::mutex> lock(g_socketMutex);
            if (g_activeWebSocket == socket) g_activeWebSocket = nullptr;
            // Receive has returned before this point, so no receive operation is pending.
            // Complete the WebSocket close handshake before releasing its WinHTTP handle.
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket); // worker is the only thread that closes this handle
        }
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);

        if (g_running.load() && sessionInvalidated) break;

        if (g_running.load()) {
            const bool wasReconfigured = g_reconfigureRequested.exchange(false);
            if (!wasReconfigured) {
                if (identified) {
                    SetTrayStatus(ConnectionStatus::Disconnected);
                    if (!WaitForReconnectOrShutdown(4000)) break;
                } else if (identifyFailure == ConnectionStatus::AuthenticationFailed) {
                    // Keep the failure visible and wait until the user changes settings.
                    if (!WaitForReconnectOrShutdown(INFINITE)) break;
                } else {
                    if (!WaitForReconnectOrShutdown(4000)) break;
                }
            } else {
                // Clear the auto-reset signal if the active socket was already torn down.
                WaitForSingleObject(g_reconnectEvent, 0);
            }
        }
    }
    Log(L"OBS worker stopped.");
}

void UpdateTrayTooltip(const wchar_t* text) {
    if (!g_trayData.hWnd) return;
    wcsncpy_s(g_trayData.szTip, text, _TRUNCATE);
    g_trayData.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_trayData);
}

bool AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g_trayData, sizeof(g_trayData));
    g_trayData.cbSize = sizeof(g_trayData);
    g_trayData.hWnd = hwnd;
    g_trayData.uID = kTrayId;
    g_trayData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayData.uCallbackMessage = kTrayMessage;
    g_trayData.hIcon = g_smallIcon ? g_smallIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_trayData.szTip, L"ReplayHUD | Connecting to OBS", _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &g_trayData)) return false;
    g_trayData.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_trayData);
    return true;
}

void ShowSettingsWindow();
void SetConnectionTooltip(ConnectionStatus status);
void ShowNotification(const std::wstring& path, bool test,
    const std::wstring& customTitle = L"", const std::wstring& customSubtitle = L"");

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"Settings...");
    AppendMenuW(menu, MF_STRING, kMenuTest, L"Test notification");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, g_lastReplayPath.empty() ? MF_STRING | MF_GRAYED : MF_STRING,
        kMenuOpenLast, L"Open last replay");
    AppendMenuW(menu, MF_STRING, kMenuLog, L"Open log file");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit ReplayHUD");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    UINT command = TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x, point.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (command) {
    case kMenuSettings: ShowSettingsWindow(); break;
    case kMenuTest: ShowNotification(L"", true); break;
    case kMenuOpenLast:
        if (!g_lastReplayPath.empty()) ShellExecuteW(hwnd, L"open",
            g_lastReplayPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kMenuLog:
        ShellExecuteW(hwnd, L"open", L"notepad.exe",
            (L"\"" + g_logPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        break;
    case kMenuExit: DestroyWindow(hwnd); break;
    }
}

std::wstring FileNameFromPath(const std::wstring& path) {
    if (path.empty()) return L"Replay clip";
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

void OpenLastReplay() {
    if (!g_toastPath.empty()) {
        ShellExecuteW(g_mainWindow, L"open", g_toastPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void SetToastText(const std::wstring& title, const std::wstring& subtitle,
    const std::wstring& path) {
    g_toastTitle = title;
    g_toastSubtitle = subtitle;
    g_toastPath = path;
}

void ShowNotification(const std::wstring& path, bool test,
    const std::wstring& customTitle, const std::wstring& customSubtitle) {
    if (!g_overlayWindow) return;
    Settings settings = SnapshotSettings();

    if (!customTitle.empty() || !customSubtitle.empty()) {
        SetToastText(customTitle, customSubtitle, L"");
    } else if (test) {
        SetToastText(L"REPLAYHUD TEST", L"This is how saved clips will appear", L"");
    } else {
        std::wstring fileName = FileNameFromPath(path);
        SetToastText(L"REPLAY SAVED", fileName, path);
        g_lastReplayPath = path;
    }


    HANDLE readyEvent = CreateEventW(nullptr, TRUE, FALSE, replayhud::kOverlayReadyEventName);
    if (readyEvent) ResetEvent(readyEvent);
    PublishOverlayPayload(g_toastTitle, g_toastSubtitle, settings.durationMs, settings.position, settings.marginPx);
    const bool gameOverlayReady = EnsureForegroundHookInjected(readyEvent);
    if (readyEvent) CloseHandle(readyEvent);
    if (gameOverlayReady) {
        KillTimer(g_overlayWindow, kToastTimer);
        KillTimer(g_overlayWindow, kAnimationTimer);
        ShowWindow(g_overlayWindow, SW_HIDE);
        return;
    }
    // No compatible renderer became ready. Clear the shared payload so a late
    // hook startup cannot duplicate the desktop fallback toast.
    ClearOverlayPayload();

    POINT cursor{};
    GetCursorPos(&cursor);
    HWND foreground = GetForegroundWindow();
    RECT foregroundRect{};
    HMONITOR monitor = nullptr;
    if (foreground && GetWindowRect(foreground, &foregroundRect))
        monitor = MonitorFromRect(&foregroundRect, MONITOR_DEFAULTTONEAREST);
    if (!monitor) monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT targetDpi = 96, targetDpiY = 96;
    if (monitor) GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &targetDpi, &targetDpiY);
    if (!targetDpi) targetDpi = 96;
    auto scaleForTarget = [targetDpi](int value) { return MulDiv(value, static_cast<int>(targetDpi), 96); };

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT bounds{};
    if (monitor && GetMonitorInfoW(monitor, &info)) bounds = info.rcWork;
    else {
        bounds.left = 0; bounds.top = 0;
        bounds.right = GetSystemMetrics(SM_CXSCREEN);
        bounds.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int width = scaleForTarget(kToastWidth);
    const int height = scaleForTarget(kToastHeight);
    const int margin = scaleForTarget(settings.marginPx);
    int x = bounds.right - width - margin;
    int y = bounds.top + margin;
    switch (settings.position) {
    case 1: x = bounds.left + margin; y = bounds.top + margin; break;
    case 2: x = bounds.right - width - margin; y = bounds.bottom - height - margin; break;
    case 3: x = bounds.left + margin; y = bounds.bottom - height - margin; break;
    default: break;
    }

    KillTimer(g_overlayWindow, kToastTimer);
    KillTimer(g_overlayWindow, kAnimationTimer);
    const int cornerDiameter = scaleForTarget(kToastCornerDiameter);
    HRGN toastRegion = CreateRoundRectRgn(0, 0, width, height,
        cornerDiameter, cornerDiameter);
    if (toastRegion && !SetWindowRgn(g_overlayWindow, toastRegion, TRUE))
        DeleteObject(toastRegion);
    SetWindowPos(g_overlayWindow, HWND_TOPMOST, x + scaleForTarget(18), y,
        width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetLayeredWindowAttributes(g_overlayWindow, 0, 0, LWA_ALPHA);
    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    InvalidateRect(g_overlayWindow, nullptr, TRUE);
    SetTimer(g_overlayWindow, kAnimationTimer, 15, nullptr);
    SetTimer(g_overlayWindow, kToastTimer, settings.durationMs, nullptr);
}

void DrawTextLine(HDC dc, const std::wstring& text, HFONT font,
    COLORREF color, RECT rect, UINT flags) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags);
    SelectObject(dc, oldFont);
}

LRESULT CALLBACK OverlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if (!g_toastPath.empty()) OpenLastReplay();
        ClearOverlayPayload();
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (wParam == kAnimationTimer) {
            BYTE alpha = 246;
            SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
            RECT r{};
            GetWindowRect(hwnd, &r);
            const int targetX = r.left - DpiScale(hwnd, 18);
            SetWindowPos(hwnd, HWND_TOPMOST, targetX, r.top,
                r.right - r.left, r.bottom - r.top,
                SWP_NOACTIVATE | SWP_NOSIZE);
            KillTimer(hwnd, kAnimationTimer);
        } else if (wParam == kToastTimer) {
            KillTimer(hwnd, kToastTimer);
            ClearOverlayPayload();
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC target = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right;
        const int height = client.bottom;
        HDC buffer = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
        if (!buffer || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (buffer) DeleteDC(buffer);
            FillRect(target, &client, GetSysColorBrush(COLOR_WINDOW));
            EndPaint(hwnd, &ps);
            return 0;
        }
        HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
        if (!oldBitmap || oldBitmap == HGDI_ERROR) {
            DeleteObject(bitmap);
            DeleteDC(buffer);
            FillRect(target, &client, GetSysColorBrush(COLOR_WINDOW));
            EndPaint(hwnd, &ps);
            return 0;
        }

        HBRUSH borderBrush = CreateSolidBrush(kBorder);
        HBRUSH bg = CreateSolidBrush(kBackground);
        HBRUSH panel = CreateSolidBrush(kPanel);
        HGDIOBJ nullPen = GetStockObject(NULL_PEN);
        HGDIOBJ oldPen = SelectObject(buffer, nullPen);
        HGDIOBJ oldBrush = SelectObject(buffer, borderBrush);

        // Paint the border as a solid outer rounded shape, then inset the
        // background. A stroked GDI RoundRect can leave uneven corner pixels
        // when its pen is clipped by the layered window's rounded region.
        const int cornerDiameter = DpiScale(hwnd, kToastCornerDiameter);
        const int borderWidth = (std::max)(1, DpiScale(hwnd, kToastBorderThickness));
        RoundRect(buffer, 0, 0, width, height, cornerDiameter, cornerDiameter);

        RECT innerRect{ borderWidth, borderWidth, width - borderWidth, height - borderWidth };
        SelectObject(buffer, bg);
        const int innerCornerDiameter = (std::max)(0, cornerDiameter - 2 * borderWidth);
        RoundRect(buffer, innerRect.left, innerRect.top, innerRect.right, innerRect.bottom,
            innerCornerDiameter, innerCornerDiameter);

        SelectObject(buffer, oldBrush);
        SelectObject(buffer, oldPen);
        DeleteObject(borderBrush);
        DeleteObject(bg);

        // Accent strip and icon tile.
        HBRUSH accent = CreateSolidBrush(kGreen);
        RECT accentRect{ 0, DpiScale(hwnd, 16), DpiScale(hwnd, 4), height - DpiScale(hwnd, 16) };
        FillRect(buffer, &accentRect, accent);
        DeleteObject(accent);

        RECT tile{ DpiScale(hwnd, 22), DpiScale(hwnd, 25), DpiScale(hwnd, 74), DpiScale(hwnd, 77) };
        HGDIOBJ oldP = SelectObject(buffer, GetStockObject(NULL_PEN));
        HGDIOBJ oldPanelBrush = SelectObject(buffer, panel);
        RoundRect(buffer, tile.left, tile.top, tile.right, tile.bottom,
            DpiScale(hwnd, 13), DpiScale(hwnd, 13));
        SelectObject(buffer, oldPanelBrush);
        SelectObject(buffer, oldP);
        DeleteObject(panel);

        if (g_appIcon) {
            DrawIconEx(buffer, DpiScale(hwnd, 31), DpiScale(hwnd, 34), g_appIcon,
                DpiScale(hwnd, 34), DpiScale(hwnd, 34), 0, nullptr, DI_NORMAL);
        }

        HFONT titleFont = CreateFontW(DpiScale(hwnd, 14), 0, 0, 0, FW_BOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT fileFont = CreateFontW(DpiScale(hwnd, 17), 0, 0, 0, FW_SEMIBOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT hintFont = CreateFontW(DpiScale(hwnd, 11), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        SetBkMode(buffer, TRANSPARENT);
        RECT titleRect{ DpiScale(hwnd, 92), DpiScale(hwnd, 17), width - DpiScale(hwnd, 24), DpiScale(hwnd, 39) };
        DrawTextLine(buffer, g_toastTitle, titleFont, kGreen, titleRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT fileRect{ DpiScale(hwnd, 92), DpiScale(hwnd, 39), width - DpiScale(hwnd, 22), DpiScale(hwnd, 68) };
        DrawTextLine(buffer, g_toastSubtitle, fileFont, kText, fileRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT hintRect{ DpiScale(hwnd, 92), DpiScale(hwnd, 72), width - DpiScale(hwnd, 20), DpiScale(hwnd, 94) };
        DrawTextLine(buffer, g_toastPath.empty() ? L"ReplayHUD  •  OBS Replay Buffer" : L"Click to open clip  •  ReplayHUD",
            hintFont, kMuted, hintRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        DeleteObject(titleFont);
        DeleteObject(fileFont);
        DeleteObject(hintFont);
        BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(buffer);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateUiControl(HWND parent, const wchar_t* klass, const wchar_t* text,
    DWORD style, int id, int x, int y, int width, int height, DWORD exStyle = 0) {
    HWND control = CreateWindowExW(exStyle, klass, text,
        WS_CHILD | WS_VISIBLE | style,
        DpiScale(parent, x), DpiScale(parent, y),
        DpiScale(parent, width), DpiScale(parent, height),
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)), nullptr);
    if (control && g_uiFont) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return control;
}

void SetControlText(HWND hwnd, int id, const std::wstring& text) {
    SetDlgItemTextW(hwnd, id, text.c_str());
}

std::wstring GetControlText(HWND hwnd, int id) {
    HWND control = GetDlgItem(hwnd, id);
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(std::max(0, length)));
    return text;
}

bool ParseUnsigned(const std::wstring& text, unsigned long& value) {
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    SetLastError(ERROR_SUCCESS);
    unsigned long parsed = wcstoul(text.c_str(), &end, 10);
    if (GetLastError() != ERROR_SUCCESS || end == text.c_str() || *end != L'\0') return false;
    value = parsed;
    return true;
}

void PopulateSettingsControls(HWND hwnd) {
    Settings s = SnapshotSettings();
    SetControlText(hwnd, IDC_PORT, std::to_wstring(s.port));
    SetControlText(hwnd, IDC_PASSWORD, s.password);
    SetControlText(hwnd, IDC_DURATION, std::to_wstring(s.durationMs));
    SetControlText(hwnd, IDC_MARGIN, std::to_wstring(s.marginPx));
    SendDlgItemMessageW(hwnd, IDC_POSITION, CB_SETCURSEL, s.position, 0);
    SendDlgItemMessageW(hwnd, IDC_STARTUP, BM_SETCHECK,
        s.startWithWindows ? BST_CHECKED : BST_UNCHECKED, 0);
}

void CreateSettingsControls(HWND hwnd) {
    const int left = 28;
    HWND heading = CreateUiControl(hwnd, L"STATIC", L"ReplayHUD settings", SS_LEFT,
        0, left, 20, 430, 30);
    if (heading && g_uiBoldFont) SendMessageW(heading, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiBoldFont), TRUE);
    CreateUiControl(hwnd, L"STATIC", L"Configure the OBS connection and notification behaviour.", SS_LEFT,
        0, left, 45, 430, 20);
    CreateUiControl(hwnd, L"STATIC", L"OBS CONNECTION", SS_LEFT,
        0, left, 66, 430, 20);
    CreateUiControl(hwnd, L"STATIC", L"WebSocket port", SS_LEFT,
        0, left, 96, 130, 24);
    CreateUiControl(hwnd, L"EDIT", L"4455", ES_AUTOHSCROLL | ES_NUMBER,
        IDC_PORT, 180, 92, 270, 28, WS_EX_CLIENTEDGE);
    CreateUiControl(hwnd, L"STATIC", L"WebSocket password", SS_LEFT,
        0, left, 133, 140, 24);
    CreateUiControl(hwnd, L"EDIT", L"", ES_AUTOHSCROLL | ES_PASSWORD,
        IDC_PASSWORD, 180, 129, 270, 28, WS_EX_CLIENTEDGE);
    CreateUiControl(hwnd, L"BUTTON", L"Show password", BS_AUTOCHECKBOX | BS_LEFTTEXT,
        IDC_SHOW_PASSWORD, 180, 160, 160, 24);
    CreateUiControl(hwnd, L"STATIC", L"Password is encrypted for your Windows user account.", SS_LEFT,
        0, 180, 183, 290, 30);

    CreateUiControl(hwnd, L"STATIC", L"NOTIFICATION", SS_LEFT,
        0, left, 226, 430, 20);
    CreateUiControl(hwnd, L"STATIC", L"Duration (ms)", SS_LEFT,
        0, left, 291, 130, 24);
    CreateUiControl(hwnd, L"EDIT", L"2400", ES_AUTOHSCROLL | ES_NUMBER,
        IDC_DURATION, 180, 287, 100, 28, WS_EX_CLIENTEDGE);
    CreateUiControl(hwnd, L"STATIC", L"Position", SS_LEFT,
        0, left, 328, 130, 24);
    HWND position = CreateUiControl(hwnd, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | WS_VSCROLL, IDC_POSITION, 180, 324, 270, 180);
    SendMessageW(position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top right"));
    SendMessageW(position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Top left"));
    SendMessageW(position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom right"));
    SendMessageW(position, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bottom left"));
    CreateUiControl(hwnd, L"STATIC", L"Screen edge margin (px)", SS_LEFT,
        0, left, 365, 160, 24);
    CreateUiControl(hwnd, L"EDIT", L"24", ES_AUTOHSCROLL | ES_NUMBER,
        IDC_MARGIN, 180, 361, 100, 28, WS_EX_CLIENTEDGE);

    CreateUiControl(hwnd, L"BUTTON", L"Start ReplayHUD when Windows starts", BS_AUTOCHECKBOX,
        IDC_STARTUP, left, 410, 300, 25);
    CreateUiControl(hwnd, L"STATIC", L"Port/password changes trigger an OBS reconnect.", SS_LEFT,
        0, left, 443, 430, 22);

    CreateUiControl(hwnd, L"BUTTON", L"Test notification", BS_PUSHBUTTON,
        IDC_SETTINGS_TEST, left, 492, 140, 32);
    CreateUiControl(hwnd, L"BUTTON", L"Save settings", BS_DEFPUSHBUTTON,
        IDC_SAVE, 285, 492, 95, 32);
    CreateUiControl(hwnd, L"BUTTON", L"Cancel", BS_PUSHBUTTON,
        IDC_CANCEL, 385, 492, 65, 32);
    CreateUiControl(hwnd, L"STATIC", L"", SS_LEFT,
        IDC_SETTINGS_STATUS, left, 535, 430, 24);

    PopulateSettingsControls(hwnd);
}

void RequestReconnect() {
    g_reconfigureRequested.store(true);
    if (g_reconnectEvent) SetEvent(g_reconnectEvent);
    ShutdownActiveWebSocket();
}

bool SaveSettingsFromWindow(HWND hwnd) {
    unsigned long port = 0, duration = 0, margin = 0;
    if (!ParseUnsigned(GetControlText(hwnd, IDC_PORT), port) || port < 1 || port > 65535) {
        MessageBoxW(hwnd, L"Enter a valid OBS WebSocket port from 1 to 65535.",
            L"Invalid port", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd, IDC_PORT));
        return false;
    }
    if (!ParseUnsigned(GetControlText(hwnd, IDC_DURATION), duration) || duration < 800 || duration > 10000) {
        MessageBoxW(hwnd, L"Notification duration must be between 800 and 10000 ms.",
            L"Invalid duration", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd, IDC_DURATION));
        return false;
    }
    if (!ParseUnsigned(GetControlText(hwnd, IDC_MARGIN), margin) || margin < 8 || margin > 200) {
        MessageBoxW(hwnd, L"Screen margin must be between 8 and 200 pixels.",
            L"Invalid margin", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd, IDC_MARGIN));
        return false;
    }

    Settings s;
    s.port = static_cast<INTERNET_PORT>(port);
    s.password = GetControlText(hwnd, IDC_PASSWORD);
    s.durationMs = static_cast<UINT>(duration);
    s.marginPx = static_cast<int>(margin);
    LRESULT position = SendDlgItemMessageW(hwnd, IDC_POSITION, CB_GETCURSEL, 0, 0);
    s.position = static_cast<int>(std::clamp<LRESULT>(position, static_cast<LRESULT>(0), static_cast<LRESULT>(3)));
    s.startWithWindows = SendDlgItemMessageW(hwnd, IDC_STARTUP, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (!SaveSettings(s)) {
        MessageBoxW(hwnd, L"ReplayHUD could not save your settings. Check that your user profile is writable.",
            L"Save failed", MB_OK | MB_ICONERROR);
        return false;
    }

    SetDlgItemTextW(hwnd, IDC_SETTINGS_STATUS, L"Saved. Reconnecting to OBS...");
    RequestReconnect();
    return true;
}

LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateSettingsControls(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SHOW_PASSWORD: {
            bool show = SendDlgItemMessageW(hwnd, IDC_SHOW_PASSWORD, BM_GETCHECK, 0, 0) == BST_CHECKED;
            HWND edit = GetDlgItem(hwnd, IDC_PASSWORD);
            LONG_PTR style = GetWindowLongPtrW(edit, GWL_STYLE);
            if (show) style &= ~ES_PASSWORD; else style |= ES_PASSWORD;
            SetWindowLongPtrW(edit, GWL_STYLE, style);
            SendMessageW(edit, EM_SETPASSWORDCHAR, show ? 0 : L'*', 0);
            SetWindowPos(edit, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            InvalidateRect(edit, nullptr, TRUE);
            return 0;
        }
        case IDC_SETTINGS_TEST:
            ShowNotification(L"", true);
            return 0;
        case IDC_SAVE:
            SaveSettingsFromWindow(hwnd);
            return 0;
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_settingsWindow = nullptr;
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkColor(dc, kBackground);
        SetTextColor(dc, kText);
        return reinterpret_cast<LRESULT>(g_settingsBackgroundBrush);
    }
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ShowSettingsWindow() {
    if (g_settingsWindow) {
        ShowWindow(g_settingsWindow, SW_RESTORE);
        SetForegroundWindow(g_settingsWindow);
        return;
    }
    g_settingsWindow = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSettingsClass, L"ReplayHUD Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, DpiScale(g_mainWindow, 500), DpiScale(g_mainWindow, 610),
        nullptr, nullptr,
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(g_mainWindow, GWLP_HINSTANCE)), nullptr);
    if (!g_settingsWindow) {
        MessageBoxW(g_mainWindow, L"Could not create the Settings window.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        return;
    }
    RECT window{};
    GetWindowRect(g_settingsWindow, &window);
    int width = window.right - window.left;
    int height = window.bottom - window.top;
    SetWindowPos(g_settingsWindow, HWND_TOP,
        GetSystemMetrics(SM_CXSCREEN) / 2 - width / 2,
        GetSystemMetrics(SM_CYSCREEN) / 2 - height / 2,
        0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    SetConnectionTooltip(g_status);
    UpdateWindow(g_settingsWindow);
}

void SetConnectionTooltip(ConnectionStatus status) {
    g_status = status;
    switch (status) {
    case ConnectionStatus::Connected:
        UpdateTrayTooltip(L"ReplayHUD | Connected to OBS"); break;
    case ConnectionStatus::Connecting:
        UpdateTrayTooltip(L"ReplayHUD | Connecting to OBS..."); break;
    case ConnectionStatus::AuthenticationFailed:
        UpdateTrayTooltip(L"ReplayHUD | OBS authentication failed"); break;
    case ConnectionStatus::Error:
        UpdateTrayTooltip(L"ReplayHUD | OBS WebSocket error"); break;
    case ConnectionStatus::SessionInvalidated:
        UpdateTrayTooltip(L"ReplayHUD | Session invalidated; restart the app"); break;
    default:
        UpdateTrayTooltip(L"ReplayHUD | Waiting for OBS"); break;
    }
    if (g_settingsWindow) {
        const wchar_t* text = L"Waiting for OBS WebSocket...";
        if (status == ConnectionStatus::Connected) text = L"Connected to OBS. Replay Buffer events are active.";
        else if (status == ConnectionStatus::AuthenticationFailed) text = L"Authentication failed. Check your OBS password.";
        else if (status == ConnectionStatus::Error) text = L"OBS rejected the WebSocket connection. Check the log.";
        else if (status == ConnectionStatus::SessionInvalidated) text = L"OBS invalidated this session. Restart ReplayHUD to reconnect.";
        SetDlgItemTextW(g_settingsWindow, IDC_SETTINGS_STATUS, text);
    }
}

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage != 0) {
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (message) {
    case kTrayMessage: {
        const UINT eventCode = LOWORD(lParam);
        if (eventCode == WM_RBUTTONUP || eventCode == WM_CONTEXTMENU) ShowTrayMenu(hwnd);
        else if (eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT || eventCode == WM_LBUTTONDBLCLK)
            ShowSettingsWindow();
        return 0;
    }
    case kReplaySavedMessage: {
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(wParam));
        if (path) ShowNotification(*path, false);
        return 0;
    }
    case kReplayBufferStateMessage:
        if (wParam != 0)
            ShowNotification(L"", false, L"REPLAY BUFFER STARTED", L"OBS replay buffer is running");
        else
            ShowNotification(L"", false, L"REPLAY BUFFER STOPPED", L"OBS replay buffer is off");
        return 0;
    case kStatusMessage:
        SetConnectionTooltip(static_cast<ConnectionStatus>(wParam));
        return 0;
    case WM_COMMAND:
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_running.store(false);
        ShutdownOverlayBridge();
        if (g_shutdownEvent) SetEvent(g_shutdownEvent);
        ShutdownActiveWebSocket();
        Shell_NotifyIconW(NIM_DELETE, &g_trayData);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterWindowClasses(HINSTANCE instance) {
    WNDCLASSEXW overlay{};
    overlay.cbSize = sizeof(overlay);
    overlay.lpfnWndProc = OverlayWindowProc;
    overlay.hInstance = instance;
    overlay.lpszClassName = kOverlayClass;
    overlay.hCursor = LoadCursorW(nullptr, IDC_HAND);
    overlay.hIcon = g_appIcon;
    if (!RegisterClassExW(&overlay)) return false;

    WNDCLASSEXW settings{};
    settings.cbSize = sizeof(settings);
    settings.lpfnWndProc = SettingsWindowProc;
    settings.hInstance = instance;
    settings.lpszClassName = kSettingsClass;
    settings.hIcon = g_appIcon;
    settings.hIconSm = g_smallIcon;
    settings.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settings.hbrBackground = g_settingsBackgroundBrush ? g_settingsBackgroundBrush :
        static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&settings)) return false;

    WNDCLASSEXW main{};
    main.cbSize = sizeof(main);
    main.lpfnWndProc = MainWindowProc;
    main.hInstance = instance;
    main.lpszClassName = kMainClass;
    main.hIcon = g_appIcon;
    main.hIconSm = g_smallIcon;
    main.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    return RegisterClassExW(&main) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&controls);

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\ReplayHUD.SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    g_appIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_REPLAYHUD), IMAGE_ICON, 48, 48, LR_DEFAULTCOLOR));
    g_smallIcon = static_cast<HICON>(LoadImageW(instance,
        MAKEINTRESOURCEW(IDI_REPLAYHUD), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    g_ownsAppIcon = (g_appIcon != nullptr);
    g_ownsSmallIcon = (g_smallIcon != nullptr);
    if (!g_appIcon) g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!g_smallIcon) g_smallIcon = g_appIcon;

    g_uiFont = CreateFontW(-MulDiv(9, 96, 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_uiBoldFont = CreateFontW(-MulDiv(11, 96, 72), 0, 0, 0, FW_SEMIBOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    g_settingsBackgroundBrush = CreateSolidBrush(kBackground);

    LoadSettings();
    if (!InitializeRuntimeFiles(instance)) {
        Log(L"In-game overlay is unavailable; desktop notifications will continue to work.");
    }
    if (!RegisterWindowClasses(instance)) {
        MessageBoxW(nullptr, L"Could not register ReplayHUD window classes.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_reconnectEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_shutdownEvent || !g_reconnectEvent) {
        MessageBoxW(nullptr, L"Could not initialize ReplayHUD events.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        if (g_shutdownEvent) CloseHandle(g_shutdownEvent);
        if (g_reconnectEvent) CloseHandle(g_reconnectEvent);
        CloseHandle(mutex);
        return 1;
    }

    g_mainWindow = CreateWindowExW(WS_EX_TOOLWINDOW,
        kMainClass, L"ReplayHUD", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    if (!g_mainWindow) {
        MessageBoxW(nullptr, L"Could not create the ReplayHUD main window.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        CloseHandle(g_shutdownEvent);
        CloseHandle(g_reconnectEvent);
        CloseHandle(mutex);
        return 1;
    }

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayClass, L"ReplayHUD notification", WS_POPUP,
        0, 0, kToastWidth, kToastHeight,
        nullptr, nullptr, instance, nullptr);
    if (!g_overlayWindow) {
        MessageBoxW(g_mainWindow, L"Could not create the notification window.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        DestroyWindow(g_mainWindow);
        CloseHandle(g_shutdownEvent);
        CloseHandle(g_reconnectEvent);
        CloseHandle(mutex);
        return 1;
    }

    HRGN region = CreateRoundRectRgn(0, 0, kToastWidth, kToastHeight,
        kToastCornerDiameter, kToastCornerDiameter);
    if (region && !SetWindowRgn(g_overlayWindow, region, TRUE))
        DeleteObject(region);

    if (!InitializeOverlayBridge()) {
        Log(L"Overlay hook bridge unavailable; continuing with desktop notification window only.");
    }

    if (!AddTrayIcon(g_mainWindow)) {
        MessageBoxW(g_mainWindow, L"Windows could not create the ReplayHUD tray icon.",
            L"ReplayHUD", MB_OK | MB_ICONERROR);
        DestroyWindow(g_mainWindow);
        CloseHandle(g_shutdownEvent);
        CloseHandle(g_reconnectEvent);
        CloseHandle(mutex);
        return 1;
    }
    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    ShowWindow(g_mainWindow, SW_HIDE);
    Log(L"ReplayHUD started.");

    g_worker = std::thread(ObsWorker);

    MSG message{};
    BOOL result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_running.store(false);
    if (g_shutdownEvent) SetEvent(g_shutdownEvent);
    // WM_DESTROY already requested the WebSocket shutdown. The worker owns
    // the socket handle and closes it after its receive operation returns.
    if (g_worker.joinable()) g_worker.join();

    if (g_settingsWindow) DestroyWindow(g_settingsWindow);
    if (g_overlayWindow) DestroyWindow(g_overlayWindow);
    if (g_ownsSmallIcon && g_smallIcon && g_smallIcon != g_appIcon) DestroyIcon(g_smallIcon);
    if (g_ownsAppIcon && g_appIcon) DestroyIcon(g_appIcon);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_uiBoldFont) DeleteObject(g_uiBoldFont);
    if (g_settingsBackgroundBrush) DeleteObject(g_settingsBackgroundBrush);
    if (g_shutdownEvent) CloseHandle(g_shutdownEvent);
    if (g_reconnectEvent) CloseHandle(g_reconnectEvent);
    CloseHandle(mutex);
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
