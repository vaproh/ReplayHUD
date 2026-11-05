#pragma once

#include <windows.h>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v4";
constexpr wchar_t kOverlayMutexName[] = L"Local\\ReplayHUD.Overlay.Mutex.v4";
constexpr wchar_t kOverlayReadyEventName[] = L"Local\\ReplayHUD.Overlay.Ready.v1";
constexpr ULONG kOverlayPayloadVersion = 4;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

// Shared between ReplayHUD.exe and ReplayHUDHook.dll. All payload reads and
// writes are protected by the named mutex; no cross-process C++ data races.
struct OverlayPayload {
    ULONG version = kOverlayPayloadVersion;
    ULONG active = 0;
    ULONGLONG expiresAtTick = 0;
    LONG position = 0; // 0 TR, 1 TL, 2 BR, 3 BL
    LONG marginPx = 24;
    WCHAR title[kOverlayTitleLength]{};
    WCHAR subtitle[kOverlaySubtitleLength]{};
};

} // namespace replayhud
