#pragma once

#include <windows.h>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v1";
constexpr ULONG kOverlayPayloadVersion = 1;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

struct OverlayPayload {
    ULONG version = kOverlayPayloadVersion;
    ULONG active = 0;
    ULONGLONG expiresAtTick = 0;
    WCHAR title[kOverlayTitleLength]{};
    WCHAR subtitle[kOverlaySubtitleLength]{};
};

} // namespace replayhud
