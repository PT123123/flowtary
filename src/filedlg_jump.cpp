// filedlg_jump.cpp
//
// 宿主侧实现（编译进 flowtary.exe）。负责：
//   - 通过 SetWinEventHook 跨进程检测系统标准文件对话框（#32770 + shell 视图子窗口）
//   - 跟踪“当前焦点对话框”（只对焦点对话框生效）
//   - 读取/写入功能开关（注册表）
//   - 通过 WM_COPYDATA 把目标路径发送给焦点对话框，由注入 DLL 在目标进程内完成跳转
//   - 辅助热键：把前台资源管理器当前目录同步到焦点对话框
//   - 内部日志（OutputDebugStringW 始终输出；若环境变量 FLOW_FDJ_LOG=1 同时写文件）
//
// 失败一律静默：不弹窗、不阻塞，仅记录日志。
//
#include "filedlg_jump.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <vector>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// ---------------- 日志 ----------------
static bool g_logFile = false;
static void Log(const WCHAR* fmt, ...) {
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringW(L"[FDJ] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
    if (g_logFile) {
        WCHAR path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
            lstrcatW(path, L"\\Flowtary\\filedlg_jump.log");
            HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w;
                std::wstring line = L"[FDJ] ";
                line += buf;
                line += L"\n";
                WriteFile(h, line.c_str(), (DWORD)(line.size() * sizeof(WCHAR)), &w, nullptr);
                CloseHandle(h);
            }
        }
    }
}

// ---------------- 对话框识别（宿主侧，可跨进程）----------------
// 判定某 HWND 是否为系统标准文件对话框：
//   1) 顶层窗口、窗口类为 #32770
//   2) 包含 shell 视图子窗口（旧版 SHELLDLL_DefView，或 Vista+ 的
//      NamespaceTreeControl / DirectUIHWND 等）。纯自绘第三方对话框不含这些，直接跳过。
struct DlgProbe { BOOL hasDefView; BOOL hasTree; BOOL hasDirectUI; BOOL hasEdit; BOOL hasToolbar; };
static BOOL CALLBACK ProbeChild(HWND hwnd, LPARAM lp) {
    DlgProbe* p = (DlgProbe*)lp;
    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, _countof(cls)) == 0) return TRUE;
    if (lstrcmpiW(cls, L"SHELLDLL_DefView") == 0) p->hasDefView = TRUE;
    else if (lstrcmpiW(cls, L"NamespaceTreeControl") == 0) p->hasTree = TRUE;
    else if (lstrcmpiW(cls, L"DirectUIHWND") == 0) p->hasDirectUI = TRUE;
    else if (lstrcmpiW(cls, L"Edit") == 0) p->hasEdit = TRUE;
    else if (lstrcmpiW(cls, L"ToolbarWindow32") == 0) p->hasToolbar = TRUE;
    return TRUE;
}
static BOOL IsFileDialogWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FALSE;
    // 必须是顶层窗口（文件对话框是顶层窗口）
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;
    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, _countof(cls)) == 0) return FALSE;
    if (lstrcmpiW(cls, L"#32770") != 0) return FALSE;
    DlgProbe p{};
    EnumChildWindows(hwnd, ProbeChild, (LPARAM)&p);
    // 旧版：SHELLDLL_DefView 一定出现；Vista+：NamespaceTreeControl 或地址栏 DirectUI
    if (p.hasDefView) return TRUE;
    if (p.hasTree) return TRUE;
    // 退一步：同时具备地址栏编辑框与工具栏，也视为标准对话框（防漏判）
    if (p.hasEdit && p.hasToolbar) return TRUE;
    if (p.hasDirectUI && p.hasToolbar) return TRUE;
    return FALSE;
}

// ---------------- 状态 ----------------
static HWND        g_hostWnd = nullptr;
static HWND        g_lastActive = nullptr;   // 最近被聚焦的文件对话框 = 跳转目标（不要求此刻前台）
static HMODULE     g_hMod64 = nullptr;
static HHOOK       g_hhk64 = nullptr;        // 64 位 CBT 钩子句柄
static HHOOK       g_hhk64Msg = nullptr;     // 64 位 GetMsg 钩子句柄
static PROCESS_INFORMATION g_agent32{};      // 32 位钩子安装助手
static HWINEVENTHOOK g_weCreate = nullptr, g_weDestroy = nullptr, g_weForeground = nullptr;
static CRITICAL_SECTION g_cs;
static std::vector<HWND> g_dialogs;

