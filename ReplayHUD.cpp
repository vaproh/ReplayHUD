
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0602

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <atomic>
#include <algorithm>
#include <string>
#include <iterator>
#include <thread>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winmm.lib")

namespace {
constexpr wchar_t kMainClass[] = L"ReplayHUD.MainWindow";
constexpr wchar_t kOverlayClass[] = L"ReplayHUD.OverlayWindow";
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kShowReplayMessage = WM_APP + 11;
constexpr UINT kStatusMessage = WM_APP + 12;
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT kTrayId = 1;
constexpr int kOverlayWidth = 352;
constexpr int kOverlayHeight = 86;
constexpr int kMenuTest = 1001;
constexpr int kMenuConfig = 1002;
constexpr int kMenuExit = 1003;

struct Settings {
    INTERNET_PORT port = 4455;
    std::wstring password;
    bool sound = true;
    UINT durationMs = 1800;
    int marginPx = 28;
};

Settings g_settings;
std::wstring g_configPath;
HWND g_mainWindow = nullptr;
HWND g_overlayWindow = nullptr;
NOTIFYICONDATAW g_trayData{};
std::thread g_worker;
std::atomic_bool g_running{ true };
std::atomic<HINTERNET> g_activeWebSocket{ nullptr };
HANDLE g_stopEvent = nullptr;

std::wstring GetLocalConfigPath() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? localAppData : L".";
    base += L"\\ReplayHUD";
    CreateDirectoryW(base.c_str(), nullptr);
    return base + L"\\ReplayHUD.ini";
}

void EnsureDefaultConfig() {
    HANDLE file = CreateFileW(g_configPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    static constexpr char defaults[] =
        "; ReplayHUD settings. OBS WebSocket listens on 127.0.0.1 only.\r\n"
        "[OBS]\r\n"
        "port=4455\r\n"
        "password=\r\n"
        "\r\n"
        "[HUD]\r\n"
        "sound=1\r\n"
        "duration_ms=1800\r\n"
        "margin_px=28\r\n";

    DWORD written = 0;
    WriteFile(file, defaults, static_cast<DWORD>(sizeof(defaults) - 1), &written, nullptr);
    CloseHandle(file);
}

void LoadSettings() {
    g_configPath = GetLocalConfigPath();
    EnsureDefaultConfig();

    g_settings.port = static_cast<INTERNET_PORT>(std::clamp<UINT>(
        GetPrivateProfileIntW(L"OBS", L"port", 4455, g_configPath.c_str()),
        1u, 65535u));

    wchar_t password[2048]{};

    GetPrivateProfileStringW(
        L"OBS", L"password", L"", password,
        static_cast<DWORD>(std::size(password)),
        g_configPath.c_str()
    );

    g_settings.password = password;

    g_settings.sound =
        GetPrivateProfileIntW(L"HUD", L"sound", 1, g_configPath.c_str()) != 0;

    g_settings.durationMs = std::clamp<UINT>(
        GetPrivateProfileIntW(L"HUD", L"duration_ms", 1800, g_configPath.c_str()),
        800u, 7000u
    );

    g_settings.marginPx = static_cast<int>(std::clamp<UINT>(
        GetPrivateProfileIntW(L"HUD", L"margin_px", 28, g_configPath.c_str()),
        8u, 160u
    ));
}

std::string WideToUtf8(const std::wstring& input) {
    if (input.empty()) return {};

    int count = WideCharToMultiByte(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        nullptr, 0, nullptr, nullptr
    );

    if (count <= 0) return {};

    std::string output(static_cast<size_t>(count), '\0');

    WideCharToMultiByte(
        CP_UTF8, 0, input.data(), static_cast<int>(input.size()),
        output.data(), count, nullptr, nullptr
    );

    return output;
}

bool Sha256Base64(const std::string& input, std::string& output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    DWORD objectLength = 0, hashLength = 0, copied = 0;
    std::vector<UCHAR> hashObject;
    std::vector<UCHAR> hashBytes;
    bool success = false;

    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        return false;

    if (BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &copied, 0) < 0)
        goto cleanup;

    if (BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
            &copied, 0) < 0)
        goto cleanup;

    hashObject.resize(objectLength);
    hashBytes.resize(hashLength);

    if (BCryptCreateHash(
            algorithm, &hashHandle, hashObject.data(), objectLength,
            nullptr, 0, 0) < 0)
        goto cleanup;

    if (!input.empty() && BCryptHashData(
            hashHandle,
            reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
            static_cast<ULONG>(input.size()), 0) < 0)
        goto cleanup;

    if (BCryptFinishHash(
            hashHandle, hashBytes.data(), hashLength, 0) < 0)
        goto cleanup;

    {
        DWORD chars = 0;

        if (!CryptBinaryToStringA(
                hashBytes.data(), hashLength,
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                nullptr, &chars))
            goto cleanup;

        output.assign(chars, '\0');

        if (!CryptBinaryToStringA(
                hashBytes.data(), hashLength,
                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                output.data(), &chars))
            goto cleanup;

        while (!output.empty() && output.back() == '\0')
            output.pop_back();
    }

    success = true;

