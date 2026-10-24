// ============================================================
// wsquery.exe — d/f 搜索后端冒烟工具（控制台，与 evtest.exe 同位）
//   用法:
//     wsquery --check                    探测 Windows Search / fd 可用性
//     wsquery [-d] [-p] [--fd] 词1 词2…  -d 只搜文件夹；-p 路径级兜底；--fd 用 fd 后端
//   示例:
//     wsquery 报告            Windows Search 按名字搜文件
//     wsquery -d repo         搜文件夹
//     wsquery -d -p aw software   路径级兜底（词可分布在不同层级）
//     wsquery --fd -d repo        fd 现查固定盘
// ============================================================
#include <windows.h>
#include <string>
#include <vector>
#include "df_search.h"

static void PrintLine(const std::wstring& w) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h && GetConsoleMode(h, &mode)) {
        DWORD wr = 0;
        WriteConsoleW(h, w.c_str(), (DWORD)w.size(), &wr, nullptr);
    } else if (h) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        DWORD wr = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr);
    }
}

int wmain(int argc, wchar_t** argv) {
    df::Query q;
    q.isFile = true;
    bool pathMode = false, useFd = false, check = false, useMem = false, memStat = false;
    bool useDisk = false, diskStat = false;
    int bench = 0;
    std::wstring fdExe;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-d") q.isFile = false;
        else if (a == L"-p") pathMode = true;
        else if (a == L"--fd") useFd = true;
        else if (a == L"--mem") useMem = true;
        else if (a == L"--memstat") memStat = true;
        else if (a == L"--disk") useDisk = true;
        else if (a == L"--diskstat") diskStat = true;
        else if (a == L"--check") check = true;
        else if (a == L"--bench" && i + 1 < argc) bench = _wtoi(argv[++i]);
        else if (a == L"--fdpath" && i + 1 < argc) fdExe = argv[++i];
        else if (!a.empty() && a[0] == L'-') { PrintLine(L"未知参数: " + a); return 2; }
        else q.words.push_back(a);
    }
    if (memStat) {
        df::MemIndexStart();
        while (!df::MemIndexReady()) Sleep(200);
        PrintLine(L"内存索引就绪，条目数: " + std::to_wstring((long long)df::MemIndexSize()));
        return 0;
    }
    if (diskStat) {
        df::DiskIndexStart();
        while (!df::DiskIndexReady()) Sleep(500);   // 建库可达数分钟：等待（空闲优先级）
        PrintLine(L"磁盘索引就绪，条目数: " + std::to_wstring((long long)df::DiskIndexRecords()));
        return 0;
    }
    if (check) {
        bool ok = df::WindowsSearchAvailable();
        PrintLine(std::wstring(L"Windows Search: ") +
                  (ok ? L"可用（索引有内容）" : L"不可用（服务未运行或索引为空）"));
        if (!ok) PrintLine(L"  诊断: " + df::DiagProbe());
        std::wstring fd = df::FindFdExe();
        PrintLine(std::wstring(L"fd: ") +
                  (fd.empty() ? L"未找到（把 fd.exe 放到 wsquery 同目录或 PATH）" : fd));
        return 0;
    }
    if (q.words.empty()) {
        PrintLine(L"用法: wsquery [-d] [-p] [--fd|--mem] [--bench N] 词1 词2 …（或 --check / --memstat）");
        return 2;
    }
    // --mem：内存索引（建库中会等待就绪）
    if (useMem) {
        df::MemIndexStart();
        while (!df::MemIndexReady()) Sleep(200);
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0)
            PrintLine(L"[dbg] ready, size=" + std::to_wstring((long long)df::MemIndexSize()));
        std::vector<df::Hit> hits;
        int n = df::MemIndexSearch(q, pathMode, hits, 50);
        if (GetEnvironmentVariableW(L"FT_DF_DEBUG", nullptr, 0) > 0)
            PrintLine(L"[dbg] searched n=" + std::to_wstring((long long)n));
        PrintLine(L"内存索引命中 " + std::to_wstring((long long)n) + L" 条:");
        for (const auto& h : hits)
            PrintLine((h.isFolder ? L"[D] " : L"[F] ") + h.path);
        return 0;
    }
    // --disk：磁盘索引（全盘 mmap 扫描；建库中会等就绪后查询）
    if (useDisk) {
        df::DiskIndexStart();
        while (!df::DiskIndexReady()) Sleep(500);
        std::vector<df::Hit> hits;
        ULONGLONG t0 = GetTickCount64();
        int n = df::DiskIndexSearch(q, pathMode, hits, 50);
        PrintLine(L"磁盘索引命中 " + std::to_wstring((long long)n) + L" 条, " +
                  std::to_wstring((long long)(GetTickCount64() - t0)) + L" ms:");
        for (const auto& h : hits)
            PrintLine((h.isFolder ? L"[D] " : L"[F] ") + h.path);
        return 0;
    }
    // --bench N：同一进程内连查 N 次（会话缓存生效 = flowtary 常驻线程的暖查询表现），
    // 打印每次耗时毫秒与命中数
    if (bench > 0) {
        for (int i = 0; i < bench; ++i) {
            std::vector<df::Hit> hits;
            ULONGLONG t0 = GetTickCount64();
            int n;
            if (useFd) {
                if (fdExe.empty()) fdExe = df::FindFdExe();
                bool timedOut = false;
                n = df::FdQuery(fdExe, q, pathMode, true, hits, df::DefaultFdDeadlineMs(), &timedOut);
            } else {
                n = df::WindowsSearchQuery(q, pathMode, hits);
            }
            PrintLine(L"第 " + std::to_wstring(i + 1) + L" 次: " +
                      std::to_wstring((long long)(GetTickCount64() - t0)) + L" ms, 命中 " +
                      std::to_wstring((long long)n) + L" 条");
        }
        return 0;
    }
    std::vector<df::Hit> hits;
    int n;
    if (useFd) {
        if (fdExe.empty()) fdExe = df::FindFdExe();
        if (fdExe.empty()) { PrintLine(L"未找到 fd.exe"); return 1; }
        bool timedOut = false;
        n = df::FdQuery(fdExe, q, pathMode, /*skipHidden=*/true, hits,
                        df::DefaultFdDeadlineMs(), &timedOut);
        if (n < 0) { PrintLine(L"fd 查询失败（启动失败）"); return 1; }
        if (timedOut) PrintLine(L"[超时截断：已到 fd 预算，以下为部分结果]");
    } else {
        n = df::WindowsSearchQuery(q, pathMode, hits);
        if (n < 0) { PrintLine(L"Windows Search 查询失败（WSearch 未运行？）"); return 1; }
    }
    PrintLine(L"命中 " + std::to_wstring((long long)n) + L" 条:");
    for (const auto& h : hits)
        PrintLine((h.isFolder ? L"[D] " : L"[F] ") + h.path);
    return 0;
}
