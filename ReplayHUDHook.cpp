#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>

#include <imgui.h>
#include <uih.h>

#include "ReplayHUDOverlayBridge.h"

namespace {

HANDLE g_overlayMapping = nullptr;
HANDLE g_overlayMutex = nullptr;
std::atomic<HANDLE> g_overlayReadyEvent{ nullptr };
std::atomic<unsigned int> g_mappingOpenAttempts{ 0 };
std::atomic<unsigned int> g_readyEventOpenAttempts{ 0 };
std::atomic_bool g_loggedFirstFrame{ false };
const replayhud::OverlayPayload* g_overlayPayload = nullptr;

void LogHook(const wchar_t* message) {
    OutputDebugStringW(message);
    OutputDebugStringW(L"\r\n");

    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    std::wstring directory = localAppData;
    directory += L"\\ReplayHUD";
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring path = directory + L"\\ReplayHUDHook.log";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[1024]{};
    swprintf_s(line, L"[%04u-%02u-%02u %02u:%02u:%02u] PID %lu: %s\r\n",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond, GetCurrentProcessId(), message);

    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        std::string utf8(static_cast<size_t>(bytes), '\0');
        if (WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr) > 0) {
            utf8.pop_back();
            DWORD written = 0;
            WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    }
    CloseHandle(file);
}

bool InitializeOverlayMapping() {
    if (g_overlayPayload) return true;
    const unsigned int attempt = g_mappingOpenAttempts.fetch_add(1, std::memory_order_relaxed);
    if (attempt != 0 && attempt % 30 != 0) return false;
    g_overlayMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
        replayhud::kOverlayMutexName);
    if (!g_overlayMutex) return false;
    g_overlayMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, replayhud::kOverlayMappingName);
    if (!g_overlayMapping) {
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
        return false;
    }
    g_overlayPayload = static_cast<const replayhud::OverlayPayload*>(
        MapViewOfFile(g_overlayMapping, FILE_MAP_READ, 0, 0, sizeof(replayhud::OverlayPayload)));
    if (!g_overlayPayload) {
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
        CloseHandle(g_overlayMutex);
        g_overlayMutex = nullptr;
        return false;
    }
    return true;
}

bool ReadOverlayPayload(replayhud::OverlayPayload& snapshot) {
    if (!InitializeOverlayMapping() || !g_overlayPayload || !g_overlayMutex) return false;

    // Never stall the game's render thread waiting for a notification update.
    const DWORD wait = WaitForSingleObject(g_overlayMutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;

    snapshot.version = g_overlayPayload->version;
    snapshot.active = g_overlayPayload->active;
    snapshot.expiresAtTick = g_overlayPayload->expiresAtTick;
    snapshot.position = g_overlayPayload->position;
    snapshot.marginPx = g_overlayPayload->marginPx;
    memcpy(snapshot.title, g_overlayPayload->title, sizeof(snapshot.title));
    memcpy(snapshot.subtitle, g_overlayPayload->subtitle, sizeof(snapshot.subtitle));
    ReleaseMutex(g_overlayMutex);
    return true;
}

std::string WideToUtf8(const wchar_t* text, size_t length) {
    if (!text || length == 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text,
        static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length),
            result.data(), bytes, nullptr, nullptr)) return {};
    return result;
}

void SignalOverlayReady() {
    HANDLE event = g_overlayReadyEvent.load(std::memory_order_acquire);
    if (!event) {
        const unsigned int attempt = g_readyEventOpenAttempts.fetch_add(1, std::memory_order_relaxed);
        if (attempt == 0 || attempt % 15 == 0) {
            HANDLE opened = OpenEventW(EVENT_MODIFY_STATE, FALSE, replayhud::kOverlayReadyEventName);
            if (opened) {
                HANDLE expected = nullptr;
                if (!g_overlayReadyEvent.compare_exchange_strong(expected, opened,
                        std::memory_order_acq_rel, std::memory_order_acquire)) {
                    CloseHandle(opened);
                }
                event = g_overlayReadyEvent.load(std::memory_order_acquire);
            }
        }
    }
    if (event) SetEvent(event);
}

void DrawOverlay() {
    // A process can remain hooked after the player alt-tabs away. Only draw
    // into the currently foreground process, never behind another game/app.
    HWND foreground = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (!foreground) return;
    GetWindowThreadProcessId(foreground, &foregroundPid);
    if (foregroundPid != GetCurrentProcessId()) return;

    replayhud::OverlayPayload payload{};
    if (!ReadOverlayPayload(payload)) return;
    if (payload.version != replayhud::kOverlayPayloadVersion || payload.active == 0) return;
    if (GetTickCount64() > payload.expiresAtTick) return;

    const std::string title = WideToUtf8(payload.title,
        wcsnlen_s(payload.title, _countof(payload.title)));
    const std::string subtitle = WideToUtf8(payload.subtitle,
        wcsnlen_s(payload.subtitle, _countof(payload.subtitle)));
    if (title.empty() && subtitle.empty()) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (!draw || display.x <= 0.0f || display.y <= 0.0f) return;

    constexpr float panelWidth = 440.0f;
    constexpr float panelHeight = 110.0f;
    const float margin = static_cast<float>((std::clamp)(payload.marginPx, 8L, 200L));
    const float width = (std::min)(panelWidth, (std::max)(1.0f, display.x - margin * 2.0f));
    const float height = (std::min)(panelHeight, (std::max)(1.0f, display.y - margin * 2.0f));
    const LONG position = (std::clamp)(payload.position, 0L, 3L);
    const bool leftAligned = (position == 1 || position == 3);
    const bool bottomAligned = (position >= 2);
    const float left = leftAligned ? margin : (std::max)(margin, display.x - width - margin);
    const float top = bottomAligned ? (std::max)(margin, display.y - height - margin) : margin;
    const ImVec2 min(left, top);
    const ImVec2 max(left + width, top + height);

    draw->AddRectFilled(min, max, IM_COL32(20, 23, 29, 230), 16.0f);
    draw->AddRect(min, max, IM_COL32(54, 61, 72, 255), 16.0f, 1.0f, ImDrawFlags_RoundCornersAll);

    const ImVec2 titlePosition(left + 20.0f, top + 17.0f);
    const ImVec2 subtitlePosition(left + 20.0f, top + 57.0f);
    draw->AddText(titlePosition, IM_COL32(240, 243, 247, 255), title.c_str());
    draw->AddText(subtitlePosition, IM_COL32(157, 166, 180, 255), subtitle.c_str());
    if (!g_loggedFirstFrame.exchange(true, std::memory_order_relaxed))
        LogHook(L"First in-game notification frame rendered.");
    SignalOverlayReady();
}

DWORD WINAPI HookThreadProc(LPVOID) {
    if (!InitializeOverlayMapping()) {
        LogHook(L"Could not open the shared notification mapping; overlay will remain inactive.");
    }

    uih::Config config{};
    config.module = nullptr;
    config.backend = uih::Backend::None;
    config.menuKey = 0;
    config.unloadKey = 0;
    config.menuButtons = 0;
    config.debugLog = true;
    config.render = DrawOverlay;

    // Universal Dear ImGui Hook owns API detection, hook installation, device
    // lifecycle, and renderer state restoration for its supported backends.
    uih::Start(config);
    LogHook(L"Multi-API overlay hook startup requested.");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else OutputDebugStringW(L"ReplayHUD: failed to create overlay initialization thread.\r\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        // Do not tear down graphics hooks while the loader lock is held.
        break;
    default:
        break;
    }
    return TRUE;
}
