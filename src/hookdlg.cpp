// hookdlg.cpp
//
// 注入 DLL（按位数分别编译为 filedlg_hook32.dll / filedlg_hook64.dll）。
//
// 由宿主（或 32 位 agent32.exe）通过 SetWindowsHookEx(WH_CBT, CbtProc, ...) 安装为全局钩子。
// 当任意进程创建/激活一个系统标准文件对话框时，本 DLL 在该“对话框所属进程内”子类化对话框，
// 之后宿主通过 WM_COPYDATA(FDJ_CD_NAVIGATE) 把目标路径发到对话框窗口，由本 DLL 的子类过程
// 在对话框自己的进程里调用原生 COM 接口完成原地跳转：
//   - Vista+ 通用项对话框：IFileDialog::SetFolder（由对话框 GWLP_USERDATA 取得 IFileDialog）
//   - 旧版 GetOpenFileName / 回退：IShellBrowser::BrowseObject
//
// 全程不模拟键盘、不粘贴剪贴板，仅调用系统对话框公开接口。
// 纯自绘第三方对话框（无 shell 视图子窗口）在识别阶段即跳过，不做任何 hack。

#include "filedlg_jump.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <map>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

static HHOOK g_hhk = nullptr;        // WH_CBT 钩子句柄（捕获安装后创建/激活的对话框）
static HHOOK g_hhkMsg = nullptr;     // WH_GETMESSAGE 钩子句柄（把 DLL 注入到“已存在”的对话框进程）
static CRITICAL_SECTION g_cs;
static std::map<HWND, WNDPROC> g_orig;   // 每进程一份（DLL 按进程加载）

static void Log(const WCHAR* fmt, ...) {
    WCHAR buf[512];
    va_list ap; va_start(ap, fmt);
    wvsprintfW(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringW(L"[FDJ-DLL] ");
    OutputDebugStringW(buf);
    OutputDebugStringW(L"\n");
}

// ---------------- 对话框识别（进程内，逻辑与宿主一致）----------------
struct DlgProbe { BOOL def; BOOL tree; BOOL dui; BOOL edit; BOOL tb; };
static BOOL CALLBACK ProbeChild(HWND hwnd, LPARAM lp) {
    DlgProbe* p = (DlgProbe*)lp;
    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, _countof(cls)) == 0) return TRUE;
    if (lstrcmpiW(cls, L"SHELLDLL_DefView") == 0) p->def = TRUE;
    else if (lstrcmpiW(cls, L"NamespaceTreeControl") == 0) p->tree = TRUE;
    else if (lstrcmpiW(cls, L"DirectUIHWND") == 0) p->dui = TRUE;
    else if (lstrcmpiW(cls, L"Edit") == 0) p->edit = TRUE;
    else if (lstrcmpiW(cls, L"ToolbarWindow32") == 0) p->tb = TRUE;
    return TRUE;
}
static BOOL IsFileDialogWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FALSE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return FALSE;
    WCHAR cls[64];
    if (GetClassNameW(hwnd, cls, _countof(cls)) == 0) return FALSE;
    if (lstrcmpiW(cls, L"#32770") != 0) return FALSE;
    DlgProbe p{};
    EnumChildWindows(hwnd, ProbeChild, (LPARAM)&p);
    if (p.def) return TRUE;
    if (p.tree) return TRUE;
    if (p.edit && p.tb) return TRUE;
    if (p.dui && p.tb) return TRUE;
    return FALSE;
}

// ---------------- 取得 IFileDialog（现代对话框）----------------
// 通用项对话框把其 IFileDialog 实现指针放在窗口 GWLP_USERDATA 上（标准做法），
// 这里在“对话框所属进程内”QI 取得，再调用 SetFolder。若 QI 失败则走 IShellBrowser 回退。
static IFileDialog* GetIFileDialog(HWND dlg) {
    void* p = (void*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
    if (!p) return nullptr;
    IUnknown* unk = (IUnknown*)p;
    IFileDialog* pfd = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_IFileDialog, (void**)&pfd))) return pfd;
    return nullptr;
}

// ---------------- 取得 IShellBrowser（旧版 / 回退）----------------
struct SBProbe { IShellBrowser* psb; };
static BOOL CALLBACK ProbeSB(HWND hwnd, LPARAM lp) {
    SBProbe* p = (SBProbe*)lp;
    if (p->psb) return FALSE;
    void* pv = (void*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!pv) return TRUE;
    IUnknown* unk = (IUnknown*)pv;
    IServiceProvider* sp = nullptr;
    if (SUCCEEDED(unk->QueryInterface(IID_IServiceProvider, (void**)&sp))) {
        IShellBrowser* sb = nullptr;
        if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&sb)))
            p->psb = sb;
        sp->Release();
    }
    return p->psb ? FALSE : TRUE;
}
static IShellBrowser* FindShellBrowser(HWND dlg) {
    SBProbe p{};
    EnumChildWindows(dlg, ProbeSB, (LPARAM)&p);
    if (!p.psb) {
        // 退一步：对话框窗口自身的 GWLP_USERDATA
        void* pv = (void*)GetWindowLongPtrW(dlg, GWLP_USERDATA);
        if (pv) {
            IUnknown* unk = (IUnknown*)pv;
            IServiceProvider* sp = nullptr;
            if (SUCCEEDED(unk->QueryInterface(IID_IServiceProvider, (void**)&sp))) {
                sp->QueryService(SID_STopLevelBrowser, IID_IShellBrowser, (void**)&p.psb);
                sp->Release();
            }
        }
    }
    return p.psb;
}

