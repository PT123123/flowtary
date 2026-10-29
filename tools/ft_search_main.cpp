// ============================================================
// flowtary-search — Flowtary 搜索守护进程（--serve 常驻模式）
//
//   职责：持有内存索引池（用户目录）+ 磁盘索引 blob 映射 + 目录变更监听，
//   通过 127.0.0.1 TCP 行协议向 flowtary.exe 提供毫秒级文件/文件夹搜索。
//   索引内存全部记在本进程名下——任务管理器里 flowtary.exe（UI）与
//   本进程（索引）分离，便于与 Everything 的内存占用对比。
//
//   生命周期：由 flowtary.exe 拉起（--serve），单实例；空闲 30 分钟自动退出，
//   磁盘/内存缓存均已落盘，重拉秒级恢复。fd.exe 为其遍历子组件（瞬时）。
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>

#include <string>
#include <vector>
#include <cstdio>
#include <exception>
#include <cstdlib>

#include "df_search.h"

static std::vector<std::wstring> IndexDirsFromRegistry() {
    std::vector<std::wstring> out;
    DWORD cb = 0;   // 变长读，理由同 df_search.cpp：固定缓冲超量会让优先根整体消失
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"IndexDirs",
                     RRF_RT_REG_SZ, nullptr, nullptr, &cb) == ERROR_SUCCESS &&
        cb > sizeof(WCHAR)) {
        std::vector<WCHAR> buf(cb / sizeof(WCHAR) + 1);
        DWORD cb2 = (DWORD)buf.size() * sizeof(WCHAR);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"IndexDirs",
                         RRF_RT_REG_SZ, nullptr, buf.data(), &cb2) != ERROR_SUCCESS)
            return out;
        std::wstring text(buf.data());
        size_t i = 0;
        while (i < text.size()) {
            size_t j = text.find_first_of(L"\r\n", i);
            if (j == std::wstring::npos) j = text.size();
            std::wstring ln = text.substr(i, j - i);
            size_t a = ln.find_first_not_of(L" \t");
            if (a != std::wstring::npos && ln[a] != L'#') {
                size_t b = ln.find_last_not_of(L" \t");
                if (b != std::wstring::npos) out.push_back(ln.substr(a, b - a + 1));
            }
            i = j + 1;
        }
    }
    return out;
}

int wmain(int argc, wchar_t** argv) {
    std::set_terminate([] {
        try {
            if (auto error = std::current_exception()) std::rethrow_exception(error);
        } catch (const std::exception& error) {
            fprintf(stderr, "[fatal] unhandled C++ exception: %s\n", error.what());
        } catch (...) {
            fprintf(stderr, "[fatal] unhandled non-standard exception\n");
        }
        void* frames[32]{};
        fprintf(stderr, "[fatal] image-base=%p\n", GetModuleHandleW(nullptr));
        USHORT count = CaptureStackBackTrace(0, 32, frames, nullptr);
        for (USHORT i = 0; i < count; ++i) fprintf(stderr, "[fatal] frame=%p\n", frames[i]);
        fflush(stderr);
        std::abort();
    });
    bool serve = false;
    for (int i = 1; i < argc; ++i)
        if (wcscmp(argv[i], L"--serve") == 0) serve = true;
    if (!serve) return 2;   // 本 exe 仅 --serve 模式；遍历由随包 fd.exe 承担

    // 单实例：已有守护进程在跑则静默退出（互斥体随进程消失，无僵尸锁）。
    // FT_DF_DAEMON_MUTEX 可换名（配合 FT_DF_DAEMON_PORT 做 A/B：不抢常驻实例的槽）。
    wchar_t mtxEnv[128]{};
    const wchar_t* mtxName = L"Local\\Flowtary.SearchDaemon";
    if (GetEnvironmentVariableW(L"FT_DF_DAEMON_MUTEX", mtxEnv, 128) > 0) mtxName = mtxEnv;
    HANDLE mtx = CreateMutexW(nullptr, TRUE, mtxName);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    return df::SearchDaemonServe(IndexDirsFromRegistry());
}