static BOOL IsTracked(HWND h) {
    for (HWND x : g_dialogs) if (x == h) return TRUE;
    return FALSE;
}
static void AddTracked(HWND h) {
    EnterCriticalSection(&g_cs);
    if (!IsTracked(h)) g_dialogs.push_back(h);
    LeaveCriticalSection(&g_cs);
}
static void RemoveTracked(HWND h) {
    EnterCriticalSection(&g_cs);
    for (size_t i = 0; i < g_dialogs.size(); ++i) {
        if (g_dialogs[i] == h) { g_dialogs.erase(g_dialogs.begin() + i); break; }
    }
    if (g_lastActive == h) g_lastActive = nullptr;
    LeaveCriticalSection(&g_cs);
}

// 选择“跳转目标对话框”：
//   优先用最近被聚焦的文件对话框（即用户唤出 Flowtary 前正在用的那个），
//   否则回退到任意一个仍存在的已跟踪对话框。
// 关键：不再要求对话框“此刻处于前台”——因为用户点击 Flowtary 里的文件夹时，
//       Flowtary 自己会变成前台窗口，对话框只是被覆盖，但依旧是有效的跳转目标。
//       （这正是类 Listary 的行为：搜索面板抢走焦点，但跳转仍命中背后的对话框。）
static HWND TargetDialog() {
    EnterCriticalSection(&g_cs);
    HWND cand = nullptr;
    if (g_lastActive && IsWindow(g_lastActive) && IsTracked(g_lastActive))
        cand = g_lastActive;
    else {
        for (auto it = g_dialogs.rbegin(); it != g_dialogs.rend(); ++it) {
            if (IsWindow(*it)) { cand = *it; break; }
        }
    }
    LeaveCriticalSection(&g_cs);
    return cand;
}

// ---------------- WinEvent 回调（宿主线程内执行）----------------
static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG, LONG, DWORD, DWORD) {
    switch (event) {
        case EVENT_OBJECT_CREATE:
            if (IsFileDialogWindow(hwnd)) {
                if (!IsTracked(hwnd)) { AddTracked(hwnd); Log(L"detected dialog hwnd=%p", (void*)hwnd); }
            }
            break;
        case EVENT_OBJECT_DESTROY:
            if (IsTracked(hwnd)) { RemoveTracked(hwnd); Log(L"dialog destroyed hwnd=%p", (void*)hwnd); }
            break;
        case EVENT_SYSTEM_FOREGROUND:
            // 对话框首次创建时其子窗口（shell 视图）可能尚未就绪，识别可能在此时失败；
            // 当它成为前台窗口时子窗口已存在，这里补一次识别（也恰好是用户想跳转的时刻）。
            // 同时记下“最近被聚焦的文件对话框”——它即为后续跳转的目标，即使之后 Flowtary
            // 抢走了前台，目标仍有效（见 TargetDialog）。
            if (IsFileDialogWindow(hwnd)) {
                if (!IsTracked(hwnd)) { AddTracked(hwnd); Log(L"detected dialog (fg) hwnd=%p", (void*)hwnd); }
                g_lastActive = hwnd;
                Log(L"dialog foreground hwnd=%p", (void*)hwnd);
            }
            break;
    }
}

