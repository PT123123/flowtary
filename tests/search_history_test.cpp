// 搜索历史台账的行格式与落盘断言（ctest 目标 search_history）。
// 纯逻辑部分直接断言；读写部分把 APPDATA 指到临时目录，写真文件再读回来——
// 绝不打到用户真实的 %APPDATA%\Flowtary\search_history.log 上。
#include "search_history.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_fail;
}

static void TestOneLine() {
    std::wstring s = ft::hist::OneLine(L"aura\r\nexe\t\tv1.2");
    Check(s == L"aura exe v1.2", "OneLine: 换行/制表压成单空格");
    Check(ft::hist::OneLine(L"  前后空白都要去掉  ") == L"前后空白都要去掉", "OneLine: 去首尾空白");
    Check(ft::hist::OneLine(L"全角空格　也要归一") == L"全角空格 也要归一", "OneLine: 全角空格");
    std::wstring longIn(2000, L'x');
    Check(ft::hist::OneLine(longIn).size() == ft::hist::kMaxFieldChars, "OneLine: 超长截到上限");
    Check(ft::hist::OneLine(L"").empty(), "OneLine: 空串仍是空串");
}

static void TestFormat() {
    ft::hist::QueryLine q;
    q.ts = L"2026-10-30 14:22:03.412";
    q.layer = L"F";
    q.query = L"f aura exe";
    q.firstMs = 16;
    q.totalMs = 210;
    q.rows = 128;
    q.daemonRows = 96;
    q.fdRows = 30;
    q.wsRows = 2;
    q.top = L"C:\\Users\\Public\\workshop-demo\\aura-1.2\\aura.exe";
    std::wstring line = ft::hist::FormatQueryLine(q);
    Check(line.find(L"2026-10-30 14:22:03.412  Q  [F] f aura exe") == 0, "Q 行: 时间戳+类型+层+原文打头");
    Check(line.find(L"first=16ms total=210ms rows=128") != std::wstring::npos, "Q 行: 三个耗时/条数字段");
    Check(line.find(L"src=索引96/fd30/WS2") != std::wstring::npos, "Q 行: 分层给了几条");
    Check(line.find(L"top=C:\\Users\\Public\\workshop-demo") != std::wstring::npos, "Q 行: 首条结果全路径");

    ft::hist::QueryLine empty = q;
    empty.firstMs = -1;
    empty.rows = 0;
    empty.daemonRows = empty.fdRows = empty.wsRows = 0;
    empty.top.clear();
    std::wstring eline = ft::hist::FormatQueryLine(empty);
    Check(eline.find(L"first=-") != std::wstring::npos, "Q 行: 无结果时首屏写成 -");
    Check(eline.find(L"rows=0") != std::wstring::npos && eline.find(L"top=-") != std::wstring::npos,
          "Q 行: 零结果行仍完整落盘（这类行最该留）");
    Check(eline.find(L"未回") == std::wstring::npos, "Q 行: 各层都报齐时不写未回");

    ft::hist::QueryLine stuck = empty;
    stuck.missed = L"fd+WS";
    std::wstring sline = ft::hist::FormatQueryLine(stuck);
    Check(sline.find(L"  未回=fd+WS") != std::wstring::npos,
          "Q 行: 卡住的那一轮写明还欠哪几层");

    std::wstring oline = ft::hist::FormatOpenLine(L"2026-10-30 14:22:05.700", L"F",
                                                  L"f aura exe",
                                                  L"C:\\Users\\Public\\aura-1.3\\aura.exe");
    Check(oline ==
              L"2026-10-30 14:22:05.700  O  [F] f aura exe  -> C:\\Users\\Public\\aura-1.3\\aura.exe",
          "O 行: 原文 → 实际打开的路径");
}

static void TestTakeTail() {
    std::wstring text = L"1\r\n2\r\n3\r\n4\r\n5\r\n";
    Check(ft::hist::TakeTail(text, 3) == L"3\n4\n5\n", "TakeTail: 从尾部取 N 行并归一换行");
    Check(ft::hist::TakeTail(text, 99) == L"1\n2\n3\n4\n5\n", "TakeTail: 不足 N 行时全给");
    Check(ft::hist::TakeTail(L"", 10).empty(), "TakeTail: 空文件给空串");
    Check(ft::hist::TakeTail(text, 0).empty(), "TakeTail: 上限 0 不返回内容");
    Check(ft::hist::TakeTail(L"最后一行没有换行", 5) == L"最后一行没有换行\n",
          "TakeTail: 末行缺换行也算一条");
}

