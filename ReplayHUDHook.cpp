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

constexpr SIZE_T kPatchSize = 12;

PresentFn g_originalPresent = nullptr;
void* g_presentTarget = nullptr;
BYTE g_originalPatch[kPatchSize]{};
void* g_trampoline = nullptr;
std::mutex g_renderMutex;

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

void DrawOverlay(IDXGISwapChain* swapChain) {
    if (!InitializeOverlayMapping() || !g_overlayPayload) return;
    if (g_overlayPayload->version != replayhud::kOverlayPayloadVersion || g_overlayPayload->active == 0) return;
    if (GetTickCount64() > g_overlayPayload->expiresAtTick) return;
    if (!EnsureRenderTarget(swapChain) || !g_renderTarget) return;

    D2D1_SIZE_F size = g_renderTarget->GetSize();
    const float width = 440.0f;
    const float height = 110.0f;
    const float margin = 24.0f;
    const D2D1_ROUNDED_RECT panel{
        D2D1::RectF(size.width - width - margin, margin, size.width - margin, margin + height), 16.0f, 16.0f
    };
    const D2D1_RECT_F titleRect = D2D1::RectF(panel.rect.left + 20.0f, panel.rect.top + 14.0f,
        panel.rect.right - 20.0f, panel.rect.top + 52.0f);
    const D2D1_RECT_F subtitleRect = D2D1::RectF(panel.rect.left + 20.0f, panel.rect.top + 50.0f,
        panel.rect.right - 20.0f, panel.rect.bottom - 14.0f);

    g_renderTarget->BeginDraw();
    g_renderTarget->FillRoundedRectangle(panel, g_backgroundBrush);
    g_renderTarget->DrawRoundedRectangle(panel, g_borderBrush, 1.0f);
    g_renderTarget->DrawTextW(g_overlayPayload->title,
        static_cast<UINT32>(wcslen(g_overlayPayload->title)), g_titleFormat, titleRect, g_titleBrush);
    g_renderTarget->DrawTextW(g_overlayPayload->subtitle,
        static_cast<UINT32>(wcslen(g_overlayPayload->subtitle)), g_subtitleFormat, subtitleRect, g_subtitleBrush);
    if (g_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        ReleaseRenderObjects();
    }
}

HRESULT __stdcall HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    std::lock_guard<std::mutex> lock(g_renderMutex);
    DrawOverlay(swapChain);
    return g_originalPresent ? g_originalPresent(swapChain, syncInterval, flags)
                             : S_OK;
}

bool WriteAbsoluteJump(void* source, const void* destination) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(source, kPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;
    BYTE patch[kPatchSize] = { 0x48, 0xB8 };
    *reinterpret_cast<void**>(&patch[2]) = const_cast<void*>(destination);
    patch[10] = 0xFF;
    patch[11] = 0xE0;
    memcpy(source, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), source, kPatchSize);
    DWORD ignored = 0;
    VirtualProtect(source, kPatchSize, oldProtect, &ignored);
    return true;
}

bool InstallPresentHook() {
    if (g_originalPresent) return true;

    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ReplayHUD.HookDummyWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* swapChain = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level{};
    HRESULT created = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &desc, &swapChain, &device, &level, &context);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (FAILED(created) || !swapChain) {
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(swapChain);
    g_presentTarget = vtable[8];
    memcpy(g_originalPatch, g_presentTarget, kPatchSize);

    g_trampoline = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_trampoline) {
        SafeRelease(context);
        SafeRelease(device);
        SafeRelease(swapChain);
        return false;
    }
    memcpy(g_trampoline, g_originalPatch, kPatchSize);
    BYTE* jumpBack = static_cast<BYTE*>(g_trampoline) + kPatchSize;
    jumpBack[0] = 0x48;
    jumpBack[1] = 0xB8;
    *reinterpret_cast<void**>(&jumpBack[2]) = static_cast<BYTE*>(g_presentTarget) + kPatchSize;
    jumpBack[10] = 0xFF;
    jumpBack[11] = 0xE0;
    g_originalPresent = reinterpret_cast<PresentFn>(g_trampoline);

    bool hooked = WriteAbsoluteJump(g_presentTarget, reinterpret_cast<void*>(&HookedPresent));
    SafeRelease(context);
    SafeRelease(device);
    SafeRelease(swapChain);
    return hooked;
}

void ShutdownHook() {
    if (g_presentTarget) {
        DWORD oldProtect = 0;
        if (VirtualProtect(g_presentTarget, kPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(g_presentTarget, g_originalPatch, kPatchSize);
            FlushInstructionCache(GetCurrentProcess(), g_presentTarget, kPatchSize);
            DWORD ignored = 0;
            VirtualProtect(g_presentTarget, kPatchSize, oldProtect, &ignored);
        }
    }
    g_presentTarget = nullptr;
    g_originalPresent = nullptr;
    if (g_trampoline) {
        VirtualFree(g_trampoline, 0, MEM_RELEASE);
        g_trampoline = nullptr;
    }
    ReleaseRenderObjects();
    SafeRelease(g_d2dFactory);
    SafeRelease(g_writeFactory);
    ShutdownOverlayMapping();
}

DWORD WINAPI HookThreadProc(LPVOID) {
    InstallPresentHook();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, HookThreadProc, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        ShutdownHook();
        break;
    default:
        break;
    }
    return TRUE;
}
