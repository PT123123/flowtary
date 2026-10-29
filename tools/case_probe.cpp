// 大小写还原的三种取证通道实测：整目录枚举 / 单名 FindFirstFile / GetLongPathNameW。
// 用法：case_probe <dirWithManyEntries> <childName> [repeat]  —— 输出单位是微秒。
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <cstdio>

static double Us() {
    LARGE_INTEGER c{}, f{};
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargc < 3) { printf("usage: case_probe <dir> <child> [repeat]\n"); return 2; }
    std::wstring dir = wargv[1];
    std::wstring child = wargv[2];
    int reps = wargc > 3 ? _wtoi(wargv[3]) : 5;
    if (reps < 1) reps = 1;
    std::wstring pat = dir;
    if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
    std::wstring enumPat = pat + L'*';
    std::wstring exactPat = pat + child;

    double bestEnum = 1e18, bestExact = 1e18, bestLong = 1e18;
    size_t lastEntries = 0;
    for (int i = 0; i < reps; ++i) {
        size_t entries = 0;
        double t0 = Us();
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(enumPat.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            ++entries;
            while (FindNextFileW(h, &fd)) ++entries;
            FindClose(h);
        }
        double d = Us() - t0;
        if (d < bestEnum) bestEnum = d;
        lastEntries = entries;
    }
    for (int i = 0; i < reps; ++i) {
        double t0 = Us();
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(exactPat.c_str(), &fd);
        bool ok = h != INVALID_HANDLE_VALUE;
        if (ok) FindClose(h);
        double d = Us() - t0;
        if (d < bestExact) bestExact = d;
    }
    for (int i = 0; i < reps; ++i) {
        double t0 = Us();
        wchar_t buf[4096]{};
        DWORD n = GetLongPathNameW(exactPat.c_str(), buf, (DWORD)(sizeof(buf) / 2));
        double d = Us() - t0;
        if (d < bestLong && n > 0) bestLong = d;
    }
    wprintf(L"entries=%zu  enum=%7.1f us  exact=%7.1f us  longpath=%7.1f us\n",
            lastEntries, bestEnum, bestExact, bestLong);
    return 0;
}
