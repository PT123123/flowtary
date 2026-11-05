// 开机自启的「启动对齐」门：Run 里记的路径 vs 本次真正跑起来的 exe。
// 背景：部署目录和 exe 名都带版本号（Flowtary-1.1.28.11\flowtary-1.1.28.11.exe），
// 而自启项记的是「点保存那一刻的 exe 全路径」。发版后没人再进设置页点保存，登录就会
// 起在上一个版本上——所以主程序每次启动自己比一次、不一致就改写。这里钉住三条性质：
//   1) 路径变了必须判定为「要改写」；
//   2) 同一路径（带不带引号、大小写、多余空格）不改写，免得每次启动都戳注册表；
//   3) 没有这条值 = 用户没开自启，永远不由启动来替他打开。
// 判定逻辑是纯函数；另一段在本机的 HKCU Run 上用**临时值名**真跑一遍读→比→写→删，
// 证明确实是注册表本身在配合，不是纸面推断。
#include "settings_model.h"

#include <cstdio>
#include <string>

static int Check(bool got, bool want, const wchar_t* what) {
    if (got != want) {
        wprintf(L"FAIL %s got=%d want=%d\n", what, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int CheckEq(const std::wstring& got, const std::wstring& want, const wchar_t* what) {
    if (got != want) {
        wprintf(L"FAIL %s got=[%s] want=[%s]\n", what, got.c_str(), want.c_str());
        return 1;
    }
    return 0;
}

static int PureLogic() {
    const std::wstring oldExe = L"C:\\workshop\\Flowtary-1.1.25.10\\flowtary-1.1.25.10.exe";
    const std::wstring newExe = L"C:\\workshop\\Flowtary-1.1.28.11\\flowtary-1.1.28.11.exe";
    int bad = 0;

    // 去引号 / 去空白：Run 值可能由旧写法存成不带引号的裸路径。
    bad += CheckEq(ft::NormalizeStartupValue(L"\"" + newExe + L"\""), newExe, L"unquote");
    bad += CheckEq(ft::NormalizeStartupValue(L"  " + newExe + L"  "), newExe, L"trim");
    bad += CheckEq(ft::NormalizeStartupValue(L"\"  " + newExe + L" \" "), newExe, L"trim-inside");
    bad += CheckEq(ft::NormalizeStartupValue(L"\"\""), L"", L"empty-quotes");
    bad += CheckEq(ft::NormalizeStartupValue(L"   "), L"", L"blank");
    bad += CheckEq(ft::NormalizeStartupValue(L""), L"", L"absent");
    // 带空格的路径整体加引号，去掉外层引号后中间的空格必须还在。
    bad += CheckEq(ft::NormalizeStartupValue(L"\"C:\\Program Files\\Flowtary.exe\""),
                   L"C:\\Program Files\\Flowtary.exe", L"quoted-space-path");

    // 1) 换版本目录 / 换 exe 名：必须改写。
    bad += Check(ft::StartupNeedsSync(oldExe, newExe), true, L"stale-unquoted");
    bad += Check(ft::StartupNeedsSync(L"\"" + oldExe + L"\"", newExe), true, L"stale-quoted");
    // 自启项指向 sidecar（历史上写错过）也要纠回主 exe。
    bad += Check(ft::StartupNeedsSync(L"C:\\workshop\\Flowtary-1.1.25.10\\flowtary-settings.exe",
                                      newExe),
                 true, L"points-at-sidecar");

    // 2) 同一条路径的各种写法都不该再戳注册表。
    bad += Check(ft::StartupNeedsSync(newExe, newExe), false, L"same-plain");
    bad += Check(ft::StartupNeedsSync(L"\"" + newExe + L"\"", newExe), false, L"same-quoted");
    bad += Check(ft::StartupNeedsSync(L"  \"" + newExe + L"\"  ", newExe), false, L"same-padded");
    bad += Check(ft::StartupNeedsSync(L"\"" + newExe + L"\"", L"\"" + newExe + L"\""), false,
                 L"same-both-quoted");
    // NTFS 路径不区分大小写：只有大小写不同的同一条路径不算变化。
    bad += Check(ft::StartupNeedsSync(newExe,
                                      L"C:\\WORKSHOP\\Flowtary-1.1.28.11\\Flowtary-1.1.28.11.exe"),
                 false, L"case-insensitive");

    // 3) 值缺失或空值 = 没开自启：启动对齐绝不替用户打开。
    bad += Check(ft::StartupNeedsSync(L"", newExe), false, L"absent-value");
    bad += Check(ft::StartupNeedsSync(L"   ", newExe), false, L"blank-value");
    bad += Check(ft::StartupNeedsSync(L"\"\"", newExe), false, L"empty-quoted-value");
    // 拿不到自身路径时别乱写。
    bad += Check(ft::StartupNeedsSync(oldExe, L""), false, L"no-exe-path");
    return bad;
}

// 临时值名带 PID，绝不与线上那条 `Flowtary` 撞车；析构里删掉，断言失败也不留残值。
// 里面存的路径故意指向不存在的位置：万一进程被硬杀没删干净，登录时也不会真起一个程序。
struct ScratchRunValue {
    std::wstring name;
    bool alive = false;
    explicit ScratchRunValue(std::wstring n) : name(std::move(n)) {}
    ~ScratchRunValue() {
        if (alive) ft::DeleteRunValue(name.c_str());
    }
};

static int LiveOnThisMachine() {
    int bad = 0;
    const std::wstring oldExe = L"C:\\flowtary-selftest\\flowtary-0.0.0.1.exe";
    const std::wstring newExe = L"C:\\flowtary-selftest\\Flowtary 0.0.0.2.exe";  // 带空格
    ScratchRunValue s(L"Flowtary-selftest-" + std::to_wstring(GetCurrentProcessId()));

    if (!ft::WriteRunValue(s.name.c_str(), L"\"" + oldExe + L"\"")) {
        wprintf(L"FAIL write-scratch（写不进 HKCU Run，本机权限异常）\n");
        return 1;
    }
    s.alive = true;

    // 现场复原：自启开着，但记的还是上一个版本目录。
    std::wstring have;
    if (!ft::ReadRunValue(s.name.c_str(), have)) {
        wprintf(L"FAIL read-scratch（刚写的值读不回来）\n");
        ++bad;
    } else if (!ft::StartupNeedsSync(have, newExe)) {
        wprintf(L"FAIL needs-sync got=[%s]\n", have.c_str());
        ++bad;
    }

    if (!ft::WriteRunValue(s.name.c_str(), L"\"" + newExe + L"\"")) {
        wprintf(L"FAIL rewrite-scratch\n");
        ++bad;
    }
    have.clear();
    if (!ft::ReadRunValue(s.name.c_str(), have) ||
        ft::StartupNeedsSync(have, newExe)) {  // 对齐过后不该再判成「要改」
        wprintf(L"FAIL synced-again got=[%s]\n", have.c_str());
        ++bad;
    }
    // 引号必须原样存着：不带引号的带空格路径，登录时会被拆成两个参数。
    bad += CheckEq(have, L"\"" + newExe + L"\"", L"quoted-survives-registry");

    if (!ft::DeleteRunValue(s.name.c_str())) {
        wprintf(L"FAIL delete-scratch\n");
        ++bad;
    }
    s.alive = false;
    std::wstring dummy;
    bad += Check(ft::ReadRunValue(s.name.c_str(), dummy), false, L"absent-after-delete");
    // 自启关着的现场：读不到值 → 永远不该由启动来打开（这条值删掉了）。
    bad += Check(ft::StartupNeedsSync(dummy, newExe), false, L"off-stays-off");
    return bad;
}

int wmain() {
    int bad = PureLogic() + LiveOnThisMachine();
    wprintf(bad ? L"startup_sync_test: FAIL\n" : L"startup_sync_test: OK\n");
    return bad ? 1 : 0;
}
