// evtest — Everything IPC 协议冒烟测试
// 向 Everything 发送 WM_COPYDATA 查询，打印前若干条结果，用于验证协议与本机环境。
// 用法: evtest.exe [查询串]   默认查询 "file:hosts"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#pragma pack(push, 1)
struct EvQuery {
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    WCHAR search_string[1];
};
struct EvItem {
    DWORD flags;
    DWORD filename_offset;
    DWORD path_offset;
};
struct EvList {
    DWORD totfolders, totfiles, totitems;
    DWORD numfolders, numfiles, numitems;
    DWORD offset;
    EvItem items[1];
};
#pragma pack(pop)

constexpr DWORD kCopyDataQueryW = 2;
constexpr DWORD kReplyMsg = 0x46540001;

struct Result {
    std::wstring name, path;
    bool folder;
};
static std::vector<Result> g_results;
static DWORD g_total = 0;
static bool g_done = false;

static std::string W2U(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COPYDATA) {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
        if (cds && cds->dwData == kReplyMsg && cds->lpData &&
            cds->cbData >= sizeof(EvList) - sizeof(EvItem)) {
            const EvList* list = (const EvList*)cds->lpData;
            g_total = list->totitems;
            for (DWORD i = 0; i < list->numitems && i < 8; ++i) {
                const EvItem& it = list->items[i];
                if (it.filename_offset >= cds->cbData || it.path_offset >= cds->cbData) continue;
                Result r;
                r.name = (const WCHAR*)((const BYTE*)list + it.filename_offset);
                r.path = (const WCHAR*)((const BYTE*)list + it.path_offset);
                r.folder = (it.flags & 0x1) != 0;
                g_results.push_back(std::move(r));
            }
            g_done = true;
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int main() {
    SetConsoleOutputCP(65001);

    std::wstring query = L"file:hosts";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) query = argv[1];

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"EvTestReply";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, nullptr);
    if (!hwnd) {
        printf("FAIL: cannot create reply window\n");
        return 1;
    }

    HWND ev = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!ev) {
        printf("FAIL: Everything IPC window not found (Everything UI not running in this session)\n");
        return 2;
    }

    size_t len = query.size();
    std::vector<BYTE> buf(sizeof(EvQuery) - sizeof(WCHAR) + (len + 1) * sizeof(WCHAR));
    EvQuery* q = (EvQuery*)buf.data();
    q->reply_hwnd = (DWORD)(DWORD_PTR)hwnd;
    q->reply_copydata_message = kReplyMsg;
    q->search_flags = 0;
    q->offset = 0;
    q->max_results = 8;
    memcpy(q->search_string, query.c_str(), (len + 1) * sizeof(WCHAR));

    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataQueryW;
    cds.cbData = (DWORD)buf.size();
    cds.lpData = buf.data();

    DWORD_PTR res = 0;
    LRESULT lr = SendMessageTimeoutW(ev, WM_COPYDATA, (WPARAM)hwnd, (LPARAM)&cds, SMTO_ABORTIFHUNG,
                                     1500, &res);
    if (lr == 0 || res == 0) {
        printf("FAIL: Everything rejected the query (lr=%lld res=%llu)\n", (long long)lr,
               (unsigned long long)res);
        return 3;
    }

    DWORD t0 = GetTickCount();
    while (!g_done && GetTickCount() - t0 < 3000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
    if (!g_done) {
        printf("FAIL: no reply within 3s\n");
        return 4;
    }

    printf("OK: query=\"%s\" total=%lu shown=%zu\n", W2U(query).c_str(), (unsigned long)g_total,
           g_results.size());
    for (auto& r : g_results) {
        printf("  [%s] %s\\%s\n", r.folder ? "DIR" : "FILE", W2U(r.path).c_str(),
               W2U(r.name).c_str());
    }
    return 0;
}
