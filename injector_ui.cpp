#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {
constexpr wchar_t kWindowClass[] = L"BadlionUnlockInjectorWindow";
constexpr UINT kInjectComplete = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;

constexpr COLORREF kBackground = RGB(16, 18, 23);
constexpr COLORREF kSurface = RGB(25, 28, 35);
constexpr COLORREF kSurfaceRaised = RGB(31, 35, 43);
constexpr COLORREF kBorder = RGB(52, 58, 69);
constexpr COLORREF kText = RGB(241, 244, 248);
constexpr COLORREF kMuted = RGB(153, 161, 174);
constexpr COLORREF kAccent = RGB(29, 184, 117);
constexpr COLORREF kAccentHover = RGB(36, 202, 130);
constexpr COLORREF kWarning = RGB(238, 178, 67);
constexpr COLORREF kDanger = RGB(231, 91, 91);

enum class UiState { NoTarget, Ready, AlreadyLoaded, Working, Success, Error };
enum class HitTarget { None, Refresh, Inject, OpenLog };

struct AppState {
    HWND window = nullptr;
    HFONT titleFont = nullptr;
    HFONT headingFont = nullptr;
    HFONT bodyFont = nullptr;
    HFONT smallFont = nullptr;
    HFONT buttonFont = nullptr;
    DWORD pid = 0;
    std::wstring processTitle;
    std::wstring processPath;
    std::wstring dllPath;
    std::wstring logPath;
    std::wstring detail;
    UiState state = UiState::NoTarget;
    HitTarget hover = HitTarget::None;
    bool busy = false;
};

struct InjectJob {
    HWND window;
    DWORD pid;
    std::wstring dllPath;
};

struct InjectResult {
    bool success = false;
    bool alreadyLoaded = false;
    DWORD error = ERROR_SUCCESS;
    std::wstring detail;
};

bool readResourceBytes(int id, const void*& data, DWORD& size) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) return false;
    data = LockResource(loaded);
    size = SizeofResource(module, resource);
    return data != nullptr && size != 0;
}

uint64_t hashBytes(const void* data, DWORD size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = 1469598103934665603ULL;
    for (DWORD i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool writeBytes(const std::wstring& path, const void* data, DWORD size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(static_cast<const char*>(data), size);
    return output.good();
}

std::wstring materializeEmbeddedPayload() {
    const void* dllData = nullptr;
    DWORD dllSize = 0;
    const void* classesData = nullptr;
    DWORD classesSize = 0;
    if (!readResourceBytes(IDR_AGENT_DLL, dllData, dllSize) ||
        !readResourceBytes(IDR_TARGET_CLASSES, classesData, classesSize)) return {};

    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return {};
    std::wstring directory = std::wstring(temp) + L"BadlionUnlockInjector";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};

    std::wostringstream suffix;
    suffix << std::hex << std::setfill(L'0') << std::setw(16) << hashBytes(dllData, dllSize);
    std::wstring dllPath = directory + L"\\blc_unlock_agent_" + suffix.str() + L".dll";
    std::wstring classesPath = directory + L"\\target-classes.txt";

    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES && !writeBytes(dllPath, dllData, dllSize)) return {};
    if (!writeBytes(classesPath, classesData, classesSize)) return {};
    return dllPath;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return value;
}

bool containsInsensitive(const std::wstring& value, const std::wstring& needle) {
    return lower(value).find(lower(needle)) != std::wstring::npos;
}

std::wstring processPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<wchar_t> buffer(32768);
    DWORD length = static_cast<DWORD>(buffer.size());
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, buffer.data(), &length)) result.assign(buffer.data(), length);
    CloseHandle(process);
    return result;
}

std::wstring badlionWindowTitle(DWORD pid) {
    struct Context { DWORD pid; std::wstring title; } context{pid, {}};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* context = reinterpret_cast<Context*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != context->pid || !IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[512]{};
        GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        const std::wstring value = title;
        if (containsInsensitive(value, L"Badlion Minecraft Client") && containsInsensitive(value, L"1.8.9")) {
            context->title = value;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.title;
}

DWORD findTarget(std::wstring& title, std::wstring& path) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"javaw.exe") != 0 && _wcsicmp(entry.szExeFile, L"java.exe") != 0) continue;
            std::wstring candidatePath = processPath(entry.th32ProcessID);
            if (!containsInsensitive(candidatePath, L"Badlion Client\\Data\\jdk-") ||
                !containsInsensitive(candidatePath, L"\\bin\\java")) continue;
            std::wstring candidateTitle = badlionWindowTitle(entry.th32ProcessID);
            if (candidateTitle.empty()) continue;
            result = entry.th32ProcessID;
            title = std::move(candidateTitle);
            path = std::move(candidatePath);
            break;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

