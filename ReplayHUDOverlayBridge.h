#pragma once

#include <windows.h>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v2";
constexpr ULONG kOverlayPayloadVersion = 2;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

// Shared between ReplayHUD.exe and ReplayHUDHook.dll.
// sequence is a process-shared seqlock: odd while the writer updates the payload,
// even when the payload is stable.
struct OverlayPayload {
    volatile LONG sequence = 0;
    ULONG version = kOverlayPayloadVersion;
    ULONG active = 0;
    ULONGLONG expiresAtTick = 0;
    WCHAR title[kOverlayTitleLength]{};
    WCHAR subtitle[kOverlaySubtitleLength]{};
};

} // namespace replayhud