cleanup:
    if (hashHandle) BCryptDestroyHash(hashHandle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);

    return success;
}

bool MakeObsAuthentication(
    const std::wstring& password,
    const std::string& salt,
    const std::string& challenge,
    std::string& result) {

    const std::string utf8Password = WideToUtf8(password);
    std::string secret;

    if (!Sha256Base64(utf8Password + salt, secret))
        return false;

    return Sha256Base64(secret + challenge, result);
}

bool JsonString(
    const std::string& json,
    const char* key,
    std::string& value) {

    const std::string marker = std::string("\"") + key + "\"";

    size_t position = json.find(marker);
    if (position == std::string::npos) return false;

    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return false;

    ++position;

    while (position < json.size() &&
           (json[position] == ' ' ||
            json[position] == '\t' ||
            json[position] == '\r' ||
            json[position] == '\n'))
        ++position;

    if (position >= json.size() || json[position] != '"')
        return false;

    ++position;

    value.clear();

    while (position < json.size()) {
        char c = json[position++];

        if (c == '"')
            return true;

        if (c != '\\') {
            value.push_back(c);
            continue;
        }

        if (position >= json.size())
            return false;

        char escaped = json[position++];

        switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(escaped); break;
        }
    }

    return false;
}

bool JsonInteger(
    const std::string& json,
    const char* key,
    int& value) {

    const std::string marker = std::string("\"") + key + "\"";

    size_t position = json.find(marker);
    if (position == std::string::npos) return false;

    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return false;

    ++position;

    while (position < json.size() &&
           (json[position] == ' ' ||
            json[position] == '\t' ||
            json[position] == '\r' ||
            json[position] == '\n'))
        ++position;

    try {
        size_t used = 0;
        value = std::stoi(json.substr(position), &used);
        return used > 0;
    }
    catch (...) {
        return false;
    }
}

bool ReceiveWebSocketMessage(
    HINTERNET socket,
    std::string& message) {

    char buffer[4096];
    message.clear();

    for (;;) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};

        DWORD error = WinHttpWebSocketReceive(
            socket, buffer, sizeof(buffer), &bytesRead, &type
        );

        if (error != NO_ERROR)
            return false;

        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            return false;

        if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
            return false;

        message.append(buffer, bytesRead);

        if (message.size() > 65536)
            return false;

        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
            return true;

        if (type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE)
            return false;
    }
}

bool SendWebSocketText(
    HINTERNET socket,
    const std::string& message) {

    return WinHttpWebSocketSend(
        socket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<DWORD>(message.size())
    ) == NO_ERROR;
}

void SetTrayStatus(int status) {
    if (!g_mainWindow) return;

    PostMessageW(
        g_mainWindow,
        kStatusMessage,
        static_cast<WPARAM>(status),
        0
    );
}

void WaitBeforeReconnect() {
    if (g_stopEvent)
        WaitForSingleObject(g_stopEvent, 4500);
}

