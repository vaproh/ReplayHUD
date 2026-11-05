#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <d3d11.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dxgi.h>

#include <cwchar>
#include <cstring>
#include <mutex>
#include <string>

#include <MinHook.h>

#include "ReplayHUDOverlayBridge.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_originalPresent = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
std::mutex g_renderMutex;

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
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

HANDLE g_overlayMapping = nullptr;
const replayhud::OverlayPayload* g_overlayPayload = nullptr;

ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_writeFactory = nullptr;
ID2D1RenderTarget* g_renderTarget = nullptr;
ID2D1SolidColorBrush* g_backgroundBrush = nullptr;
ID2D1SolidColorBrush* g_borderBrush = nullptr;
ID2D1SolidColorBrush* g_titleBrush = nullptr;
ID2D1SolidColorBrush* g_subtitleBrush = nullptr;
IDWriteTextFormat* g_titleFormat = nullptr;
IDWriteTextFormat* g_subtitleFormat = nullptr;
IDXGISwapChain* g_boundSwapChain = nullptr;

bool InitializeOverlayMapping() {
    if (g_overlayPayload) return true;
    g_overlayMapping = OpenFileMappingW(FILE_MAP_READ, FALSE, replayhud::kOverlayMappingName);
    if (!g_overlayMapping) return false;
    g_overlayPayload = static_cast<const replayhud::OverlayPayload*>(
        MapViewOfFile(g_overlayMapping, FILE_MAP_READ, 0, 0, sizeof(replayhud::OverlayPayload)));
    if (!g_overlayPayload) {
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
        return false;
    }
    return true;
}

void ShutdownOverlayMapping() {
    if (g_overlayPayload) {
        UnmapViewOfFile(g_overlayPayload);
        g_overlayPayload = nullptr;
    }
    if (g_overlayMapping) {
        CloseHandle(g_overlayMapping);
        g_overlayMapping = nullptr;
    }
}

void ReleaseRenderObjects() {
    SafeRelease(g_backgroundBrush);
    SafeRelease(g_borderBrush);
    SafeRelease(g_titleBrush);
    SafeRelease(g_subtitleBrush);
    SafeRelease(g_titleFormat);
    SafeRelease(g_subtitleFormat);
    SafeRelease(g_renderTarget);
    if (g_boundSwapChain) {
        g_boundSwapChain->Release();
        g_boundSwapChain = nullptr;
    }
}

bool EnsureFactories() {
    if (!g_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory))) return false;
    }
    if (!g_writeFactory) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&g_writeFactory)))) return false;
    }
    return true;
}

bool EnsureRenderTarget(IDXGISwapChain* swapChain) {
    if (!EnsureFactories()) return false;
    if (g_boundSwapChain != swapChain) {
        ReleaseRenderObjects();
    }
    if (g_renderTarget) return true;

    IDXGISurface* surface = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(IDXGISurface), reinterpret_cast<void**>(&surface)))) {
        return false;
    }

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
        0.0f, 0.0f);
    HRESULT hr = g_d2dFactory->CreateDxgiSurfaceRenderTarget(surface, &props, &g_renderTarget);
    surface->Release();
    if (FAILED(hr)) return false;

    g_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x14171D, 0.90f), &g_backgroundBrush);
    g_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x363D48, 1.0f), &g_borderBrush);
    g_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xF0F3F7, 1.0f), &g_titleBrush);
    g_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x9DA6B4, 1.0f), &g_subtitleBrush);
    if (!g_backgroundBrush || !g_borderBrush || !g_titleBrush || !g_subtitleBrush) return false;

    if (FAILED(g_writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"", &g_titleFormat))) {
        return false;
    }
    if (FAILED(g_writeFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 19.0f, L"", &g_subtitleFormat))) {
        return false;
    }
    g_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_subtitleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    g_boundSwapChain = swapChain;
    g_boundSwapChain->AddRef();
    return true;
}

bool ReadOverlayPayload(replayhud::OverlayPayload& snapshot) {
    if (!InitializeOverlayMapping() || !g_overlayPayload) return false;

    const volatile LONG* sequence =
        reinterpret_cast<const volatile LONG*>(&g_overlayPayload->sequence);
    for (int attempt = 0; attempt < 3; ++attempt) {
        const LONG before = InterlockedCompareExchange(
            const_cast<volatile LONG*>(sequence), 0, 0);
        if (before & 1) continue;

        snapshot.version = g_overlayPayload->version;
        snapshot.active = g_overlayPayload->active;
        snapshot.expiresAtTick = g_overlayPayload->expiresAtTick;
        memcpy(snapshot.title, g_overlayPayload->title, sizeof(snapshot.title));
        memcpy(snapshot.subtitle, g_overlayPayload->subtitle, sizeof(snapshot.subtitle));
        MemoryBarrier();

        const LONG after = InterlockedCompareExchange(
            const_cast<volatile LONG*>(sequence), 0, 0);
        if (before == after && !(after & 1)) return true;
    }
    return false;
}

