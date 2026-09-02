#define UNICODE
#define _UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cwctype>

namespace {
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}
bool containsInsensitive(const std::wstring& value, const std::wstring& needle) {
    return lower(value).find(lower(needle)) != std::wstring::npos;
}

std::wstring normalizedPath(std::wstring value) {
    std::replace(value.begin(), value.end(), L'/', L'\\');
    return lower(std::move(value));
}

bool isJavaImage(const std::wstring& path) {
    const std::wstring value = normalizedPath(path);
    const size_t slash = value.find_last_of(L'\\');
    const std::wstring name = slash == std::wstring::npos ? value : value.substr(slash + 1);
    return name == L"java.exe" || name == L"javaw.exe";
}

bool pathSuggestsBadlion(const std::wstring& path) {
    const std::wstring value = normalizedPath(path);
    return containsInsensitive(value, L"badlion client") ||
           containsInsensitive(value, L"badlionclient") ||
           containsInsensitive(value, L"lunar client") ||
           containsInsensitive(value, L"lunarclient");
}

bool titleHas18(const std::wstring& title) {
    return containsInsensitive(title, L"1.8.9") ||
           containsInsensitive(title, L"1_8_9") ||
           containsInsensitive(title, L"1-8-9");
}

bool titleSuggestsBadlion(const std::wstring& title) {
    return containsInsensitive(title, L"badlion");
}

bool gameWindowClass(const std::wstring& className) {
    return containsInsensitive(className, L"lwjgl") ||
           containsInsensitive(className, L"glfw") ||
           containsInsensitive(className, L"minecraft");
}
std::wstring processPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<wchar_t> buffer(32768); DWORD length = static_cast<DWORD>(buffer.size());
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) result.assign(buffer.data(), length);
    CloseHandle(process); return result;
}
bool hasAgentLoaded(DWORD pid, const std::wstring& agentPath) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return false;
    HMODULE modules[1024]{}; DWORD bytes = 0; bool found = false;
    if (EnumProcessModulesEx(process, modules, sizeof(modules), &bytes, LIST_MODULES_ALL)) {
        const std::wstring expected = lower(agentPath.substr(agentPath.find_last_of(L"\\/") + 1));
        for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i) {
            wchar_t name[MAX_PATH]{};
            if (GetModuleBaseNameW(process, modules[i], name, MAX_PATH) > 0 && lower(name) == expected) { found = true; break; }
        }
    }
    CloseHandle(process); return found;
}
bool hasBadlionWindow(DWORD pid, const std::wstring& path) {
    struct Context { DWORD pid; bool pathSignal; bool found; } context{pid, pathSuggestsBadlion(path), false};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* context = reinterpret_cast<Context*>(param); DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != context->pid || (!IsWindowVisible(hwnd) && !IsIconic(hwnd))) return TRUE;
        wchar_t title[1024]{}; GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        wchar_t className[128]{}; GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        const std::wstring titleValue = title;
        const std::wstring classValue = className;
        const bool launcher = containsInsensitive(titleValue, L"launcher") || containsInsensitive(titleValue, L"updater");
        if (!launcher && ((titleSuggestsBadlion(titleValue) && (titleHas18(titleValue) || context->pathSignal)) ||
                          (titleHas18(titleValue) && (context->pathSignal || gameWindowClass(classValue))) ||
                          (context->pathSignal && gameWindowClass(classValue)))) {
            context->found = true; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.found;
}
DWORD findTarget() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)}; DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) do {
        if (_wcsicmp(entry.szExeFile, L"javaw.exe") != 0 && _wcsicmp(entry.szExeFile, L"java.exe") != 0) continue;
        const std::wstring path = processPath(entry.th32ProcessID);
        // Process image queries may be denied for elevated JVMs. The snapshot
        // already confirmed java/javaw.exe, so rely on the window evidence when
        // the full path is unavailable.
        if ((path.empty() || isJavaImage(path)) && hasBadlionWindow(entry.th32ProcessID, path)) {
            result = entry.th32ProcessID; break;
        }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return result;
}
bool inject(DWORD pid, const std::wstring& dllPath, DWORD timeoutMs) {
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) { std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n"; return false; }
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n"; CloseHandle(process); return false; }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n"; VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return false;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW")) : nullptr;
    if (!loadLibrary) {
        std::wcerr << L"LoadLibraryW address lookup failed\n";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) { std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n"; VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process); return false; }
    const DWORD waitResult = WaitForSingleObject(thread, timeoutMs); DWORD module = 0; GetExitCodeThread(thread, &module);
    CloseHandle(thread); VirtualFreeEx(process, remote, 0, MEM_RELEASE); CloseHandle(process);
    if (waitResult != WAIT_OBJECT_0 || module == 0) { std::wcerr << L"Remote LoadLibrary failed, wait=" << waitResult << L" exit=" << module << L"\n"; return false; }
    return true;
}
void usage() {
    std::wcout << L"BadlionUnlockInjector.exe [--pid PID] [--dll path] [--timeout-ms N]\n";
}
}
int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0, timeoutMs = 15000; std::wstring dllPath;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--pid" && i + 1 < argc) pid = wcstoul(argv[++i], nullptr, 10);
        else if (arg == L"--dll" && i + 1 < argc) dllPath = argv[++i];
        else if (arg == L"--timeout-ms" && i + 1 < argc) timeoutMs = wcstoul(argv[++i], nullptr, 10);
        else if (arg == L"--help" || arg == L"-h") { usage(); return 0; }
        else { usage(); return 2; }
    }
    if (dllPath.empty()) { wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH); std::wstring path = self; dllPath = path.substr(0, path.find_last_of(L"\\/")) + L"\\blc_unlock_agent.dll"; }
    DWORD attributes = GetFileAttributesW(dllPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) { std::wcerr << L"DLL not found: " << dllPath << L"\n"; return 3; }
    if (!pid) { pid = findTarget(); if (!pid) { std::wcerr << L"No active Badlion 1.8.9 JVM found.\n"; return 4; } }
    const std::wstring path = processPath(pid); std::wcout << L"Target PID " << pid << L"\nPath: " << path << L"\n";
    if (!path.empty() && !isJavaImage(path)) { std::wcerr << L"PID is not a Java process.\n"; return 5; }
    if (hasAgentLoaded(pid, dllPath)) { std::wcout << L"Agent already loaded.\n"; return 0; }
    std::wcout << L"Loading " << dllPath << L" ...\n";
    if (!inject(pid, dllPath, timeoutMs)) return 6;
    std::wcout << L"DLL loaded successfully.\n"; return 0;
}
