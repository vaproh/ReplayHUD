#pragma once

#include <windows.h>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v3";
constexpr wchar_t kOverlayMutexName[] = L"Local\\ReplayHUD.Overlay.Mutex.v3";
constexpr ULONG kOverlayPayloadVersion = 3;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

// Shared between ReplayHUD.exe and ReplayHUDHook.dll. All payload reads and
// writes are protected by the named mutex; no cross-process C++ data races.
struct OverlayPayload {
    ULONG version = kOverlayPayloadVersion;
    ULONG active = 0;
    ULONGLONG expiresAtTick = 0;
    WCHAR title[kOverlayTitleLength]{};
    WCHAR subtitle[kOverlaySubtitleLength]{};
};

} // namespace replayhud
