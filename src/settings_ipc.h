#pragma once
// =====================================================================
// Flowtary 设置窗 sidecar ↔ 主进程消息协议（WM_COPYDATA）
//
// 形态：设置窗是独立进程 flowtary-settings.exe。它对「注册表」的写入只发生在
// 保存那一刻；在此之前所有改动都通过 Preview 消息推给主进程做实时预览（只改
// 内存，不落盘）。主进程在打开设置窗时对运行态做快照，收到 Cancel/Closed 或
// 检测到 sidecar 进程消失时用快照回退，语义与旧的「取消」按钮一致。
//
// 约定：
//   · dwData 一律取本文件的 Msg* 常量（0x4654xxxx，避开 Everything IPC 的 0x7FFx）。
//   · 每个 sidecar→主进程 的负载第一个字段都是 sender（sidecar 自己的 HWND），
//     主进程据此回消息（Stats / MainQuitting）。
//   · 负载全部是 POD，两个进程编译同一份头文件，禁止一侧私自改字段顺序。
//
// 行为依据与逐项证据见 docs/SETTINGS_SIDECAR_CONTRACT.md。
// =====================================================================

#include <windows.h>

#include <cstddef>

namespace ipc {

constexpr DWORD kMsgBase = 0x46540000;  // 'FT'

// —— sidecar → 主进程 ——
constexpr DWORD MsgHello = kMsgBase + 1;    // 打开：主进程据 sender 记住 sidecar
constexpr DWORD MsgPreview = kMsgBase + 2;  // 实时预览（纯内存，不落盘）
constexpr DWORD MsgCommit = kMsgBase + 3;   // 已写注册表，主进程全量重读并重放
constexpr DWORD MsgCancel = kMsgBase + 4;   // 取消：回退快照
constexpr DWORD MsgClosed = kMsgBase + 5;   // 窗口关闭（已保存过则等价于「不再预览」）
constexpr DWORD MsgWipeWeights = kMsgBase + 6;  // 清空点击权重（须由主进程执行）
constexpr DWORD MsgWipeLearned = kMsgBase + 7;  // 清空自学习（须由主进程执行）
constexpr DWORD MsgQueryStats = kMsgBase + 8;   // 请求权威计数 → 回 MsgStats
constexpr DWORD MsgCaptureInstalled = kMsgBase + 9;  // 截图工具下载完成 → 重解析路径

// —— 主进程 → sidecar ——
constexpr DWORD MsgStats = kMsgBase + 0x8001;  // {weightCount, learnCount}
constexpr DWORD MsgMainQuitting = kMsgBase + 0x8002;  // 主进程退出，sidecar 自行关闭

constexpr int kMaxThemes = 16;  // ThemeTune 数组上限（当前 8 个主题）

// sidecar→主进程 通用头：所有负载以此开头，主进程据此拿到回信地址。
struct Header {
    HWND sender;
};

// 主题微调（与 ft::ThemeTune 对应，POD 化）
struct Tune {
    int alpha;
    int radius;
};

// 运行态预览：只带「影响主进程内存行为」的标量字段。
// 四个多行编辑器（网页规则 / 启动组 / 关闭组 / 排除路径）不在这里 ——
// 它们只在保存时落盘，由主进程重解析，避免把整段文本塞进定长负载。
struct Preview {
    Header hdr;

    int themeIdx;
    int hotkeyMode;
    BOOL centerWake;
    BOOL beautify;
    BOOL skipDotFolders;
    BOOL fdjEnabled;

    BOOL weightEnabled;
    int weightFlush;
    int weightMaxEntries;

    BOOL learnEnabled;
    int learnScope;
    int learnMaxEntries;
    int learnExpire;

    BOOL topEnabled, cmdEnabled, winEnabled, captureEnabled;

    int shellType;
    int shellDefaultCwd;
    BOOL shellShowWindow;
    BOOL winGroupProc, winShowUwp, winShowProc;
    int winCacheSec;

    // 逐主题微调；仅 themeIdx 那条会灌进 kThemes，其余随快照回退
    int tuneCount;
    Tune tune[kMaxThemes];
};

struct Stats {
    Header hdr;
    ULONGLONG weightCount;
    ULONGLONG learnCount;
};

// 启动 sidecar 时由主进程通过命令行传入：
//   flowtary-settings.exe --ipc <主进程窗口HWND> --main-exe "<主进程 exe 完整路径>"
constexpr WCHAR kArgIpc[] = L"--ipc";
constexpr WCHAR kArgMainExe[] = L"--main-exe";

inline bool IsOurMsg(DWORD dwData) { return dwData >= kMsgBase && dwData < (kMsgBase + 0x10000); }

}  // namespace ipc