// ---------------- CBT 钩子 DLL 安装（64 位由本进程直接安装）----------------
// 64 位 CBT 钩子直接在本进程安装（LoadLibrary 64 位 DLL）。
// 32 位钩子由 agent32.exe 负责（64 位进程无法加载 32 位 DLL）。
typedef LRESULT (CALLBACK* CBTProcType)(int, WPARAM, LPARAM);
static BOOL InstallHook64() {
    WCHAR dir[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, dir, _countof(dir))) return FALSE;
    // 取本 exe 所在目录
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (slash) lstrcpyW(slash + 1, L"filedlg_hook64.dll");
    g_hMod64 = LoadLibraryW(dir);
    if (!g_hMod64) { Log(L"LoadLibrary filedlg_hook64.dll failed %lu", GetLastError()); return FALSE; }
    CBTProcType cbtProc = (CBTProcType)GetProcAddress(g_hMod64, "CbtProc");
    CBTProcType msgProc = (CBTProcType)GetProcAddress(g_hMod64, "GetMsgProc");
    if (!cbtProc || !msgProc) { Log(L"GetProcAddress CbtProc/GetMsgProc failed"); return FALSE; }
    g_hhk64 = SetWindowsHookExW(WH_CBT, (HOOKPROC)cbtProc, g_hMod64, 0);
    g_hhk64Msg = SetWindowsHookExW(WH_GETMESSAGE, (HOOKPROC)msgProc, g_hMod64, 0);
    if (!g_hhk64 || !g_hhk64Msg) { Log(L"SetWindowsHookEx failed %lu", GetLastError()); return FALSE; }
    // 把钩子句柄回传给 DLL，使其 CallNextHookEx 能正确链式调用
    typedef void (*SetHookFn)(HHOOK);
    if (SetHookFn f = (SetHookFn)GetProcAddress(g_hMod64, "CbtSetHook")) f(g_hhk64);
    if (SetHookFn f = (SetHookFn)GetProcAddress(g_hMod64, "MsgSetHook")) f(g_hhk64Msg);
    Log(L"64-bit hooks installed (cbt=%p msg=%p)", (void*)g_hhk64, (void*)g_hhk64Msg);
    return TRUE;
}

static BOOL StartAgent32() {
    WCHAR dir[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, dir, _countof(dir))) return FALSE;
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) return FALSE;
    lstrcpyW(slash + 1, L"filedlg_agent32.exe");
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    if (!CreateProcessW(dir, nullptr, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &g_agent32)) {
        Log(L"CreateProcess filedlg_agent32.exe failed %lu", GetLastError());
        return FALSE;
    }
    Log(L"32-bit agent started pid=%lu", g_agent32.dwProcessId);
    return TRUE;
}

// ---------------- 公共 API ----------------
BOOL fdj_init(HWND hostWnd) {
    g_hostWnd = hostWnd;
    g_logFile = (GetEnvironmentVariableW(L"FLOW_FDJ_LOG", nullptr, 0) > 0);
    InitializeCriticalSection(&g_cs);

    g_weCreate = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_DESTROY, nullptr,
                                 WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_weForeground = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                     WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_weCreate || !g_weForeground)
        Log(L"SetWinEventHook partial failure (create=%p fg=%p)", (void*)g_weCreate, (void*)g_weForeground);

    InstallHook64();
    StartAgent32();
    Log(L"fdj_init done (enabled=%d)", fdj_enabled());
    return TRUE;
}

void fdj_uninit(void) {
    if (g_weCreate) { UnhookWinEvent(g_weCreate); g_weCreate = nullptr; }
    if (g_weForeground) { UnhookWinEvent(g_weForeground); g_weForeground = nullptr; }
    if (g_hhk64) { UnhookWindowsHookEx(g_hhk64); g_hhk64 = nullptr; }
    if (g_hhk64Msg) { UnhookWindowsHookEx(g_hhk64Msg); g_hhk64Msg = nullptr; }
    if (g_hMod64) { FreeLibrary(g_hMod64); g_hMod64 = nullptr; }
    if (g_agent32.hProcess) {
        // 通知 32 位助手退出（它有一个隐藏窗口，发 WM_CLOSE）
        if (HWND w = FindWindowW(FDJ_AGENT32_CLASS, nullptr)) PostMessageW(w, WM_CLOSE, 0, 0);
        // 兜底：若 3 秒内未退出则强杀
        if (WaitForSingleObject(g_agent32.hProcess, 3000) == WAIT_TIMEOUT)
            TerminateProcess(g_agent32.hProcess, 0);
        CloseHandle(g_agent32.hProcess); CloseHandle(g_agent32.hThread);
        g_agent32 = PROCESS_INFORMATION{};
    }
    EnterCriticalSection(&g_cs); g_dialogs.clear(); LeaveCriticalSection(&g_cs);
    DeleteCriticalSection(&g_cs);
    Log(L"fdj_uninit done");
}

BOOL fdj_enabled(void) {
    DWORD v = 1, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, FDJ_REG_KEY, FDJ_REG_VAL, RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        return v ? TRUE : FALSE;
    return TRUE;  // 默认开
}
void fdj_set_enabled(BOOL on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, FDJ_REG_KEY, FDJ_REG_VAL, REG_DWORD, &v, sizeof(v));
    Log(L"set_enabled %d", on);
}