static void TestTrimThreshold() {
    Check(!ft::hist::NeedTrim(0, 0), "NeedTrim: 上限 0 表示不裁剪");
    Check(!ft::hist::NeedTrim(ft::hist::kAvgLineBytes * 2000 - 1, 2000), "NeedTrim: 未超上限不触发");
    Check(ft::hist::NeedTrim(ft::hist::kAvgLineBytes * 2000 + 1, 2000), "NeedTrim: 超上限即触发");
}

// 真写文件：APPDATA 指到临时目录，读回来验格式与轮转
static void TestFileRoundTrip() {
    namespace hs = ft::hist;
    WCHAR base[MAX_PATH]{};
    GetTempPathW(MAX_PATH, base);
    std::wstring appdata = std::wstring(base) + L"flowtary-hist-test";
    SetEnvironmentVariableW(L"APPDATA", appdata.c_str());
    hs::RemoveFile();   // 上一轮残留会打乱条数断言

    for (int i = 1; i <= 5; ++i) {
        hs::QueryLine q;
        q.ts = hs::NowText();
        q.layer = L"D";
        q.query = L"d demo" + std::to_wstring(i);
        q.firstMs = i;
        q.totalMs = i * 2;
        q.rows = i;
        q.daemonRows = i;
        q.top = L"C:\\Users\\Public\\demo-" + std::to_wstring(i);
        Check(hs::Write(hs::FormatQueryLine(q), 2000), "落盘: 追加成功");
    }
    std::wstring tail = hs::ReadTail(2000);
    Check(!tail.empty(), "落盘: 读回非空");
    size_t lines = 0;
    for (WCHAR c : tail) if (c == L'\n') ++lines;
    Check(lines == 5, "落盘: 写几条读回几条");
    Check(tail.find(L"d demo1") != std::wstring::npos && tail.find(L"d demo5") != std::wstring::npos,
          "落盘: 首尾记录都在");

    // 轮转：上限设成 3 行，写入触发裁剪后只剩尾部 3 条
    for (int i = 6; i <= 12; ++i) {
        hs::QueryLine q;
        q.ts = hs::NowText();
        q.layer = L"D";
        q.query = L"d demo" + std::to_wstring(i);
        q.firstMs = 1;
        q.totalMs = 2;
        q.rows = 3;
        q.daemonRows = 3;
        q.top = L"C:\\Users\\Public\\demo";
        Check(hs::Write(hs::FormatQueryLine(q), 3), "轮转: 写入成功");
    }
    std::wstring kept = hs::ReadTail(100);
    size_t keptLines = 0;
    for (WCHAR c : kept) if (c == L'\n') ++keptLines;
    Check(keptLines <= 4, "轮转: 超上限后只留尾部（写入即裁剪）");
    Check(kept.find(L"d demo1\n") == std::wstring::npos, "轮转: 最早的记录被裁掉");
    Check(kept.find(L"d demo12") != std::wstring::npos, "轮转: 最新记录留下");

    // 空文件读回不炸
    hs::RemoveFile();
    Check(hs::ReadTail(10).empty(), "落盘: 文件不存在时读回空串");
    Check(hs::Write(L"d demo-恢复", 2000), "落盘: 删除后能重新建文件");
    Check(hs::ReadTail(10).find(L"demo-恢复") != std::wstring::npos, "落盘: 重建后内容可读");
    Check(hs::ReadTail(10).find(L"\r") == std::wstring::npos, "落盘: 读回的行内不带 \\r");
    hs::RemoveFile();
}

int main() {
    TestOneLine();
    TestFormat();
    TestTakeTail();
    TestTrimThreshold();
    TestFileRoundTrip();
    if (g_fail) {
        std::printf("search_history_test: %d FAIL\n", g_fail);
        return 1;
    }
    std::printf("search_history_test: OK\n");
    return 0;
}
