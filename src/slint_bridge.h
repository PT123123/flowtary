#pragma once
// =====================================================================
// Flowtary Slint 渲染器 ↔ GDI 主进程 的共享内存桥
//
// 分工（Slint 引擎默认开启后，两边职责固定不变）：
//   · 主进程 flowtary.exe  = 「大脑」：搜索管线（程序 / 文件 / 网页）、
//     自学习、点击权重、全局热键、托盘、设置 —— 逻辑只有这一份。
//   · flowtary-slint.exe   = 「皮」：只负责画界面、收键盘、回传用户意图。
//     渲染器不复制任何搜索逻辑，所以两个引擎的功能完全一致。
//
// 为什么用共享内存而不是 WM_COPYDATA：
//   WM_COPYDATA 跨进程必须走 SendMessage（PostMessage 不做指针编组），
//   两边互发时存在「渲染器 SendMessage 给大脑、同时大脑 SendMessage 回
//   渲染器」的互相等待死锁。本桥改成共享内存 + 纯通知消息
//   （PostMessage 不带数据），两个方向都不阻塞，结构上不可能死锁。
//
// 内存布局（%APPDATA%\Flowtary\slint_bridge.bin，双方各开一份文件映射）：
//   · req[]   请求环：渲染器是生产者（写 reqHead），大脑是消费者（写 reqTail）
//   · state[] 状态槽：大脑写（seq 递增 + 槽位轮转），渲染器读
//
// 负载全部是 POD，两个进程编译同一份头文件，禁止一侧私自改字段顺序。
// =====================================================================

#include <windows.h>

#include <string>

namespace slintbridge {

// 桥文件：%APPDATA%\Flowtary\slint_bridge.bin
// 用「真实文件 + 文件映射」而不是命名共享内存：命名内核对象在不同启动方式
// （ShellExecute / 沙箱 / 私有命名空间）下可能不互通，文件映射只看文件路径。
inline std::wstring BridgeFilePath() {
    WCHAR buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring dir(buf, n);
    dir += L"\\Flowtary";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\slint_bridge.bin";
}

// 通知消息（不带数据，只表示「有更新了，去桥文件里读」）
constexpr UINT kKickToBrain = WM_APP + 0x31;   // 渲染器 → 大脑：请求环里有新请求
constexpr UINT kKickToRender = WM_APP + 0x32;  // 大脑 → 渲染器：状态有更新

// 渲染器 IPC 窗口的类名（大脑用它定位渲染器）
constexpr WCHAR kIpcClass[] = L"FlowtarySlintIpc";

// 请求操作码（渲染器 → 大脑）
enum ReqOp : int {
    kReqText = 1,      // 输入框完整文本（utf8，落在 Req::text）
    kReqNav = 2,       // 上下移动选择，arg = -1 / +1
    kReqActivate = 3,  // 激活第 arg 行
    kReqHide = 4,      // 隐藏启动器
    kReqAltKey = 5,    // Alt+数字 / Alt+字母，arg = 虚拟键码
    kReqReady = 6,     // 渲染器就绪（请下发状态并显示窗口）
};

// 状态命令（大脑 → 渲染器）
enum StateCmd : int {
    kCmdNone = 0,
    kCmdShow = 1,   // 显示窗口并聚焦输入框
    kCmdHide = 2,   // 隐藏窗口
    kCmdQuit = 3,   // 主进程退出，渲染器自行关闭
};

// 行类型（与主程序 Row::Kind 的取值顺序一一对应，改动必须两边同步）
enum RowKind : int {
    kRowFile = 0,
    kRowFolder,
    kRowWeb,
    kRowProg,
    kRowHint,
    kRowGroup,
    kRowShell,
    kRowWindow,
    kRowTop,
    kRowCapture,
    kRowReveal,
};

constexpr int kReqRingSize = 16;       // 请求环长度（必须是 2 的幂）
constexpr int kStateSlots = 4;         // 状态槽个数（轮转，避免读到半截）
constexpr int kTextMax = 4096;         // 单次文本上限（字节）
constexpr int kRowsMax = 96 * 1024;    // 行区上限（字节）

#pragma pack(push, 1)
struct Req {
    int op;              // ReqOp
    int arg;             // 数值参数（delta / 行号 / 虚拟键）
    unsigned seq;        // 写入时的环位序号（调试用）
    unsigned textLen;    // 有效文本字节数（<= kTextMax）
    char text[kTextMax];
};

struct RowHead {
    int kind;            // RowKind
    int hotkey;          // 徽章字符的 ASCII 码（0 = 不显示）
    int usage;           // 历史使用次数
    unsigned titleLen;   // 标题 utf8 字节数
    unsigned subLen;     // 副标题 utf8 字节数
    unsigned iconLen;    // 图标来源路径 utf8 字节数（0 = 用文字徽标）
};

struct StateSlot {
    int selected;        // 当前选中行下标
    int hotkeyMode;      // 0=Alt+数字 1=Alt+字母 2=关闭
    int rowCount;        // 可见行数
    int centerWake;      // 1=屏幕居中唤醒 0=跟随鼠标
    unsigned seq;        // 状态序号
    unsigned textLen;    // 文本 utf8 字节数
    unsigned rowsLen;    // 行区字节数
    char payload[kTextMax + kRowsMax];
};
#pragma pack(pop)

struct Bridge {
    // 请求环：生产者（渲染器）只写 reqHead，消费者（大脑）只写 reqTail
    volatile LONG reqHead;
    volatile LONG reqTail;
    Req req[kReqRingSize];
    // 状态：大脑写 stateSeq + 对应槽位，渲染器按 seq 取槽
    volatile LONG stateSeq;
    StateSlot state[kStateSlots];
    // 命令环：大脑写 cmdHead，渲染器读 cmdTail（显示 / 隐藏 / 退出）
    volatile LONG cmdHead;
    volatile LONG cmdTail;
    int cmd[8];
};

}  // namespace slintbridge
