#pragma once
// =====================================================================
// 搜索历史台账（诊断用，只追加不回放）
//
// 目的：把「搜了什么、多久出结果、出了几条、哪一层给的、最后点了什么」留在
// 一份纯文本里，出问题时先看台账再改代码。文件在 %APPDATA%\Flowtary\
// search_history.log（UTF-8 带 BOM，每行一条，\r\n 结尾），不进仓库。
//
// 主程序（src/main.cpp）与设置侧车（src/settings_main.cpp）共用这里的实现，
// 行格式与截取规则只写一遍。
// =====================================================================

#include <windows.h>

#include <string>

namespace ft {
namespace hist {

// ---------------- 行格式（纯函数，单测直接钉） ----------------
struct QueryLine {
    std::wstring ts;         // "2026-10-30 14:22:03.412"（调用方给，便于单测钉死）
    std::wstring layer;      // 层标签：D / F / P（程序搜索）/ EV（Everything 旧链路）
    std::wstring query;      // 本轮输入原文（已单行化）
    int firstMs = -1;        // 首屏上屏延迟；-1 = 全程无结果
    int totalMs = 0;         // 本轮耗时（各层报齐时=总耗时；带 missed 时=到落盘为止）
    int rows = 0;            // 最终可打开的结果条数（提示行不计）
    int daemonRows = 0;      // 守护进程层（内存+磁盘索引）最后一块行数
    int fdRows = 0;          // fd 实时遍历层最后一块行数
    int wsRows = 0;          // Windows Search 层行数
    std::wstring top;        // 第一行结果的完整路径
    std::wstring missed;     // 到写这行时还没报齐的层（空=全部报齐）；非空即「卡住/搜不出来」的现场
};

// "…  Q  [D] aura exe  first=16ms total=210ms rows=128  src=索引96/fd0/WS0  top=C:\…"
// 有层没报齐时再挂一段 "  未回=fd"（或 "未回=索引+fd+WS"）。
std::wstring FormatQueryLine(const QueryLine& q);
// "…  O  [D] aura exe  -> C:\workshop\aura-1.2\aura.exe"
std::wstring FormatOpenLine(const std::wstring& ts, const std::wstring& layer,
                            const std::wstring& query, const std::wstring& path);

// 压成一行：换行/制表/控制符换成空格，连续空格归一，长度截到 kMaxFieldChars
std::wstring OneLine(const std::wstring& s);
// 取最后 maxLines 行（不含空尾行），行间用 \n —— 侧车展示用
std::wstring TakeTail(const std::wstring& text, size_t maxLines);
// 本轮该写几行才算超：按字节粗筛，避免每次写入都全文件数行
bool NeedTrim(size_t fileBytes, size_t maxLines);

constexpr size_t kMaxFieldChars = 300;   // query / path 各自的字符上限
constexpr size_t kAvgLineBytes = 220;    // 一行经验的 UTF-8 字节数（实测本机 ~160，留冗余）

// ---------------- 落盘 ----------------
std::wstring NowText();      // 本地时间 "YYYY-MM-DD HH:MM:SS.mmm"
std::wstring FilePath();     // %APPDATA%\Flowtary\search_history.log

// 追加一行并按需裁剪到尾部 maxLines 行。失败（占用/只读盘）返回 false，调用方不重试。
bool Write(const std::wstring& line, size_t maxLines);
bool TrimTo(size_t maxLines);
// 读尾部 maxLines 行；文件不存在返回空串
std::wstring ReadTail(size_t maxLines);
bool RemoveFile();
}  // namespace hist
}  // namespace ft
