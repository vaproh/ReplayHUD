#pragma once

#include <windows.h>
#include <type_traits>

namespace replayhud {

constexpr wchar_t kOverlayMappingName[] = L"Local\\ReplayHUD.Overlay.v5";
constexpr wchar_t kOverlayMutexName[] = L"Local\\ReplayHUD.Overlay.Mutex.v5";
constexpr wchar_t kOverlayReadyEventName[] = L"Local\\ReplayHUD.Overlay.Ready.v1";
constexpr ULONG kOverlayPayloadVersion = 5;
constexpr size_t kOverlayTitleLength = 64;
constexpr size_t kOverlaySubtitleLength = 256;

// Shared between the x64 app and x86/x64 game hooks. Keep the layout fixed
// across architectures; all reads and writes use the named mutex.
#pragma pack(push, 8)
struct OverlayPayload {
    ULONG version;
    ULONG active;
    ULONGLONG expiresAtTick;
    ULONGLONG sequence;
    LONG position; // 0 TR, 1 TL, 2 BR, 3 BL
    LONG marginPx;
    WCHAR title[kOverlayTitleLength];
    WCHAR subtitle[kOverlaySubtitleLength];
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<OverlayPayload>);
static_assert(sizeof(OverlayPayload) == 672, "Shared overlay payload layout changed");

} // namespace replayhud
