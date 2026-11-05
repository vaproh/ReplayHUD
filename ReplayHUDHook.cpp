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
std::atomic<ULONGLONG> g_lastQueuedNotification{ 0 };
std::atomic<ULONGLONG> g_lastReadyNotification{ 0 };
const replayhud::OverlayPayload* g_overlayPayload = nullptr;
ImFont* g_titleFont = nullptr;
ImFont* g_bodyFont = nullptr;
ImFont* g_hintFont = nullptr;

void SetupOverlayUi() {
    ImGuiIO& io = ImGui::GetIO();

    // Resolve the Windows font directory instead of assuming Windows is on C:.
    wchar_t windowsDirectory[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windowsDirectory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    std::wstring fontDirectory(windowsDirectory, length);
    fontDirectory += L"\\Fonts\\";
    const std::wstring regularFont = fontDirectory + L"segoeui.ttf";
    const std::wstring semiboldFont = fontDirectory + L"segoeuisb.ttf";
    const std::wstring boldFont = fontDirectory + L"segoeuib.ttf";

    // ImGui's file loader accepts narrow paths. Convert using the active
    // Windows code page so CreateFile/fopen can resolve the installed font.
    auto toNarrowPath = [](const std::wstring& path) {
        const int count = WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1,
            nullptr, 0, nullptr, nullptr);
        if (count <= 0) return std::string{};
        std::string result(static_cast<size_t>(count), '\0');
        if (!WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1,
                result.data(), count, nullptr, nullptr)) return std::string{};
        result.pop_back();
        return result;
    };
    const std::string regularPath = toNarrowPath(regularFont);
    const std::string semiboldPath = toNarrowPath(semiboldFont);
    const std::string boldPath = toNarrowPath(boldFont);

    ImFont* regular = regularPath.empty() ? nullptr :
        io.Fonts->AddFontFromFileTTF(regularPath.c_str(), 16.0f);
    ImFont* semibold = semiboldPath.empty() ? nullptr :
        io.Fonts->AddFontFromFileTTF(semiboldPath.c_str(), 17.0f);
    ImFont* bold = boldPath.empty() ? nullptr :
        io.Fonts->AddFontFromFileTTF(boldPath.c_str(), 14.0f);
    ImFont* hint = regularPath.empty() ? nullptr :
        io.Fonts->AddFontFromFileTTF(regularPath.c_str(), 11.0f);

    g_titleFont = bold ? bold : (semibold ? semibold : io.FontDefault);
    g_bodyFont = semibold ? semibold : (regular ? regular : io.FontDefault);
    g_hintFont = hint ? hint : io.FontDefault;
    if (regular) io.FontDefault = regular;
}

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
    if (attempt != 0 && attempt % 10 != 0) return false;
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
    snapshot.sequence = g_overlayPayload->sequence;
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

bool SignalOverlayReady() {
    HANDLE event = g_overlayReadyEvent.load(std::memory_order_acquire);
    if (!event) {
        const unsigned int attempt = g_readyEventOpenAttempts.fetch_add(1, std::memory_order_relaxed);
        if (attempt == 0 || attempt % 5 == 0) {
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
    return event && SetEvent(event) != FALSE;
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

    // Mirror the desktop GDI toast: same dimensions, accent, icon tile,
    // Segoe UI hierarchy, spacing and muted footer.
    constexpr float panelWidth = 440.0f;
    constexpr float panelHeight = 112.0f;
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

    const ImU32 border = IM_COL32(54, 61, 72, 255);
    const ImU32 background = IM_COL32(20, 23, 29, 250);
    const ImU32 panel = IM_COL32(29, 33, 41, 255);
    const ImU32 green = IM_COL32(118, 185, 0, 255);
    const ImU32 text = IM_COL32(240, 243, 247, 255);
    const ImU32 muted = IM_COL32(157, 166, 180, 255);

    draw->AddRectFilled(min, max, border, 11.0f, ImDrawFlags_RoundCornersAll);
    draw->AddRectFilled(ImVec2(left + 1.0f, top + 1.0f),
        ImVec2(left + width - 1.0f, top + height - 1.0f),
        background, 10.0f, ImDrawFlags_RoundCornersAll);
    draw->AddRectFilled(ImVec2(left, top + 16.0f),
        ImVec2(left + 4.0f, top + height - 16.0f), green, 1.0f);

    // Same 52 px icon tile position as the desktop notification.
    const ImVec2 tileMin(left + 22.0f, top + 25.0f);
    const ImVec2 tileMax(left + 74.0f, top + 77.0f);
    draw->AddRectFilled(tileMin, tileMax, panel, 9.0f, ImDrawFlags_RoundCornersAll);

    const ImVec2 center((tileMin.x + tileMax.x) * 0.5f, (tileMin.y + tileMax.y) * 0.5f);
    const bool isStopped = title.find("STOPPED") != std::string::npos;
    const bool isBufferEvent = title.find("REPLAY BUFFER") != std::string::npos;
    const ImU32 glyph = isStopped ? IM_COL32(232, 170, 76, 255) : green;
    if (isBufferEvent && isStopped) {
        draw->AddRectFilled(ImVec2(center.x - 8.0f, center.y - 8.0f),
            ImVec2(center.x - 2.0f, center.y + 8.0f), glyph, 1.0f);
        draw->AddRectFilled(ImVec2(center.x + 2.0f, center.y - 8.0f),
            ImVec2(center.x + 8.0f, center.y + 8.0f), glyph, 1.0f);
    } else {
        draw->AddCircle(center, 12.0f, glyph, 0, 2.5f);
        draw->AddTriangleFilled(ImVec2(center.x - 3.0f, center.y - 6.0f),
            ImVec2(center.x - 3.0f, center.y + 6.0f),
            ImVec2(center.x + 7.0f, center.y), glyph);
    }

    const float textLeft = left + 92.0f;
    draw->PushClipRect(ImVec2(textLeft, top + 12.0f),
        ImVec2(left + width - 18.0f, top + height - 10.0f), true);
    draw->AddText(g_titleFont, 14.0f, ImVec2(textLeft, top + 17.0f),
        green, title.c_str());
    draw->AddText(g_bodyFont, 17.0f, ImVec2(textLeft, top + 41.0f),
        text, subtitle.c_str());
    const char* footer = isBufferEvent
        ? "ReplayHUD  |  OBS Replay Buffer"
        : "Click to open clip  |  ReplayHUD";
    draw->AddText(g_hintFont, 11.0f, ImVec2(textLeft, top + 75.0f),
        muted, footer);
    draw->PopClipRect();

    // The UIH render callback runs before ImGui::Render(). Wait until the
    // same notification reaches a subsequent frame before telling the app
    // that the in-game renderer has had a chance to submit it.
    const ULONGLONG previousQueued =
        g_lastQueuedNotification.exchange(payload.sequence, std::memory_order_acq_rel);
    if (previousQueued == payload.sequence &&
        g_lastReadyNotification.load(std::memory_order_acquire) != payload.sequence &&
        SignalOverlayReady()) {
        g_lastReadyNotification.store(payload.sequence, std::memory_order_release);
        if (!g_loggedFirstFrame.exchange(true, std::memory_order_relaxed))
            LogHook(L"First in-game notification frame submitted to the renderer.");
    }
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
    config.debugLog = GetEnvironmentVariableW(L"REPLAYHUD_DEBUG", nullptr, 0) > 0;
    config.setup = SetupOverlayUi;
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