void ObsWorker() {
    while (g_running.load()) {

        HINTERNET session = WinHttpOpen(
            L"ReplayHUD/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );

        if (!session) {
            SetTrayStatus(0);
            WaitBeforeReconnect();
            continue;
        }

        WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);

        HINTERNET connection = WinHttpConnect(
            session, L"127.0.0.1", g_settings.port, 0
        );

        HINTERNET request = nullptr;
        HINTERNET socket = nullptr;
        bool identified = false;

        if (connection) {
            request = WinHttpOpenRequest(
                connection, L"GET", L"/", nullptr,
                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0
            );
        }

        if (request && WinHttpSetOption(
                request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                nullptr, 0)) {

            if (WinHttpSendRequest(
                    request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {

                DWORD statusCode = 0;
                DWORD statusSize = sizeof(statusCode);

                if (WinHttpQueryHeaders(
                        request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX) &&
                    statusCode == 101) {

                    socket = WinHttpWebSocketCompleteUpgrade(request, 0);
                }
            }
        }

        if (request) {
            WinHttpCloseHandle(request);
            request = nullptr;
        }

        if (!socket) {
            if (connection) WinHttpCloseHandle(connection);

            WinHttpCloseHandle(session);

            SetTrayStatus(0);
            WaitBeforeReconnect();
            continue;
        }

        g_activeWebSocket.store(socket);

        if (!g_running.load()) {
            if (g_activeWebSocket.exchange(nullptr) == socket)
                WinHttpCloseHandle(socket);

            if (connection) WinHttpCloseHandle(connection);

            WinHttpCloseHandle(session);
            break;
        }

        std::string hello;

        if (ReceiveWebSocketMessage(socket, hello)) {
            int op = -1;

            if (JsonInteger(hello, "op", op) && op == 0) {

                std::string identify =
                    "{\"op\":1,\"d\":{\"rpcVersion\":1,\"eventSubscriptions\":64";

                std::string challenge, salt;

                if (JsonString(hello, "challenge", challenge) &&
                    JsonString(hello, "salt", salt)) {

                    std::string authentication;

                    if (MakeObsAuthentication(
                            g_settings.password,
                            salt,
                            challenge,
                            authentication)) {

                        identify += ",\"authentication\":\"" +
                                    authentication + "\"";
                    }
                    else {
                        identify.clear();
                    }
                }
                else if (hello.find("\"authentication\"") != std::string::npos) {
                    identify.clear();
                }

                if (!identify.empty()) {
                    identify += "}}";

                    if (SendWebSocketText(socket, identify)) {

                        std::string incoming;

                        if (ReceiveWebSocketMessage(socket, incoming)) {
                            int responseOp = -1;

                            identified =
                                JsonInteger(incoming, "op", responseOp) &&
                                responseOp == 2;
                        }
                    }
                }
            }
        }

        if (identified && g_running.load()) {
            SetTrayStatus(1);

            std::string message;

            while (g_running.load() &&
                   ReceiveWebSocketMessage(socket, message)) {

                int op = -1;
                std::string eventType;

                if (JsonInteger(message, "op", op) &&
                    op == 5 &&
                    JsonString(message, "eventType", eventType) &&
                    eventType == "ReplayBufferSaved") {

                    if (g_mainWindow) {
                        PostMessageW(
                            g_mainWindow,
                            kShowReplayMessage,
                            0,
                            0
                        );
                    }
                }
            }
        }
        else if (g_running.load()) {
            SetTrayStatus(-1);
        }

        if (g_activeWebSocket.exchange(nullptr) == socket) {
            WinHttpCloseHandle(socket);
        }

        if (connection) WinHttpCloseHandle(connection);

        WinHttpCloseHandle(session);

        if (g_running.load()) {
            SetTrayStatus(0);
            WaitBeforeReconnect();
        }
    }
}

void UpdateTrayTooltip(const wchar_t* text) {
    if (!g_trayData.hWnd) return;

    wcsncpy_s(g_trayData.szTip, text, _TRUNCATE);
    g_trayData.uFlags = NIF_TIP;

    Shell_NotifyIconW(NIM_MODIFY, &g_trayData);
}

void AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g_trayData, sizeof(g_trayData));

    g_trayData.cbSize = sizeof(g_trayData);
    g_trayData.hWnd = hwnd;
    g_trayData.uID = kTrayId;
    g_trayData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_trayData.uCallbackMessage = kTrayMessage;
    g_trayData.hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    wcsncpy_s(
        g_trayData.szTip,
        L"ReplayHUD | Waiting for OBS",
        _TRUNCATE
    );

    Shell_NotifyIconW(NIM_ADD, &g_trayData);

    g_trayData.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_trayData);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, kMenuTest, L"Test notification");
    AppendMenuW(menu, MF_STRING, kMenuConfig, L"Open settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit ReplayHUD");

    POINT point{};
    GetCursorPos(&point);

    SetForegroundWindow(hwnd);

    UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD,
        point.x,
        point.y,
        0,
        hwnd,
        nullptr
    );

    // Required for reliable dismissal of notification-area menus.
    PostMessageW(hwnd, WM_NULL, 0, 0);

    DestroyMenu(menu);

    if (command)
        SendMessageW(hwnd, WM_COMMAND, command, 0);
}