bool hasAgentLoaded(DWORD pid, const std::wstring& agentPath) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return false;
    std::vector<HMODULE> modules(1024);
    DWORD bytes = 0;
    bool found = false;
    if (EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytes, LIST_MODULES_ALL)) {
        const std::wstring expected = lower(agentPath.substr(agentPath.find_last_of(L"\\/") + 1));
        const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), static_cast<DWORD>(modules.size()));
        for (DWORD i = 0; i < count; ++i) {
            wchar_t name[MAX_PATH]{};
            if (GetModuleBaseNameW(process, modules[i], name, MAX_PATH) > 0 && lower(name) == expected) {
                found = true;
                break;
            }
        }
    }
    CloseHandle(process);
    return found;
}

std::wstring win32Error(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"未知错误";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

InjectResult injectDll(DWORD pid, const std::wstring& dllPath, DWORD timeoutMs) {
    InjectResult result;
    if (hasAgentLoaded(pid, dllPath)) {
        result.success = true;
        result.alreadyLoaded = true;
        result.detail = L"当前游戏进程已经完成注入，无需重复操作。";
        return result;
    }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        result.error = GetLastError();
        result.detail = L"无法打开目标进程：" + win32Error(result.error);
        return result;
    }
    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        result.error = GetLastError();
        result.detail = L"无法分配远程内存：" + win32Error(result.error);
        CloseHandle(process);
        return result;
    }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written) || written != bytes) {
        result.error = GetLastError();
        result.detail = L"无法写入 DLL 路径：" + win32Error(result.error);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibrary = kernel32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW")) : nullptr;
    if (!loadLibrary) {
        result.error = ERROR_PROC_NOT_FOUND;
        result.detail = L"无法定位 LoadLibraryW。";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
    if (!thread) {
        result.error = GetLastError();
        result.detail = L"无法创建注入线程：" + win32Error(result.error);
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        CloseHandle(process);
        return result;
    }
    const DWORD wait = WaitForSingleObject(thread, timeoutMs);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (wait != WAIT_OBJECT_0 || module == 0) {
        result.error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
        result.detail = wait == WAIT_TIMEOUT ? L"注入等待超时，请确认游戏仍在运行。" : L"目标进程拒绝加载 DLL。";
        return result;
    }
    result.success = true;
    result.detail = L"DLL 已加载；本地饰品选择会自动保存并在下次注入时恢复。";
    return result;
}

RECT makeRect(int left, int top, int right, int bottom) { return RECT{left, top, right, bottom}; }

bool pointIn(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top && point.y < rect.bottom;
}

RECT refreshRect() { return makeRect(468, 112, 570, 148); }
RECT injectRect() { return makeRect(36, 337, 570, 393); }
RECT logRect() { return makeRect(36, 414, 132, 444); }

void fillRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius = 7) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void strokeRoundRect(HDC dc, const RECT& rect, COLORREF color, int radius = 7) {
    HBRUSH brush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
}

void drawText(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT format) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, format);
    SelectObject(dc, old);
}

