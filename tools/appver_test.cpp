// ============================================================
// appver_test.cpp — 应用版本目录解析的断言（src/app_ver.h 的门）
//   钉住 workshop / software 实际存在的目录形状，以及「不能误判」的形状
//   （HWiNFO64、纯数字日期目录、9.0c、Windows NT 等）。
//   构建：cmake --build build --target appver_test   运行：build\appver_test.exe
// ============================================================
#include "../src/app_ver.h"

#include <cstdio>
#include <string>
#include <vector>

using appver::Info;
using appver::Table;

static int failures = 0;

static void Fail(const char* what, const std::wstring& path) {
    ++failures;
    fwprintf(stderr, L"FAIL %hs: %ls\n", what, path.c_str());
}

// 解析并断言：期望 has / ver（点分串）/ plain
static void Check(const std::wstring& path, bool wantHas, const wchar_t* wantVer, bool wantPlain,
                  const char* label) {
    Info v = appver::Parse(path);
    if (v.has != wantHas) { Fail(label, path); return; }
    if (!wantHas) return;
    // 期望版本号按同样的打包规则换算（测试里写人类可读的 "1.2.25"）
    const unsigned long long want = appver::Pack(wantVer);
    if (v.ver != want) {
        ++failures;
        fwprintf(stderr, L"FAIL %hs: %ls 版本 %llu ≠ %ls(%llu)\n", label, path.c_str(),
                 v.ver, wantVer, want);
        return;
    }
    if (v.plain != wantPlain) { Fail(label, path); }
}

int main() {
    // ---- workshop 真实布局：同应用多版本 + 快照副本 ----
    Check(L"C:\\workshop\\aura-1.2.25\\aura.exe", true, L"1.2.25", true, "workshop plain");
    Check(L"C:\\workshop\\aura-1.2.25-backup-20261028-1755\\aura.exe", true, L"1.2.25", false,
          "workshop backup");
    Check(L"C:\\workshop\\aura-1.2.25-backup-20261029-133050-traylock\\aura.exe", true, L"1.2.25",
          false, "workshop traylock");
    Check(L"C:\\workshop\\Flowtary-1.1.23.10\\flowtary-search.exe", true, L"1.1.23.10", true,
          "four segments");
    Check(L"C:\\workshop\\WinTopPreview-0.0.3\\WinTopPreview.exe", true, L"0.0.3", true,
          "camel name");
    Check(L"C:\\workshop\\w-justbrowse-0.2.0\\w-justbrowse.exe", true, L"0.2.0", true,
          "dash in app name");
    Check(L"C:\\workshop\\quire-slint-0.1.47\\quire.exe", true, L"0.1.47", true, "quire slint");

    // ---- 版本号数值比较（不能按字典序：1.1.23.10 > 1.1.23.9，1.2.26 > 1.2.25）----
    if (!(appver::Pack(L"1.1.23.10") > appver::Pack(L"1.1.23.9"))) { ++failures; printf("FAIL pack 4th seg\n"); }
    if (!(appver::Pack(L"1.2.26") > appver::Pack(L"1.2.25"))) { ++failures; printf("FAIL pack compare\n"); }
    if (appver::Pack(L"1.2") != appver::Pack(L"1.2.0.0")) { ++failures; printf("FAIL pack pad\n"); }
    if (appver::Pack(L"0.1.47") <= appver::Pack(L"0.1.9")) { ++failures; printf("FAIL pack 47>9\n"); }

    // ---- 同应用分组：正式版与快照同键；不同应用不同键 ----
    {
        const std::wstring a = appver::Parse(L"C:\\workshop\\aura-1.2.25\\aura.exe").app;
        const std::wstring b =
            appver::Parse(L"C:\\workshop\\aura-1.2.25-backup-20261028-1755\\aura.exe").app;
        const std::wstring c = appver::Parse(L"C:\\workshop\\quire-slint-0.1.47\\quire.exe").app;
        if (a != b) { ++failures; printf("FAIL backup groups with release\n"); }
        if (a == c) { ++failures; printf("FAIL different apps share key\n"); }
    }

    // ---- 嵌套发布布局：版本段在祖先层 ----
    Check(L"C:\\workshop\\snip-0.1.15\\dist\\snip.exe", true, L"0.1.15", true, "nested dist");

    // ---- software 里的其他形状 ----
    Check(L"C:\\software\\LogNote-1.1\\LogNote.exe", true, L"1.1", true, "two segments");
    Check(L"C:\\software\\TaskbarX_1.7.8.0_x86\\taskbarx.x86.exe", true, L"1.7.8.0", false,
          "underscore + arch suffix");
    Check(L"C:\\software\\CapsWriter-Offline-GUI-v2.0.4\\main.exe", true, L"2.0.4", true,
          "v prefix");

    // ---- 不能误判的形状 ----
    Check(L"C:\\software\\HWiNFO64\\hwinfo64.exe", false, L"", false, "digits glued to name");
    Check(L"C:\\Users\\Public\\Desktop\\crawlbase\\20261028\\report.txt", false, L"", false,
          "pure numeric dir");
    Check(L"C:\\software\\Microsoft\\Windows NT\\Winver.exe", false, L"", false, "no version");
    Check(L"C:\\software\\DirectX 9.0c\\dxsetup.exe", false, L"", false, "9.0c glued letter");
    Check(L"C:\\Users\\Public\\notes.md", false, L"", false, "no dir");
    Check(L"C:\\", false, L"", false, "drive root");
    Check(L"C:\\Program Files\\Common Files\\microsoft shared\\OFFICE15\\excel.exe", false, L"",
          false, "office15 glued");   // OFFICE15 数字粘在名字后 → 不算版本

    // ---- 版本档：最新版=0、无版本=1、旧版本=2；只有快照时快照当目标 ----
    {
        std::vector<std::wstring> rows = {
            L"C:\\workshop\\aura-1.2.25\\aura.exe",
            L"C:\\workshop\\aura-1.2.24\\aura.exe",
            L"C:\\workshop\\aura-1.2.25-backup-20261028-1755\\aura.exe",
            L"C:\\Users\\Public\\Desktop\\a-wiztree\\README.md",
        };
        Table tab;
        std::vector<Info> vs(rows.size());
        for (size_t i = 0; i < rows.size(); ++i) { vs[i] = appver::Parse(rows[i]); tab.Feed(vs[i]); }
        const int want[4] = {0, 2, 0, 1};   // 快照与 1.2.25 同号 → 也在 0 档，靠 plain 分先后
        for (size_t i = 0; i < rows.size(); ++i)
            if (tab.Rank(vs[i]) != want[i]) {
                ++failures;
                printf("FAIL rank row %zu got %d want %d\n", i, tab.Rank(vs[i]), want[i]);
            }
        Table solo;
        solo.Feed(appver::Parse(L"C:\\workshop\\aura-1.2.25-backup-20261028-1755\\aura.exe"));
        if (solo.Rank(appver::Parse(L"C:\\workshop\\aura-1.2.25-backup-20261028-1755\\aura.exe")) != 0)
            ++failures, printf("FAIL snapshot-only should be target\n");
    }

    if (failures) { printf("appver_test: %d FAIL\n", failures); return 1; }
    printf("appver_test: OK\n");
    return 0;
}