BOOL fdj_dialog_open(void) {
    // 只要有“已识别且仍打开”的文件对话框，即认为跳转条件成立（不要求它此刻前台）。
    return TargetDialog() != nullptr;
}

BOOL fdj_jump_to_folder(const WCHAR* path) {
    if (!fdj_enabled()) return FALSE;
    HWND dlg = TargetDialog();
    if (!dlg) { Log(L"jump: no target dialog"); return FALSE; }
    if (!PathFileExistsW(path)) { Log(L"jump: path not exist %s", path); return FALSE; }

    COPYDATASTRUCT cds{};
    cds.dwData = FDJ_CD_NAVIGATE;
    cds.cbData = (DWORD)((wcslen(path) + 1) * sizeof(WCHAR));
    cds.lpData = (void*)path;
    // 同步 SendMessage：WM_COPYDATA 会由系统把 path 拷贝进目标进程，
    // 交付给 DLL 子类过程，在对话框所属进程内完成跳转。
    // （对话框此时可能是被覆盖的非前台窗口，但窗口消息仍能正常投递并被其线程处理。）
    LRESULT r = SendMessageW(dlg, WM_COPYDATA, (WPARAM)g_hostWnd, (LPARAM)&cds);
    Log(L"jump: dlg=%p path=%s ret=%ld", (void*)dlg, path, r);
    if (r) {
        // 让对话框重新回到前台，用户立即看到跳转结果（Flowtary 自身即将隐藏）
        SetForegroundWindow(dlg);
    }
    return r ? TRUE : FALSE;
}

// 取前台资源管理器当前目录（file:// 形式经 IWebBrowser2::get_LocationURL 取得）
static std::wstring ExplorerLocationURL(HWND hwndExplorer) {
    std::wstring out;
    IShellWindows* pSW = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows, (void**)&pSW)))
        return out;
    VARIANT var;
    long count = 0;
    pSW->get_Count(&count);
    for (long i = 0; i < count; ++i) {
        VariantInit(&var); var.vt = VT_I4; var.lVal = i;
        IDispatch* pDisp = nullptr;
        if (FAILED(pSW->Item(var, &pDisp)) || !pDisp) continue;
        IWebBrowser2* pWB = nullptr;
        if (SUCCEEDED(pDisp->QueryInterface(IID_IWebBrowser2, (void**)&pWB))) {
            SHANDLE_PTR hwL = 0;
            pWB->get_HWND(&hwL);   // SHANDLE_PTR 为指针宽度的句柄值，可直接转 HWND
            HWND hw = (HWND)hwL;
            if (hwndExplorer == nullptr || hw == hwndExplorer ||
                GetAncestor(hwndExplorer, GA_ROOT) == hw) {
                BSTR url = nullptr;
                if (SUCCEEDED(pWB->get_LocationURL(&url)) && url) {
                    out = url;
                    SysFreeString(url);
                }
            }
            pWB->Release();
        }
        pDisp->Release();
        if (!out.empty()) break;
    }
    pSW->Release();
    return out;
}
static std::wstring FileURLToPath(const std::wstring& url) {
    // 形如 file:///C:/Users/... 或 file://server/share
    if (url.rfind(L"file:///", 0) == 0) return url.substr(8);
    if (url.rfind(L"file://", 0) == 0) return url.substr(7);
    return url;
}

BOOL fdj_sync_from_explorer(void) {
    if (!fdj_enabled()) return FALSE;
    HWND dlg = TargetDialog();
    if (!dlg) { Log(L"sync: no target dialog"); return FALSE; }
    HWND fg = GetForegroundWindow();
    // 优先用真正的资源管理器前台窗口（可能与对话框不同，用户先选好目录再切回对话框）
    HWND exp = fg;
    std::wstring url = ExplorerLocationURL(exp);
    if (url.empty()) {
        // 退一步：枚举所有资源管理器，取任意一个
        url = ExplorerLocationURL(nullptr);
    }
    if (url.empty()) { Log(L"sync: no explorer location"); return FALSE; }
    std::wstring path = FileURLToPath(url);
    Log(L"sync: explorer=%s", path.c_str());
    return fdj_jump_to_folder(path.c_str());
}
