// agent32.cpp
//
// 32 位钩子安装助手（编译为 filedlg_agent32.exe，x86）。
//
// 64 位宿主（flowtary.exe）无法加载 32 位 DLL，因此 32 位 CBT 钩子由本助手负责安装：
//   1) 加载同目录的 filedlg_hook32.dll
//   2) SetWindowsHookEx(WH_CBT) 安装全局 32 位钩子
//   3) 创建隐藏消息窗口，运行消息循环；宿主退出时 PostMessage(WM_CLOSE) 通知本进程卸载钩子
//
// 本进程仅做“安装 + 保活”，真正的识别与跳转逻辑都在 hookdlg.cpp（注入到其他 32 位进程）。

#include "filedlg_jump.h"
#include <windows.h>

static HHOOK  g_hhk = nullptr;     // WH_CBT 钩子
static HHOOK  g_hhkMsg = nullptr;  // WH_GETMESSAGE 钩子
static HMODULE g_mod = nullptr;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, _countof(path))) return 1;
    WCHAR* s = wcsrchr(path, L'\\');
    if (s) lstrcpyW(s + 1, L"filedlg_hook32.dll");
    g_mod = LoadLibraryW(path);
    if (!g_mod) return 1;

    typedef LRESULT(CALLBACK* CBT)(int, WPARAM, LPARAM);
    CBT proc = (CBT)GetProcAddress(g_mod, "CbtProc");
    CBT msgProc = (CBT)GetProcAddress(g_mod, "GetMsgProc");
    if (proc && msgProc) {
        g_hhk = SetWindowsHookExW(WH_CBT, (HOOKPROC)proc, g_mod, 0);
        g_hhkMsg = SetWindowsHookExW(WH_GETMESSAGE, (HOOKPROC)msgProc, g_mod, 0);
        if (g_hhk && g_hhkMsg) {
            typedef void (*SetHookFn)(HHOOK);
            if (SetHookFn f = (SetHookFn)GetProcAddress(g_mod, "CbtSetHook")) f(g_hhk);
            if (SetHookFn f = (SetHookFn)GetProcAddress(g_mod, "MsgSetHook")) f(g_hhkMsg);
        }
    }
    if (!g_hhk || !g_hhkMsg) { FreeLibrary(g_mod); return 1; }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = FDJ_AGENT32_CLASS;
    RegisterClassExW(&wc);
    CreateWindowExW(0, FDJ_AGENT32_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_hhk);
    UnhookWindowsHookEx(g_hhkMsg);
    FreeLibrary(g_mod);
    return 0;
}