void drawButton(HDC dc, const RECT& rect, const std::wstring& label, HFONT font,
                bool primary, bool hover, bool disabled = false) {
    COLORREF fill = primary ? kAccent : kSurfaceRaised;
    COLORREF border = primary ? kAccent : kBorder;
    COLORREF text = kText;
    if (hover && !disabled) fill = primary ? kAccentHover : RGB(39, 44, 54);
    if (disabled) {
        fill = RGB(42, 46, 53);
        border = fill;
        text = RGB(112, 119, 130);
    }
    fillRoundRect(dc, rect, fill);
    if (!primary && !disabled) strokeRoundRect(dc, rect, border);
    RECT textRect = rect;
    drawText(dc, label, textRect, font, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

COLORREF stateColor(UiState state) {
    if (state == UiState::Ready || state == UiState::AlreadyLoaded || state == UiState::Success) return kAccent;
    if (state == UiState::Working) return kWarning;
    if (state == UiState::Error) return kDanger;
    return kMuted;
}

std::wstring stateLabel(UiState state) {
    switch (state) {
        case UiState::Ready: return L"目标已就绪";
        case UiState::AlreadyLoaded: return L"已经注入";
        case UiState::Working: return L"正在注入";
        case UiState::Success: return L"注入成功";
        case UiState::Error: return L"注入失败";
        default: return L"等待游戏";
    }
}

void paint(AppState* app) {
    PAINTSTRUCT ps{};
    HDC target = BeginPaint(app->window, &ps);
    RECT client{};
    GetClientRect(app->window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HBRUSH background = CreateSolidBrush(kBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    drawText(dc, L"Badlion 本地饰品注入器", makeRect(36, 28, 570, 58), app->titleFont, kText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, std::wstring(L"ColdEternity Team  ·  v") + APP_VERSION_W + L"  ·  适用于 Badlion Client 1.8.9，仅当前客户端显示", makeRect(36, 62, 570, 84),
              app->smallFont, kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const RECT targetCard = makeRect(36, 100, 570, 220);
    fillRoundRect(dc, targetCard, kSurface);
    strokeRoundRect(dc, targetCard, kBorder);
    drawText(dc, L"游戏进程", makeRect(54, 116, 300, 140), app->headingFont, kText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawButton(dc, refreshRect(), L"重新检测", app->smallFont, false, app->hover == HitTarget::Refresh, app->busy);

    HBRUSH dot = CreateSolidBrush(stateColor(app->state));
    HGDIOBJ oldBrush = SelectObject(dc, dot);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 55, 160, 67, 172);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(dot);
    drawText(dc, stateLabel(app->state), makeRect(77, 153, 240, 180), app->bodyFont, kText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    std::wstring pidText = app->pid ? L"PID " + std::to_wstring(app->pid) : L"未找到进程";
    drawText(dc, pidText, makeRect(250, 153, 548, 180), app->bodyFont, app->pid ? kMuted : kWarning,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    std::wstring targetDetail = app->processTitle.empty() ? L"未检测到符合版本的 Java 游戏窗口" : app->processTitle;
    drawText(dc, targetDetail, makeRect(54, 187, 548, 207), app->smallFont, kMuted,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    drawText(dc, L"注入组件", makeRect(36, 239, 220, 262), app->headingFont, kText,
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    fillRoundRect(dc, makeRect(36, 270, 440, 310), kSurface);
    strokeRoundRect(dc, makeRect(36, 270, 440, 310), kBorder);
    const bool payloadReady = !app->dllPath.empty() && GetFileAttributesW(app->dllPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    drawText(dc, L"内置 DLL 组件", makeRect(50, 270, 260, 310), app->bodyFont,
             payloadReady ? kText : kDanger, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, payloadReady ? L"已嵌入并就绪" : L"内嵌资源不可用", makeRect(260, 270, 424, 310), app->smallFont,
             payloadReady ? kAccent : kDanger, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, L"本地饰品选择：自动保存 / 自动恢复", makeRect(36, 311, 570, 332), app->smallFont, kMuted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    const bool injectDisabled = app->busy || !app->pid || !payloadReady;
    drawButton(dc, injectRect(), app->busy ? L"正在注入，请稍候..." : L"注入并解锁", app->buttonFont,
               true, app->hover == HitTarget::Inject, injectDisabled);
    drawText(dc, app->detail, makeRect(36, 397, 570, 418), app->smallFont,
             app->state == UiState::Error ? kDanger : kMuted,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    drawText(dc, L"打开日志", logRect(), app->smallFont,
             app->hover == HitTarget::OpenLog ? kText : kMuted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    drawText(dc, L"F5 刷新  |  Enter 注入", makeRect(320, 414, 570, 444), app->smallFont, kMuted,
             DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(app->window, &ps);
}

void refreshTarget(AppState* app, bool userInitiated) {
    if (app->busy) return;
    if (app->dllPath.empty()) {
        app->pid = 0;
        app->processTitle.clear();
        app->processPath.clear();
        app->state = UiState::Error;
        app->detail = L"内嵌组件不可用，请重新下载单文件程序。";
        InvalidateRect(app->window, nullptr, FALSE);
        return;
    }
    std::wstring title;
    std::wstring path;
    const DWORD pid = findTarget(title, path);
    app->pid = pid;
    app->processTitle = std::move(title);
    app->processPath = std::move(path);
    if (!pid) {
        app->state = UiState::NoTarget;
        app->detail = userInitiated ? L"未找到游戏，请等待 Badlion 1.8.9 完全启动。" : L"等待 Badlion 1.8.9 游戏进程...";
    } else if (hasAgentLoaded(pid, app->dllPath)) {
        app->state = UiState::AlreadyLoaded;
        app->detail = L"当前进程已经注入，可直接打开 Cosmetics。";
    } else {
        app->state = UiState::Ready;
        app->detail = L"目标与 DLL 均已就绪。";
    }
    InvalidateRect(app->window, nullptr, FALSE);
}

DWORD WINAPI injectWorker(void* parameter) {
    std::unique_ptr<InjectJob> job(static_cast<InjectJob*>(parameter));
    auto result = new InjectResult(injectDll(job->pid, job->dllPath, 15000));
    PostMessageW(job->window, kInjectComplete, 0, reinterpret_cast<LPARAM>(result));
    return 0;
}

void startInjection(AppState* app) {
    if (app->busy || !app->pid || app->dllPath.empty() || GetFileAttributesW(app->dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    app->busy = true;
    app->state = UiState::Working;
    app->detail = L"正在将本地饰品组件加载到 Badlion JVM...";
    InvalidateRect(app->window, nullptr, FALSE);
    auto* job = new InjectJob{app->window, app->pid, app->dllPath};
    HANDLE thread = CreateThread(nullptr, 0, injectWorker, job, 0, nullptr);
    if (!thread) {
        delete job;
        app->busy = false;
        app->state = UiState::Error;
        app->detail = L"无法启动后台注入任务：" + win32Error(GetLastError());
        InvalidateRect(app->window, nullptr, FALSE);
        return;
    }
    CloseHandle(thread);
}

HitTarget hitTest(POINT point) {
    if (pointIn(refreshRect(), point)) return HitTarget::Refresh;
    if (pointIn(injectRect(), point)) return HitTarget::Inject;
    if (pointIn(logRect(), point)) return HitTarget::OpenLog;
    return HitTarget::None;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            auto* created = new AppState();
            created->window = hwnd;
            created->titleFont = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                             DEFAULT_PITCH, L"Segoe UI");
            created->headingFont = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                               DEFAULT_PITCH, L"Microsoft YaHei UI");
            created->bodyFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                            DEFAULT_PITCH, L"Microsoft YaHei UI");
            created->smallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                             DEFAULT_PITCH, L"Microsoft YaHei UI");
            created->buttonFont = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                              DEFAULT_PITCH, L"Microsoft YaHei UI");
            created->dllPath = materializeEmbeddedPayload();
            if (!created->dllPath.empty()) {
                created->logPath = created->dllPath.substr(0, created->dllPath.find_last_of(L"\\/")) + L"\\blc_unlock_agent.log";
            } else {
                created->state = UiState::Error;
                created->detail = L"无法释放内嵌组件，请重新下载单文件程序。";
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
            SetTimer(hwnd, kRefreshTimer, 2000, nullptr);
            if (!created->dllPath.empty()) refreshTarget(created, false);
            else InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            if (app) paint(app);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            if (app) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                HitTarget next = hitTest(point);
                if (next != app->hover) {
                    app->hover = next;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (app && app->hover != HitTarget::None) {
                app->hover = HitTarget::None;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_SETCURSOR:
            if (app && app->hover != HitTarget::None) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
            if (app) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                switch (hitTest(point)) {
                    case HitTarget::Refresh: refreshTarget(app, true); break;
                    case HitTarget::Inject: startInjection(app); break;
                    case HitTarget::OpenLog: {
                        if (!app->logPath.empty()) ShellExecuteW(hwnd, L"open", app->logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                        break;
                    }
                    default: break;
                }
            }
            return 0;
        case WM_KEYDOWN:
            if (app && wParam == VK_F5) refreshTarget(app, true);
            else if (app && wParam == VK_RETURN) startInjection(app);
            return 0;
        case WM_TIMER:
            if (app && wParam == kRefreshTimer && !app->busy && app->state != UiState::Error) refreshTarget(app, false);
            return 0;
        case kInjectComplete:
            if (app) {
                std::unique_ptr<InjectResult> result(reinterpret_cast<InjectResult*>(lParam));
                app->busy = false;
                app->state = result->success ? (result->alreadyLoaded ? UiState::AlreadyLoaded : UiState::Success) : UiState::Error;
                app->detail = result->detail;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            if (app) {
                KillTimer(hwnd, kRefreshTimer);
                DeleteObject(app->titleFont);
                DeleteObject(app->headingFont);
                DeleteObject(app->bodyFont);
                DeleteObject(app->smallFont);
                DeleteObject(app->buttonFont);
                delete app;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
}

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int showCommand) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto setDpi = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    RECT desired{0, 0, 606, 488};
    AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);
    const int width = desired.right - desired.left;
    const int height = desired.bottom - desired.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    const std::wstring windowTitle = std::wstring(L"Badlion 本地饰品注入器 v") + APP_VERSION_W;
    HWND window = CreateWindowExW(0, kWindowClass, windowTitle.c_str(),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) return 2;
    ShowWindow(window, showCommand);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
