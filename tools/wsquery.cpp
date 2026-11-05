// Diagnostic client for the existing Windows Search catalog; never builds an index.
#include <windows.h>
#include <string>
#include <vector>
#include "df_search.h"

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
    df::Query query;
    bool pathMode = false, check = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-d") query.isFile = false;
        else if (arg == L"-p") pathMode = true;
        else if (arg == L"--check") check = true;
        else if (arg == L"--fd" || arg == L"--fdpath" || arg == L"--mem" ||
                 arg == L"--disk" || arg == L"--memstat" || arg == L"--diskstat") {
            PrintLine(L"目录扫描与独立索引已移除；此参数不会启动扫描或建库。");
            return 2;
        } else if (!arg.empty() && arg[0] == L'-') {
            PrintLine(L"未知参数：" + arg);
            return 2;
        } else query.words.push_back(arg);
    }
    if (check) {
        bool available = df::WindowsSearchAvailable();
        PrintLine(available ? L"Windows Search：可查询（空索引也视为可用）" : L"Windows Search：不可用");
        if (!available) PrintLine(df::DiagProbe());
        PrintLine(L"Flowtary 不创建索引、不监听目录、不改变系统索引范围。");
        return available ? 0 : 1;
    }
    if (query.words.empty()) {
        PrintLine(L"用法：wsquery [-d] [-p] 关键词… 或 wsquery --check");
        return 2;
    }
    std::vector<df::Hit> hits;
    int count = df::WindowsSearchQuery(query, pathMode, hits);
    if (count < 0) { PrintLine(L"Windows Search 不可用；未启动文件扫描。"); return 1; }
    PrintLine(L"现有 Windows Search 索引命中 " + std::to_wstring(count) + L" 条：");
    for (const auto& hit : hits) PrintLine((hit.isFolder ? L"[D] " : L"[F] ") + hit.path);
    return 0;
}
