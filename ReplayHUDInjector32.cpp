#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

namespace {

bool Inject(DWORD processId, const std::wstring& dllPath) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
    if (!process) return false;

    BOOL targetWow64 = FALSE;
    if (!IsWow64Process(process, &targetWow64) || !targetWow64) {
        CloseHandle(process);
        return false;
    }

    const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(process, nullptr, bytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remotePath) {
        CloseHandle(process);
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remotePath, dllPath.c_str(), bytes, &written) ||
        written != bytes) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        kernel ? GetProcAddress(kernel, "LoadLibraryW") : nullptr);
    if (!loadLibrary) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remotePath, 0, nullptr);
    if (!thread) {
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    const DWORD wait = WaitForSingleObject(thread, INFINITE);
    DWORD moduleResult = 0;
    const bool loaded = wait == WAIT_OBJECT_0 &&
        GetExitCodeThread(thread, &moduleResult) != FALSE && moduleResult != 0;
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    CloseHandle(process);
    return loaded;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;

    DWORD processId = 0;
    std::wstring dllPath;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0) {
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(argv[++i], &end, 10);
            if (end && *end == L'\0' && parsed > 0 && parsed <= MAXDWORD)
                processId = static_cast<DWORD>(parsed);
        } else if (_wcsicmp(argv[i], L"--dll") == 0) {
            dllPath = argv[++i];
        }
    }
    LocalFree(argv);

    if (!processId || dllPath.empty() || GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 2;
    return Inject(processId, dllPath) ? 0 : 1;
}