// ---------------- 在对话框所属进程内执行跳转 ----------------
static BOOL NavigateDialog(HWND dlg, LPCWSTR path) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHParseDisplayName(path, nullptr, &pidl, 0, nullptr);
    if (FAILED(hr) || !pidl) { Log(L"SHParseDisplayName fail %s hr=%ld", path, hr); return FALSE; }

    BOOL ok = FALSE;

    // 1) 现代通用项对话框：IFileDialog::SetFolder
    IFileDialog* pfd = GetIFileDialog(dlg);
    if (pfd) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_IShellItem, (void**)&psi))) {
            hr = pfd->SetFolder(psi);
            ok = SUCCEEDED(hr);
            Log(L"IFileDialog::SetFolder %s hr=%ld", path, hr);
            psi->Release();
        } else {
            Log(L"SHCreateItemFromParsingName fail %s", path);
        }
        pfd->Release();
    }

    // 2) 旧版 / 回退：IShellBrowser::BrowseObject（原地导航，不关闭对话框）
    if (!ok) {
        IShellBrowser* psb = FindShellBrowser(dlg);
        if (psb) {
            hr = psb->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
            ok = SUCCEEDED(hr);
            Log(L"IShellBrowser::BrowseObject %s hr=%ld", path, hr);
            psb->Release();
        } else {
            Log(L"no IFileDialog/IShellBrowser for dlg %p (path %s)", (void*)dlg, path);
        }
    }

    ILFree(pidl);
    return ok;
}

// ---------------- 子类过程（运行在对话框所属进程）----------------
static LRESULT CALLBACK SubclassProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    WNDPROC orig = nullptr;
    EnterCriticalSection(&g_cs);
    auto it = g_orig.find(h);
    if (it != g_orig.end()) orig = it->second;
    LeaveCriticalSection(&g_cs);

    if (m == WM_COPYDATA) {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)l;
        if (cds && cds->dwData == FDJ_CD_NAVIGATE && cds->lpData) {
            BOOL ok = NavigateDialog(h, (LPCWSTR)cds->lpData);
            return ok ? TRUE : FALSE;   // 已处理，不再转发给原过程
        }
    } else if (m == WM_NCDESTROY) {
        if (orig) {
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
            EnterCriticalSection(&g_cs); g_orig.erase(h); LeaveCriticalSection(&g_cs);
        }
    }

    if (orig) return CallWindowProcW(orig, h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// ---------------- 子类化一个对话框（去重）----------------
static void SubclassIfDialog(HWND h) {
    if (!IsFileDialogWindow(h)) return;
    EnterCriticalSection(&g_cs);
    if (g_orig.find(h) == g_orig.end()) {
        WNDPROC orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
        if (orig) {
            SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
            g_orig[h] = orig;
            Log(L"subclassed dialog %p", (void*)h);
        }
    }
    LeaveCriticalSection(&g_cs);
}

// 枚举本进程内所有顶层窗口，子类化其中已是系统文件对话框的。
// 关键点：覆盖“钩子安装前就已打开”的对话框——否则用户“先开对话框、再启动 Flowtary”
// 时，对话框因从未触发过 HCBT_ACTIVATE 而不会被子类化，跳转会静默失败。
static BOOL CALLBACK EnumSubclass(HWND hwnd, LPARAM) {
    SubclassIfDialog(hwnd);
    return TRUE;
}
static void SweepProcess() {
    EnumWindows(EnumSubclass, 0);
}

// ---------------- CBT 钩子过程（运行在对话框所属进程）----------------
// 捕获“钩子安装后”创建/激活的文件对话框。
extern "C" __declspec(dllexport) LRESULT CALLBACK CbtProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HCBT_ACTIVATE) SubclassIfDialog((HWND)wParam);
    return CallNextHookEx(g_hhk, nCode, wParam, lParam);
}

// ---------------- GetMsg 钩子过程（用于把 DLL 注入到“已存在”的对话框进程）----------------
// 本过程本身只做透传；其存在意义是让 DLL 进入每一个 pumping 消息的 GUI 进程，
// 从而在 DLL 首次被加载进某进程时（首条消息到来时）对该进程内已有的文件对话框做子类化。
static BOOL g_swept = FALSE;
extern "C" __declspec(dllexport) LRESULT CALLBACK GetMsgProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (!g_swept) {            // 每个进程首次进入时扫一次本进程内的文件对话框
        g_swept = TRUE;
        SweepProcess();
    }
    return CallNextHookEx(g_hhkMsg, nCode, wParam, lParam);
}

// 宿主/agent 安装钩子后调用，传入 HHOOK 以便正确链式调用
extern "C" __declspec(dllexport) void CbtSetHook(HHOOK h) { g_hhk = h; }
extern "C" __declspec(dllexport) void MsgSetHook(HHOOK h) { g_hhkMsg = h; }

BOOL APIENTRY DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) InitializeCriticalSection(&g_cs);
    else if (reason == DLL_PROCESS_DETACH) DeleteCriticalSection(&g_cs);
    return TRUE;
}