void DrawOverlay(IDXGISwapChain* swapChain) {
    replayhud::OverlayPayload payload{};
    if (!ReadOverlayPayload(payload)) return;
    if (payload.version != replayhud::kOverlayPayloadVersion || payload.active == 0) return;
    if (GetTickCount64() > payload.expiresAtTick) return;
    if (!EnsureRenderTarget(swapChain) || !g_renderTarget) return;

    const D2D1_SIZE_F size = g_renderTarget->GetSize();
    const float margin = 24.0f;
    const float width = (std::min)(440.0f, (std::max)(160.0f, size.width - margin * 2.0f));
    const float height = 110.0f;
    const float left = (std::max)(margin, size.width - width - margin);
    const D2D1_ROUNDED_RECT panel{
        D2D1::RectF(left, margin, left + width, margin + height), 16.0f, 16.0f
    };
    const D2D1_RECT_F titleRect = D2D1::RectF(panel.rect.left + 20.0f, panel.rect.top + 14.0f,
        panel.rect.right - 20.0f, panel.rect.top + 52.0f);
    const D2D1_RECT_F subtitleRect = D2D1::RectF(panel.rect.left + 20.0f, panel.rect.top + 50.0f,
        panel.rect.right - 20.0f, panel.rect.bottom - 14.0f);

    g_renderTarget->BeginDraw();
    g_renderTarget->FillRoundedRectangle(panel, g_backgroundBrush);
    g_renderTarget->DrawRoundedRectangle(panel, g_borderBrush, 1.0f);
    g_renderTarget->DrawTextW(payload.title,
        static_cast<UINT32>(wcsnlen_s(payload.title, _countof(payload.title))),
        g_titleFormat, titleRect, g_titleBrush);
    g_renderTarget->DrawTextW(payload.subtitle,
        static_cast<UINT32>(wcsnlen_s(payload.subtitle, _countof(payload.subtitle))),
        g_subtitleFormat, subtitleRect, g_subtitleBrush);
    const HRESULT drawResult = g_renderTarget->EndDraw();
    if (drawResult == D2DERR_RECREATE_TARGET || FAILED(drawResult)) {
        ReleaseRenderObjects();
    }
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if ((flags & DXGI_PRESENT_TEST) == 0) {
        std::lock_guard<std::mutex> lock(g_renderMutex);
        DrawOverlay(swapChain);
    }
    return g_originalPresent
        ? g_originalPresent(swapChain, syncInterval, flags)
        : E_FAIL;
}

HRESULT __stdcall HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    if (g_boundSwapChain == swapChain) {
        // Release the D2D surface reference before DXGI reallocates the back buffers.
        ReleaseRenderObjects();
    }
    return g_originalResizeBuffers
        ? g_originalResizeBuffers(swapChain, bufferCount, width, height, newFormat, swapChainFlags)
        : E_FAIL;
}

bool InstallPresentHook() {
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        LogHook(L"MinHook initialization failed.");
        return false;
    }

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"ReplayHUD hook probe",
        WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
        LogHook(L"Could not create the temporary DXGI probe window.");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 1;
    desc.OutputWindow = hwnd;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level{};

    auto createProbe = [&](D3D_DRIVER_TYPE driverType) {
        return D3D11CreateDeviceAndSwapChain(nullptr, driverType, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, _countof(levels),
            D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
    };

    HRESULT created = createProbe(D3D_DRIVER_TYPE_HARDWARE);
    if (FAILED(created)) {
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        created = createProbe(D3D_DRIVER_TYPE_WARP);
    }

    DestroyWindow(hwnd);
    if (FAILED(created) || !swapChain) {
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        wchar_t message[128]{};
        swprintf_s(message, L"Could not create a D3D11 probe swapchain (HRESULT 0x%08X).",
            static_cast<unsigned>(created));
        LogHook(message);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    void* presentTarget = vtable[8];
    void* resizeTarget = vtable[13];

    mhStatus = MH_CreateHook(presentTarget, &HookedPresent,
        reinterpret_cast<LPVOID*>(&g_originalPresent));
    if (mhStatus != MH_OK) {
        LogHook(L"MinHook could not create the IDXGISwapChain::Present hook.");
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        return false;
    }

    mhStatus = MH_CreateHook(resizeTarget, &HookedResizeBuffers,
        reinterpret_cast<LPVOID*>(&g_originalResizeBuffers));
    if (mhStatus != MH_OK) {
        MH_RemoveHook(presentTarget);
        LogHook(L"MinHook could not create the IDXGISwapChain::ResizeBuffers hook.");
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        return false;
    }

    mhStatus = MH_EnableHook(MH_ALL_HOOKS);
    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(swapChain);

    if (mhStatus != MH_OK) {
        MH_RemoveHook(presentTarget);
        MH_RemoveHook(resizeTarget);
        LogHook(L"MinHook could not enable the DXGI hooks.");
        g_originalPresent = nullptr;
        g_originalResizeBuffers = nullptr;
        return false;
    }

    LogHook(L"D3D11 Present and ResizeBuffers hooks installed successfully.");
    return true;
}

DWORD WINAPI HookThreadProc(LPVOID) {
    if (!InstallPresentHook()) {
        LogHook(L"Overlay hook initialization failed; desktop notifications remain available.");
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else OutputDebugStringW(L"ReplayHUD: failed to create hook initialization thread.\r\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        // The DLL is process-lifetime scoped. Avoid COM/D3D teardown under the loader lock.
        break;
    default:
        break;
    }
    return TRUE;
}
