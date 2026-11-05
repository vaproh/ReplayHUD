#pragma once

#include <windows.h>
#include <type_traits>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v4";
constexpr wchar_t kOverlayMutexName[] = L"Local\\ReplayHUD.Overlay.Mutex.v4";
constexpr wchar_t kOverlayReadyEventName[] = L"Local\\ReplayHUD.Overlay.Ready.v1";
constexpr ULONG kOverlayPayloadVersion = 4;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

// Shared between the x64 app and x86/x64 game hooks. Keep the layout fixed
// across architectures; all reads and writes use the named mutex.
#pragma pack(push, 8)
struct OverlayPayload {
    ULONG version;
    ULONG active;
    ULONGLONG expiresAtTick;
    LONG position; // 0 TR, 1 TL, 2 BR, 3 BL
    LONG marginPx;
    WCHAR title[kOverlayTitleLength];
    WCHAR subtitle[kOverlaySubtitleLength];
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<OverlayPayload>);
static_assert(sizeof(OverlayPayload) == 664, "Shared overlay payload layout changed");

} // namespace replayhud