void ShowReplayNotification() {
    if (!g_overlayWindow) return;

    if (g_settings.sound) {
        if (!PlaySoundW(
                L"SystemAsterisk",
                nullptr,
                SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) {

            MessageBeep(MB_OK);
        }
    }

    HWND foreground = GetForegroundWindow();

    HMONITOR monitor = MonitorFromWindow(
        foreground, MONITOR_DEFAULTTONEAREST
    );

    MONITORINFO info{};
    info.cbSize = sizeof(info);

    RECT bounds{};

    if (GetMonitorInfoW(monitor, &info))
        bounds = info.rcMonitor;
    else {
        bounds.right = GetSystemMetrics(SM_CXSCREEN);
        bounds.bottom = GetSystemMetrics(SM_CYSCREEN);
    }

    const int x = bounds.right - kOverlayWidth - g_settings.marginPx;
    const int y = bounds.top + g_settings.marginPx + 12;

    SetWindowPos(
        g_overlayWindow,
        HWND_TOPMOST,
        x,
        y,
        kOverlayWidth,
        kOverlayHeight,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );

    ShowWindow(g_overlayWindow, SW_SHOWNOACTIVATE);
    InvalidateRect(g_overlayWindow, nullptr, TRUE);

    SetTimer(g_overlayWindow, kHideTimer, g_settings.durationMs, nullptr);
}

LRESULT CALLBACK OverlayWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    switch (message) {

    case WM_ERASEBKGND:
        return 1;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_TIMER:
        if (wParam == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);

        RECT client{};
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(RGB(24, 27, 32));
        HPEN outline = CreatePen(PS_SOLID, 1, RGB(48, 53, 61));

        HGDIOBJ oldBrush = SelectObject(dc, background);
        HGDIOBJ oldPen = SelectObject(dc, outline);

        RoundRect(dc, 0, 0, client.right, client.bottom, 18, 18);

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);

        DeleteObject(background);
        DeleteObject(outline);

        HBRUSH green = CreateSolidBrush(RGB(118, 185, 0));
        HPEN greenPen = CreatePen(PS_SOLID, 1, RGB(118, 185, 0));

        oldBrush = SelectObject(dc, green);
        oldPen = SelectObject(dc, greenPen);

        Ellipse(dc, 24, 22, 64, 62);

        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);

        DeleteObject(green);
        DeleteObject(greenPen);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        HFONT iconFont = CreateFontW(
            27, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Symbol"
        );

        HFONT titleFont = CreateFontW(
            17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

        HFONT subtitleFont = CreateFontW(
            13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI"
        );

        RECT iconRect{ 24, 18, 64, 63 };

        HGDIOBJ oldFont = SelectObject(dc, iconFont);

        DrawTextW(
            dc, L"✓", -1, &iconRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE
        );

        RECT titleRect{ 82, 17, 330, 43 };

        SelectObject(dc, titleFont);
        SetTextColor(dc, RGB(245, 247, 250));

        DrawTextW(
            dc, L"REPLAY SAVED", -1, &titleRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE
        );

        RECT subRect{ 83, 45, 330, 68 };

        SelectObject(dc, subtitleFont);
        SetTextColor(dc, RGB(164, 172, 184));

        DrawTextW(
            dc, L"Clip captured successfully", -1, &subRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE
        );

        SelectObject(dc, oldFont);

        DeleteObject(iconFont);
        DeleteObject(titleFont);
        DeleteObject(subtitleFont);

        EndPaint(hwnd, &paint);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK MainWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {

    switch (message) {

    case kTrayMessage: {
        // NOTIFYICON_VERSION_4 packs the notification code into LOWORD(lParam).
        const UINT event = LOWORD(lParam);

        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        }
        else if (event == WM_LBUTTONDBLCLK ||
                 event == NIN_SELECT ||
                 event == NIN_KEYSELECT) {

            ShowReplayNotification();
        }

        return 0;
    }

    case kShowReplayMessage:
        ShowReplayNotification();
        return 0;

    case kStatusMessage:
        if (wParam == 1)
            UpdateTrayTooltip(L"ReplayHUD | Connected to OBS");
        else if (wParam == static_cast<WPARAM>(-1))
            UpdateTrayTooltip(L"ReplayHUD | OBS authentication failed; check settings");
        else
            UpdateTrayTooltip(L"ReplayHUD | Waiting for OBS");

        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {

        case kMenuTest:
            ShowReplayNotification();
            return 0;

        case kMenuConfig:
            ShellExecuteW(
                hwnd,
                L"open",
                L"notepad.exe",
                (L"\"" + g_configPath + L"\"").c_str(),
                nullptr,
                SW_SHOWNORMAL
            );
            return 0;

        case kMenuExit:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DESTROY: {
        g_running.store(false);

        if (g_stopEvent)
            SetEvent(g_stopEvent);

        HINTERNET socket = g_activeWebSocket.exchange(nullptr);

        if (socket)
            WinHttpCloseHandle(socket);

        Shell_NotifyIconW(NIM_DELETE, &g_trayData);

        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterWindowClasses(HINSTANCE instance) {
    WNDCLASSEXW overlay{};
    overlay.cbSize = sizeof(overlay);
    overlay.lpfnWndProc = OverlayWindowProc;
    overlay.hInstance = instance;
    overlay.lpszClassName = kOverlayClass;
    overlay.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassExW(&overlay))
        return false;

    WNDCLASSEXW main{};
    main.cbSize = sizeof(main);
    main.lpfnWndProc = MainWindowProc;
    main.hInstance = instance;
    main.lpszClassName = kMainClass;
    main.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    main.hIconSm = main.hIcon;
    main.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    return RegisterClassExW(&main) != 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {

    HANDLE mutex = CreateMutexW(
        nullptr, TRUE, L"Local\\ReplayHUD.SingleInstance"
    );

    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex)
            CloseHandle(mutex);

        return 0;
    }

    LoadSettings();

    if (!RegisterWindowClasses(instance)) {
        CloseHandle(mutex);
        return 1;
    }

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    g_mainWindow = CreateWindowExW(
        0,
        kMainClass,
        L"ReplayHUD",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        200,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!g_mainWindow) {
        if (g_stopEvent)
            CloseHandle(g_stopEvent);

        CloseHandle(mutex);
        return 1;
    }

    g_overlayWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kOverlayClass,
        L"ReplayHUD notification",
        WS_POPUP,
        0,
        0,
        kOverlayWidth,
        kOverlayHeight,
        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (g_overlayWindow) {
        SetLayeredWindowAttributes(
            g_overlayWindow, 0, 246, LWA_ALPHA
        );

        HRGN region = CreateRoundRectRgn(
            0, 0,
            kOverlayWidth + 1,
            kOverlayHeight + 1,
            18, 18
        );

        if (region)
            SetWindowRgn(g_overlayWindow, region, TRUE);
    }

    AddTrayIcon(g_mainWindow);

    ShowWindow(g_mainWindow, SW_HIDE);

    g_worker = std::thread(ObsWorker);

    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_worker.joinable())
        g_worker.join();

    if (g_stopEvent)
        CloseHandle(g_stopEvent);

    CloseHandle(mutex);

    return 0;
}
