// ============================================================
// wsquery.exe — d/f 搜索后端冒烟工具（控制台，与 evtest.exe 同位）
//   用法:
//     wsquery --check                    探测 Windows Search / fd 可用性
//     wsquery [-d] [-p] [--fd] 词1 词2…  -d 只搜文件夹；-p 路径级兜底；--fd 用 fd 后端
//     wsquery [--rows N] …               --rows 限守护进程返回行数（默认 50，bench 200）
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

// 搜索索引档位（设置「常规」页）：0=极简（守护进程不驻留）。wsquery 直读注册表，
// 与守护进程/主程序同一真源，用于给出不误导的状态输出。
static int QueryIndexTier() {
    DWORD v = 2, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"DfIndexTier",
                     RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS && v <= 3)
        return (int)v;
    return 2;
}

static void PrintLine(const std::wstring& w) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h && GetConsoleMode(h, &mode)) {
        std::wstring out = w + L"\n";
        DWORD wr = 0;
        WriteConsoleW(h, out.c_str(), (DWORD)out.size(), &wr, nullptr);
    } else if (h) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(n + 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
        s[n] = '\n';
        DWORD wr = 0;
        WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr);
    }
}

int wmain(int argc, wchar_t** argv) {
    df::Query q;
    q.isFile = true;
    bool pathMode = false, useFd = false, check = false, useMem = false, memStat = false;
    bool useDisk = false, diskStat = false;
    int bench = 0, rows = 0;
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
        else if (a == L"--rows" && i + 1 < argc) rows = _wtoi(argv[++i]);
        else if (a == L"--fdpath" && i + 1 < argc) fdExe = argv[++i];
        else if (!a.empty() && a[0] == L'-') { PrintLine(L"未知参数: " + a); return 2; }
        else q.words.push_back(a);
    }
    if (memStat || diskStat) {
        if (QueryIndexTier() == 0) {
            PrintLine(L"索引档位: 极简（不建索引，守护进程不驻留，内存占用 ≈0）");
            PrintLine(L"查询路径: 实时 fd / Windows Search 兜底（秒级）");
            PrintLine(L"== 索引已按极简档关闭 ==");
            return 0;
        }
        df::SearchDaemonStart();
        int memReady = 0, memSize = 0, diskReady = 0, diskRecords = 0;
        int diskError = 0, diskBuilding = -1;
        if (!df::SearchDaemonStat(memReady, memSize, diskReady, diskRecords, &diskError, &diskBuilding)) {
            PrintLine(L"无法读取守护进程状态");
            return 1;
        }
        PrintLine(L"内存索引: " + std::wstring(memReady ? L"就绪，" : L"建库中，") +
                  std::to_wstring((long long)memSize) + L" 条");
        PrintLine(L"磁盘索引: " + std::wstring(diskReady ? L"就绪，" : diskBuilding == 1 ? L"建库中，" : L"未就绪，") +
                  std::to_wstring((long long)diskRecords) + L" 条");
        if (diskError) PrintLine(diskError == ERROR_FILE_NOT_FOUND
            ? L"磁盘索引错误: 未找到 fd.exe，请检查部署目录。"
            : L"磁盘索引上次错误: " + std::to_wstring(diskError));
        if (memReady && diskReady)
            PrintLine(L"== 建库全部完成 ==");
        else if (diskBuilding == 1 || !memReady)
            PrintLine(L"== 建库进行中 ==");
        else PrintLine(L"== 索引未完成，当前没有磁盘建库任务 ==");
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
        PrintLine(L"用法: wsquery [-d] [-p] [--fd|--mem|--disk] [--bench N] [--rows N] 词1 词2 …（或 --check / --memstat / --diskstat）");
        return 2;
    }
    // --bench N：同一进程内连查 N 次，打印每次耗时毫秒与命中数。
    // 必须放在 --mem/--disk 单查分支之前——否则 mem 分支先 return，bench 永远不可达。
    if (bench > 0) {
        for (int i = 0; i < bench; ++i) {
            std::vector<df::Hit> hits;
            ULONGLONG t0 = GetTickCount64();
            int n;
            if (useMem || useDisk) {   // 守护进程层（内存+磁盘索引合并）
                std::vector<df::Hit> dh;
                if (!df::SearchDaemonQuery(q, pathMode, dh, rows > 0 ? rows : 200)) {
                    PrintLine(L"守护进程查询失败（未在运行？）");
                    return 1;
                }
                n = (int)dh.size();
            } else if (useFd) {
                if (fdExe.empty()) fdExe = df::FindFdExe();
                bool timedOut = false;
                n = df::FdQuery(fdExe, q, pathMode, true, hits, df::DefaultFdDeadlineMs(), &timedOut);
            } else {
                n = df::WindowsSearchQuery(q, pathMode, hits);
            }
            PrintLine(L"第 " + std::to_wstring(i + 1) + L" 次: " +
                      std::to_wstring((long long)(GetTickCount64() - t0)) + L" ms, 命中 " +
                      std::to_wstring((long long)n) + L" 条\n");
        }
        return 0;
    }
    // --mem / --disk：统一走守护进程（内存+磁盘索引合并结果，毫秒级）
    if (useMem || useDisk) {
        df::SearchDaemonStart();
        std::vector<df::Hit> hits;
        ULONGLONG t0 = GetTickCount64();
        bool ok = df::SearchDaemonQuery(q, pathMode, hits, rows > 0 ? rows : 50);
        if (!ok) {
            PrintLine(QueryIndexTier() == 0
                          ? L"守护进程查询失败（极简档不驻留守护进程，属正常；实际查询由主程序落 fd/WS 兜底）"
                          : L"守护进程查询失败（未在运行？）");
            return 1;
        }
        PrintLine(L"守护进程命中 " + std::to_wstring((long long)hits.size()) + L" 条, " +
                  std::to_wstring((long long)(GetTickCount64() - t0)) + L" ms:\n");
        for (const auto& h : hits)
            PrintLine((h.isFolder ? L"[D] " : L"[F] ") + h.path + L"\n");
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
