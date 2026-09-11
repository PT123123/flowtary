// ============================================================
// Flowtary — Windows 极简全局搜索启动器
//   Alt+Space 唤醒 | d/f Everything 搜索 | 网页前缀跳转 | 程序启动
//   纯 Win32 + GDI，零第三方依赖
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include "filedlg_jump.h"
#include "version.h"  // FT_APPICON_ID（应用图标资源）、版本号等
#include <imm.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <process.h>
#include <new>

#include <algorithm>
#include <map>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>
#include <urlmon.h>
#include <wininet.h>

// comctl6：让公共控件（编辑框滚动条/组合框）走现代主题，配合 DarkMode_* 变体变暗
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "urlmon.lib")

// ---------------- Everything IPC（官方 SDK 协议，Everything 1.3+） ----------------
namespace ev {
constexpr DWORD kCopyDataQueryW = 2;  // EVERYTHING_IPC_COPYDATAQUERYW
constexpr DWORD kReplyBase      = 0x46540000;  // 回复消息基值，低 16 位放查询序号
constexpr DWORD kFlagFolder     = 0x1;
constexpr DWORD kMaxResults     = 10;

#pragma pack(push, 1)
struct Query {
    DWORD reply_hwnd;
    DWORD reply_copydata_message;
    DWORD search_flags;
    DWORD offset;
    DWORD max_results;
    WCHAR search_string[1];
};
struct Item {
    DWORD flags;
    DWORD filename_offset;
    DWORD path_offset;
};
struct List {
    DWORD totfolders, totfiles, totitems;
    DWORD numfolders, numfiles, numitems;
    DWORD offset;
    Item items[1];
};
#pragma pack(pop)
}  // namespace ev

// ---------------- 汉字拼音转换（使用 cpp-pinyin 库） ----------------
#include <cpp-pinyin/Pinyin.h>
#include <cpp-pinyin/ManTone.h>
#include <cpp-pinyin/G2pglobal.h>

static std::unique_ptr<Pinyin::Pinyin> g_pinyin;

static std::string WStringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

static std::wstring Utf8ToWString(const std::string& str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), result.data(), size);
    return result;
}

static std::wstring ToPinyin(const std::wstring& hanzi) {
    if (!g_pinyin) return hanzi;
    std::string utf8 = WStringToUtf8(hanzi);
    auto res = g_pinyin->hanziToPinyin(utf8, Pinyin::ManTone::Style::NORMAL, Pinyin::Error::Default, false, false, false);
    std::string pinyin;
    for (const auto& r : res) {
        if (!r.error && !r.pinyin.empty()) {
            pinyin += r.pinyin;
        } else if (!r.hanzi.empty()) {
            pinyin += r.hanzi;
        }
    }
    return Utf8ToWString(pinyin);
}

// ---------------- 网页指令规则（可在设置中编辑，存注册表） ----------------
// 每条规则一行：前缀 + 空格 + URL 模板（{q} 为关键词占位符）
struct WebCmd {
    std::wstring prefix;
    std::wstring urlTemplate;
};

static const WCHAR* kDefaultRulesText =
    L"bd https://www.baidu.com/#ie=UTF-8&wd={q}\r\n"
    L"bili https://search.bilibili.com/all?keyword={q}\r\n"
    L"xhs https://www.xiaohongshu.com/search_result?keyword={q}\r\n"
    L"zhihu https://www.zhihu.com/search?q={q}\r\n"
    L"douban https://www.douban.com/search?q={q}\r\n"
    L"gh https://github.com/search?q={q}\r\n"
    L"google https://www.google.com/search?q={q}";

static std::wstring DefaultRulesText() { return kDefaultRulesText; }

// ---------------- 主题系统 ----------------
// 每个预设统一定义：透明度、输入框/列表字号、主窗配色（含可选渐变+星点）、
// 菜单配色、设置窗配色。主题应用于主窗、弹出菜单、设置窗口的全部可样式化元素。
struct Theme {
    const WCHAR* name;   // 显示名
    bool dark;           // 菜单 uxtheme 用深色（false=浅色主题菜单）
    BYTE alpha;          // 主窗/设置窗透明度 0-255（255=不透明）
    int fontInput;       // 输入框逻辑字号
    int fontList;        // 列表逻辑字号
    COLORREF bg, bg2;    // 主窗背景（bg2 != bg 时垂直渐变）
    COLORREF text;       // 主文字
    COLORREF sub;        // 副标题（路径）
    COLORREF hintText;   // 占位/提示文字
    COLORREF selBg;      // 选中行背景
    COLORREF divider;    // 分隔线
    COLORREF menuBg;     // 菜单/弹出列表背景
    COLORREF menuHi;     // 菜单/控件选中高亮
    COLORREF editBg;     // 设置窗规则编辑框背景
    COLORREF border;     // GDI 描边回退（同时作为默认描边色）
    bool stars;          // 星空点点缀

    // —— 1. 形状几何（逻辑 px，绘制时经 S() 缩放）——
    int radiusWindow = 10;  // 窗口圆角
    int radiusCard   = 8;   // 卡片/结果面板圆角
    int radiusButton = 6;   // 按钮圆角
    int radiusInput  = 6;   // 输入框圆角
    int radiusSmall  = 3;   // 小控件（徽章/勾选/滑块）圆角
    int borderWidth  = 1;   // 描边粗细：0 或 1
    bool capsuleButtons = false;  // 胶囊按钮：radius = 控件半高

    // —— 2. 颜色系统（扩展）——
    COLORREF accent   = RGB(64, 134, 196);  // 主强调色（按钮/选中/高亮/进度）
    COLORREF accent2  = RGB(94, 162, 224);  // 次强调（hover/focus）
    COLORREF bgCard   = RGB(28, 28, 28);    // 卡片/结果面板背景（> 窗体 bg）
    COLORREF bgInput  = RGB(24, 24, 24);    // 输入框背景（> 卡片 bg）
    COLORREF textTitle = RGB(255, 255, 255); // 标题级文字
    COLORREF textBody  = RGB(235, 235, 235); // 正文（≈原 text）
    COLORREF textSec   = RGB(150, 150, 150); // 次要（≈原 sub）
    COLORREF textDis   = RGB(110, 110, 110); // 禁用/占位（≈原 hintText）
    COLORREF statusOK   = RGB(76, 175, 80);  // 成功绿
    COLORREF statusWarn = RGB(245, 166, 35); // 警告橙
    COLORREF statusErr  = RGB(229, 83, 75);  // 错误红

    // —— 3. 阴影/层级（逻辑 px + alpha 0-255）；固定 3 组可复用参数 ——
    int shWinX = 4, shWinY = 5, shWinBlur = 16, shWinA = 51; COLORREF shWinColor = RGB(0, 0, 0);
    int shCardX = 2, shCardY = 3, shCardBlur = 10, shCardA = 30; COLORREF shCardColor = RGB(0, 0, 0);
    int shHoverX = 5, shHoverY = 8, shHoverBlur = 20, shHoverA = 40; COLORREF shHoverColor = RGB(0, 0, 0);
    int shInset = 6;  // 内阴影深度（输入凹陷/玻璃高光用，统一弱值）

    // —— 7. 动效 ——
    int animMs = 200;    // 过渡时长 150-350，ease-out
    int animCurve = 0;   // 0=ease-out-cubic，1=ease-out-quad

    // —— 9. 图标与装饰 ——
    bool accentStrip = true;   // 顶部强调色条
    bool cornerGlow  = false;  // 低透明度角部辉光
    bool iconFilled  = false;  // 图标风格：false=线性描边 / true=填充
};

static Theme kThemes[] = {
    {L"黑色简洁", true, 255, 16, 13,
     RGB(0, 0, 0), RGB(0, 0, 0), RGB(255, 255, 255), RGB(140, 140, 140),
     RGB(150, 150, 150), RGB(51, 51, 51), RGB(34, 34, 34), RGB(28, 28, 28),
     RGB(51, 51, 51), RGB(24, 24, 24), RGB(80, 80, 80), false},
    {L"透明Mac黑暗", true, 224, 16, 13,
     RGB(16, 16, 16), RGB(12, 12, 12), RGB(255, 255, 255), RGB(153, 153, 153),
     RGB(135, 135, 135), RGB(42, 42, 42), RGB(33, 33, 33), RGB(28, 28, 28),
     RGB(51, 51, 51), RGB(18, 18, 18), RGB(85, 85, 85), false},
    {L"透明蓝色", true, 224, 16, 13,
     RGB(26, 52, 96), RGB(12, 23, 48), RGB(234, 243, 255), RGB(127, 163, 201),
     RGB(110, 143, 186), RGB(31, 58, 92), RGB(40, 72, 110), RGB(20, 39, 63),
     RGB(31, 58, 92), RGB(15, 30, 54), RGB(63, 107, 150), false},
    {L"星空风格", true, 250, 17, 14,
     RGB(12, 16, 48), RGB(4, 5, 15), RGB(232, 236, 255), RGB(138, 146, 200),
     RGB(112, 119, 173), RGB(30, 36, 72), RGB(40, 48, 92), RGB(13, 18, 38),
     RGB(30, 36, 72), RGB(9, 13, 32), RGB(74, 86, 160), true},
    {L"日出浅白", false, 255, 16, 13,
     RGB(244, 244, 244), RGB(244, 244, 244), RGB(26, 26, 26), RGB(110, 110, 110),
     RGB(155, 160, 166), RGB(223, 223, 223), RGB(208, 208, 208), RGB(255, 255, 255),
     RGB(228, 228, 228), RGB(243, 243, 243), RGB(196, 196, 196), false,
     10, 8, 6, 6, 3, 1, false,
     RGB(0x3B,0x7E,0xA8), RGB(0x63,0xA8,0xD4),
     RGB(255,255,255), RGB(252,252,252),
     RGB(26,26,26), RGB(26,26,26), RGB(110,110,110), RGB(155,160,166),
     RGB(0x2E,0x9E,0x5B), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     4, 5, 16, 51, RGB(0x82,0x9C,0xB3),
     5, 8, 20, 40, RGB(0x82,0x9C,0xB3),
     2, 3, 10, 30, RGB(0x82,0x9C,0xB3),
     6, 220, 0, true, false, false},

    // 6. Ice-Quartz（浅冷色，真实 Mica / Win10 回退哑光）
    {L"Ice-Quartz", false, 255, 16, 13,
     RGB(0xEE,0xF3,0xF7), RGB(0xEE,0xF3,0xF7), RGB(0x1A,0x27,0x33), RGB(0x64,0x74,0x84),
     RGB(0x9A,0xA8,0xB6), RGB(0xDD,0xE6,0xEE), RGB(0xC8,0xD6,0xE2), RGB(0xFF,0xFF,0xFF),
     RGB(0xE7,0xEE,0xF3), RGB(0xF4,0xF8,0xFB), RGB(0xB9,0xC9,0xD8), false,
     10, 8, 6, 6, 3, 1, false,
     RGB(0x3B,0x7E,0xA8), RGB(0x63,0xA8,0xD4),
     RGB(0xFF,0xFF,0xFF), RGB(0xF4,0xF8,0xFB),
     RGB(0x1A,0x27,0x33), RGB(0x1A,0x27,0x33), RGB(0x64,0x74,0x84), RGB(0x9A,0xA8,0xB6),
     RGB(0x2E,0x9E,0x5B), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     4, 5, 16, 51, RGB(0x82,0x9C,0xB3),
     5, 8, 20, 40, RGB(0x82,0x9C,0xB3),
     2, 3, 10, 30, RGB(0x82,0x9C,0xB3),
     6, 220, 0, true, false, false},

    // 7. Obsidian（极深暗黑工具风，真实 Mica + 微弱蓝辉光）
    {L"Obsidian", true, 255, 16, 13,
     RGB(0x0D,0x0F,0x14), RGB(0x0D,0x0F,0x14), RGB(0xFF,0xFF,0xFF), RGB(0x88,0x96,0xB0),
     RGB(0x50,0x5A,0x6E), RGB(0x1A,0x1E,0x27), RGB(0x2C,0x34,0x42), RGB(0x1A,0x1E,0x27),
     RGB(0x10,0x13,0x18), RGB(0x12,0x15,0x1C), RGB(0x3A,0x44,0x55), false,
     8, 6, 5, 5, 3, 1, false,
     RGB(0x40,0x86,0xC4), RGB(0x5E,0xA2,0xE0),
     RGB(0x1A,0x1E,0x27), RGB(0x12,0x15,0x1C),
     RGB(0xFF,0xFF,0xFF), RGB(0xFF,0xFF,0xFF), RGB(0x88,0x96,0xB0), RGB(0x50,0x5A,0x6E),
     RGB(0x3E,0xC0,0x7A), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     2, 3, 12, 89, RGB(0,0,0),
     0, 0, 8, 20, RGB(0,0,0),
     1, 2, 6, 22, RGB(0,0,0),
     6, 200, 0, true, true, false},

    // 8. Honey-Amber（暖暗色，真实 Mica / Win10 回退哑光）
    {L"Honey-Amber", true, 255, 16, 13,
     RGB(0x1C,0x19,0x17), RGB(0x1C,0x19,0x17), RGB(0xF8,0xE9,0xD7), RGB(0xB8,0xA4,0x8C),
     RGB(0x7E,0x6E,0x5A), RGB(0x2D,0x28,0x23), RGB(0x3E,0x36,0x2E), RGB(0x2D,0x28,0x23),
     RGB(0x18,0x15,0x12), RGB(0x22,0x1E,0x19), RGB(0x4A,0x40,0x34), false,
     7, 8, 7, 7, 3, 1, false,
     RGB(0xD4,0x80,0x38), RGB(0xE8,0xA2,0x5C),
     RGB(0x2D,0x28,0x23), RGB(0x22,0x1E,0x19),
     RGB(0xF8,0xE9,0xD7), RGB(0xF8,0xE9,0xD7), RGB(0xB8,0xA4,0x8C), RGB(0x7E,0x6E,0x5A),
     RGB(0x6F,0xCF,0x8A), RGB(0xE8,0xA2,0x5C), RGB(0xE5,0x6B,0x4B),
     3, 4, 14, 66, RGB(0x0A,0x08,0x06),
     1, 2, 7, 24, RGB(0x0A,0x08,0x06),
     1, 2, 7, 24, RGB(0x0A,0x08,0x06),
     6, 200, 0, true, false, false},
};

// 主题微调快照（透明度/圆角半径），随主题索引一一对应，持久化到注册表 ThemeTune blob
struct ThemeTune {
    int alpha = 255;     // 透明度 0-255（影响弹窗等离屏层）
    int radius = 10;     // 窗体圆角（卡片/按钮/输入按比例推导）
};
static std::vector<ThemeTune> sTune;      // 当前生效值（滑块实时预览会改 kThemes，这里只作回退/读写中转）
static std::vector<ThemeTune> sTuneSaved;  // 打开设置窗口时的快照（取消时用它回退）

// 星空风格的星星（相对主窗客户区，logical；点亮后每帧绘制）
struct Star {
    int x, y;    // 位置
    int s;       // 大小 1-2
    BYTE v;      // 亮度基准
};

// ---------------- 数据结构 ----------------
struct Program {
    std::wstring name;    // 显示名称
    std::wstring path;    // .lnk 或 .exe 完整路径（执行用）
    std::wstring target;  // 快捷方式解析出的目标（工作目录用）
    std::wstring pinyin; // 名称的拼音（用于拼音搜索）
    bool startMenu = false;
    HICON icon = nullptr;
    bool iconTried = false;
};

struct Row {
    // Reveal：搜索框里直接粘贴了「文件」路径时的直达项 —— 回车不启动该文件，
    // 而是在资源管理器中打开其所在文件夹并选中它（action 为文件完整路径）。
    enum Kind { File, Folder, Web, Prog, EvFallback, Hint, Group, Shell, Window, Top, Capture, Reveal };
    Kind kind = Hint;
    std::wstring title;
    std::wstring sub;     // 路径 / URL 说明
    std::wstring action;  // 打开目标；EvFallback 时为 Everything 命令行参数；
                          // Shell 时为命令行文本；Window 时为 HWND 十六进制串
    Program* prog = nullptr;
    // 一键组（Row::Group）：groupKill=false 启动 groupTargets 里的文件；
    // groupKill=true 结束 groupTargets 里的进程名
    bool groupKill = false;
    std::vector<std::wstring> groupTargets;
    // —— Shell 命令（kind=Shell）：整条命令行见 action ——
    // —— 窗口切换（kind=Window）——
    HWND winHwnd = nullptr;        // 目标窗口句柄
    std::wstring procName;         // 进程名（副标题显示）
    bool winIsGroup = false;       // 是否为同进程分组头
    std::vector<HWND> winMembers;  // 分组头：成员窗口列表
};

// 一键组：关键字 → 目标列表（启动组存文件路径，关闭组存进程名）
struct CmdGroup {
    std::wstring key;                   // 关键字（小写存储，比较时忽略大小写）
    std::vector<std::wstring> targets;
};

enum class Mode { None, Everything, Web, Programs, Shell, Window, Top, Capture };

// 窗口枚举缓存项
struct WinInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    std::wstring className;
    DWORD pid = 0;
    bool isUwp = false;
};

struct App {
    HWND hwnd = nullptr;
    HFONT fInput = nullptr;
    HFONT fList = nullptr;
    HBRUSH brDivider = nullptr;
    HBRUSH brEditBg = nullptr;   // 设置窗口编辑框背景（跟随主题 editBg）
    HBRUSH brSettingsBg = nullptr;  // 设置窗口客户区背景（跟随主题 bg）
    HINSTANCE inst = nullptr;
    float scale = 1.0f;          // 全局比例：max(DPI, 屏幕物理高度/1080)，不写死像素
    bool centerWake = true;    // 唤醒位置：true=屏幕居中，false=跟随鼠标
    bool dwmBorderOk = false;  // DWM 描边可用（否则 Paint 回退 GDI 描边）
    int hotkeyMode = 0;        // 结果项快捷键方案：0=Alt+数字, 1=Alt+字母, 2=关闭
    bool startupWanted = false;// 设置窗“开机自动启动”勾选状态（自绘复选框用变量驱动，
                               // 因为 BS_OWNERDRAW 按钮的 Button_GetCheck/SetCheck 不生效）
    bool startupSaved = false; // 设置窗打开时的初始值（取消时回退）
    bool hotkeyWake = true;    // 唤起快捷键总开关（托盘菜单切换；关闭时不再注册 Alt+Space 等）
    int hotkeyModeSaved = 0;   // 同上，结果项快捷键方案（取消时回退）
    int settingsTab = 0;        // 设置窗当前 Tab：0=常规, 1=网页规则, 2=主题, 3=一键, 4=搜索权重, 5=排除路径, 6=Shell 与窗口, 7=命令, 8=截图工具（关闭后仍记住上次选择）
    int themeIdx = 0;          // 当前主题索引（设置窗切换后、保存前为暂存值）
    int themeSaved = 0;        // 设置窗打开时的初始主题（取消时回退）
    bool beautify = true;      // 界面美化：暗色标题栏 + 圆角窗口 + 强制暗色菜单（默认开）
    bool beautifySaved = true; // 设置窗打开时的初始值（取消时回退）
    bool antiGhost = false;    // 抗残影双缓冲（实验性）：开启 WS_EX_COMPOSITED 整窗双缓冲，消除切换 Tab 残影
    bool antiGhostSaved = false; // 设置窗打开时的初始值（取消时回退）
    bool fdjEnabled = true;    // 文件对话框跳转总开关（默认开；UI 在设置「常规」Tab，不再放托盘菜单）
    bool fdjEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
    bool topEnabled = true;    // top 命令开关（默认开：空格+top 回车置顶/取消置顶当前窗口）
    bool topEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
    bool cmdEnabled = true;    // cmd 命令开关（默认开：空格+cmd 执行 Shell 命令，原 "> 命令"）
    bool cmdEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
    bool winEnabled = true;    // w 命令开关（默认开：空格+w 切换窗口，原 "< 关键词"）
    bool winEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
    // ---- 点击加权排序（性能参数在设置「搜索权重」Tab） ----
    bool weightEnabled = true; // 点击权重记忆总开关（默认开）
    int weightFlush = 1;       // 写盘时机：0=每次点击立即写入, 1=延迟合并写入, 2=仅退出时写入
    int weightMaxEntries = 5000;  // 权重条目上限（内存 + 磁盘均按此裁剪）
    bool weightEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
    int weightFlushSaved = 1;
    int weightMaxSaved = 5000;
    std::wstring evTermKey;    // 当前 Everything 查询对应的标准化有效搜索词（回复到达时用于按权重重排）
    // 权重存储：有效搜索词（去首尾空格+小写） → 文件完整绝对路径 → 点击次数。
    // 不同搜索词之间互相独立；以完整路径区分同名文件。
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, int>> weights;
    size_t weightCount = 0;    // weights 中（词,路径）对总数
    bool weightsDirty = false; // 有未写盘的权重变更
    bool startingUp = true;    // 程序扫描尚未完成：托盘提示/右键菜单显示「正在启动中」
    Theme* theme = nullptr;
    std::vector<Star> stars;   // 星空主题星点坐标

    HFONT fontPool[15] = {};   // 逻辑字号 10..24 预建字体池（11-24），永不中途删除
    HBRUSH brMenuBg = nullptr;   // 弹出菜单背景（跟随主题重建）

    // —— 主题扩展刷子/笔（随 ApplyTheme 重建）——
    HBRUSH brCardBg = nullptr;   // 卡片/结果面板背景
    HBRUSH brInputBg = nullptr;  // 输入框背景
    HBRUSH brAccent = nullptr;   // 主强调色
    HBRUSH brAccent2 = nullptr;  // 次强调（hover/focus）
    HBRUSH brTextDis = nullptr;  // 禁用/占位文字
    HBRUSH brStatusOK = nullptr, brStatusWarn = nullptr, brStatusErr = nullptr; // 状态色
    HPEN penBorder = nullptr;    // 描边笔（DWM 描边不可用时 GDI 回退）
    int hoverRow = -1;           // 鼠标悬停的结果行（Phase D 动画 tween 驱动）
    int pressRow = -1;           // 鼠标按下的结果行
    double hoverT = 0, pressT = 0; // 悬停/按下动画进度 0..1（Phase D tween 写入）

    std::vector<WebCmd> webCmds;  // 网页跳转规则（设置可编辑）
    std::vector<CmdGroup> groupsLaunch;  // 一键启动组：关键字 → 文件列表
    std::vector<CmdGroup> groupsKill;    // 一键关闭组：关键字 → 进程名列表

    // 排除路径（设置可编辑）：用户原文，每行一条（保留给设置页编辑框原样回显）。
    // 命中 Everything 结果路径或程序 .lnk/.exe 完整路径时丢弃该项。
    // 仅在 Everything 模式（d/f）与程序模式生效；网页/一键组不参与过滤。
    std::vector<std::wstring> excludePaths;
    // excludePaths 的归一化镜像（反斜杠统一 + 去尾部分隔符 + 全小写），与前者下标一一对应。
    // 匹配时只看这一份，避免用户填的大小写/尾部反斜杠写法导致静默失效。
    std::vector<std::wstring> excludePathsNorm;

    std::wstring text;
    size_t caret = 0;
    size_t selStart = 0;
    size_t selEnd = 0;
    bool dragging = false;       // 输入框内左键拖拽选区中
    int scrollX = 0;
    bool caretOn = true;
    std::wstring compText;     // IME 组合中（未上屏）文本：内联渲染，不弹系统浮窗

    Mode mode = Mode::None;
    std::vector<Row> items;
    int sel = 0;

    std::wstring evQuery;      // 待发往 Everything 的完整查询串（含 file:/folder:）
    DWORD expectReply = 0;     // 期待的结果消息 dwData
    DWORD serial = 0;

    std::vector<Program> programs;
    std::wstring everythingExe;
    std::wstring hotkeyName = L"Alt+Space";
    HHOOK hWakeHook = nullptr;     // 低级键盘钩子：让首选唤起组合优先于其他程序的 RegisterHotKey
    bool hotkeyRegistered = false; // 唤起热键是否已成功 RegisterHotKey（失败由重试定时器回收）
    int wakeIndex = -1;            // 当前注册的唤起组合下标（-1=未注册；0=首选 Alt+Space）

    NOTIFYICONDATAW nid{};     // 托盘图标
    HICON hTrayIcon = nullptr;
    HICON hAppIcon = nullptr;  // 窗口图标（与托盘同款，字体绘制，不依赖 .ico 资源）
    HWND hSettings = nullptr;  // 设置窗口
    HWND prevForeground = nullptr;  // 唤醒前的前台窗口（top 命令的目标窗口）
    UINT msgTaskbarCreated = 0;

    // —— Shell 命令 & 窗口切换配置（设置「Shell 与窗口」Tab）——
    int shellType = 0;            // 0=cmd, 1=powershell, 2=git-bash, 3=自定义
    std::wstring shellCustomPath; // 自定义 shell 程序完整路径
    std::wstring shellCustomArgs; // 自定义启动参数模板（{c} 替换为命令）
    int shellDefaultCwd = 0;      // 0=用户目录, 1=系统默认(System32), 2=桌面
    bool shellShowWindow = true;   // 前台显示输出窗口（否则后台静默执行）默认勾选
    // 窗口切换
    bool winGroupProc = true;     // 合并同进程窗口
    bool winShowUwp = true;       // 显示 UWP 应用窗口
    bool winShowProc = true;      // 副标题显示进程名
    int winCacheSec = 5;          // 窗口枚举缓存刷新间隔（秒）
    // 取消时回退用
    int shellTypeSaved = 0;
    std::wstring shellCustomPathSaved;
    std::wstring shellCustomArgsSaved;
    int shellDefaultCwdSaved = 0;
    bool shellShowWindowSaved = true;
    bool winGroupProcSaved = true;
    bool winShowUwpSaved = true;
    bool winShowProcSaved = true;
    int winCacheSecSaved = 5;
    // 窗口枚举缓存（避免频繁 EnumWindows 卡顿）
    std::vector<WinInfo> windowCache;
    DWORD windowCacheTick = 0;

    // —— 截图命令（ScreenCapture.exe）——
    bool captureEnabled = true;        // ss 命令开关（默认开）
    bool captureEnabledSaved = true;   // 取消时回退用
    std::wstring captureExe;           // ScreenCapture.exe 完整路径
    std::wstring captureArgs;          // 本次启动的命令行参数
} g;

constexpr int kBaseW = 600, kBaseInputH = 56, kBaseRowH = 30, kBasePad = 12;
constexpr int kFontMin = 11, kFontMax = 24;  // 字体池逻辑字号范围
constexpr UINT_PTR kTimerDebounce = 1;
constexpr UINT_PTR kTimerBlink = 2;
constexpr UINT_PTR kTimerBalloon = 3;
constexpr UINT_PTR kTimerWeightSave = 4;   // 点击权重延迟合并写盘
constexpr UINT_PTR kTimerTween = 5;        // 补间动画（与光标闪烁 kTimerBlink 区分，互不干扰）
constexpr UINT_PTR kTimerHotkeyRetry = 6;  // 唤起快捷键被占用时定时重试回收
constexpr int kDebounceMs = 120;
constexpr int kWeightSaveDelayMs = 3000;   // 延迟合并写盘的等待时间
constexpr int WM_APP_TRAY = WM_APP + 1;
constexpr int WM_APP_PROGRAMS_READY = WM_APP + 2;  // 工作线程扫描完成，回主线程接管结果
constexpr int WM_APP_QUIT = WM_APP + 3;  // 新版本接管：通知旧实例退出
constexpr int IDM_SETTINGS = 2001;
constexpr int IDM_EXIT = 2002;
constexpr int IDM_REFRESH = 2003;   // 托盘菜单：刷新应用缓存
constexpr int IDM_WAKE_HOTKEY = 2004;  // 托盘菜单：开启/关闭唤起快捷键
constexpr int IDM_OPEN = 2011;      // 结果右键菜单：打开
constexpr int IDM_OPENLOC = 2012;   // 结果右键菜单：打开所在文件夹
constexpr int IDM_COPYPATH = 2013;  // 结果右键菜单：复制路径
constexpr int IDM_RUNAS = 2014;     // 结果右键菜单：以管理员模式打开
constexpr int IDM_WIN_SWITCH = 2021;  // 窗口项：切换到此窗口
constexpr int IDM_WIN_CLOSE = 2022;   // 窗口项：关闭窗口
constexpr int IDM_WIN_KILL = 2023;    // 窗口项：结束进程
constexpr int IDM_SHELL_ADMIN = 2024; // Shell 项：以管理员运行
constexpr int IDM_COPYTITLE = 2025;   // 复制窗口标题
constexpr int IDM_COPYPROC = 2026;    // 复制进程名
constexpr int IDM_OPENCMD = 2027;     // 结果右键菜单：用此路径（所在目录）打开命令行
constexpr int IDM_OPENFILE = 2028;    // 路径直达行：打开文件本身

// 结果项快捷键方案：
//   方案0 Alt+数字：按结果优先级自上而下分配 1..9,0（列表最多 10 行，1=最高优先级）
//   方案1 Alt+字母：按结果优先级自上而下分配 A..J（列表最多 10 行，A=最高优先级）
static const WCHAR* kHotkeyOrder = L"1234567890";
static const WCHAR* kHotkeyLetterOrder = L"ABCDEFGHIJ";

// 唤起快捷键候选（依次尝试）：Alt+Space -> Alt+Q -> Ctrl+Alt+Space
// （避免与其他启动器冲突导致完全不可用；注册成功即停）
static const struct { UINT mod, vk; const WCHAR* name; } kWakeHotkeys[] = {
    {MOD_ALT | MOD_NOREPEAT, VK_SPACE, L"Alt+Space"},
    {MOD_ALT | MOD_NOREPEAT, 'Q', L"Alt+Q"},
    {MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE, L"Ctrl+Alt+Space"},
};

// 前置声明（后文定义，ApplyTheme 需要）
static void EnableDarkMenus();
static const CmdGroup* FindGroup(const std::vector<CmdGroup>& gs, const std::wstring& key);
static int KillProcessesByName(const std::wstring& name);
static void LayoutSettings(HWND h);
static void ApplyDarkTitlebar(HWND h);
static void Layout();
static void RepaintNow();
static void GenerateStars();
static BOOL CALLBACK RefreshChildFont(HWND child, LPARAM lp);
// 点击加权排序（定义在「点击权重」小节）
static std::wstring NormalizeSearchTerm(const std::wstring& s);
static int GetClickWeight(const std::wstring& term, const std::wstring& path);
static void RecordClickWeight(const Row& r);
static void SaveClickWeightsNow();
static void TrayBalloon(const std::wstring& title, const std::wstring& msg);
static HWND TopTargetWindow();  // top 命令的目标窗口（唤醒前的前台窗口）
static void ShowTopToast(HWND target, const std::wstring& text);  // top 命令轻量 toast

// 行 → 可触发序号（Hint 行不算）：返回第几个可执行项（用于分配数字徽章），-1=不可触发
static int HotkeySeq(int itemIdx) {
    if (itemIdx < 0 || itemIdx >= (int)g.items.size()) return -1;
    if (g.items[itemIdx].kind == Row::Hint) return -1;
    int n = 0;
    for (int i = 0; i < itemIdx; ++i)
        if (g.items[i].kind != Row::Hint) ++n;
    return n;
}
static WCHAR HotkeyDigit(int itemIdx) {
    int seq = HotkeySeq(itemIdx);
    return (seq >= 0 && seq < (int)wcslen(kHotkeyOrder)) ? kHotkeyOrder[seq] : 0;
}
static WCHAR HotkeyLetter(int itemIdx) {
    int seq = HotkeySeq(itemIdx);
    return (seq >= 0 && seq < (int)wcslen(kHotkeyLetterOrder)) ? kHotkeyLetterOrder[seq] : 0;
}
// 按当前方案返回该行结果项的快捷键徽章字符（mode=2 关闭时返回 0，不显示徽章）
static WCHAR HotkeyChar(int itemIdx) {
    if (g.hotkeyMode == 0) return HotkeyDigit(itemIdx);
    if (g.hotkeyMode == 1) return HotkeyLetter(itemIdx);
    return 0;
}
static const WCHAR* HotkeyModeText(int m) {
    return m == 0 ? L"Alt + 数字（1–9,0）"
         : m == 1 ? L"Alt + 字母（A–J）"
                  : L"关闭";
}

// ---------------- 工具函数 ----------------
// 逻辑像素 → 物理像素：全部尺寸走统一比例，不写死像素点
inline int S(int v) { return MulDiv(v, (int)(g.scale * 1000 + 0.5f), 1000); }

// 比例 = max(系统 DPI 缩放, 屏幕物理高度/1080p)。
// 高分辨率小屏（如 2.8K 13 寸）上按物理分辨率等比放大，避免界面显得过小；
// 换屏唤醒时重新计算，字体随之重建。
// ---------------- 字体池 ----------------
// 逻辑字号 11..24 的字体在比例变化时整体重建；g.fInput/g.fList 只是池内指针。
// 主题切换只换指针，不再 DeleteObject，避免设置窗口子控件仍引用旧字体造成悬垂句柄。
static inline int FontSlot(int logSize) {
    return (std::max)(kFontMin, (std::min)(kFontMax, logSize)) - kFontMin;
}
static HFONT MakeFont(int logSize) {
    return CreateFontW(-S(logSize), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}
static void RebuildFontPool() {
    for (int i = 0; i < kFontMax - kFontMin + 1; ++i) {
        if (g.fontPool[i]) DeleteObject(g.fontPool[i]);
        g.fontPool[i] = MakeFont(kFontMin + i);
    }
}
static void ResetFonts() {
    if (!g.theme) return;
    g.fInput = g.fontPool[FontSlot(g.theme->fontInput)];
    g.fList = g.fontPool[FontSlot(g.theme->fontList)];
}

// 比例 = max(系统 DPI 缩放, 屏幕物理高度/1080p)。
// 高分辨率小屏（如 2.8K 13 寸）上按物理分辨率等比放大，避免界面显得过小；
// 换屏唤醒时重新计算，字体随之重建。
static void UpdateScale(HMONITOR mon) {
    float s = GetDpiForSystem() / 96.0f;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        float h = (mi.rcMonitor.bottom - mi.rcMonitor.top) / 1080.0f;
        if (h > s) s = h;
    }
    if (s < 1.0f) s = 1.0f;
    if (s == g.scale && g.fontPool[0]) return;
    g.scale = s;
    RebuildFontPool();
    ResetFonts();
    if (g.hSettings) SendMessageW(g.hSettings, WM_CLOSE, 0, 0);  // 尺寸随比例变化，下次打开重建
}

// 强制整窗重绘并触发 DWM 重新合成：切换主题/美化时主动刷新可避免残影。
static void RefreshComposition(HWND h) {
    if (!h || !IsWindow(h)) return;
    RedrawWindow(h, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// 主题应用：字体指针、透明度、各刷子、菜单深色模式、星点，全部按预设重置
    static void ApplyTheme(bool repaint = true) {
    if (!g.theme) return;
    const Theme& t = *g.theme;
    g.fInput = g.fontPool[FontSlot(t.fontInput)];
    g.fList = g.fontPool[FontSlot(t.fontList)];
    if (g.hwnd) {
        if (g.brDivider) { DeleteObject(g.brDivider); g.brDivider = nullptr; }
        g.brDivider = CreateSolidBrush(t.divider);
    }
    if (g.brMenuBg) { DeleteObject(g.brMenuBg); g.brMenuBg = nullptr; }
    g.brMenuBg = CreateSolidBrush(t.menuBg);
    if (g.brEditBg) { DeleteObject(g.brEditBg); g.brEditBg = nullptr; }
    g.brEditBg = CreateSolidBrush(t.editBg);
    if (g.brSettingsBg) { DeleteObject(g.brSettingsBg); g.brSettingsBg = nullptr; }
    g.brSettingsBg = CreateSolidBrush(t.bg);
    // 主题扩展刷子/笔
    auto rebuild = [](HBRUSH& b, COLORREF c) {
        if (b) DeleteObject(b);
        b = CreateSolidBrush(c);
    };
    rebuild(g.brCardBg, t.bgCard);
    rebuild(g.brInputBg, t.bgInput);
    rebuild(g.brAccent, t.accent);
    rebuild(g.brAccent2, t.accent2);
    rebuild(g.brTextDis, t.textDis);
    rebuild(g.brStatusOK, t.statusOK);
    rebuild(g.brStatusWarn, t.statusWarn);
    rebuild(g.brStatusErr, t.statusErr);
    if (g.penBorder) DeleteObject(g.penBorder);
    g.penBorder = CreatePen(PS_SOLID, t.borderWidth, t.border);
    EnableDarkMenus();
    // 切换主题后同步标题栏明暗（深色↔浅色主题时标题栏要跟着变）
    if (g.hSettings && g.beautify) ApplyDarkTitlebar(g.hSettings);
    if (t.stars) GenerateStars();
    if (g.hSettings && repaint) {
        EnumChildWindows(g.hSettings, RefreshChildFont, 0);
        RefreshComposition(g.hSettings);  // 切换主题/美化后强制重合成（含 SWP_FRAMECHANGED），消除残影
    }
    if (g.hwnd && repaint) {
        Layout();
        RepaintNow();
    }
}

// 星空星点：固定种子生成，主题切换时刷新
static void GenerateStars() {
    g.stars.clear();
    unsigned seed = 0x9E3779B9u;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 16) & 0xFFFF;
    };
    for (int i = 0; i < 96; ++i) {
        Star st;
        st.x = 2 + (int)(rnd() % (kBaseW - 4));
        st.y = 2 + (int)(rnd() % (kBaseInputH + ev::kMaxResults * kBaseRowH - 6));
        st.s = (rnd() % 100) < 16 ? 2 : 1;
        st.v = (BYTE)(120 + rnd() % 135);  // 120..254 亮度
        g.stars.push_back(st);
    }
}

// 垂直渐变填充（bg!=bg2 时）
static void FillVGradient(HDC hdc, int x, int y, int w, int h, COLORREF c0, COLORREF c1) {
    if (w <= 0 || h <= 0) return;
    TRIVERTEX tv[2];
    GRADIENT_RECT gr{0, 1};
    tv[0].x = x; tv[0].y = y; tv[0].Red   = GetRValue(c0) << 8;
    tv[0].Green = GetGValue(c0) << 8; tv[0].Blue = GetBValue(c0) << 8; tv[0].Alpha = 0;
    tv[1].x = x + w; tv[1].y = y + h; tv[1].Red   = GetRValue(c1) << 8;
    tv[1].Green = GetGValue(c1) << 8; tv[1].Blue = GetBValue(c1) << 8; tv[1].Alpha = 0;
    GradientFill(hdc, tv, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

// ---------------- 主题扩展绘制辅助 ----------------
#pragma comment(lib, "msimg32.lib")  // AlphaBlend（半透明阴影/分割线/辉光）
#pragma comment(lib, "comctl32.lib") // 通用控件（设置窗微调滑块 TRACKBAR_CLASS）

static inline COLORREF Lighten(COLORREF c, int p) {
    int r = (GetRValue(c) * (100 + p)) / 100, g = (GetGValue(c) * (100 + p)) / 100, b = (GetBValue(c) * (100 + p)) / 100;
    return RGB((BYTE)(r > 255 ? 255 : r), (BYTE)(g > 255 ? 255 : g), (BYTE)(b > 255 ? 255 : b));
}
static inline COLORREF Darken(COLORREF c, int p) {
    int r = (GetRValue(c) * (100 - p)) / 100, g = (GetGValue(c) * (100 - p)) / 100, b = (GetBValue(c) * (100 - p)) / 100;
    return RGB((BYTE)r, (BYTE)g, (BYTE)b);
}
static inline COLORREF Blend(COLORREF a, COLORREF b, float t) {
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB((BYTE)r, (BYTE)g, (BYTE)bl);
}

// 圆角矩形填充（GDI path）
static void FillRoundRect(HDC h, const RECT& r, int rad, HBRUSH b) {
    if (rad <= 0 || r.right <= r.left || r.bottom <= r.top) { FillRect(h, &r, b); return; }
    int d = 2 * rad;
    BeginPath(h);
    RoundRect(h, r.left, r.top, r.right, r.bottom, d, d);
    EndPath(h);
    HBRUSH ob = (HBRUSH)SelectObject(h, b);
    FillPath(h);
    SelectObject(h, ob);
}
// 圆角矩形描边（GDI path）
static void StrokeRoundRect(HDC h, const RECT& r, int rad, HPEN p) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    int d = 2 * ((rad > 0) ? rad : 0);
    BeginPath(h);
    RoundRect(h, r.left, r.top, r.right, r.bottom, d, d);
    EndPath(h);
    HPEN op = (HPEN)SelectObject(h, p);
    StrokePath(h);
    SelectObject(h, op);
}

// 软外阴影：在 r 外侧偏移 (ox,oy) 画一张带 alpha 的圆角矩形（无模糊，靠低 alpha 模拟海拔）
static void PaintOutShadow(HDC h, const RECT& r, int rad, int ox, int oy, int /*blur*/, int a, COLORREF col) {
    RECT sr{r.left + ox, r.top + oy, r.right + ox, r.bottom + oy};
    int w = sr.right - sr.left, hh = sr.bottom - sr.top;
    if (w <= 0 || hh <= 0 || a <= 0) return;
    HDC tmp = CreateCompatibleDC(h);
    if (!tmp) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP dib = CreateDIBSection(tmp, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!dib) { DeleteDC(tmp); return; }
    HBITMAP o = (HBITMAP)SelectObject(tmp, dib);
    int rr = rad, cx0 = rr, cy0 = rr, cx1 = w - rr, cy1 = hh - rr;
    auto inside = [&](int x, int y) -> bool {
        if (x < cx0 && y < cy0) { if ((x - cx0)*(x - cx0) + (y - cy0)*(y - cy0) > rr*rr) return false; }
        else if (x > cx1 && y < cy0) { if ((x - cx1)*(x - cx1) + (y - cy0)*(y - cy0) > rr*rr) return false; }
        else if (x < cx0 && y > cy1) { if ((x - cx0)*(x - cx0) + (y - cy1)*(y - cy1) > rr*rr) return false; }
        else if (x > cx1 && y > cy1) { if ((x - cx1)*(x - cx1) + (y - cy1)*(y - cy1) > rr*rr) return false; }
        return true;
    };
    for (int y = 0; y < hh; ++y) {
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            bits[i].rgbRed = GetRValue(col); bits[i].rgbGreen = GetGValue(col); bits[i].rgbBlue = GetBValue(col);
            bits[i].rgbReserved = inside(x, y) ? (BYTE)a : 0;
        }
    }
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(h, sr.left, sr.top, w, hh, tmp, 0, 0, w, hh, bf);
    SelectObject(tmp, o); DeleteObject(dib); DeleteDC(tmp);
}

// 内阴影/高光：顶部内高光 + 底部内暗线，模拟凹陷/玻璃表面反光
static void PaintInsetShadow(HDC h, const RECT& r, int rad, COLORREF base) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH hi = CreateSolidBrush(Lighten(base, 16));
    HBRUSH lo = CreateSolidBrush(Darken(base, 12));
    RECT r1{r.left + rad, r.top + 1, r.right - rad, r.top + 2};
    RECT r2{r.left + rad, r.bottom - 2, r.right - rad, r.bottom - 1};
    FillRect(h, &r1, hi);
    FillRect(h, &r2, lo);
    DeleteObject(hi); DeleteObject(lo);
}

// 半透明分割线（避免纯黑/纯白硬线）
static void PaintDivider(HDC h, const RECT& r, COLORREF col, int a) {
    int w = r.right - r.left, hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0) return;
    HDC tmp = CreateCompatibleDC(h);
    if (!tmp) return;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    RGBQUAD* bits = nullptr;
    HBITMAP dib = CreateDIBSection(tmp, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0);
    if (!dib) { DeleteDC(tmp); return; }
    HBITMAP o = (HBITMAP)SelectObject(tmp, dib);
    for (int y = 0; y < hh; ++y)
        for (int x = 0; x < w; ++x) {
            int i = y * w + x;
            bits[i].rgbRed = GetRValue(col); bits[i].rgbGreen = GetGValue(col); bits[i].rgbBlue = GetBValue(col);
            bits[i].rgbReserved = (BYTE)a;
        }
    BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(h, r.left, r.top, w, hh, tmp, 0, 0, w, hh, bf);
    SelectObject(tmp, o); DeleteObject(dib); DeleteDC(tmp);
}

// ---------------- 主题动画：轻量补间引擎 ----------------
// 不干扰光标闪烁定时器（kTimerBlink=2）；所有过渡统一 ease-out，无 linear 无突兀。
struct Tween {
    double* val = nullptr;   // 被驱动的双精度字段（如 g.hoverT / g.pressT）
    double from = 0, to = 0;
    DWORD t0 = 0, dur = 200;
    int curve = 0;           // 0=ease-out-cubic（默认）, 1=ease-out-quad
    bool active = false;
};
static std::vector<Tween> sTweens;

static double EaseOut(double t, int curve) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    if (curve == 1) return 1 - (1 - t) * (1 - t);  // ease-out quad
    return 1 - pow(1 - t, 3);                       // ease-out cubic
}

static void StartTween(double* val, double to, int ms, int curve = 0) {
    if (!val) return;
    for (auto& w : sTweens) {
        if (w.val == val) {            // 同字段已存在 tween：从当前值平滑接管
            w.from = *val; w.to = to; w.t0 = GetTickCount();
            w.dur = (DWORD)(ms > 0 ? ms : 1); w.curve = curve; w.active = true;
            return;
        }
    }
    sTweens.push_back({val, *val, to, GetTickCount(), (DWORD)(ms > 0 ? ms : 1), curve, true});
    if (g.hwnd) SetTimer(g.hwnd, kTimerTween, 16, nullptr);  // ~60fps
}

static void TickTweens() {
    DWORD now = GetTickCount();
    bool any = false;
    for (auto& w : sTweens) {
        if (!w.active) continue;
        double p = (double)(now - w.t0) / w.dur;
        if (p >= 1) { *w.val = w.to; w.active = false; }
        else { *w.val = w.from + (w.to - w.from) * EaseOut(p, w.curve); any = true; }
    }
    if (g.hwnd) InvalidateRect(g.hwnd, nullptr, FALSE);  // 600px 双缓冲重绘很便宜
    if (!any) { sTweens.clear(); if (g.hwnd) KillTimer(g.hwnd, kTimerTween); }
}

// 设置窗口子控件字体刷新回调（主题字号变化时）
static BOOL CALLBACK RefreshChildFont(HWND child, LPARAM) {
    WCHAR cls[16]{};
    GetClassNameW(child, cls, 16);
    HFONT f = _wcsicmp(cls, L"Edit") == 0 ? g.fList : g.fInput;
    SendMessageW(child, WM_SETFONT, (WPARAM)f, TRUE);
    return TRUE;
}

static std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)std::towlower(c);
    return r;
}

static std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ---------------- 排除路径（通配符 * ?，大小写不敏感） ----------------
// 通配符匹配：* 匹配任意长（含空），? 匹配单个字符；其他字符大小写不敏感相等即匹配
static bool MatchWildcardI(const wchar_t* pat, const wchar_t* s) {
    // 经典 DP 匹配：pat[i..] vs s[j..]
    size_t pn = wcslen(pat), sn = wcslen(s);
    std::vector<std::vector<bool>> dp(pn + 1, std::vector<bool>(sn + 1, false));
    dp[0][0] = true;
    for (size_t i = 1; i <= pn; ++i) {
        if (pat[i - 1] == L'*') dp[i][0] = dp[i - 1][0];
        for (size_t j = 1; j <= sn; ++j) {
            wchar_t pc = pat[i - 1];
            if (pc == L'*')
                dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
            else if (pc == L'?')
                dp[i][j] = dp[i - 1][j - 1];
            else
                dp[i][j] = dp[i - 1][j - 1] &&
                           (std::towlower(pc) == std::towlower(s[j - 1]));
        }
    }
    return dp[pn][sn];
}

// 路径归一化：反斜杠统一为 '\\'、去除末尾分隔符、整体转小写，便于大小写不敏感比较
static std::wstring NormalizePathForExclude(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (auto c : s) {
        if (c == L'/') c = L'\\';
        r.push_back(c);
    }
    while (r.size() > 3 /* 保留盘符 C:\ */ && r.back() == L'\\') r.pop_back();
    for (auto& c : r) c = (wchar_t)std::towlower(c);
    return r;
}

// 通配符模式归一化：与路径同样规则；用户输入的 * 仍作 wildcard
static std::wstring NormalizePatternForExclude(const std::wstring& s) {
    return NormalizePathForExclude(s);
}

// 重建排除模式的归一化镜像。凡是改写 g.excludePaths 的地方（加载 / 恢复默认 / 设置页保存）
// 都必须调一次，否则匹配会退化成拿用户原文和小写路径比较。
static void RebuildExcludeNorm() {
    g.excludePathsNorm.clear();
    g.excludePathsNorm.reserve(g.excludePaths.size());
    for (const auto& p : g.excludePaths) g.excludePathsNorm.push_back(NormalizePatternForExclude(p));
}

// 路径是否被排除：与 g.excludePaths 任一模式匹配（模式不含通配符时按前缀匹配）
// 匹配一律大小写不敏感：np 已小写，模式取 g.excludePathsNorm 里对应的归一化副本
// （曾经直接拿用户原文比较，导致 `C:\$RECYCLE.BIN` 这类含大写的默认项永远不命中）。
static bool IsPathExcluded(const std::wstring& path) {
    if (g.excludePaths.empty() || path.empty()) return false;
    std::wstring np = NormalizePathForExclude(path);
    for (size_t i = 0; i < g.excludePaths.size(); ++i) {
        const std::wstring& raw = g.excludePaths[i];
        const std::wstring& pat =
            (i < g.excludePathsNorm.size()) ? g.excludePathsNorm[i] : raw;
        // 通配符判定看原文（归一化不改动 * / ?，两者等价，这里用原文更直观）
        bool wild = raw.find(L'*') != std::wstring::npos || raw.find(L'?') != std::wstring::npos;
        // 含 * / ? 走通配符，否则按「路径以前缀开头」匹配（直觉：填一个目录就排除整棵）
        if (wild) {
            if (MatchWildcardI(pat.c_str(), np.c_str())) return true;
        } else {
            size_t pn = pat.size();
            if (pn && np.size() >= pn && np.compare(0, pn, pat) == 0) {
                // 命中后还要确认是「目录边界」：完全相等，或紧随其后的字符是 '\\'
                if (np.size() == pn || np[pn] == L'\\') return true;
            }
        }
    }
    return false;
}

static bool EndsWithI(const std::wstring& s, const WCHAR* suf) {
    size_t n = wcslen(suf);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, suf) == 0;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 关键词 URL 编码（UTF-8 percent-encoding）
static std::string UrlEncodeUtf8(const std::wstring& q) {
    static const char* kHex = "0123456789ABCDEF";
    std::string u8 = WideToUtf8(q), out;
    out.reserve(u8.size() * 3);
    for (unsigned char c : u8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// 检测输入是否为网址：以 http:// https:// ftp:// ftps:// 开头，
// 或 www. 开头，或包含 ://（自定义协议），或无斜杠点分域名格式
static bool IsUrl(const std::wstring& s) {
    if (s.empty()) return false;
    if (_wcsnicmp(s.c_str(), L"http://", 7) == 0) return true;
    if (_wcsnicmp(s.c_str(), L"https://", 8) == 0) return true;
    if (_wcsnicmp(s.c_str(), L"ftp://", 6) == 0) return true;
    if (_wcsnicmp(s.c_str(), L"ftps://", 7) == 0) return true;
    if (_wcsnicmp(s.c_str(), L"www.", 4) == 0) return true;
    size_t pos = s.find(L"://");
    if (pos != std::wstring::npos && pos > 0) return true;
    // 无斜杠点分域名：xxx.yyy.zzz（有至少一个点，点不在首尾，无斜杠/空格）
    size_t dot = s.find(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot == s.size() - 1) return false;
    for (wchar_t c : s) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
            c == L'\\' || c == L'/') return false;
    }
    return true;
}

// 检测输入是否为「已存在的本地路径」：绝对路径（X:\ 或 \\server\share）或含 %环境变量% 的写法。
// 命中时 out 返回归一化后的真实路径（/ 统一为 \、去掉尾部分隔符），isDir 指示目录还是文件。
// 只做存在性判断，不解析相对路径（.、..），也不接受 `C:foo` 这种盘符相对形式。
static bool DetectExistingPath(const std::wstring& input, std::wstring& out, bool& isDir) {
    out.clear();
    isDir = false;
    std::wstring s = TrimW(input);
    // 容忍整串带引号（复制「复制为路径」拿到的 "C:\xxx" 形式）
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = TrimW(s.substr(1, s.size() - 2));
    if (s.empty()) return false;

    const bool looksAbs = (s.size() >= 3 && s[1] == L':' && iswalpha(s[0]) && s[2] == L'\\') ||
                          (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\');
    const bool looksEnv = (s[0] == L'%');
    if (!looksAbs && !looksEnv) return false;  // 普通关键词不参与，避免把搜索词误判成路径

    std::wstring path = s;
    if (s.find(L'%') != std::wstring::npos) {
        std::vector<WCHAR> buf(32768, 0);
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf.data(), (DWORD)buf.size());
        if (n > 0 && n <= (DWORD)buf.size()) {
            std::wstring ex(buf.data(), (size_t)n - 1);  // n 含结尾 NUL
            if (ex != s) path = ex;                      // 确有替换才采纳（未定义的变量会原样返回）
            else if (looksEnv) return false;             // 纯 %VAR% 形式却没展开 → 不是有效路径
        } else if (looksEnv) {
            return false;
        }
    }
    for (auto& c : path) if (c == L'/') c = L'\\';
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();  // 保留盘符根 C:\

    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out = path;
    return true;
}

static std::wstring BuildUrl(const WCHAR* tmpl, const std::wstring& keyword) {
    std::wstring url = tmpl;
    std::wstring q = Utf8ToWide(UrlEncodeUtf8(keyword));
    size_t pos = url.find(L"{q}");
    if (pos != std::wstring::npos) url.replace(pos, 3, q);
    return url;
}

static std::wstring DirOf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? std::wstring() : p.substr(0, i);
}

static std::wstring StripExt(const std::wstring& name) {
    size_t i = name.find_last_of(L'.');
    return i == std::wstring::npos ? name : name.substr(0, i);
}

// ---------------- 快捷方式解析 ----------------
static const GUID kCLSID_ShellLink  = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID kIID_IShellLinkW  = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID kIID_IPersistFile = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

static std::wstring ResolveLnkTarget(const std::wstring& lnk) {
    std::wstring out;
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   kIID_IShellLinkW, (void**)&psl))) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(kIID_IPersistFile, (void**)&pf))) {
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
                psl->Resolve(nullptr, SLR_NO_UI);
                WCHAR buf[MAX_PATH]{};
                WIN32_FIND_DATAW wfd{};
                if (SUCCEEDED(psl->GetPath(buf, MAX_PATH, &wfd, SLGP_RAWPATH))) out = buf;
            }
            pf->Release();
        }
        psl->Release();
    }
    return out;
}

// ---------------- 程序枚举（启动时一次性，之后纯内存匹配） ----------------
static void AddLnk(const std::wstring& full, std::vector<Program>& out) {
    size_t i = full.find_last_of(L"\\/");
    Program p;
    p.name = StripExt(i == std::wstring::npos ? full : full.substr(i + 1));
    p.path = full;
    p.target = ResolveLnkTarget(full);
    p.startMenu = true;
    if (!p.name.empty()) out.push_back(std::move(p));
}

static void EnumLnkDir(const std::wstring& dir, int depth, std::vector<Program>& out) {
    if (depth > 6) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            EnumLnkDir(full, depth + 1, out);
        } else if (EndsWithI(name, L".lnk")) {
            AddLnk(full, out);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void EnumProgramFiles(const std::wstring& pf, std::vector<Program>& out) {
    if (pf.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((pf + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring sub = pf + L"\\" + fd.cFileName;
        WIN32_FIND_DATAW fe{};
        HANDLE he = FindFirstFileW((sub + L"\\*.exe").c_str(), &fe);
        if (he == INVALID_HANDLE_VALUE) continue;
        do {
            if (fe.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Program p;
            p.name = StripExt(fe.cFileName);
            p.path = sub + L"\\" + fe.cFileName;
            p.target = p.path;
            p.startMenu = false;
            if (!p.name.empty()) out.push_back(std::move(p));
        } while (FindNextFileW(he, &fe));
        FindClose(he);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 扫描已装软件到 out（按显示名称去重，保留优先级更高的来源）。
// 只写传入容器、不碰 g.programs，因此可以安全地在工作线程调用。
static HICON GetProgramIcon(Program* p);  // 前置声明：扫描线程预抽图标用

static void BuildProgramsInto(std::vector<Program>& out) {
    std::vector<Program> raw;
    WCHAR buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf)))
        EnumLnkDir(std::wstring(buf) + L"\\Microsoft\\Windows\\Start Menu\\Programs", 0, raw);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, SHGFP_TYPE_CURRENT,
                                   buf)))
        EnumLnkDir(buf, 0, raw);
    WCHAR pf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) EnumProgramFiles(pf, raw);
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH)) EnumProgramFiles(pf, raw);

    for (auto& p : raw) {
        p.pinyin = ToPinyin(ToLowerW(p.name));
    }

    // 按显示名称去重（保留优先级更高的来源）
    for (auto& p : raw) {
        std::wstring k = ToLowerW(p.name);
        bool dup = false;
        for (auto& q : out) {
            if (ToLowerW(q.name) == k) { dup = true; break; }
        }
        if (!dup) out.push_back(std::move(p));
    }

    // 预抽图标：SHGetFileInfoW(SHGFI_ICON) 首次抽取较慢（.lnk 还要经 shell 解析），
    // 若留到 WM_PAINT 懒抽取，第一次搜索结果的绘帧会同步卡一下。
    // 这里在扫描线程一次性抽完，之后绘制永远命中缓存，输入不再掉帧。
    for (auto& p : out) GetProgramIcon(&p);
}

static void BuildPrograms() {  // 主线程：原地重建 g.programs
    std::vector<Program> out;
    BuildProgramsInto(out);
    g.programs = std::move(out);
}

// 启动扫描放到工作线程：托盘图标先就位并可响应（右键显示「正在启动中」），
// 扫描结果通过 WM_APP_PROGRAMS_READY 交回主线程接管，避免与搜索并发读写 g.programs。
static unsigned __stdcall ScanProgramsThread(void*) {
    // ResolveLnkTarget 走 CoCreateInstance：COM 按线程初始化，工作线程必须自己来一次
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    auto* out = new (std::nothrow) std::vector<Program>();
    if (out) BuildProgramsInto(*out);
    CoUninitialize();
    if (g.hwnd && out)
        PostMessageW(g.hwnd, WM_APP_PROGRAMS_READY, 0, (LPARAM)out);
    else
        delete out;
    return 0;
}

static std::wstring FindEverythingExe() {
    std::vector<std::wstring> cand;
    cand.push_back(L"C:\\Program Files\\Everything\\Everything.exe");
    cand.push_back(L"C:\\Program Files (x86)\\Everything\\Everything.exe");
    WCHAR buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        cand.push_back(std::wstring(buf) + L"\\Programs\\Everything\\Everything.exe");
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH))
        cand.push_back(DirOf(buf) + L"\\Everything.exe");
    for (auto& c : cand)
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    if (SearchPathW(nullptr, L"Everything.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    return L"Everything.exe";
}

// ---------------- 程序模糊匹配 ----------------
static bool IsSubsequence(const std::wstring& s, const std::wstring& q) {
    size_t i = 0;
    for (wchar_t c : s) {
        if (i < q.size() && c == q[i]) ++i;
    }
    return i == q.size();
}

static int MatchScore(const std::wstring& name, const std::wstring& ql) {
    if (ql.empty()) return 0;
    if (name == ql) return 4;
    if (name.size() >= ql.size() && wcsncmp(name.c_str(), ql.c_str(), ql.size()) == 0) return 3;
    if (name.find(ql) != std::wstring::npos) return 2;
    if (IsSubsequence(name, ql)) return 1;
    return 0;
}

static int MatchScoreWithPinyin(const std::wstring& name, const std::wstring& pinyin, const std::wstring& ql) {
    int nameScore = MatchScore(name, ql);
    if (nameScore > 0) return nameScore;
    if (ql.size() >= 2 && pinyin.find(ql) != std::wstring::npos) return 1;
    if (ql.size() >= 2 && IsSubsequence(pinyin, ql)) return 1;
    return 0;
}

static void SearchPrograms(const std::wstring& query) {
    std::wstring ql = NormalizeSearchTerm(query);  // 与权重 key 同款标准化（去首尾空格+小写）
    struct Cand { Program* p; int score; int w; };
    std::vector<Cand> cands;
    // 单字符查询只保留精确(4)/前缀(3)：带上「包含/子序列」时命中面过宽
    // （实测 d 命中全部程序的 32%），刚敲第一个字母就撑满 10 行噪音结果。
    const bool prefixOnly = (ql.size() < 2);
    for (auto& p : g.programs) {
        int s = MatchScoreWithPinyin(ToLowerW(p.name), p.pinyin, ql);
        if (s > 0 && !(prefixOnly && s < 3)) {
            int w = g.weightEnabled ? GetClickWeight(ql, p.path) : 0;
            cands.push_back({&p, s, w});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.w != b.w) return a.w > b.w;  // 点击权重最优先：点过的条目排前面
        if (a.score != b.score) return a.score > b.score;
        if (a.p->startMenu != b.p->startMenu) return a.p->startMenu;
        return _wcsicmp(a.p->name.c_str(), b.p->name.c_str()) < 0;
    });
    size_t n = (std::min)(cands.size(), (size_t)ev::kMaxResults);
    for (size_t i = 0; i < n; ++i) {
        Row r;
        r.kind = Row::Prog;
        r.prog = cands[i].p;
        r.title = cands[i].p->name;
        r.sub = cands[i].p->path;
        r.action = cands[i].p->path;
        if (IsPathExcluded(r.action)) continue;  // 排除命中：跳过该程序
        g.items.push_back(std::move(r));
    }
}

static HICON GetProgramIcon(Program* p) {
    if (!p->iconTried) {
        p->iconTried = true;
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(p->path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
            p->icon = sfi.hIcon;
    }
    return p->icon;
}

// ---------------- UI 布局 ----------------
static void Layout() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int rows = (std::min)((int)g.items.size(), (int)ev::kMaxResults);
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    UpdateScale(mon);  // 按目标屏幕物理分辨率重算比例（字体随比例重建）
    int W = S(kBaseW), H = S(kBaseInputH + rows * kBaseRowH);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x, y;
    if (g.centerWake) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    } else {
        x = cur.x + S(16);
        y = cur.y + S(12);
        if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
        if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    UINT flags = SWP_NOACTIVATE | SWP_NOZORDER;
    SetWindowPos(g.hwnd, nullptr, x, y, W, H, flags);
    (void)rc;
}

static void RepaintNow() {
    RedrawWindow(g.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
}

static void LayoutAndRepaint() {
    Layout();
    RepaintNow();
}

static void Show() {
    g.text.clear();
    g.compText.clear();
    g.caret = 0;
    g.selStart = g.selEnd = 0;
    g.dragging = false;
    g.scrollX = 0;
    g.items.clear();
    g.sel = 0;
    g.hoverRow = -1;     // 重置悬停/按下瞬态，避免上次点击残留投影
    g.pressRow = -1;
    g.hoverT = 0;
    g.pressT = 0;
    g.mode = Mode::None;
    g.expectReply = 0;
    g.evTermKey.clear();
    g.caretOn = true;
    KillTimer(g.hwnd, kTimerDebounce);
    Layout();
    // 前台被其他进程占用时，借助 AttachThreadInput 提高抢占成功率
    DWORD myTid = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    g.prevForeground = fg;  // 记录唤醒前的前台窗口（top 命令的目标窗口）
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE);
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    SetFocus(g.hwnd);
}

static void Hide() {
    if (!IsWindowVisible(g.hwnd)) return;
    KillTimer(g.hwnd, kTimerDebounce);
    g.dragging = false;
    g.selStart = g.selEnd = 0;
    // 若正在组词，先取消 IME 组合，避免残留未上屏状态
    HIMC hIMC = ImmGetContext(g.hwnd);
    if (hIMC) {
        ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(g.hwnd, hIMC);
    }
    g.compText.clear();
    ShowWindow(g.hwnd, SW_HIDE);
}

// ---------------- Everything 查询 ----------------
static void AddHint(const std::wstring& text) {
    Row r;
    r.kind = Row::Hint;
    r.title = text;
    g.items.push_back(std::move(r));
}

static void UseEverythingFallback() {
    g.items.clear();
    Row r;
    r.kind = Row::EvFallback;
    std::wstring kw = g.evQuery;
    size_t c = kw.find(L':');
    if (c != std::wstring::npos) kw = kw.substr(c + 1);
    r.title = L"在 Everything 中搜索：" + kw;
    r.sub = L"IPC 未连接，回车将打开 Everything 窗口";
    r.action = L"-search \"" + g.evQuery + L"\"";
    g.items.push_back(std::move(r));
    g.sel = 0;
    LayoutAndRepaint();
}

static void ExecuteEverythingQuery() {
    HWND evw = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    if (!evw) { UseEverythingFallback(); return; }

    size_t len = g.evQuery.size();
    std::vector<BYTE> buf(sizeof(ev::Query) - sizeof(WCHAR) + (len + 1) * sizeof(WCHAR));
    ev::Query* q = (ev::Query*)buf.data();
    q->reply_hwnd = (DWORD)(DWORD_PTR)g.hwnd;
    g.serial++;
    g.expectReply = ev::kReplyBase | (g.serial & 0x7FFF);
    q->reply_copydata_message = g.expectReply;
    q->search_flags = 0;
    q->offset = 0;
    q->max_results = ev::kMaxResults;
    memcpy(q->search_string, g.evQuery.c_str(), (len + 1) * sizeof(WCHAR));

    COPYDATASTRUCT cds{};
    cds.dwData = ev::kCopyDataQueryW;
    cds.cbData = (DWORD)buf.size();
    cds.lpData = buf.data();

    DWORD_PTR res = 0;
    LRESULT lr = SendMessageTimeoutW(evw, WM_COPYDATA, (WPARAM)g.hwnd, (LPARAM)&cds,
                                     SMTO_ABORTIFHUNG, 1500, &res);
    if (lr == 0 || res == 0) { UseEverythingFallback(); return; }
    // 结果异步回到本窗口（WM_COPYDATA，dwData == g.expectReply）
}

static void HandleEverythingReply(const COPYDATASTRUCT* cds) {
    if (cds->dwData != g.expectReply || !cds->lpData) return;
    if (cds->cbData < sizeof(ev::List) - sizeof(ev::Item)) return;
    const ev::List* list = (const ev::List*)cds->lpData;

    g.items.clear();
    DWORD n = (std::min)(list->numitems, ev::kMaxResults);
    size_t headerBytes = sizeof(ev::List) - sizeof(ev::Item) + (size_t)n * sizeof(ev::Item);
    if (headerBytes > cds->cbData) n = 0;
    int kept = 0;  // 排除路径过滤后仍向 Everything 索取的最大条数（保证展示满 10 行）
    int want = (int)ev::kMaxResults;
    for (DWORD i = 0; i < n && kept < want; ++i) {
        const ev::Item& it = list->items[i];
        if (it.filename_offset >= cds->cbData || it.path_offset >= cds->cbData) continue;
        const WCHAR* fn = (const WCHAR*)((const BYTE*)list + it.filename_offset);
        const WCHAR* pa = (const WCHAR*)((const BYTE*)list + it.path_offset);
        Row r;
        r.kind = (it.flags & ev::kFlagFolder) ? Row::Folder : Row::File;
        r.title = fn ? fn : L"";
        r.sub = pa ? pa : L"";
        if (r.sub.empty()) {
            r.action = r.title;  // 盘符根目录：Everything 返回的 path 为空
        } else if (r.sub.back() == L'\\') {
            r.action = r.sub + r.title;
        } else {
            r.action = r.sub + L"\\" + r.title;
        }
        if (IsPathExcluded(r.action)) continue;  // 排除命中：直接丢弃
        g.items.push_back(std::move(r));
        ++kept;
    }
    // 点击加权重排：同一有效搜索词下点过的文件/文件夹排前面（权重相同保持 Everything 原序）
    if (g.weightEnabled && !g.evTermKey.empty() && g.items.size() > 1) {
        const std::wstring& tk = g.evTermKey;
        std::stable_sort(g.items.begin(), g.items.end(), [&tk](const Row& a, const Row& b) {
            return GetClickWeight(tk, a.action) > GetClickWeight(tk, b.action);
        });
    }
    if (g.items.empty()) AddHint(g.startingUp ? L"正在启动中…" : L"无结果");
    g.sel = 0;
    LayoutAndRepaint();
}

// ---------------- 输入解析与刷新 ----------------
static const WebCmd* FindWebCmd(const std::wstring& tok) {
    for (auto& w : g.webCmds)
        if (tok == w.prefix) return &w;
    return nullptr;
}

// Everything 修饰符（file:/folder:）仅作用于紧随其后的一个词，
// 因此多词关键词需逐词添加前缀：d foo bar -> file:foo file:bar。
// 引号短语（"foo bar"）视为一个词整体传递。
static std::wstring BuildModifierQuery(const std::wstring& mod, const std::wstring& kw) {
    std::wstring out;
    size_t i = 0, n = kw.size();
    while (i < n) {
        while (i < n && iswspace(kw[i])) ++i;
        if (i >= n) break;
        size_t j;
        if (kw[i] == L'"') {
            j = kw.find(L'"', i + 1);
            j = (j == std::wstring::npos) ? n : j + 1;
        } else {
            j = i;
            while (j < n && !iswspace(kw[j])) ++j;
        }
        out += mod;
        out.append(kw, i, j - i);
        out += L' ';
        i = j;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// ---------------- Shell 命令 & 窗口切换 ----------------
enum class ExecKind { Normal, Close, Kill, Admin };

// 取窗口所属进程的可执行文件名（basename）
static std::wstring ModuleBaseName(HWND hwnd) {
    WCHAR path[MAX_PATH] = {0};
    if (!GetWindowModuleFileNameW(hwnd, path, MAX_PATH)) return L"";
    std::wstring p = path;
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

// Shell 命令默认工作目录
static std::wstring GetShellCwd() {
    WCHAR buf[MAX_PATH];
    if (g.shellDefaultCwd == 0) {            // 用户目录
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) return buf;
    } else if (g.shellDefaultCwd == 1) {     // 系统默认目录 (System32)
        if (GetSystemDirectoryW(buf, MAX_PATH)) return buf;
    } else {                                 // 桌面目录
        std::wstring up;
        if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) up = buf;
        return up.empty() ? L"" : up + L"\\Desktop";
    }
    return L"";
}

// 查找 Git Bash 可执行文件（常见安装位置，回退到 PATH 上的 bash.exe）
static std::wstring FindGitBash() {
    const WCHAR* envs[] = { L"ProgramFiles", L"ProgramFiles(x86)" };
    for (auto e : envs) {
        WCHAR pf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(e, pf, MAX_PATH)) {
            std::wstring p = std::wstring(pf) + L"\\Git\\bin\\bash.exe";
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
        }
    }
    return L"bash.exe";
}

static void KillProcessByPid(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
}

// 依据当前 shell 配置构造要执行的 exe / 参数 / cwd
// cwdOverride：非空时用它替代设置里的默认工作目录（「用此路径打开命令行」用）
// interactive：交互式终端 —— 强制显示窗口，且 cmd 为空时各 shell 都进入可输入的提示符
static void BuildShellCommand(const std::wstring& cmd, bool admin,
                              std::wstring& exe, std::wstring& args, std::wstring& cwd,
                              const std::wstring* cwdOverride = nullptr,
                              bool interactive = false) {
    exe.clear(); args.clear();
    cwd = (cwdOverride && !cwdOverride->empty()) ? *cwdOverride : GetShellCwd();
    bool visible = g.shellShowWindow || admin || interactive;  // 提权时必然可见（UAC 弹窗）
    const bool bare = cmd.empty();  // 无命令：只开一个交互式终端
    if (g.shellType == 0) {            // 命令提示符
        exe = L"cmd.exe";
        if (bare) args = visible ? L"/k" : L"/c";
        else      args = (visible ? L"/k " : L"/c ") + cmd;
    } else if (g.shellType == 1) {     // PowerShell
        exe = L"powershell.exe";
        if (bare) args = visible ? L"-NoExit -NoProfile" : L"-NoProfile";
        else      args = (visible ? L"-NoExit -NoProfile -Command " : L"-NoProfile -Command ") + cmd;
    } else if (g.shellType == 2) {     // Git Bash
        exe = FindGitBash();
        if (bare) args.clear();        // 无参数启动 bash，工作目录由 lpDirectory 决定
        else if (visible) args = L"-c \"" + cmd + L"; exec bash\"";
        else              args = L"-c \"" + cmd + L"\"";
    } else {                           // 自定义
        exe = g.shellCustomPath.empty() ? L"cmd.exe" : g.shellCustomPath;
        std::wstring t = g.shellCustomArgs;
        size_t p = t.find(L"{c}");
        if (p != std::wstring::npos) t.replace(p, 3, cmd);
        else if (!cmd.empty() || !t.empty()) t = t + (t.empty() ? L"" : L" ") + cmd;
        args = t;
    }
}

// 在指定目录打开一个交互式命令行窗口（走设置页配置的 Shell 程序）
static bool OpenShellAt(const std::wstring& dir) {
    if (dir.empty()) return false;
    std::wstring exe, args, cwd;
    BuildShellCommand(std::wstring(), false, exe, args, cwd, &dir, /*interactive=*/true);
    if (exe.empty()) return false;
    HINSTANCE h = ShellExecuteW(nullptr, L"open", exe.c_str(),
                                args.empty() ? nullptr : args.c_str(),
                                cwd.empty() ? nullptr : cwd.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

static BOOL CALLBACK EnumWinProc(HWND hwnd, LPARAM lp) {
    auto* out = (std::vector<WinInfo>*)lp;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    WCHAR title[512];
    if (!GetWindowTextW(hwnd, title, 512) || title[0] == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;   // 排除自身
    WCHAR cls[64];
    GetClassNameW(hwnd, cls, 64);
    std::wstring cs = cls;
    if (cs == L"Progman" || cs == L"WorkerW" || cs == L"Shell_TrayWnd") return TRUE;
    WinInfo wi;
    wi.hwnd = hwnd;
    wi.title = title;
    wi.className = cs;
    wi.pid = pid;
    wi.processName = ModuleBaseName(hwnd);
    wi.isUwp = (cs == L"Windows.UI.Core.CoreWindow");
    out->push_back(std::move(wi));
    return TRUE;
}

// 枚举 + 缓存 + 匹配 + （可选）分组，生成 Row::Window 结果
static void SearchWindows(const std::wstring& query) {
    DWORD now = GetTickCount();
    if (g.windowCache.empty() || now - g.windowCacheTick >= (DWORD)(g.winCacheSec * 1000)) {
        std::vector<WinInfo> all;
        EnumWindows(EnumWinProc, (LPARAM)&all);
        g.windowCache = std::move(all);
        g.windowCacheTick = now;
    }
    std::wstring q = ToLowerW(TrimW(query));
    struct Cand { int score; size_t idx; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < g.windowCache.size(); ++i) {
        const WinInfo& w = g.windowCache[i];
        if (!g.winShowUwp && w.isUwp) continue;
        int s = 0;
        if (q.empty()) {
            s = 1;
        } else {
            int a = MatchScore(ToLowerW(w.title), q);
            int b = MatchScore(ToLowerW(w.processName), q);
            int c = MatchScore(ToLowerW(w.className), q);
            s = a; if (b > s) s = b; if (c > s) s = c;
        }
        if (s > 0) cands.push_back({s, i});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        return g.windowCache[a.idx].title < g.windowCache[b.idx].title;
    });
    if (g.winGroupProc) {
        std::map<DWORD, std::vector<size_t>> groups;
        std::vector<DWORD> order;
        for (size_t k = 0; k < cands.size(); ++k) {
            DWORD pid = g.windowCache[cands[k].idx].pid;
            if (groups.find(pid) == groups.end()) order.push_back(pid);
            groups[pid].push_back(k);
        }
        for (DWORD pid : order) {
            auto& idxs = groups[pid];
            if (idxs.size() == 1) {
                const WinInfo& w = g.windowCache[cands[idxs[0]].idx];
                Row r; r.kind = Row::Window; r.winHwnd = w.hwnd;
                r.title = w.title; r.procName = w.processName;
                r.sub = g.winShowProc ? w.processName : L"";
                r.action = std::to_wstring((uintptr_t)w.hwnd);
                g.items.push_back(std::move(r));
            } else {
                Row r; r.kind = Row::Window; r.winIsGroup = true;
                for (size_t k : idxs) r.winMembers.push_back(g.windowCache[cands[k].idx].hwnd);
                const WinInfo& rep = g.windowCache[cands[idxs[0]].idx];
                r.title = L"▶ " + rep.processName + L" (" + std::to_wstring(idxs.size()) + L")";
                r.procName = rep.processName;
                r.sub = g.winShowProc ? rep.processName : L"";
                g.items.push_back(std::move(r));
            }
            if (g.items.size() >= ev::kMaxResults) break;
        }
    } else {
        for (auto& c : cands) {
            const WinInfo& w = g.windowCache[c.idx];
            Row r; r.kind = Row::Window; r.winHwnd = w.hwnd;
            r.title = w.title; r.procName = w.processName;
            r.sub = g.winShowProc ? w.processName : L"";
            r.action = std::to_wstring((uintptr_t)w.hwnd);
            g.items.push_back(std::move(r));
            if (g.items.size() >= ev::kMaxResults) break;
        }
    }
}

static void Refresh() {
    KillTimer(g.hwnd, kTimerDebounce);
    g.items.clear();
    g.sel = 0;
    g.mode = Mode::None;
    g.expectReply = 0;
    g.evTermKey.clear();

    const std::wstring& t = g.text;
    if (!t.empty()) {
        // 网址直达：输入为网址时，结果最前面加一条「打开网址」项
        if (IsUrl(t)) {
            Row r;
            r.kind = Row::Web;
            r.title = L"打开网址：" + t;
            r.action = t;
            r.sub = t;
            g.items.push_back(std::move(r));
        }
        // 路径直达：输入为已存在的路径时置顶一条（目录→打开文件夹；文件→打开所在文件夹并选中）。
        // 与网址直达一样只做「前置插入」，后面的普通搜索照常跑，回车默认命中首条即本项。
        {
            std::wstring hitPath;
            bool hitIsDir = false;
            if (DetectExistingPath(t, hitPath, hitIsDir)) {
                Row r;
                r.kind = hitIsDir ? Row::Folder : Row::Reveal;
                r.action = hitPath;
                r.sub = hitPath;
                r.title = hitIsDir ? (L"打开文件夹：" + hitPath)
                                   : (L"打开所在文件夹：" + hitPath);
                g.items.push_back(std::move(r));
            }
        }
        // 命令类前缀：必须以空格开头（去掉前导空格后才是真正的 token）。
        // top / cmd / w 三种命令各自可在「命令」设置页开关。
        bool spaceCmd = (t[0] == L' ');
        std::wstring body = spaceCmd ? TrimW(t) : t;
        size_t sp = body.find(L' ');
        std::wstring tok = ToLowerW(sp == std::wstring::npos ? body : body.substr(0, sp));
        std::wstring rest = sp == std::wstring::npos ? L"" : TrimW(body.substr(sp + 1));

        if (spaceCmd) {
            // —— 空格前缀命令：top / cmd（原 "> 命令"）/ w（原 "< 关键词"）——
            if (g.topEnabled && tok == L"top" && rest.empty()) {
                // top 命令：整串恰好为「空格 top」时，回车对「唤醒前的前台窗口」切换置顶
                g.mode = Mode::Top;
                Row r;
                r.kind = Row::Top;
                r.action = L"top";
                HWND target = TopTargetWindow();
                bool valid = target != nullptr;
                bool isTop = false;
                WCHAR winTitle[256] = {};
                if (valid && GetWindowTextW(target, winTitle, 256) == 0) winTitle[0] = 0;
                if (valid) isTop = (GetWindowLongPtrW(target, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
                r.title = valid ? (isTop ? L"取消置顶当前窗口" : L"置顶当前窗口")
                                : L"置顶当前窗口";
                r.sub = valid ? (std::wstring(isTop ? L"已置顶：" : L"目标：") + winTitle)
                              : L"未检测到可置顶的窗口";
                g.items.push_back(std::move(r));
            } else if (g.cmdEnabled && tok == L"cmd") {
                // cmd 命令（原 "> 命令"）：执行 Shell 命令
                g.mode = Mode::Shell;
                if (rest.empty()) {
                    AddHint(L"输入 Shell 命令，回车执行；Ctrl+Shift+Enter 以管理员运行");
                } else {
                    Row r;
                    r.kind = Row::Shell;
                    r.title = L"cmd " + rest;
                    r.action = rest;
                    r.sub = L"Shell 命令";
                    g.items.push_back(std::move(r));
                }
            } else if (g.winEnabled && tok == L"w") {
                // w 命令（原 "< 关键词"）：切换窗口
                g.mode = Mode::Window;
                if (rest.empty()) {
                    AddHint(L"输入窗口标题/进程名切换；回车切换，Ctrl+Enter 关闭，Ctrl+Shift+Enter 结束进程");
                } else {
                    SearchWindows(rest);
                }
            } else if (g.captureEnabled && tok == L"ss") {
                // ss 命令：截图（ScreenCapture.exe）
                g.mode = Mode::Capture;
                if (rest.empty()) {
                    // 无参数：显示默认截图项，回车直接执行
                    if (g.captureExe.empty()) {
                        AddHint(L"未找到 ScreenCapture.exe，请在设置→截图工具中下载");
                    } else {
                        Row r;
                        r.kind = Row::Capture;
                        r.title = L"截图";
                        r.action = L"cap";
                        r.sub = L"ScreenCapture（回车截图，或输入 pin / ocr）";
                        g.items.push_back(std::move(r));
                    }
                } else {
                    std::wstring enterMode;
                    if (rest == L"pin") enterMode = L"pin";
                    else if (rest == L"cap") enterMode = L"cap";
                    else if (rest == L"ocr") enterMode = L"ocr";
                    else {
                        AddHint(L"不支持的模式，可用：pin（贴图）/ ocr（文字识别）");
                    }
                    if (!enterMode.empty() && g.captureExe.empty()) {
                        AddHint(L"未找到 ScreenCapture.exe，请确认与 flowtary.exe 同目录");
                    } else if (!enterMode.empty()) {
                        Row r;
                        r.kind = Row::Capture;
                        r.title = (enterMode == L"cap") ? L"截图" :
                                  (enterMode == L"pin") ? L"截图 + 贴图"
                                                         : L"截图 + 文字识别";
                        r.action = enterMode;  // 保存模式，执行时构造完整命令行
                        r.sub = L"ScreenCapture " + enterMode;
                        g.items.push_back(std::move(r));
                    }
                }
            } else {
                // 空格开头但不是已知命令：按普通程序搜索处理
                g.mode = Mode::Programs;
                SearchPrograms(body);
            }
        } else if (tok == L"d" || tok == L"f") {
            g.mode = Mode::Everything;
            if (rest.empty()) {
                AddHint(tok == L"f" ? L"输入关键词搜索文件" : L"输入关键词搜索文件夹");
            } else {
                g.evQuery = BuildModifierQuery(tok == L"f" ? L"file:" : L"folder:", rest);
                g.evTermKey = NormalizeSearchTerm(rest);  // 本次查询的有效搜索词（权重 key）
                AddHint(L"正在搜索…");
                SetTimer(g.hwnd, kTimerDebounce, kDebounceMs, nullptr);
            }
        } else if (FindWebCmd(tok)) {
            const WebCmd* w = FindWebCmd(tok);
            g.mode = Mode::Web;
            if (rest.empty()) {
                AddHint(std::wstring(L"输入关键词，回车用 ") + w->prefix + L" 搜索");
            } else {
                Row r;
                r.kind = Row::Web;
                r.title = std::wstring(L"用 ") + w->prefix + L" 搜索：" + rest;
                r.action = BuildUrl(w->urlTemplate.c_str(), rest);
                r.sub = r.action;
                g.items.push_back(std::move(r));
            }
        } else {
            g.mode = Mode::Programs;
            // 一键启动 / 一键关闭：整串（去空白、忽略大小写）命中关键字时，
            // 把组动作插到结果最前面（回车即执行；下方向键仍可选到普通程序结果）
            std::wstring key = ToLowerW(TrimW(t));
            auto addGroup = [&](const CmdGroup* gp, bool kill) {
                if (!gp) return;
                Row r;
                r.kind = Row::Group;
                r.groupKill = kill;
                r.groupTargets = gp->targets;
                r.title = (kill ? L"一键关闭：" : L"一键启动：") + gp->key;
                std::wstring sub;
                for (size_t i = 0; i < gp->targets.size(); ++i) {
                    if (i) sub += L"、";
                    sub += gp->targets[i];
                }
                r.sub = sub;
                r.action = sub;  // 右键「复制路径」时复制目标清单
                g.items.push_back(std::move(r));
            };
            addGroup(FindGroup(g.groupsLaunch, key), false);
            addGroup(FindGroup(g.groupsKill, key), true);
            SearchPrograms(t);
        }
    }
    LayoutAndRepaint();
}

// ---------------- 执行 ----------------
// top 命令的目标窗口：唤醒前的前台窗口；排除桌面/任务栏/自身等无效目标
static HWND TopTargetWindow() {
    HWND h = g.prevForeground;
    if (!h || !IsWindow(h) || h == g.hwnd) return nullptr;
    WCHAR cls[64];
    if (GetClassNameW(h, cls, 64)) {
        if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
            wcscmp(cls, L"Shell_TrayWnd") == 0)
            return nullptr;
    }
    return h;
}

static bool ExecuteRow(Row& r, ExecKind ek = ExecKind::Normal) {
    switch (r.kind) {
        case Row::Folder:
            // 若当前有系统文件对话框处于焦点，则把该对话框原地跳转到所选文件夹，
            // 而非在资源管理器中打开（类 Listary Quick-Switch）。失败静默。
            if (fdj_enabled() && fdj_dialog_open()) {
                fdj_jump_to_folder(r.action.c_str());
                return true;
            }
            ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        case Row::File:
        case Row::Web:
            ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return true;
        case Row::Reveal: {
            // 路径直达（文件路径）：在资源管理器中定位并选中，不直接启动该文件
            std::wstring args = L"/select,\"" + r.action + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            return true;
        }
        case Row::Prog: {
            std::wstring dir = DirOf(!r.prog->target.empty() ? r.prog->target : r.action);
            ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr,
                          dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
            return true;
        }
        case Row::EvFallback:
            ShellExecuteW(nullptr, L"open", g.everythingExe.c_str(), r.action.c_str(), nullptr,
                          SW_SHOWNORMAL);
            return true;
        case Row::Group: {
            // 一键组：启动组逐个打开文件路径；关闭组按进程名结束进程
            if (r.groupKill) {
                for (auto& name : r.groupTargets) KillProcessesByName(name);
            } else {
                for (auto& p : r.groupTargets) {
                    std::wstring dir = DirOf(p);
                    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr,
                                  dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
                }
            }
            return true;
        }
        case Row::Shell: {
            std::wstring exe, args, cwd;
            BuildShellCommand(r.action, ek == ExecKind::Admin, exe, args, cwd);
            if (exe.empty()) return false;
            HINSTANCE h = ShellExecuteW(nullptr, ek == ExecKind::Admin ? L"runas" : L"open",
                                       exe.c_str(), args.empty() ? nullptr : args.c_str(),
                                       cwd.empty() ? nullptr : cwd.c_str(),
                                       ek == ExecKind::Admin ? SW_SHOWNORMAL
                                                           : (g.shellShowWindow ? SW_SHOWNORMAL
                                                                                : SW_HIDE));
            return (INT_PTR)h > 32;
        }
        case Row::Top: {
            // top 命令：对唤醒前的前台窗口切换置顶（WS_EX_TOPMOST），
            // 并在目标窗口位置弹一个快速消失的轻量 toast（不走系统通知）
            HWND target = TopTargetWindow();
            if (!target) {
                ShowTopToast(nullptr, L"未找到可置顶的窗口");
                return true;
            }
            LONG_PTR ex = GetWindowLongPtrW(target, GWL_EXSTYLE);
            bool wasTop = (ex & WS_EX_TOPMOST) != 0;
            SetWindowPos(target, wasTop ? HWND_NOTOPMOST : HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            WCHAR winTitle[256] = {};
            if (GetWindowTextW(target, winTitle, 256) == 0) winTitle[0] = 0;
            std::wstring toast = wasTop ? L"取消置顶" : L"已置顶";
            if (winTitle[0]) toast += L"：" + std::wstring(winTitle);
            ShowTopToast(target, toast);
            return true;
        }
        case Row::Window: {
            if (ek == ExecKind::Close) {
                if (r.winIsGroup) {
                    for (HWND m : r.winMembers) PostMessageW(m, WM_CLOSE, 0, 0);
                } else if (IsWindow(r.winHwnd)) {
                    PostMessageW(r.winHwnd, WM_CLOSE, 0, 0);
                }
                return true;
            }
            if (ek == ExecKind::Kill) {
                DWORD pid = 0;
                if (r.winIsGroup && !r.winMembers.empty())
                    GetWindowThreadProcessId(r.winMembers.front(), &pid);
                else if (r.winHwnd)
                    GetWindowThreadProcessId(r.winHwnd, &pid);
                if (pid) KillProcessByPid(pid);
                return true;
            }
            // 普通切换：分组头切到首个成员，单窗切到自身
            HWND target = r.winIsGroup ? (r.winMembers.empty() ? nullptr : r.winMembers.front())
                                       : r.winHwnd;
            if (target && IsWindow(target)) {
                if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
                SetForegroundWindow(target);
                return true;
            }
            return false;
        }
        case Row::Capture: {
            // ss 命令：启动 ScreenCapture.exe 截图
            if (g.captureExe.empty()) return false;
            // 构造命令行参数：enter=模式 tray=false auto-quit=true
            std::wstring args = L"enter=" + r.action + L" tray=false auto-quit=true";
            HINSTANCE h = ShellExecuteW(nullptr, L"open", g.captureExe.c_str(),
                                        args.c_str(), nullptr, SW_SHOWNORMAL);
            return (INT_PTR)h > 32;
        }
        case Row::Hint:
            break;
    }
    return false;
}

// 依据当前选中行与修饰键解析执行方式
static ExecKind ResolveExecKind(bool ctrl, bool shift) {
    ExecKind ek = ExecKind::Normal;
    if (ctrl && shift) ek = ExecKind::Admin;
    else if (ctrl) ek = ExecKind::Close;
    if (g.sel >= 0 && g.sel < (int)g.items.size() && g.items[g.sel].kind == Row::Window) {
        // 窗口项：Ctrl+Enter=关闭，Ctrl+Shift+Enter=结束进程（覆盖默认的提权语义）
        if (ctrl && shift) ek = ExecKind::Kill;
        else if (ctrl) ek = ExecKind::Close;
        else ek = ExecKind::Normal;
    }
    return ek;
}

static void ExecuteSelected(ExecKind ek = ExecKind::Normal) {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    RecordClickWeight(g.items[g.sel]);  // 点击/回车/快捷键选中即记权重（内部过滤非本地条目与不保存的前缀）
    if (ExecuteRow(g.items[g.sel], ek)) Hide();
}

// 以管理员（提升权限）模式打开：仅对可执行项（程序 / 文件）有意义，使用 runas 动词提权
static bool ExecuteRowAdmin(Row& r) {
    std::wstring path, dir;
    if (r.kind == Row::Prog) {
        path = r.action;
        dir = DirOf(!r.prog->target.empty() ? r.prog->target : r.action);
    } else if (r.kind == Row::File) {
        path = r.action;
        dir = DirOf(path);
    } else {
        return false;
    }
    HINSTANCE h = ShellExecuteW(nullptr, L"runas", path.c_str(), nullptr,
                               dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    return (INT_PTR)h > 32;
}

static void CopyTextToClipboard(const std::wstring& s) {
    if (s.empty()) return;
    if (!OpenClipboard(g.hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(WCHAR));
    if (h) {
        WCHAR* p = (WCHAR*)GlobalLock(h);
        if (p) {
            memcpy(p, s.c_str(), (s.size() + 1) * sizeof(WCHAR));
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

// ---------------- 结果右键菜单（黑暗模式自绘，与主界面同风格） ----------------
// 自绘菜单项只覆盖条目矩形；菜单窗口自身的背景与边框仍用系统菜单色（浅色），
// 必须用 SetMenuInfo 把整个菜单窗体染黑，否则四周露白边。
// 注意：Win11 弹出菜单的圆角与 1px 外边框由 DWM 按系统主题绘制，MIM_BACKGROUND
// 管不到；用 uxtheme 未公开序号（135=SetPreferredAppMode/136=FlushMenuThemes）
// 强制暗色菜单主题，旧系统/未来变更导致序号缺失时静默跳过（仅剩细边框差异）。
static void EnableDarkMenus() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    typedef int (WINAPI *SetPreferredAppModeFn)(int);
    typedef void (WINAPI *FlushMenuThemesFn)(void);
    SetPreferredAppModeFn setMode =
        (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    FlushMenuThemesFn flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    // 美化开关关闭时回归系统默认外观；开启时：深色主题→ForceDark(2)，浅色主题→AllowDark(1)
    int mode = g.beautify ? (g.theme && g.theme->dark ? 2 : 1) : 0;
    if (setMode) setMode(mode);
    if (flush) flush();
}

static void StyleDarkMenu(HMENU menu) {
    if (!g.brMenuBg) g.brMenuBg = CreateSolidBrush(g.theme ? g.theme->menuBg : RGB(28, 28, 28));
    MENUINFO mi{};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    mi.hbrBack = g.brMenuBg;
    SetMenuInfo(menu, &mi);
}

// 自绘菜单项：背景/选中/勾选/文字均取当前主题色，不依赖系统暗色菜单 API（EnableDarkMenus）。
// 设置页下拉（由 SettingsProc 拥有）与主界面右键菜单（由主窗口拥有）共用，保证任何 Windows
// 版本下文字都可见。
static void OwnerMeasureMenuItem(HWND hwnd, MEASUREITEMSTRUCT* mis) {
    if (!mis || mis->CtlType != ODT_MENU) return;
    const WCHAR* text = (const WCHAR*)mis->itemData;
    if (text && *text) {
        HDC hdc = GetDC(hwnd);
        HGDIOBJ old = SelectObject(hdc, g.fList);
        SIZE sz{};
        GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
        SelectObject(hdc, old);
        ReleaseDC(hwnd, hdc);
        mis->itemWidth = sz.cx + S(44);
        mis->itemHeight = S(30);
    } else {
        mis->itemWidth = S(60);
        mis->itemHeight = S(9);
    }
}

static void OwnerDrawMenuItem(HWND hwnd, DRAWITEMSTRUCT* dis) {
    (void)hwnd;
    if (!dis || dis->CtlType != ODT_MENU) return;
    const WCHAR* text = (const WCHAR*)dis->itemData;
    const Theme& t = *g.theme;
    HBRUSH bg = CreateSolidBrush(t.menuBg);
    FillRect(dis->hDC, &dis->rcItem, bg);
    DeleteObject(bg);
    if (!text || !*text) {
        RECT lr{dis->rcItem.left + S(10), dis->rcItem.top + S(4),
                dis->rcItem.right - S(10), dis->rcItem.top + S(5)};
        HBRUSH lb = CreateSolidBrush(t.divider);
        FillRect(dis->hDC, &lr, lb);
        DeleteObject(lb);
        return;
    }
    if (dis->itemState & ODS_SELECTED) {
        HBRUSH hb = CreateSolidBrush(t.menuHi);
        FillRect(dis->hDC, &dis->rcItem, hb);
        DeleteObject(hb);
    }
    if (dis->itemState & ODS_CHECKED) {
        int cx = dis->rcItem.left + S(8);
        int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
        int s = S(4);
        HPEN pen = CreatePen(PS_SOLID, S(2), t.text);
        HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
        MoveToEx(dis->hDC, cx - s, cy, nullptr);
        LineTo(dis->hDC, cx - s / 2, cy + s / 2);
        LineTo(dis->hDC, cx + s, cy - s / 2);
        SelectObject(dis->hDC, oldPen);
        DeleteObject(pen);
    }
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, t.text);
    SelectObject(dis->hDC, g.fList);
    RECT tr = dis->rcItem;
    tr.left += S(16);
    DrawTextW(dis->hDC, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void ShowRowMenu(HWND hwnd) {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    Row& r = g.items[g.sel];
    if (r.kind == Row::Hint) return;

    HMENU menu = CreatePopupMenu();
    if (r.kind == Row::Window) {
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_SWITCH, (LPCWSTR)L"切换到此窗口");
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_CLOSE, (LPCWSTR)L"关闭窗口");
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_KILL, (LPCWSTR)L"结束进程");
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_COPYTITLE, (LPCWSTR)L"复制标题");
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_COPYPROC, (LPCWSTR)L"复制进程名");
        SetMenuDefaultItem(menu, IDM_WIN_SWITCH, FALSE);
    } else {
        // 路径直达行（Reveal）：默认动作本身就是「打开所在文件夹并选中」，标题照此写
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPEN,
                    (LPCWSTR)(r.kind == Row::Reveal ? L"打开所在文件夹" : L"打开"));
        if (r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog)
            AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPENLOC, (LPCWSTR)L"打开所在文件夹");
        if (r.kind == Row::Reveal)
            AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPENFILE, (LPCWSTR)L"打开文件");
        if (r.kind == Row::File || r.kind == Row::Prog)
            AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_RUNAS, (LPCWSTR)L"以管理员模式打开");
        if (r.kind == Row::Shell)
            AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_SHELL_ADMIN, (LPCWSTR)L"以管理员运行");
        // 用该路径（文件取所在目录）打开命令行，沿用设置页配置的 Shell 程序
        if (r.kind == Row::Folder || r.kind == Row::File || r.kind == Row::Reveal ||
            r.kind == Row::Prog)
            AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPENCMD,
                        (LPCWSTR)(r.kind == Row::Folder ? L"用此路径打开命令行"
                                                        : L"在所在目录打开命令行"));
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_COPYPATH,
                    (LPCWSTR)(r.kind == Row::Web ? L"复制链接"
                              : r.kind == Row::Shell ? L"复制命令" : L"复制路径"));
        SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
    }
    StyleDarkMenu(menu);

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, p.x, p.y, 0,
                             hwnd, nullptr);
    DestroyMenu(menu);

    bool done = true;
    switch (cmd) {
        case IDM_OPEN:
            RecordClickWeight(r);  // 右键打开同样记权重
            ExecuteRow(r);
            break;
        case IDM_OPENLOC:
            if (r.kind == Row::Folder) {
                // 文件夹项：直接打开该文件夹
                ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr, nullptr,
                              SW_SHOWNORMAL);
            } else {
                // 文件/程序：在资源管理器中定位（高亮选中）
                std::wstring args = L"/select,\"" + r.action + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                              SW_SHOWNORMAL);
            }
            break;
        case IDM_OPENFILE:
            // 路径直达行：真的把该文件交系统打开（与默认的「定位并选中」区分开）
            ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_OPENCMD: {
            // 文件夹项用自身路径；文件/程序项用其所在目录
            std::wstring dir = (r.kind == Row::Folder) ? r.action : DirOf(r.action);
            if (!dir.empty()) OpenShellAt(dir);
            break;
        }
        case IDM_RUNAS:
            ExecuteRowAdmin(r);
            break;
        case IDM_COPYPATH:
            CopyTextToClipboard(r.action);  // Web 行复制的是链接
            break;
        case IDM_WIN_SWITCH:
            ExecuteRow(r);  // Normal → SetForegroundWindow
            break;
        case IDM_WIN_CLOSE:
            ExecuteRow(r, ExecKind::Close);
            break;
        case IDM_WIN_KILL:
            ExecuteRow(r, ExecKind::Kill);
            break;
        case IDM_COPYTITLE:
            CopyTextToClipboard(r.title);
            break;
        case IDM_COPYPROC:
            CopyTextToClipboard(r.procName);
            break;
        case IDM_SHELL_ADMIN:
            ExecuteRow(r, ExecKind::Admin);
            break;
        default:
            done = false;  // 未选择任何项
    }
    if (done) Hide();
}

// ---------------- 文本编辑 ----------------
static int CaretTextWidth(HDC hdc) {
    HGDIOBJ old = SelectObject(hdc, g.fInput);
    std::wstring s = g.text.substr(0, g.caret);
    SIZE sz{};
    if (!s.empty()) GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

static int CompTextWidth(HDC hdc) {
    if (g.compText.empty()) return 0;
    HGDIOBJ old = SelectObject(hdc, g.fInput);
    SIZE sz{};
    GetTextExtentPoint32W(hdc, g.compText.c_str(), (int)g.compText.size(), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}
static int SelectionStartWidth(HDC hdc) {
    HGDIOBJ old = SelectObject(hdc, g.fInput);
    std::wstring s = g.text.substr(0, g.selStart);
    SIZE sz{};
    if (!s.empty()) GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}
static int SelectionEndWidth(HDC hdc) {
    HGDIOBJ old = SelectObject(hdc, g.fInput);
    std::wstring s = g.text.substr(0, g.selEnd);
    SIZE sz{};
    if (!s.empty()) GetTextExtentPoint32W(hdc, s.c_str(), (int)s.size(), &sz);
    SelectObject(hdc, old);
    return sz.cx;
}

static void EnsureCaretVisible() {
    HDC hdc = GetDC(g.hwnd);
    int cw = CaretTextWidth(hdc) + CompTextWidth(hdc);  // 含组合串，长拼音时保持整段可见
    ReleaseDC(g.hwnd, hdc);
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int avail = rc.right - 2 * S(kBasePad);
    if (cw - g.scrollX > avail) g.scrollX = cw - avail;
    if (cw < g.scrollX) g.scrollX = cw;
    if (g.scrollX < 0) g.scrollX = 0;
}

// 根据输入行内的 x 坐标反查最近字符索引（按字形宽度累加近似）。
// 用于鼠标点击 / 拖拽 / 双击选词时光标定位。
static size_t CaretFromX(int clientX) {
    HDC hdc = GetDC(g.hwnd);
    SelectObject(hdc, g.fInput);
    int targetX = clientX - S(kBasePad) + g.scrollX;
    if (targetX <= 0) {
        ReleaseDC(g.hwnd, hdc);
        return 0;
    }
    size_t best = g.text.size();
    int prevW = 0;
    for (size_t i = 1; i <= g.text.size(); ++i) {
        SIZE sz{};
        GetTextExtentPoint32W(hdc, g.text.c_str(), (int)i, &sz);
        if (sz.cx > targetX) {
            // 在 i-1 与 i 之间取中点更接近的那一侧
            best = (sz.cx - targetX < targetX - prevW) ? i : i - 1;
            break;
        }
        prevW = sz.cx;
    }
    ReleaseDC(g.hwnd, hdc);
    return best;
}

// 找出光标位置处的“词”边界（连续非空白字符）。用于双击选词。
static void WordBoundsAt(size_t pos, size_t& a, size_t& b) {
    a = b = pos;
    if (g.text.empty()) return;
    if (pos > g.text.size()) pos = g.text.size();
    auto isSep = [](WCHAR c) {
        return c == L' ' || c == L'\t' || c == L',' || c == L';' || c == L'/'
            || c == L'\\' || c == L'|' || c == L'\'' || c == L'"';
    };
    while (a > 0 && !isSep(g.text[a - 1])) a--;
    while (b < g.text.size() && !isSep(g.text[b])) b++;
}

// IME 定位：组合窗口与候选窗口都锚定到光标右下（客户区坐标，不可转屏幕坐标——
// COMPOSITIONFORM/CANDIDATEFORM 的契约是相对包含组合窗口的窗口客户区原点）。
// CFS_POINT 会被 IME 自行调整甚至忽略，因此用 CFS_FORCE_POSITION / CFS_CANDIDATEPOS。
static void SetImePos() {
    if (GetFocus() != g.hwnd) return;
    HIMC hIMC = ImmGetContext(g.hwnd);
    if (!hIMC) return;
    HDC hdc = GetDC(g.hwnd);
    int cw = CaretTextWidth(hdc);
    ReleaseDC(g.hwnd, hdc);
    POINT pt{S(kBasePad) - g.scrollX + cw, S(kBaseInputH) - S(10)};
    if (pt.x < 0) pt.x = 0;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_FORCE_POSITION;
    cf.ptCurrentPos = pt;
    ImmSetCompositionWindow(hIMC, &cf);
    CANDIDATEFORM cdf{};
    cdf.dwIndex = 0;
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = pt;
    ImmSetCandidateWindow(hIMC, &cdf);
    ImmReleaseContext(g.hwnd, hIMC);
}

static void AfterEdit() {
    g.caretOn = true;
    EnsureCaretVisible();
    SetImePos();
    Refresh();
}

// 删除当前选区（若有）；返回是否进行了删减。
static bool DeleteSelection() {
    if (g.selStart == g.selEnd) return false;
    size_t a = (g.selStart < g.selEnd) ? g.selStart : g.selEnd;
    size_t b = (g.selStart < g.selEnd) ? g.selEnd : g.selStart;
    g.text.erase(a, b - a);
    g.caret = a;
    g.selStart = g.selEnd = a;
    return true;
}

static void InsertChars(const WCHAR* s, size_t n) {
    if (n == 0) return;
    DeleteSelection();
    g.text.insert(g.caret, s, n);
    g.caret += n;
    AfterEdit();
}

static void CopyAll() {
    CopyTextToClipboard(g.text);
}

// 复制当前选区；无选区时退化为全选复制（与原来 Ctrl+C 行为一致）
static void CopySelection() {
    if (g.selStart != g.selEnd) {
        size_t a = (g.selStart < g.selEnd) ? g.selStart : g.selEnd;
        size_t b = (g.selStart < g.selEnd) ? g.selEnd : g.selStart;
        CopyTextToClipboard(g.text.substr(a, b - a));
    } else {
        CopyAll();
    }
}

static void Paste() {
    if (!OpenClipboard(g.hwnd)) return;
    HGLOBAL h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const WCHAR* p = (const WCHAR*)GlobalLock(h);
        if (p) InsertChars(p, wcslen(p));
        GlobalUnlock(h);
    }
    CloseClipboard();
}

// 全选：移动光标到末尾，选区覆盖整段文本
static void SelectAll() {
    g.selStart = 0;
    g.selEnd = g.text.size();
    g.caret = g.text.size();
    AfterEdit();
}

// ---------------- 托盘 / 注册表 / 圆角 ----------------
// Win11 DWM 圆角 + 描边；旧系统调用失败则保持直角（Paint 回退 GDI 描边）
static void ApplyRoundCorners(HWND h) {
    DWORD pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    COLORREF border = (g.theme ? g.theme->border : RGB(80, 80, 80));
    if (SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border))))
        g.dwmBorderOk = true;
}

// 应用图标从内置 .ico 资源（flowtary.ico，多尺寸：16/24/32/48/64/128/256）加载。
// 取代原先的字体自绘方案：exe 文件、窗口、任务栏/alt-tab、托盘都用同一套图标。
// 传给目标尺寸（px）让 LoadImageW 从多帧 ico 里取最合适的那一帧。
static HICON LoadAppIcon(int px) {
    HINSTANCE hi = GetModuleHandleW(nullptr);
    HICON h = (HICON)LoadImageW(hi, MAKEINTRESOURCE(FT_APPICON_ID), IMAGE_ICON,
                                px, px, LR_DEFAULTCOLOR);
    if (!h) h = LoadIconW(hi, MAKEINTRESOURCE(FT_APPICON_ID));  // 兜底：任选尺寸
    return h;
}

static void TrayAdd() {
    if (!g.hwnd) return;
    if (!g.hTrayIcon) g.hTrayIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON));
    if (!g.hTrayIcon) return; // Guard against icon creation failure
    ZeroMemory(&g.nid, sizeof(g.nid));
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = g.hTrayIcon;
    std::wstring tip = g.startingUp ? L"Flowtary — 正在启动中…"
                                    : (g.hotkeyWake ? (L"Flowtary — " + g.hotkeyName + L" 唤出")
                                                    : L"Flowtary — 左键点击唤出");
    lstrcpynW(g.nid.szTip, tip.c_str(), ARRAYSIZE(g.nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

static void TrayUpdateTip() {
    if (!g.hwnd) return;
    std::wstring tip = g.startingUp ? L"Flowtary — 正在启动中…"
                                    : (g.hotkeyWake ? (L"Flowtary — " + g.hotkeyName + L" 唤出")
                                                    : L"Flowtary — 左键点击唤出");
    lstrcpynW(g.nid.szTip, tip.c_str(), ARRAYSIZE(g.nid.szTip));
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
}

static void TrayBalloon(const std::wstring& title, const std::wstring& msg) {
    if (!g.nid.hWnd) return;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g.nid.dwInfoFlags = NIIF_INFO;
    lstrcpynW(g.nid.szInfoTitle, title.c_str(), ARRAYSIZE(g.nid.szInfoTitle));
    lstrcpynW(g.nid.szInfo, msg.c_str(), ARRAYSIZE(g.nid.szInfo));
    Shell_NotifyIconW(NIM_MODIFY, &g.nid);
    // 短暂展示后清除 szInfo，避免后续 NIM_MODIFY（如提示文案更新）再次弹出
    SetTimer(g.hwnd, kTimerBalloon, 3500, nullptr);
}

// 注册唤起快捷键（依次尝试候选组合，成功即停），返回是否注册成功
static bool RegisterWakeHotkey() {
    for (int i = 0; i < 3; ++i) {
        if (RegisterHotKey(g.hwnd, 1, kWakeHotkeys[i].mod, kWakeHotkeys[i].vk)) {
            g.hotkeyName = kWakeHotkeys[i].name;
            g.wakeIndex = i;
            g.hotkeyRegistered = true;
            return true;
        }
    }
    g.hotkeyRegistered = false;
    g.wakeIndex = -1;
    return false;
}

// —— 低级键盘钩子（WH_KEYBOARD_LL）：启动器快捷键权限高于其他应用 ——
// RegisterHotKey 是先到先得，组合一旦被其他程序先注册，本程序永远抢不到。
// 低级键盘钩子在系统分发键盘消息（含 RegisterHotKey 处理）的最前端，
// 命中首选唤起组合（Alt+Space）时直接吞掉按键并触发唤出，无论该组合当前被谁持有。
static LRESULT CALLBACK WakeHookProc(int code, WPARAM wParam, LPARAM lParam) {
    static bool sTriggered = false;  // 已触发且未松键：忽略按住不放的键盘重复
    if (code == HC_ACTION && g.hotkeyWake && g.hwnd) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const auto& hk = kWakeHotkeys[0];  // 首选组合：Alt+Space
        if (k->vkCode == hk.vk) {
            if (k->flags & LLKHF_UP) {
                sTriggered = false;  // 松键复位，允许下一次触发
            } else if (!sTriggered && !(k->flags & LLKHF_INJECTED) &&
                       (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
                bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                if (altDown == ((hk.mod & MOD_ALT) != 0) &&
                    ctrlDown == ((hk.mod & MOD_CONTROL) != 0)) {
                    sTriggered = true;
                    PostMessageW(g.hwnd, WM_HOTKEY, 1, 0);  // 与 RegisterHotKey 共用处理路径
                    return 1;  // 吞掉按键：系统与其他程序（含已注册者）都收不到
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

static void InstallWakeHook() {
    if (!g.hWakeHook)
        g.hWakeHook =
            SetWindowsHookExW(WH_KEYBOARD_LL, WakeHookProc, GetModuleHandleW(nullptr), 0);
}

static void RemoveWakeHook() {
    if (g.hWakeHook) {
        UnhookWindowsHookEx(g.hWakeHook);
        g.hWakeHook = nullptr;
    }
}

// 未持有首选组合（或完全没注册上）时保持重试定时器；拿到首选后自动停止
static void UpdateHotkeyRetryTimer() {
    if (g.hotkeyWake && g.hwnd && g.wakeIndex != 0)
        SetTimer(g.hwnd, kTimerHotkeyRetry, 5000, nullptr);
    else
        KillTimer(g.hwnd, kTimerHotkeyRetry);
}

// 托盘菜单：切换唤起快捷键开关（关闭=注销热键，开启=重新依次尝试注册）
static void ToggleWakeHotkey() {
    if (!g.hwnd) return;
    if (g.hotkeyWake) {
        UnregisterHotKey(g.hwnd, 1);  // 关闭：注销唤起热键
        g.hotkeyWake = false;
        g.hotkeyRegistered = false;
        g.wakeIndex = -1;
        RemoveWakeHook();
    } else {
        InstallWakeHook();  // 先装钩子：即使注册失败，首选组合依然可用
        if (!RegisterWakeHotkey()) {
            TrayBalloon(L"Flowtary", L"唤起快捷键注册暂被其他程序占用，已通过钩子接管，将自动重试");
        }
        g.hotkeyWake = true;
    }
    UpdateHotkeyRetryTimer();
    DWORD v = g.hotkeyWake ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"HotkeyWake", REG_DWORD, &v,
                    sizeof(v));
    TrayUpdateTip();  // 提示文案随开关状态刷新
}

static void TrayRemove() {
    if (g.nid.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &g.nid);
        g.nid.hWnd = nullptr;
    }
    if (g.hTrayIcon) {
        DestroyIcon(g.hTrayIcon);
        g.hTrayIcon = nullptr;
    }
    if (g.hAppIcon) {
        DestroyIcon(g.hAppIcon);
        g.hAppIcon = nullptr;
    }
    ZeroMemory(&g.nid, sizeof(g.nid)); // Clear structure for safety
}

static std::wstring GetExePath() {
    WCHAR buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

static bool GetStartupEnabled() {
    WCHAR buf[MAX_PATH]{};
    DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Flowtary",
                        RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}

static void SetStartup(bool on) {
    const WCHAR* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (on) {
        std::wstring v = L"\"" + GetExePath() + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, run, L"Flowtary", REG_SZ, v.c_str(),
                        (DWORD)((v.size() + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, run, L"Flowtary");
    }
}

static std::wstring LoadRegText(const WCHAR* name);  // 前向声明（定义见下方）
static void LoadSettings() {
    g.centerWake = true;
    g.hotkeyMode = 0;  // 默认 Alt+数字
    DWORD v = 1, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CenterWake",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        g.centerWake = v != 0;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"HotkeyMode",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) {
        g.hotkeyMode = (int)v;
    } else if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"HotkeyLetters",
                            RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS) {
        g.hotkeyMode = v ? 0 : 2;  // 兼容旧键：1=数字(0)，0=关闭(2)
    }
    if (g.hotkeyMode < 0 || g.hotkeyMode > 2) g.hotkeyMode = 0;
    // 唤起快捷键总开关（托盘菜单可切换；默认开）
    v = 1;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"HotkeyWake",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        g.hotkeyWake = v != 0;
    v = (DWORD)g.themeIdx;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Theme", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.themeIdx = (int)v;
    if (g.themeIdx < 0 || g.themeIdx >= (int)(sizeof(kThemes) / sizeof(kThemes[0])))
        g.themeIdx = 0;
    g.theme = &kThemes[g.themeIdx];
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Beautify", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.beautify = v != 0;
    else
        g.beautify = true;  // 默认开
    // 抗残影双缓冲（实验性）：默认关，需用户手动开启；开启后整窗双缓冲消除残影
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"AntiGhost", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.antiGhost = v != 0;
    else
        g.antiGhost = false;  // 默认关（实验性）
    cb = sizeof(v);
    // 主题微调（透明度/圆角）：按主题读回 blob；主题数量变化时整体忽略（用预设默认）
    {
        const int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
        DWORD cbT = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ThemeTune",
                         RRF_RT_REG_BINARY, nullptr, nullptr, &cbT) == ERROR_SUCCESS &&
            cbT == sizeof(ThemeTune) * n) {
            sTune.resize(n);
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ThemeTune",
                             RRF_RT_REG_BINARY, nullptr, sTune.data(), &cbT) == ERROR_SUCCESS) {
                for (int i = 0; i < n; ++i) {
                    int a = (std::max)(0, (std::min)(255, sTune[i].alpha));
                    int r = (std::max)(0, (std::min)(14, sTune[i].radius));
                    kThemes[i].alpha = (BYTE)a;
                    kThemes[i].radiusWindow = r;
                    kThemes[i].radiusCard = (int)(r * 0.8);
                    kThemes[i].radiusButton = (int)(r * 0.6);
                    kThemes[i].radiusInput = (int)(r * 0.6);
                }
            }
        }
    }
    // 点击加权排序（性能参数，见设置「搜索权重」Tab）
    v = 1;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightEnabled",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        g.weightEnabled = v != 0;
    else
        g.weightEnabled = true;  // 默认开
    v = 1;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightFlush",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        g.weightFlush = (int)v;
    if (g.weightFlush < 0 || g.weightFlush > 2) g.weightFlush = 1;
    v = 5000;
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightMaxEntries",
                     RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS)
        g.weightMaxEntries = (int)v;
    if (g.weightMaxEntries < 100) g.weightMaxEntries = 5000;
    if (g.weightMaxEntries > 200000) g.weightMaxEntries = 200000;

    // top 命令开关（空格+top 回车置顶/取消置顶当前窗口；默认开）
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"TopCmd", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.topEnabled = v != 0;
    else
        g.topEnabled = true;
    // cmd 命令开关（空格+cmd 执行 Shell 命令，原 "> 命令"；默认开）
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CmdCmd", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.cmdEnabled = v != 0;
    else
        g.cmdEnabled = true;
    // w 命令开关（空格+w 切换窗口，原 "< 关键词"；默认开）
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinCmd", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.winEnabled = v != 0;
    else
        g.winEnabled = true;
    // ss 截图命令开关（空格+ss 截图；默认开）
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureCmd", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.captureEnabled = v != 0;
    else
        g.captureEnabled = true;

    // —— Shell 与窗口设置 ——
    DWORD vv = 0; DWORD cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellType",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS) {
        g.shellType = (int)vv;
        if (g.shellType < 0 || g.shellType > 3) g.shellType = 0;
    }
    g.shellCustomPath = LoadRegText(L"ShellCustomPath");
    g.shellCustomArgs = LoadRegText(L"ShellCustomArgs");
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellDefaultCwd",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS) {
        g.shellDefaultCwd = (int)vv;
        if (g.shellDefaultCwd < 0 || g.shellDefaultCwd > 2) g.shellDefaultCwd = 0;
    }
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellShowWindow",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
        g.shellShowWindow = vv != 0;
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinGroupProc",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
        g.winGroupProc = vv != 0;
    else
        g.winGroupProc = true;  // 默认开
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowUwp",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
        g.winShowUwp = vv != 0;
    else
        g.winShowUwp = true;
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowProc",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
        g.winShowProc = vv != 0;
    else
        g.winShowProc = true;
    cbv = sizeof(vv);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinCacheSec",
                     RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS) {
        g.winCacheSec = (int)vv;
        if (g.winCacheSec < 1 || g.winCacheSec > 60) g.winCacheSec = 5;
    }
}

// ---------------- 网页规则解析与持久化 ----------------
// 编辑器文本 → 规则列表：每行「前缀 空格 URL模板」，# 开头为注释，空行忽略
static std::vector<WebCmd> ParseRules(const std::wstring& text) {
    std::vector<WebCmd> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = TrimW(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == L'#') continue;
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;  // 缺 URL 模板
        WebCmd r;
        r.prefix = ToLowerW(TrimW(line.substr(0, sp)));
        r.urlTemplate = TrimW(line.substr(sp + 1));
        if (r.prefix.empty() || r.urlTemplate.empty()) continue;
        bool dup = false;
        for (auto& e : out)
            if (e.prefix == r.prefix) { dup = true; break; }
        if (!dup) out.push_back(std::move(r));
    }
    return out;
}

// 规则列表 → 编辑器文本
static std::wstring RulesToText(const std::vector<WebCmd>& rules) {
    std::wstring t;
    for (auto& r : rules) t += r.prefix + L" " + r.urlTemplate + L"\r\n";
    return t;
}

static void LoadWebRules() {
    std::wstring text;
    DWORD type = 0, cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WebRules", RRF_RT_REG_SZ,
                     &type, nullptr, &cb) == ERROR_SUCCESS && cb > sizeof(WCHAR)) {
        std::vector<WCHAR> buf(cb / sizeof(WCHAR));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WebRules", RRF_RT_REG_SZ,
                         nullptr, buf.data(), &cb) == ERROR_SUCCESS)
            text = buf.data();
    }
    g.webCmds = ParseRules(text.empty() ? DefaultRulesText() : text);
}

static void SaveWebRules(const std::wstring& editorText) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WebRules", REG_SZ,
                    editorText.c_str(), (DWORD)((editorText.size() + 1) * sizeof(WCHAR)));
    g.webCmds = ParseRules(editorText);
}

// ---------------- 一键启动 / 一键关闭组 ----------------
// 文本格式：每行「关键字 + 空白 + 目标;目标;…」，`#` 开头为注释，重复关键字取第一条。
// 启动组的目标是**文件路径**；关闭组的目标是**进程名**（可带或不带 .exe）。
static std::wstring LoadRegText(const WCHAR* name) {
    std::wstring text;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", name, RRF_RT_REG_SZ, nullptr,
                     nullptr, &cb) == ERROR_SUCCESS && cb > sizeof(WCHAR)) {
        std::vector<WCHAR> buf(cb / sizeof(WCHAR));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", name, RRF_RT_REG_SZ, nullptr,
                         buf.data(), &cb) == ERROR_SUCCESS)
            text = buf.data();
    }
    return text;
}

static void SaveRegText(const WCHAR* name, const std::wstring& s) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", name, REG_SZ, s.c_str(),
                    (DWORD)((s.size() + 1) * sizeof(WCHAR)));
}

static std::vector<CmdGroup> ParseGroups(const std::wstring& text) {
    std::vector<CmdGroup> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = TrimW(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == L'#') continue;
        size_t sp = line.find_first_of(L" \t");
        if (sp == std::wstring::npos) continue;  // 缺目标列表
        CmdGroup g;
        g.key = ToLowerW(TrimW(line.substr(0, sp)));
        if (g.key.empty()) continue;
        std::wstring rest = line.substr(sp + 1);
        size_t p = 0;
        while (p <= rest.size()) {
            size_t q = rest.find(L';', p);
            if (q == std::wstring::npos) q = rest.size();
            std::wstring one = TrimW(rest.substr(p, q - p));
            if (!one.empty()) g.targets.push_back(one);
            if (q == rest.size()) break;
            p = q + 1;
        }
        if (g.targets.empty()) continue;
        bool dup = false;
        for (auto& e : out)
            if (e.key == g.key) { dup = true; break; }
        if (!dup) out.push_back(std::move(g));
    }
    return out;
}

static std::wstring GroupsToText(const std::vector<CmdGroup>& gs) {
    std::wstring t;
    for (auto& g : gs) {
        t += g.key + L" ";
        for (size_t i = 0; i < g.targets.size(); ++i) {
            if (i) t += L";";
            t += g.targets[i];
        }
        t += L"\r\n";
    }
    return t;
}

static void LoadGroupRules() {
    g.groupsLaunch = ParseGroups(LoadRegText(L"GroupLaunch"));
    g.groupsKill = ParseGroups(LoadRegText(L"GroupKill"));
}

// ---------------- 排除路径 ----------------
// 文本 → 模式列表：每行一个（支持通配符 * ?），# 开头为注释，空行忽略。
// 模式按文本原样保留通配符，匹配时再归一化（反斜杠/小写）。
static std::vector<std::wstring> ParseExcludePaths(const std::wstring& text) {
    std::vector<std::wstring> out;
    size_t i = 0, n = text.size();
    while (i < n) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = TrimW(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == L'#') continue;
        // 跳过重复：同一归一化模式只保留第一次出现的原文
        std::wstring norm = NormalizePatternForExclude(line);
        bool dup = false;
        for (auto& e : out)
            if (NormalizePatternForExclude(e) == norm) { dup = true; break; }
        if (!dup) out.push_back(line);
    }
    return out;
}

static std::wstring ExcludePathsToText(const std::vector<std::wstring>& pats) {
    std::wstring t;
    // 顶部一行注释：提示用户语法与作用范围
    t += L"# 排除路径：每行一条，支持通配符 * ?。路径以排除项开头即视为命中（含整棵子树）。\r\n";
    t += L"# 作用于 Everything 搜索（d/f 前缀）与本地程序搜索的结果，网页与一键组不受影响。\r\n";
    for (auto& p : pats) t += p + L"\r\n";
    return t;
}

// 默认排除项：每个本地盘符的回收站 + 系统还原信息 + 旧式 RECYCLER。
// 用 GetLogicalDrives 动态展开，确保多盘用户也覆盖到。
static std::wstring DefaultExcludePathsText() {
    std::wstring t =
        L"# 排除路径：每行一条，支持通配符 * ?。路径以排除项开头即视为命中（含整棵子树）。\r\n"
        L"# 作用于 Everything 搜索（d/f 前缀）与本地程序搜索的结果，网页与一键组不受影响。\r\n"
        L"# 默认已排除各盘符下的回收站、系统还原信息等系统目录，可继续追加自定义路径。\r\n";
    DWORD bits = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(bits & (1u << i))) continue;
        WCHAR letter = (WCHAR)(L'A' + i);
        t += letter;
        t += L":\\$RECYCLE.BIN\r\n";
    }
    t += L"*\\System Volume Information\\*\r\n";
    t += L"*\\RECYCLER\\*\r\n";
    return t;
}

// 「恢复默认」按钮用的纯默认（不含用户自定义项）
static std::vector<std::wstring> DefaultExcludePaths() {
    std::vector<std::wstring> out;
    DWORD bits = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(bits & (1u << i))) continue;
        std::wstring s;
        s.push_back((WCHAR)(L'A' + i));
        s += L":\\$RECYCLE.BIN";
        out.push_back(s);
    }
    out.push_back(L"*\\System Volume Information\\*");
    out.push_back(L"*\\RECYCLER\\*");
    return out;
}

static void LoadExcludePaths() {
    std::wstring text = LoadRegText(L"ExcludePaths");
    if (text.empty()) {
        // 首次启动：写入默认项文本（便于用户在编辑器看到默认行为），并把默认模式灌入内存
        std::wstring def = DefaultExcludePathsText();
        SaveRegText(L"ExcludePaths", def);
        g.excludePaths = DefaultExcludePaths();
        RebuildExcludeNorm();
        return;
    }
    auto parsed = ParseExcludePaths(text);
    g.excludePaths = parsed.empty() ? DefaultExcludePaths() : parsed;
    RebuildExcludeNorm();
}

static const CmdGroup* FindGroup(const std::vector<CmdGroup>& gs, const std::wstring& key) {
    for (auto& g : gs)
        if (g.key == key) return &g;
    return nullptr;
}

// 按进程名结束进程（等价 taskkill /F /IM，但不弹控制台窗口）；返回实际结束的进程数
static int KillProcessesByName(const std::wstring& name) {
    std::wstring target = ToLowerW(name);
    if (!EndsWithI(target, L".exe")) target += L".exe";
    int killed = 0;
    DWORD self = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (ToLowerW(pe.szExeFile) != target) continue;
            if (pe.th32ProcessID == self) continue;  // 不结束自身
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) {
                if (TerminateProcess(h, 1)) ++killed;
                CloseHandle(h);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return killed;
}

// ---------------- 点击权重（点击记忆加权排序） ----------------
// 用户点击 / 回车 / 快捷键打开某个本地文件、文件夹或程序条目时，针对本次输入框中的
// 「有效搜索词」给该条目的完整绝对路径 +1 权重；相同有效词再次搜索时该条目排序提前，
// 不同搜索词之间的权重互相独立。
// 有效词通过拆分输入得到：识别开头的前置命令关键字，只有前缀之后的实际搜索内容参与
// 权重计算与存储；命中不应保存的命令前缀（如 gg / bd 等网页命令）时本次点击完全不记录。

// 权重数据文件格式（UTF-16 文本，每行一条）：搜索词 \x1f 完整路径 \x1f 权重
static const WCHAR kWeightSep = L'\x1f';

static std::wstring WeightsFilePath() {
    WCHAR buf[MAX_PATH]{};
    std::wstring dir;
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH))
        dir = std::wstring(buf) + L"\\Flowtary";
    else
        dir = DirOf(GetExePath());
    return dir + L"\\weights.dat";
}

static const WCHAR* WeightFlushText(int m) {
    return m == 0 ? L"每次点击立即写入磁盘"
         : m == 1 ? L"延迟 3 秒合并写入磁盘"
                  : L"仅程序退出时写入磁盘";
}

// 标准化有效搜索词：去除首尾空格、统一小写，作为权重记录的 key
static std::wstring NormalizeSearchTerm(const std::wstring& s) {
    return ToLowerW(TrimW(s));
}

// 拆分输入：识别开头的前置命令关键字，把命令前缀和实际搜索内容分离。
// 返回本次点击是否应记录权重；termOut 输出标准化后的有效搜索词。
//   f/d 前缀（Everything 文件/文件夹搜索）→ 只有前缀后的关键词参与权重
//   网页命令前缀（如 gg、bd）           → 完全不记录，不写入权重存储
//   其它（程序搜索，无前缀）             → 整个输入即有效搜索词
static bool WeightTermFromInput(std::wstring& termOut) {
    termOut.clear();
    const std::wstring& t = g.text;
    size_t sp = t.find(L' ');
    std::wstring tok = ToLowerW(sp == std::wstring::npos ? t : t.substr(0, sp));
    std::wstring rest = sp == std::wstring::npos ? L"" : TrimW(t.substr(sp + 1));
    // 与 Refresh() 对齐：无空格时首词同样可能是命令关键字（f/d 或网页前缀）
    if (tok == L"d" || tok == L"f") {
        termOut = NormalizeSearchTerm(rest);
        return !termOut.empty();
    }
    if (FindWebCmd(tok)) return false;  // gg 等网页前缀：本次点击不记录权重
    // 输入本身就是一条路径（路径直达条，含 %VAR% 写法）：整条路径当搜索词存进权重表没有意义
    if (t.find(L'\\') != std::wstring::npos || t.find(L'%') != std::wstring::npos) return false;
    termOut = NormalizeSearchTerm(t);
    return !termOut.empty();
}

static int GetClickWeight(const std::wstring& term, const std::wstring& path) {
    if (term.empty() || path.empty()) return 0;
    auto it = g.weights.find(term);
    if (it == g.weights.end()) return 0;
    auto jt = it->second.find(path);
    return jt == it->second.end() ? 0 : jt->second;
}

// 到达条目上限时，淘汰全局权重最低的一条（跳过即将写入的新条目），为新记录腾位置
static void PruneOneMinWeight(const std::wstring& keepTerm, const std::wstring& keepPath) {
    std::wstring minTerm, minPath;
    int minW = INT_MAX;
    for (auto& kv : g.weights) {
        for (auto& pv : kv.second) {
            if (kv.first == keepTerm && pv.first == keepPath) continue;
            if (pv.second < minW) {
                minW = pv.second;
                minTerm = kv.first;
                minPath = pv.first;
            }
        }
    }
    if (minTerm.empty()) return;
    auto it = g.weights.find(minTerm);
    it->second.erase(minPath);
    if (it->second.empty()) g.weights.erase(it);
    if (g.weightCount > 0) --g.weightCount;
}

// 记录一次点击权重（仅本地文件 / 文件夹 / 程序条目）
static void RecordClickWeight(const Row& r) {
    if (!g.weightEnabled) return;
    if (r.kind != Row::File && r.kind != Row::Folder && r.kind != Row::Prog) return;
    std::wstring term;
    if (!WeightTermFromInput(term)) return;  // 无效词或命中不保存的前缀（如 gg）：完全不记录
    std::wstring path = r.action;           // 完整绝对路径作为条目唯一标识
    if (path.empty()) return;

    auto& m = g.weights[term];
    auto it = m.find(path);
    if (it == m.end()) {
        if (g.weightCount >= (size_t)(std::max)(1, g.weightMaxEntries))
            PruneOneMinWeight(term, path);
        m[path] = 1;
        ++g.weightCount;
    } else {
        ++it->second;
    }
    g.weightsDirty = true;

    // 按所选写盘策略持久化
    if (g.weightFlush == 0) {
        SaveClickWeightsNow();
    } else if (g.weightFlush == 1) {
        SetTimer(g.hwnd, kTimerWeightSave, kWeightSaveDelayMs, nullptr);  // 重复点击会重置计时
    }
    // weightFlush == 2：仅退出时写入（见主窗口 WM_DESTROY）
}

// 立即写盘：收集全部条目，按权重降序裁剪到上限后整体写出
static void SaveClickWeightsNow() {
    if (g.hwnd) KillTimer(g.hwnd, kTimerWeightSave);
    if (!g.weightsDirty) return;
    g.weightsDirty = false;

    struct E { const std::wstring* term; const std::wstring* path; int w; };
    std::vector<E> all;
    all.reserve(g.weightCount);
    for (auto& kv : g.weights)
        for (auto& pv : kv.second) all.push_back({&kv.first, &pv.first, pv.second});
    std::sort(all.begin(), all.end(), [](const E& a, const E& b) { return a.w > b.w; });
    size_t cap = (size_t)(std::max)(1, g.weightMaxEntries);
    if (all.size() > cap) all.resize(cap);

    std::wstring text;
    for (auto& e : all) {
        text += *e.term; text += kWeightSep;
        text += *e.path; text += kWeightSep;
        text += std::to_wstring(e.w);
        text += L"\r\n";
    }
    std::wstring file = WeightsFilePath();
    CreateDirectoryW(DirOf(file).c_str(), nullptr);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { g.weightsDirty = true; return; }
    DWORD wr = 0;
    WriteFile(f, text.data(), (DWORD)(text.size() * sizeof(WCHAR)), &wr, nullptr);
    CloseHandle(f);
}

// 启动时加载：文件按权重降序保存，超出上限的低权重条目直接截断
static void LoadClickWeights() {
    g.weights.clear();
    g.weightCount = 0;
    g.weightsDirty = false;
    HANDLE f = CreateFileW(WeightsFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(f, nullptr);
    if (sz == INVALID_FILE_SIZE || sz < sizeof(WCHAR) || sz > 64u * 1024 * 1024) {
        CloseHandle(f);
        return;
    }
    std::vector<WCHAR> buf(sz / sizeof(WCHAR) + 1, 0);
    DWORD rd = 0;
    BOOL ok = ReadFile(f, buf.data(), (sz / sizeof(WCHAR)) * sizeof(WCHAR), &rd, nullptr);
    CloseHandle(f);
    if (!ok) return;
    std::wstring text(buf.data(), rd / sizeof(WCHAR));

    size_t cap = (size_t)(std::max)(1, g.weightMaxEntries);
    size_t i = 0, n = text.size();
    while (i < n && g.weightCount < cap) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = text.substr(i, j - i);
        i = j + 1;
        size_t s1 = line.find(kWeightSep);
        size_t s2 = (s1 == std::wstring::npos) ? std::wstring::npos
                                               : line.find(kWeightSep, s1 + 1);
        if (s1 == std::wstring::npos || s2 == std::wstring::npos) continue;
        std::wstring term = line.substr(0, s1);
        std::wstring path = line.substr(s1 + 1, s2 - s1 - 1);
        int w = _wtoi(line.c_str() + s2 + 1);
        if (term.empty() || path.empty() || w <= 0) continue;
        if (g.weights[term].emplace(path, w).second) ++g.weightCount;
    }
}

// 清空全部权重（设置页两步确认后调用）：立即写空文件
static void ClearClickWeights() {
    g.weights.clear();
    g.weightCount = 0;
    g.weightsDirty = true;
    SaveClickWeightsNow();
}

// ---------------- 绘制 ----------------
static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = S(kBasePad), inputH = S(kBaseInputH), rowH = S(kBaseRowH);
    const Theme& t = *g.theme;

    HDC mem;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, W, H);
    oldBmp = SelectObject(mem, bmp);
    if (t.bg2 != t.bg) FillVGradient(mem, 0, 0, W, H, t.bg, t.bg2);
    else { HBRUSH bgBr = CreateSolidBrush(t.bg); FillRect(mem, &rc, bgBr); DeleteObject(bgBr); }
    if (t.stars) {
        HBRUSH starB[3];
        starB[0] = CreateSolidBrush(RGB(255, 255, 255));
        starB[1] = CreateSolidBrush(RGB(180, 190, 235));
        starB[2] = CreateSolidBrush(RGB(120, 130, 190));
        for (auto& s : g.stars) {
            // 亮度 ≥200 亮白、≥160 淡蓝、其余暗蓝
            HBRUSH b = s.v >= 200 ? starB[0] : (s.v >= 160 ? starB[1] : starB[2]);
            int r = s.x, bb = s.y + (s.s == 2 ? 1 : 0);
            RECT sr{r, s.y, r + s.s, bb + s.s};
            FillRect(mem, &sr, b);
        }
        for (auto& b : starB) DeleteObject(b);
    }

    // 分层面板：输入区 + 结果卡片（圆角 + 轻外影 + 顶部内高光）
    {
        RECT inR{0, 0, W, inputH};
        FillRoundRect(mem, inR, S(t.radiusInput), g.brInputBg);
        PaintInsetShadow(mem, inR, S(t.radiusInput), t.bgInput);
        int gap = S(6);
        RECT cardR{S(kBasePad), inputH + gap, W - S(kBasePad), H - gap};
        if (cardR.bottom > cardR.top && cardR.right > cardR.left)
            PaintOutShadow(mem, cardR, S(t.radiusCard), S(t.shCardX), S(t.shCardY), S(t.shCardBlur), t.shCardA, t.shCardColor);
        FillRoundRect(mem, cardR, S(t.radiusCard), g.brCardBg);
        PaintInsetShadow(mem, cardR, S(t.radiusCard), t.bgCard);
    }
    // 顶部强调色条
    if (t.accentStrip) {
        HBRUSH ab = CreateSolidBrush(t.accent);
        RECT ar{0, 0, W, S(3)};
        FillRect(mem, &ar, ab);
        DeleteObject(ab);
    }
    // 角部辉光（低透明度装饰）
    if (t.cornerGlow) {
        RECT gr0{0, 0, S(80), S(80)};
        PaintDivider(mem, gr0, t.accent2, 16);
    }

    SetBkMode(mem, TRANSPARENT);

    // 输入行
    SelectObject(mem, g.fInput);
    {
        int textX = pad - g.scrollX;
        RECT r{textX, 0, W + 4096, inputH};
        SaveDC(mem);
        IntersectClipRect(mem, 0, 0, W, inputH);
        // 选区矩形：在文本下方作为高亮底色
        if (g.selStart != g.selEnd) {
            int x1 = pad - g.scrollX + SelectionStartWidth(mem);
            int x2 = pad - g.scrollX + SelectionEndWidth(mem);
            if (x2 < x1) std::swap(x1, x2);
            RECT sr{x1, inputH / 2 - S(13), x2, inputH / 2 + S(13)};
            HBRUSH selTextBr = CreateSolidBrush(Blend(t.bgCard, t.accent, 0.35f));
            FillRect(mem, &sr, selTextBr);
            DeleteObject(selTextBr);
        }
        if (g.text.empty() && g.compText.empty()) {
            SetTextColor(mem, t.textDis);
            DrawTextW(mem, L"f/d 搜文件 · bd/bili/zhihu… 搜网页 · 直接输入启动程序", -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            SetTextColor(mem, t.textBody);
            DrawTextW(mem, g.text.c_str(), (int)g.text.size(), &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        // 内联渲染 IME 组合串（未上屏临时文字）：跟在光标后，浅灰字 + 下划线
        if (!g.compText.empty()) {
            int cx = pad - g.scrollX + CaretTextWidth(mem);
            SetTextColor(mem, t.textSec);
            RECT cr{cx, 0, W + 4096, inputH};
            DrawTextW(mem, g.compText.c_str(), (int)g.compText.size(), &cr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE csz{};
            GetTextExtentPoint32W(mem, g.compText.c_str(), (int)g.compText.size(), &csz);
            RECT ur{cx, inputH / 2 + S(13), cx + csz.cx, inputH / 2 + S(15)};
            FillRect(mem, &ur, g.brDivider);
        }
        RestoreDC(mem, -1);
    }
    // 光标
    if (g.caretOn && GetFocus() == g.hwnd) {
        int cx = pad + CaretTextWidth(mem) - g.scrollX;
        RECT cr{cx, inputH / 2 - S(13), cx + S(2), inputH / 2 + S(13)};
        HBRUSH carBr = CreateSolidBrush(t.textBody);
        FillRect(mem, &cr, carBr);
        DeleteObject(carBr);
    }
    // 分隔线（半透明，避免纯黑/纯白硬线）
    if (!g.items.empty()) {
        RECT lr{0, inputH, W, inputH + 1};
        PaintDivider(mem, lr, t.divider, 120);
    }

    // 结果列表
    SelectObject(mem, g.fList);
    int rows = (std::min)((int)g.items.size(), (int)ev::kMaxResults);
    HBRUSH selBr = CreateSolidBrush(t.selBg);
    for (int i = 0; i < rows; ++i) {
        Row& row = g.items[i];
        int y = inputH + i * rowH;
        RECT rr{0, y, W, y + rowH};
        if (i == g.sel) {
            FillRect(mem, &rr, selBr);
            // 悬停高亮随动画进度叠加（鼠标移到某行 = 选中该行，hoverT 驱动轻微提亮）
            if (i == g.hoverRow && g.hoverT > 0.001) {
                HBRUSH hb = CreateSolidBrush(Blend(t.selBg, t.accent, (float)(0.14 * g.hoverT)));
                FillRect(mem, &rr, hb);
                DeleteObject(hb);
            }
        } else if (i == g.hoverRow) {
            HBRUSH hb = CreateSolidBrush(Blend(t.bgCard, t.accent2, (float)(0.10 + 0.12 * g.hoverT)));
            FillRect(mem, &rr, hb);
            DeleteObject(hb);
        }
        if (i == g.pressRow && i != g.sel) {
            PaintInsetShadow(mem, rr, 0, t.bgCard);
        }
        int x = pad;
        if (row.kind == Row::Prog && row.prog) {
            HICON ic = GetProgramIcon(row.prog);
            if (ic) DrawIconEx(mem, x, y + (rowH - S(16)) / 2, ic, S(16), S(16), 0, nullptr, DI_NORMAL);
        }
        x += S(16) + S(8);

        // 结果项快捷键徽章（右侧小方框，按当前方案 Alt+数字 / Alt+字母；关闭时无徽章）
        int hkReserved = 0;
        WCHAR hk = HotkeyChar(i);
        if (hk) {
            int bw = S(20), bh = S(20);
            RECT br{W - pad - bw, y + (rowH - bh) / 2, W - pad, y + (rowH - bh) / 2 + bh};
            HBRUSH hbg = CreateSolidBrush(i == g.sel ? t.menuHi : t.menuBg);
            FillRect(mem, &br, hbg);
            DeleteObject(hbg);
            HBRUSH hf = CreateSolidBrush(t.divider);
            FrameRect(mem, &br, hf);
            DeleteObject(hf);
            SetBkMode(mem, TRANSPARENT);
            SetTextColor(mem, t.sub);
            DrawTextW(mem, &hk, 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            hkReserved = bw + S(6);
        }

        // 副标题（右侧灰色；有徽章时让位）
        int subReserved = 0;
        if (!row.sub.empty()) {
            SIZE sz{};
            GetTextExtentPoint32W(mem, row.sub.c_str(), (int)row.sub.size(), &sz);
            int maxSub = (W - pad - hkReserved) / 2;
            int subW = (std::min)((int)sz.cx, maxSub);
            RECT sr{W - pad - hkReserved - subW, y, W - pad - hkReserved, y + rowH};
            SetTextColor(mem, t.textSec);
            DrawTextW(mem, row.sub.c_str(), (int)row.sub.size(), &sr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            subReserved = subW + S(8);
        }
        // 标题
        RECT tr{x, y, W - pad - subReserved - hkReserved, y + rowH};
        SetTextColor(mem, row.kind == Row::Hint ? t.textDis : t.textBody);
        DrawTextW(mem, row.title.c_str(), (int)row.title.size(), &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    DeleteObject(selBr);

    // 描边（DWM 边框不可用时回退 GDI 圆角描边）
    if (!g.dwmBorderOk) {
        RECT fr{0, 0, W, H};
        StrokeRoundRect(mem, fr, S(t.radiusWindow), g.penBorder);
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------- 设置窗口（黑暗模式，全部尺寸随 S() 比例） ----------------
constexpr int IDC_CHK_START = 3001;
constexpr int IDC_LBL_WAKE = 3002;
constexpr int IDC_CMB_WAKE = 3003;
constexpr int IDC_BTN_SAVE = 3004;
constexpr int IDC_BTN_CANCEL = 3005;
constexpr int IDC_LBL_RULES = 3006;
constexpr int IDC_EDT_RULES = 3007;
constexpr int IDC_BTN_RESET = 3008;
constexpr int IDC_LBL_RULEHINT = 3009;
constexpr int IDC_LBL_HOTKEY = 3010;
constexpr int IDC_CMB_HOTKEY = 3013;
constexpr int IDC_TAB_GENERAL = 3014;
constexpr int IDC_TAB_WEB = 3015;
constexpr int IDC_TAB_THEME = 3016;
constexpr int IDC_CHK_BEAUTIFY = 3017;  // 主题页：界面美化开关
constexpr int IDC_CHK_GHOST = 3053;     // 主题页：抗残影双缓冲开关（实验性）
constexpr int IDC_LBL_GHOSTHINT = 3054; // 主题页：抗残影开关的「实验性」说明
// 一键启动 / 一键关闭 Tab（索引 3）
constexpr int IDC_TAB_GROUP = 3019;
constexpr int IDC_LBL_LAUNCH = 3020;    // 「启动组」标题
constexpr int IDC_EDT_LAUNCH = 3021;    // 启动组编辑器：关键字 → 文件路径
constexpr int IDC_LBL_KILL = 3022;      // 「关闭组」标题
constexpr int IDC_EDT_KILL = 3023;      // 关闭组编辑器：关键字 → 进程名
constexpr int IDC_LBL_GROUPHINT = 3024; // 格式说明
constexpr int IDC_LBL_THEME = 3011;
constexpr int IDC_CMB_THEME = 3012;
constexpr int IDC_TAB_WEIGHT = 3025;     // 左侧 Tab：搜索权重（点击加权排序的性能参数）
constexpr int IDC_CHK_WEIGHTON = 3026;   // 启用点击权重记忆
constexpr int IDC_LBL_FLUSH = 3027;      // 「写入时机」标签
constexpr int IDC_CMB_FLUSH = 3028;      // 写入时机下拉（立即/延迟合并/退出时）
constexpr int IDC_LBL_MAXENT = 3029;     // 「条目上限」标签
constexpr int IDC_CMB_MAXENT = 3030;     // 记忆条目上限下拉
constexpr int IDC_BTN_WIPE = 3031;       // 清空权重数据（两步确认）
constexpr int IDC_LBL_WCOUNT = 3032;     // 当前已记忆条数
constexpr int IDC_LBL_WEIHINT = 3033;    // 权重页说明文字
constexpr int IDC_CHK_FILEDLGJUMP = 3034;  // 常规页：文件对话框跳转开关
constexpr int IDC_CHK_TOP = 3035;          // 命令页：top 命令开关（空格+top 回车置顶/取消置顶当前窗口）
constexpr int IDC_CHK_CMD = 3081;          // 命令页：cmd 命令开关（空格+cmd 执行 Shell 命令，原 "> 命令"）
constexpr int IDC_CHK_WIN = 3082;          // 命令页：w 命令开关（空格+w 切换窗口，原 "< 关键词"）
constexpr int IDC_CHK_CAPTURE = 3083;      // 命令页：ss 截图命令开关（空格+ss 截图）
// 截图工具 Tab（索引 8）：下载 ScreenCapture.exe + ImageReader.exe
constexpr int IDC_TAB_CAPTURE = 3090;      // 左侧 Tab：截图工具
constexpr int IDC_BTN_DL_SCREENCAPTURE = 3091;  // 下载 ScreenCapture.exe
constexpr int IDC_BTN_DL_IMAGEREADER = 3092;    // 下载 ImageReader.exe
constexpr int IDC_LBL_CAPTURE_STATUS = 3093;    // 当前工具状态
constexpr int IDC_LBL_CAPTURE_HINT = 3094;      // 说明文字
constexpr int IDC_LBL_CAPTURE_NOTE = 3095;      // OCR 备注
// 主题页：自绘主题选择器（色板列表）+ 3 个微调滑块（实时预览，保存后生效）
constexpr int IDC_LST_THEME = 3040;  // 主题选择器：自绘列表框
constexpr int IDC_LBL_TUNE = 3041;   // 「微调」说明标签
constexpr int IDC_TRK_ALPHA = 3042;  // 透明度滑块 0-255
constexpr int IDC_LBL_ALPHA = 3043;  // 透明度滑块标签
constexpr int IDC_TRK_RADIUS = 3046; // 圆角半径滑块 0-14
constexpr int IDC_LBL_RADIUS = 3047; // 圆角半径滑块标签
// 排除路径 Tab（索引 5）：自定义编辑器 + 「恢复默认」按钮
constexpr int IDC_TAB_EXCLUDE = 3048;       // 左侧 Tab：排除路径
constexpr int IDC_EDT_EXCLUDE = 3049;        // 多行编辑器：每行一条，支持 * ?
constexpr int IDC_BTN_EXCLUDE_DEFAULT = 3050; // 「恢复默认」按钮：填入系统默认排除项
constexpr int IDC_LBL_EXCLUDEHINT = 3051;    // 排除路径说明：语法、作用范围、命中规则
constexpr int IDC_LBL_EXCLUDECOUNT = 3052;   // 当前已记忆 N 条排除项
constexpr int IDM_THEME_BASE = 4200;  // 主题下拉菜单指令基值
constexpr int IDM_FLUSH_BASE = 4410;  // 写入时机下拉菜单指令基值
constexpr int IDM_MAXENT_BASE = 4420; // 条目上限下拉菜单指令基值

// Shell 与窗口 Tab（索引 6）
constexpr int IDC_TAB_SHELL = 3060;        // 左侧 Tab：Shell 与窗口
constexpr int IDC_TAB_CMD = 3080;          // 左侧 Tab：命令（top / cmd / w 开关）
constexpr int IDC_LBL_SHELLTYPE = 3061;    // 「Shell 程序」标签
constexpr int IDC_CMB_SHELLTYPE = 3062;    // Shell 程序下拉（cmd/PowerShell/Git Bash/自定义）
constexpr int IDC_LBL_SHELLPATH = 3063;    // 「自定义路径」标签
constexpr int IDC_EDT_SHELLPATH = 3064;    // 自定义 Shell 程序路径编辑框
constexpr int IDC_LBL_SHELLARGS = 3065;    // 「自定义参数」标签
constexpr int IDC_EDT_SHELLARGS = 3066;    // 自定义启动参数模板编辑框（{c}=命令）
constexpr int IDC_LBL_SHELLCWD = 3067;     // 「默认工作目录」标签
constexpr int IDC_CMB_SHELLCWD = 3068;     // 默认工作目录下拉
constexpr int IDC_CHK_SHOWWIN = 3069;      // 前台显示输出窗口
constexpr int IDC_CHK_WINGROUP = 3070;     // 合并同进程窗口
constexpr int IDC_CHK_WINUWP = 3071;       // 显示 UWP 应用窗口
constexpr int IDC_CHK_WINPROC = 3072;      // 副标题显示进程名
constexpr int IDC_LBL_WINCACHE = 3073;     // 「缓存刷新间隔」标签
constexpr int IDC_CMB_WINCACHE = 3074;     // 缓存刷新间隔下拉
constexpr int IDC_LBL_SHELLHINT = 3075;    // 说明文字

// Shell 与窗口 Tab 下拉项的当前文字
static const WCHAR* ShellTypeName(int t) {
    return t == 0 ? L"命令提示符 (cmd)"
         : t == 1 ? L"PowerShell"
         : t == 2 ? L"Git Bash"
                  : L"自定义…";
}
static const WCHAR* ShellCwdName(int t) {
    return t == 0 ? L"用户目录 (%USERPROFILE%)"
         : t == 1 ? L"系统默认目录 (System32)"
                  : L"桌面目录";
}

// 截图工具：获取 Flowtary.exe 同目录
static std::wstring GetFlowtaryDir() {
    WCHAR path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos + 1);
    return dir;
}

// 截图工具：更新状态标签文本
static void UpdateCaptureStatus(HWND hSettings) {
    if (!hSettings) return;
    HWND hw = GetDlgItem(hSettings, IDC_LBL_CAPTURE_STATUS);
    if (!hw) return;
    std::wstring dir = GetFlowtaryDir();
    bool sc = GetFileAttributesW((dir + L"ScreenCapture.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool ir = GetFileAttributesW((dir + L"ImageReader.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring text;
    if (sc && ir)
        text = L"ScreenCapture.exe 和 ImageReader.exe 均已就绪";
    else if (sc)
        text = L"ScreenCapture.exe 已就绪，ImageReader.exe 未下载（文字识别不可用）";
    else if (ir)
        text = L"ImageReader.exe 已就绪，ScreenCapture.exe 未下载";
    else
        text = L"ScreenCapture.exe 和 ImageReader.exe 均未下载";
    SetWindowTextW(hw, text.c_str());
}

// 截图工具：从 URL 下载文件到目标路径（同步执行，用于下载按钮回调）
static bool DownloadFile(HWND hParent, const WCHAR* url, const WCHAR* destPath) {
    // 先尝试 URLDownloadToFile（urlmon.dll，同步阻塞但实现最简洁）
    HRESULT hr = URLDownloadToFileW(nullptr, url, destPath, 0, nullptr);
    if (hr == S_OK) return true;
    if (hr == E_OUTOFMEMORY) {
        MessageBoxW(hParent, L"下载失败：内存不足", L"下载错误", MB_ICONERROR);
        return false;
    }
    // URLDownloadToFile 失败时尝试 WinINET
    HINTERNET hSession = InternetOpenW(L"Flowtary/1.0",
                                       INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hSession) {
        MessageBoxW(hParent, L"下载失败：无法建立网络连接", L"下载错误", MB_ICONERROR);
        return false;
    }
    HINTERNET hUrl = InternetOpenUrlW(hSession, url, nullptr, 0,
                                       INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_PRAGMA_NOCACHE, 0);
    if (!hUrl) {
        InternetCloseHandle(hSession);
        MessageBoxW(hParent, L"下载失败：无法访问下载链接", L"下载错误", MB_ICONERROR);
        return false;
    }
    WCHAR tmpPath[MAX_PATH];
    wcscpy_s(tmpPath, destPath);
    wcscat_s(tmpPath, L".tmp");
    FILE* fp = _wfopen(tmpPath, L"wb");
    if (!fp) {
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hSession);
        MessageBoxW(hParent, L"下载失败：无法创建临时文件", L"下载错误", MB_ICONERROR);
        return false;
    }
    BYTE buf[8192];
    DWORD done = 0;
    bool ok = true;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &done) && done > 0) {
        if (fwrite(buf, 1, done, fp) != (size_t)done) {
            ok = false;
            break;
        }
    }
    fclose(fp);
    InternetCloseHandle(hUrl);
    InternetCloseHandle(hSession);
    if (!ok) {
        _wremove(tmpPath);
        MessageBoxW(hParent, L"下载失败：写入文件时出错", L"下载错误", MB_ICONERROR);
        return false;
    }
    if (!MoveFileExW(tmpPath, destPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        _wremove(tmpPath);
        MessageBoxW(hParent, L"下载失败：无法替换目标文件", L"下载错误", MB_ICONERROR);
        return false;
    }
    return true;
}

// 设置窗口 Tab 切换：按 g.settingsTab 显示/隐藏对应分组控件，
// 并把「保存/取消」按钮位置随 Tab 调整（通用页按钮上移，避免大片留白）。
static void ShowSettingsTab(HWND h, int tab) {
    g.settingsTab = tab;
    auto vis = [h](int id, bool show) {
        HWND w = GetDlgItem(h, id);
        if (w) ShowWindow(w, show ? SW_SHOW : SW_HIDE);
    };
    bool general = (tab == 0);
    vis(IDC_CHK_START, general);
    vis(IDC_LBL_HOTKEY, general);
    vis(IDC_CMB_HOTKEY, general);
    vis(IDC_LBL_WAKE, general);
    vis(IDC_CMB_WAKE, general);
    vis(IDC_CHK_FILEDLGJUMP, general);
    bool web = (tab == 1);
    vis(IDC_LBL_RULES, web);
    vis(IDC_EDT_RULES, web);
    vis(IDC_LBL_RULEHINT, web);
    vis(IDC_BTN_RESET, web);
    bool theme = (tab == 2);
    vis(IDC_CHK_BEAUTIFY, theme);
    vis(IDC_CHK_GHOST, theme);
    vis(IDC_LBL_GHOSTHINT, theme);
    vis(IDC_LBL_THEME, theme);
    vis(IDC_LST_THEME, theme);
    vis(IDC_LBL_TUNE, theme);
    vis(IDC_TRK_ALPHA, theme);
    vis(IDC_LBL_ALPHA, theme);
    vis(IDC_TRK_RADIUS, theme);
    vis(IDC_LBL_RADIUS, theme);
    bool group = (tab == 3);
    vis(IDC_LBL_LAUNCH, group);
    vis(IDC_EDT_LAUNCH, group);
    vis(IDC_LBL_KILL, group);
    vis(IDC_EDT_KILL, group);
    vis(IDC_LBL_GROUPHINT, group);
    bool weight = (tab == 4);
    vis(IDC_CHK_WEIGHTON, weight);
    vis(IDC_LBL_FLUSH, weight);
    vis(IDC_CMB_FLUSH, weight);
    vis(IDC_LBL_MAXENT, weight);
    vis(IDC_CMB_MAXENT, weight);
    vis(IDC_BTN_WIPE, weight);
    vis(IDC_LBL_WCOUNT, weight);
    vis(IDC_LBL_WEIHINT, weight);
    bool exclude = (tab == 5);
    vis(IDC_LBL_EXCLUDEHINT, exclude);
    vis(IDC_EDT_EXCLUDE, exclude);
    vis(IDC_BTN_EXCLUDE_DEFAULT, exclude);
    vis(IDC_LBL_EXCLUDECOUNT, exclude);
    bool shell = (tab == 6);
    vis(IDC_LBL_SHELLTYPE, shell);
    vis(IDC_CMB_SHELLTYPE, shell);
    vis(IDC_LBL_SHELLPATH, shell);
    vis(IDC_EDT_SHELLPATH, shell);
    vis(IDC_LBL_SHELLARGS, shell);
    vis(IDC_EDT_SHELLARGS, shell);
    vis(IDC_LBL_SHELLCWD, shell);
    vis(IDC_CMB_SHELLCWD, shell);
    vis(IDC_CHK_SHOWWIN, shell);
    vis(IDC_CHK_WINGROUP, shell);
    vis(IDC_CHK_WINUWP, shell);
    vis(IDC_CHK_WINPROC, shell);
    vis(IDC_LBL_WINCACHE, shell);
    vis(IDC_CMB_WINCACHE, shell);
    bool cmd = (tab == 7);
    vis(IDC_CHK_TOP, cmd);
    vis(IDC_CHK_CMD, cmd);
    vis(IDC_CHK_WIN, cmd);
    vis(IDC_CHK_CAPTURE, cmd);
    bool capture = (tab == 8);
    vis(IDC_BTN_DL_SCREENCAPTURE, capture);
    vis(IDC_BTN_DL_IMAGEREADER, capture);
    vis(IDC_LBL_CAPTURE_STATUS, capture);
    vis(IDC_LBL_CAPTURE_HINT, capture);
    vis(IDC_LBL_CAPTURE_NOTE, capture);
    if (capture) UpdateCaptureStatus(h);  // 切换到截图工具页时刷新状态
    // 保存/取消/恢复默认：始终显示，贴底并整行居中（由 LayoutSettings 统一处理）
    LayoutSettings(h);
    InvalidateRect(h, nullptr, TRUE);
}

// Win11 / Win10 20H1+：标题栏明暗跟随当前主题（浅色主题用浅色标题栏，避免与窗口配色割裂）
static void ApplyDarkTitlebar(HWND h) {
    BOOL dark = (g.theme && g.theme->dark) ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                                     sizeof(dark))))
        DwmSetWindowAttribute(h, 19, &dark, sizeof(dark));
}

// 界面美化：按 g.beautify 施加/撤销「暗色标题栏 + 圆角窗口 + 强制暗色菜单」
static void ApplyBeautify() {
    if (!g.hSettings) return;
    if (g.beautify) {
        ApplyDarkTitlebar(g.hSettings);
        ApplyRoundCorners(g.hSettings);
    } else {
        BOOL dark = FALSE;
        DwmSetWindowAttribute(g.hSettings, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                             sizeof(dark));
        DwmSetWindowAttribute(g.hSettings, 19, &dark, sizeof(dark));
        DWORD pref = 0;  // DWMWCP_DEFAULT：圆角回归系统默认
        DwmSetWindowAttribute(g.hSettings, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }
    EnableDarkMenus();  // 菜单深浅随美化开关与主题
}

static void DrawCheckGlyph(HDC hdc, const RECT& r, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, (std::max)(1, S(2)), color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int cy = (r.top + r.bottom) / 2;
    MoveToEx(hdc, r.left + S(3), cy, nullptr);
    LineTo(hdc, r.left + (r.right - r.left) * 2 / 5, r.bottom - S(3));
    LineTo(hdc, r.right - S(3), r.top + S(2));
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// ---------------- 设置窗口可缩放布局 ----------------
// 思路：WM_CREATE 建完所有控件后，记录各控件的像素矩形与「距客户区底边的距离」，
// resize 时按规则重算。这样不必改动每一处 CreateWindowEx 调用，且默认尺寸下的
// 观感与改之前完全一致（底边距离是实测出来的，不依赖对标题栏高度的假设）。
struct CtlGeom {
    int id = 0;
    int x = 0, y = 0, w = 0, h = 0;
    int gapPx = 0;              // 客户区底边到该控件下沿的像素距离
    bool stretchW = false;      // 宽度跟随内容区（右边距固定 24 逻辑像素）
    bool stretchH = false;      // 高度撑到底部预留区
    bool anchorBottom = false;  // Y 从底边算起
    bool centerRow = false;     // 底部按钮行，整体居中
    int centerIdx = 0;          // 在按钮行中的序号
    int growSlot = 0;           // 1=高度加 d1，2=高度加 d2（一键页两个编辑器分摊多余高度）
    int shiftSlot = 0;          // 1=Y 下移 d1，2=Y 下移 d1+d2
};
static std::vector<CtlGeom> sCtl;
static int sSettingsMargin = 0;    // 内容区左边距（像素，= S(156)）
static int sRecordedClientH = 0;   // 记录几何时的客户区高度（用于计算纵向余量）
static bool sWipeArmed = false;    // 「清空权重数据」两步确认的武装状态（3 秒后自动解除）

static void ApplyLayoutRule(int id, CtlGeom& cg) {
    switch (id) {
        case IDC_EDT_RULES:                                  // 网页规则框：跟随宽和高
            cg.stretchW = true;
            cg.stretchH = true;
            break;
        case IDC_EDT_LAUNCH:  // 一键页：启动组编辑器吃掉一半纵向余量
            cg.stretchW = true;
            cg.growSlot = 1;
            break;
        case IDC_LBL_KILL:  // 关闭组标题随之上半段一起下移
            cg.stretchW = true;
            cg.shiftSlot = 1;
            break;
        case IDC_EDT_KILL:  // 关闭组编辑器吃掉另一半
            cg.stretchW = true;
            cg.growSlot = 2;
            cg.shiftSlot = 1;
            break;
        case IDC_LBL_GROUPHINT:  // 说明文字紧跟编辑器下方（不贴底，避免拉高后出现空档）
            cg.stretchW = true;
            cg.shiftSlot = 2;
            break;
        case IDC_LBL_RULES:
        case IDC_CHK_START:
        case IDC_CHK_BEAUTIFY:
        case IDC_CHK_WEIGHTON:
        case IDC_CHK_FILEDLGJUMP:
        case IDC_CHK_TOP:
        case IDC_CMB_HOTKEY:
        case IDC_CMB_WAKE:
        case IDC_CMB_FLUSH:
        case IDC_CMB_MAXENT:
            cg.stretchW = true;  // 内容区控件：宽度跟随
            break;
        case IDC_LBL_WCOUNT:
        case IDC_LBL_WEIHINT:
            cg.stretchW = true;  // 权重页说明/计数：宽度跟随
            break;
        case IDC_EDT_EXCLUDE:  // 排除路径编辑器：跟随宽和高（占满剩余纵向区域）
            cg.stretchW = true;
            cg.stretchH = true;
            break;
        case IDC_LBL_EXCLUDEHINT:
        case IDC_LBL_EXCLUDECOUNT:
        case IDC_BTN_EXCLUDE_DEFAULT:
            cg.stretchW = true;  // 排除路径页提示/计数/按钮：宽度跟随
            break;
        // Shell 与窗口页：标签/下拉/编辑宽度跟随内容区
        case IDC_LBL_SHELLTYPE:
        case IDC_CMB_SHELLTYPE:
        case IDC_LBL_SHELLPATH:
        case IDC_EDT_SHELLPATH:
        case IDC_LBL_SHELLARGS:
        case IDC_EDT_SHELLARGS:
        case IDC_LBL_SHELLCWD:
        case IDC_CMB_SHELLCWD:
        case IDC_CHK_SHOWWIN:
        case IDC_CHK_WINGROUP:
        case IDC_CHK_WINUWP:
        case IDC_CHK_WINPROC:
        case IDC_LBL_WINCACHE:
        case IDC_CMB_WINCACHE:
            cg.stretchW = true;  // 宽度跟随内容区
            break;
        case IDC_LST_THEME:
        case IDC_LBL_TUNE:
        case IDC_TRK_ALPHA:
        case IDC_TRK_RADIUS:
            cg.stretchW = true;  // 主题页：列表框与滑块宽度跟随内容区
            break;
        case IDC_LBL_RULEHINT:
            cg.stretchW = true;  // 网页规则说明：跟随宽度并贴底
            cg.anchorBottom = true;
            break;
        case IDC_BTN_RESET:
        case IDC_BTN_SAVE:
        case IDC_BTN_CANCEL:
            cg.anchorBottom = true;  // 按钮：贴底并整行居中
            cg.centerRow = true;
            cg.centerIdx = (id == IDC_BTN_RESET) ? 0 : (id == IDC_BTN_SAVE ? 1 : 2);
            break;
        default:
            break;  // 左侧 Tab 与固定宽度标签保持原位
    }
}

// 记录初始几何（WM_CREATE 末尾调用一次）
static void RecordSettingsLayout(HWND h) {
    sCtl.clear();
    RECT crc;
    GetClientRect(h, &crc);
    sRecordedClientH = crc.bottom;
    for (HWND w = GetWindow(h, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(w);
        if (id == 0) continue;  // 0 = 无 ID（分隔线之类的占位）
        RECT r;
        GetWindowRect(w, &r);
        MapWindowPoints(nullptr, h, (LPPOINT)&r, 2);
        CtlGeom cg;
        cg.id = id;
        cg.x = r.left;
        cg.y = r.top;
        cg.w = r.right - r.left;
        cg.h = r.bottom - r.top;
        cg.gapPx = crc.bottom - r.bottom;
        ApplyLayoutRule(id, cg);
        sCtl.push_back(cg);
    }
}

// 主题微调滑块：把当前主题的 alpha/radius 同步到滑块位置与标签
static void UpdateTuneLabel(HWND h, int tid, int v) {
    int lid = (tid == IDC_TRK_ALPHA) ? IDC_LBL_ALPHA : IDC_LBL_RADIUS;
    HWND lab = GetDlgItem(h, lid);
    if (!lab) return;
    const WCHAR* name = (tid == IDC_TRK_ALPHA) ? L"透明度" : L"圆角半径";
    WCHAR buf[48];
    swprintf_s(buf, L"%s：%d", name, v);
    SetWindowTextW(lab, buf);
}
static void SyncTuneSliders(HWND h) {
    if (!g.theme) return;
    HWND a = GetDlgItem(h, IDC_TRK_ALPHA),
          r = GetDlgItem(h, IDC_TRK_RADIUS);
    if (a) SendMessageW(a, TBM_SETPOS, TRUE, g.theme->alpha);
    if (r) SendMessageW(r, TBM_SETPOS, TRUE, g.theme->radiusWindow);
    UpdateTuneLabel(h, IDC_TRK_ALPHA, g.theme->alpha);
    UpdateTuneLabel(h, IDC_TRK_RADIUS, g.theme->radiusWindow);
}

// 按当前客户区尺寸重排所有控件（WM_SIZE / 切 Tab 时调用）
static void LayoutSettings(HWND h) {
    if (sCtl.empty()) return;
    RECT crc;
    GetClientRect(h, &crc);
    int gap = S(12);
    int rowW = gap * 2;  // 底部按钮行总宽（含两处间距）
    for (auto& c : sCtl)
        if (c.centerRow) rowW += c.w;
    int contentW = crc.right - sSettingsMargin - S(24);
    // 纵向余量由一键页两个编辑器对半分摊（可为负：窗口缩小时同步收拢）
    int extra = crc.bottom - sRecordedClientH;
    int d1 = extra / 2, d2 = extra - d1;
    for (auto& cg : sCtl) {
        HWND w = GetDlgItem(h, cg.id);
        if (!w) continue;
        int x = cg.x;
        if (cg.centerRow)
            x = sSettingsMargin + (contentW - rowW) / 2 + cg.centerIdx * (cg.w + gap);
        int y = cg.y;
        if (cg.anchorBottom) y = crc.bottom - cg.gapPx - cg.h;
        else if (cg.shiftSlot == 1) y += d1;
        else if (cg.shiftSlot == 2) y += d1 + d2;
        int wd = cg.stretchW ? (crc.right - S(24) - x) : cg.w;
        int ht = cg.h;
        if (cg.stretchH) ht = crc.bottom - cg.gapPx - y;
        else if (cg.growSlot == 1) ht += d1;
        else if (cg.growSlot == 2) ht += d2;
        if (wd < 0) wd = 0;
        if (ht < 0) ht = 0;
        SetWindowPos(w, nullptr, x, y, wd, ht, SWP_NOZORDER);
    }
}

static LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            ApplyBeautify();  // 按美化开关施加/撤销暗色标题栏、圆角与暗色菜单
            int margin = S(156);    // 左侧 Tab 栏之后，内容区起点
            sSettingsMargin = margin;
            RECT crc; GetClientRect(h, &crc);
            int contentW = crc.right - margin - S(24);  // 与 ShowSettingsTab 计算一致

            // 自绘复选框：按钮实际为 BS_OWNERDRAW（BS_AUTOCHECKBOX 与之位或后会被吸收），
            // 其 Button_GetCheck/SetCheck 不生效，勾选状态由 g.startupWanted 驱动。
            g.startupWanted = GetStartupEnabled();
            g.startupSaved = g.startupWanted;
            g.hotkeyModeSaved = g.hotkeyMode;
            g.weightEnabledSaved = g.weightEnabled;  // 取消时回退用
            g.weightFlushSaved = g.weightFlush;
            g.weightMaxSaved = g.weightMaxEntries;
            g.fdjEnabled = fdj_enabled();         // 初始化自注册表（默认开）
            g.fdjEnabledSaved = g.fdjEnabled;
            g.topEnabledSaved = g.topEnabled;     // top 命令开关（取消时回退）
            g.cmdEnabledSaved = g.cmdEnabled;     // cmd 命令开关（取消时回退）
            g.winEnabledSaved = g.winEnabled;     // w 命令开关（取消时回退）
            g.captureEnabledSaved = g.captureEnabled;  // ss 截图命令开关（取消时回退）
            HWND c;

            // 左侧 Tab 栏（自绘按钮）：常规 / 网页规则 / 主题
            c = CreateWindowExW(0, L"BUTTON", L"常规",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(24), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_GENERAL, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"网页规则",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(68), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_WEB, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"主题",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(112), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_THEME, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"一键",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(156), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_GROUP, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"搜索权重",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(200), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_WEIGHT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"排除路径",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(244), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_EXCLUDE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"Shell 与窗口",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(288), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_SHELL, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"命令",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(332), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_CMD, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"截图工具",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                S(10), S(376), S(120), S(36), h,
                                (HMENU)(INT_PTR)IDC_TAB_CAPTURE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"BUTTON", L"开机自动启动",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 margin, S(20), contentW, S(24), h,
                                 (HMENU)(INT_PTR)IDC_CHK_START, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 结果项快捷键方案（自绘下拉，复用黑暗弹出菜单，与 唤醒位置/主题 同款）
            c = CreateWindowExW(0, L"STATIC", L"结果项快捷键：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(44), S(120),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_HOTKEY, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(120), S(44), contentW - S(120), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_HOTKEY, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const WCHAR* opts[] = {L"Alt + 数字（1–9,0）", L"Alt + 字母（A–J）", L"关闭"};
                for (int i = 0; i < 3; ++i)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                SendMessageW(c, CB_SETCURSEL, g.hotkeyMode, 0);
            }

            c = CreateWindowExW(0, L"STATIC", L"唤醒位置：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(72), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_WAKE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 唤醒位置下拉框：原生 ComboBox（CBS_DROPDOWNLIST），当前值与下拉列表均由系统绘制，必然可见。
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(90), S(74), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_WAKE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const WCHAR* opts[] = {L"屏幕居中", L"跟随鼠标"};
                for (int i = 0; i < 2; ++i)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                SendMessageW(c, CB_SETCURSEL, g.centerWake ? 0 : 1, 0);
            }

            // 文件对话框跳转开关（自绘复选框，状态由 g.fdjEnabled 驱动；放在「常规」Tab，
            // 不再出现在托盘右键菜单中）
            c = CreateWindowExW(0, L"BUTTON",
                                L"文件对话框跳转（在文件对话框中直接定位文件夹）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(112), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_FILEDLGJUMP, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"主题：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(92), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_THEME, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 主题选择器：自绘列表框（左侧色板预览 + 名称 + 当前项勾选）
            c = CreateWindowExW(0, L"LISTBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED |
                                    LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL,
                                margin, S(120), contentW, S(140), h,
                                (HMENU)(INT_PTR)IDC_LST_THEME, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            {
                HWND lst = c;
                int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                for (int i = 0; i < n; ++i)
                    SendMessageW(lst, LB_ADDSTRING, 0, (LPARAM)kThemes[i].name);
                SendMessageW(lst, LB_SETCURSEL, g.themeIdx, 0);
                SendMessageW(lst, LB_SETITEMHEIGHT, 0, (LPARAM)S(34));
            }

            // 微调滑块（实时预览，保存后才写盘）
            c = CreateWindowExW(0, L"STATIC", L"微调（实时预览，保存后生效）",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(268), contentW,
                                S(18), h, (HMENU)(INT_PTR)IDC_LBL_TUNE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"透明度",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(290), S(64),
                                S(20), h, (HMENU)(INT_PTR)IDC_LBL_ALPHA, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            c = CreateWindowExW(0, TRACKBAR_CLASS, nullptr,
                                WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_BOTH,
                                margin + S(68), S(290), contentW - S(68), S(20), h,
                                (HMENU)(INT_PTR)IDC_TRK_ALPHA, g.inst, nullptr);
            SendMessageW(c, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
            SendMessageW(c, TBM_SETPOS, TRUE, g.theme ? g.theme->alpha : 255);

            c = CreateWindowExW(0, L"STATIC", L"圆角半径",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(314), S(64),
                                S(20), h, (HMENU)(INT_PTR)IDC_LBL_RADIUS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            c = CreateWindowExW(0, TRACKBAR_CLASS, nullptr,
                                WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_BOTH,
                                margin + S(68), S(314), contentW - S(68), S(20), h,
                                (HMENU)(INT_PTR)IDC_TRK_RADIUS, g.inst, nullptr);
            SendMessageW(c, TBM_SETRANGE, TRUE, MAKELONG(0, 14));
            SendMessageW(c, TBM_SETPOS, TRUE, g.theme ? g.theme->radiusWindow : 10);
            SyncTuneSliders(h);  // 标签显示「名称：当前值」，与滑块位置对齐

            // 微调滑块依赖界面美化：初始按美化开关置灰/恢复
            if (!g.beautify) {
                EnableWindow(GetDlgItem(h, IDC_TRK_ALPHA), FALSE);
            }

            // 界面美化开关（自绘复选框，状态由 g.beautify 驱动；移入「主题」Tab）
            c = CreateWindowExW(0, L"BUTTON",
                                L"启用界面美化（暗色标题栏 / 圆角窗口 / 暗色菜单）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(20), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_BEAUTIFY, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 抗残影双缓冲（实验性）：自绘复选框，状态由 g.antiGhost 驱动
            c = CreateWindowExW(0, L"BUTTON",
                                L"启用抗残影双缓冲（实验性）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(48), contentW, S(22), h,
                                (HMENU)(INT_PTR)IDC_CHK_GHOST, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"STATIC",
                                L"实验性：开启整窗双缓冲消除残影；个别控件可能偶发闪烁",
                                WS_CHILD | WS_VISIBLE, margin, S(72), contentW, S(34), h,
                                (HMENU)(INT_PTR)IDC_LBL_GHOSTHINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"网页搜索规则：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(136), contentW,
                                S(24), h, (HMENU)(INT_PTR)IDC_LBL_RULES, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                margin, S(160), contentW, S(180), h,
                                (HMENU)(INT_PTR)IDC_EDT_RULES, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTextW(c, RulesToText(g.webCmds).c_str());
            // Win10 1809+：滚动条走暗色主题变体（旧系统调用无效果，保持原样）
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"STATIC",
                                L"每行一条：前缀 + 空格 + 链接模板（{q} 为关键词，回车跳转）",
                                WS_CHILD | WS_VISIBLE, margin, S(346), contentW, S(18), h,
                                (HMENU)(INT_PTR)IDC_LBL_RULEHINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            int btnW = S(96), btnGap = S(12);
            int x0 = margin + (contentW - btnW * 3 - btnGap * 2) / 2;
            c = CreateWindowExW(0, L"BUTTON", L"恢复默认",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x0, S(376),
                                btnW, S(32), h, (HMENU)(INT_PTR)IDC_BTN_RESET, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"BUTTON", L"保存",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x0 + btnW + btnGap, S(376), btnW, S(32), h,
                                (HMENU)(INT_PTR)IDC_BTN_SAVE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"BUTTON", L"取消",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x0 + (btnW + btnGap) * 2, S(376), btnW, S(32), h,
                                (HMENU)(INT_PTR)IDC_BTN_CANCEL, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 一键启动 / 一键关闭 Tab（索引 3）：两个多行编辑器
            c = CreateWindowExW(0, L"STATIC", L"启动组（关键字 → 文件）：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(20),
                                contentW, S(24), h, (HMENU)(INT_PTR)IDC_LBL_LAUNCH, g.inst,
                                nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                margin, S(46), contentW, S(132), h,
                                (HMENU)(INT_PTR)IDC_EDT_LAUNCH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTextW(c, GroupsToText(g.groupsLaunch).c_str());
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"STATIC", L"关闭组（关键字 → 进程）：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(186),
                                contentW, S(24), h, (HMENU)(INT_PTR)IDC_LBL_KILL, g.inst,
                                nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                margin, S(212), contentW, S(104), h,
                                (HMENU)(INT_PTR)IDC_EDT_KILL, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTextW(c, GroupsToText(g.groupsKill).c_str());
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"STATIC",
                                L"每行：关键字 + 空格 + 目标（多个用 ; 分隔）；# 开头为注释。"
                                L"启动组填文件全路径，关闭组填进程名（可省 .exe）",
                                WS_CHILD | WS_VISIBLE, margin, S(324), contentW, S(36), h,
                                (HMENU)(INT_PTR)IDC_LBL_GROUPHINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            // 搜索权重 Tab（索引 4）：点击加权排序的开关与性能参数
            c = CreateWindowExW(0, L"BUTTON",
                                L"启用点击权重记忆（相同搜索词下点过的条目排序提前）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(20), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_WEIGHTON, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"写入时机：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(52), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_FLUSH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(90), S(54), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_FLUSH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const WCHAR* opts[] = {L"立即写入", L"延迟合并写入", L"退出时写入"};
                for (int i = 0; i < 3; ++i)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                SendMessageW(c, CB_SETCURSEL, g.weightFlush, 0);
            }

            c = CreateWindowExW(0, L"STATIC", L"条目上限：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(88), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_MAXENT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(90), S(90), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_MAXENT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const int kMaxEntOpts[] = {1000, 5000, 20000, 50000};
                WCHAR buf[4][32];
                int sel = 0;
                for (int i = 0; i < 4; ++i) {
                    swprintf_s(buf[i], L"%d 条", kMaxEntOpts[i]);
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)buf[i]);
                    if (kMaxEntOpts[i] == g.weightMaxEntries) sel = i;
                }
                SendMessageW(c, CB_SETCURSEL, sel, 0);
            }

            c = CreateWindowExW(0, L"BUTTON", L"清空权重数据…",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(128), S(160), S(32), h,
                                (HMENU)(INT_PTR)IDC_BTN_WIPE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            {
                std::wstring cnt = L"当前已记忆 " + std::to_wstring(g.weightCount) +
                                   L" 条权重数据";
                c = CreateWindowExW(0, L"STATIC", cnt.c_str(),
                                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(170),
                                    contentW, S(20), h, (HMENU)(INT_PTR)IDC_LBL_WCOUNT,
                                    g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            }

            c = CreateWindowExW(0, L"STATIC",
                                L"权重按「搜索词 → 文件完整路径」记录，不同搜索词互相独立；"
                                L"f/d 前缀只对后面的关键词记录，网页前缀（如 gg、bd）的点击不记录。"
                                L"数据文件：%APPDATA%\\Flowtary\\weights.dat",
                                WS_CHILD | WS_VISIBLE, margin, S(198), contentW, S(54), h,
                                (HMENU)(INT_PTR)IDC_LBL_WEIHINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            // 排除路径 Tab（索引 5）：多行编辑器 + 「恢复默认」按钮 + 提示 + 当前条数
            c = CreateWindowExW(0, L"STATIC",
                                L"每行一条路径，大小写不敏感，支持通配符 * ?。以排除项开头的文件/文件夹/程序会被过滤。"
                                L"网页与一键组不受影响。保存后生效。",
                                WS_CHILD | WS_VISIBLE, margin, S(20), contentW, S(36), h,
                                (HMENU)(INT_PTR)IDC_LBL_EXCLUDEHINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);

            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                margin, S(60), contentW, S(220), h,
                                (HMENU)(INT_PTR)IDC_EDT_EXCLUDE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTextW(c, ExcludePathsToText(g.excludePaths).c_str());
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"BUTTON", L"恢复默认",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(290), S(96), S(28), h,
                                (HMENU)(INT_PTR)IDC_BTN_EXCLUDE_DEFAULT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            {
                std::wstring cnt = L"当前已记忆 " + std::to_wstring(g.excludePaths.size()) +
                                   L" 条排除项";
                c = CreateWindowExW(0, L"STATIC", cnt.c_str(),
                                    WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                                    margin + S(108), S(290), contentW - S(108), S(28), h,
                                    (HMENU)(INT_PTR)IDC_LBL_EXCLUDECOUNT, g.inst, nullptr);
                SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            }

            // Shell 与窗口 Tab（索引 6）
            // 纵向布局：行间统一留 S(6) 间距、标签/编辑对之间 S(2)，提示行底部锚定在按钮上方，
            // 避免说明文字与底部「恢复默认/保存/取消」按钮行重叠（原布局提示行 y=S(380) 会压到按钮）。
            c = CreateWindowExW(0, L"STATIC", L"Shell 程序：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(18), S(110),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_SHELLTYPE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(110), S(18), contentW - S(110), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_SHELLTYPE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const WCHAR* opts[] = {L"命令提示符 (cmd)", L"PowerShell",
                                             L"Git Bash", L"自定义…"};
                for (int i = 0; i < 4; ++i)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                SendMessageW(c, CB_SETCURSEL, g.shellType, 0);
            }

            c = CreateWindowExW(0, L"STATIC", L"自定义 Shell 路径：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(52), S(150),
                                S(24), h, (HMENU)(INT_PTR)IDC_LBL_SHELLPATH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                margin, S(78), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_EDT_SHELLPATH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"STATIC", L"自定义启动参数：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(106), S(150),
                                S(24), h, (HMENU)(INT_PTR)IDC_LBL_SHELLARGS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"EDIT", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                margin, S(132), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_EDT_SHELLARGS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

            c = CreateWindowExW(0, L"STATIC", L"默认工作目录：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(162), S(140),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_SHELLCWD, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(140), S(162), contentW - S(140), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_SHELLCWD, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const WCHAR* opts[] = {L"用户目录 (%USERPROFILE%)",
                                             L"系统默认目录 (System32)", L"桌面目录"};
                for (int i = 0; i < 3; ++i)
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                SendMessageW(c, CB_SETCURSEL, g.shellDefaultCwd, 0);
            }

            c = CreateWindowExW(0, L"BUTTON",
                                L"前台显示输出窗口（否则后台静默执行）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(196), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_SHOWWIN, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"BUTTON", L"合并同进程窗口（多窗口进程显示为分组）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(226), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_WINGROUP, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"显示 UWP 应用窗口",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(256), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_WINUWP, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", L"结果副标题显示进程名",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(286), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_WINPROC, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"窗口缓存刷新间隔(秒)：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(316), S(160),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_WINCACHE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                                margin + S(160), S(316), contentW - S(160), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_WINCACHE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            SetWindowTheme(c, L"DarkMode_Explorer", nullptr);
            {
                static const int secs[] = {1, 2, 5, 10, 30, 60};
                static const WCHAR* opts[] = {L"1 秒", L"2 秒", L"5 秒", L"10 秒",
                                              L"30 秒", L"60 秒"};
                int sel = 0;
                for (int i = 0; i < 6; ++i) {
                    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)opts[i]);
                    if (secs[i] == g.winCacheSec) sel = i;
                }
                SendMessageW(c, CB_SETCURSEL, sel, 0);
            }

            // Shell/窗口设置初始值（取消时回退）与自定义路径/参数初始文本
            g.shellTypeSaved = g.shellType;
            g.shellCustomPathSaved = g.shellCustomPath;
            g.shellCustomArgsSaved = g.shellCustomArgs;
            g.shellDefaultCwdSaved = g.shellDefaultCwd;
            g.shellShowWindowSaved = g.shellShowWindow;
            g.winGroupProcSaved = g.winGroupProc;
            g.winShowUwpSaved = g.winShowUwp;
            g.winShowProcSaved = g.winShowProc;
            g.winCacheSecSaved = g.winCacheSec;
            SetWindowTextW(GetDlgItem(h, IDC_EDT_SHELLPATH), g.shellCustomPath.c_str());
            SetWindowTextW(GetDlgItem(h, IDC_EDT_SHELLARGS), g.shellCustomArgs.c_str());
            if (g.shellType != 3) {  // 非自定义时隐藏路径/参数行
                ShowWindow(GetDlgItem(h, IDC_LBL_SHELLPATH), SW_HIDE);
                ShowWindow(GetDlgItem(h, IDC_EDT_SHELLPATH), SW_HIDE);
                ShowWindow(GetDlgItem(h, IDC_LBL_SHELLARGS), SW_HIDE);
                ShowWindow(GetDlgItem(h, IDC_EDT_SHELLARGS), SW_HIDE);
            }

            // 命令 Tab（索引 7）：top / cmd / w / ss 四个开关，各自默认开、单独可控
            c = CreateWindowExW(0, L"BUTTON",
                                L"top 命令（空格+top 回车，置顶/取消置顶当前窗口）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(20), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_TOP, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON",
                                L"cmd 命令（空格+cmd 执行 Shell 命令，原 “> 命令”）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(56), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_CMD, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON",
                                L"w 命令（空格+w 切换窗口，原 “< 关键词”）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(92), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_WIN, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON",
                                L"ss 命令（空格+ss 截图，空格+ss pin 贴图）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(128), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_CAPTURE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 截图工具 Tab（索引 8）：下载 ScreenCapture + ImageReader
            // 状态标签（显示当前工具是否已就位）
            c = CreateWindowExW(0, L"STATIC", L"",
                                WS_CHILD | SS_CENTERIMAGE,
                                margin, S(20), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_LBL_CAPTURE_STATUS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            // ScreenCapture.exe 下载按钮
            c = CreateWindowExW(0, L"BUTTON",
                                L"下载 ScreenCapture.exe（约 1MB）",
                                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(56), contentW, S(32), h,
                                (HMENU)(INT_PTR)IDC_BTN_DL_SCREENCAPTURE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            // ImageReader.exe 下载按钮（OCR 依赖）
            c = CreateWindowExW(0, L"BUTTON",
                                L"下载 ImageReader.exe（约 25MB，文字识别必需）",
                                WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(100), contentW, S(32), h,
                                (HMENU)(INT_PTR)IDC_BTN_DL_IMAGEREADER, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            // 说明文字
            c = CreateWindowExW(0, L"STATIC",
                                L"下载后将自动保存到 Flowtary.exe 同目录。"
                                L"OCR 功能（空格+ss ocr）需要 ImageReader.exe 才可使用。",
                                WS_CHILD | SS_LEFT,
                                margin, S(148), contentW, S(48), h,
                                (HMENU)(INT_PTR)IDC_LBL_CAPTURE_HINT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            // OCR 备注
            c = CreateWindowExW(0, L"STATIC",
                                L"提示：文字识别结果会自动复制到剪贴板。"
                                L"支持的命令：空格+ss（截图）/ 空格+ss pin（贴图）/ 空格+ss ocr（文字识别）",
                                WS_CHILD | SS_LEFT,
                                margin, S(204), contentW, S(72), h,
                                (HMENU)(INT_PTR)IDC_LBL_CAPTURE_NOTE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            RecordSettingsLayout(h);            // 记录初始几何，之后可随窗口缩放重排
            ShowSettingsTab(h, g.settingsTab);  // 按当前 Tab 初始化分组可见性并重排
            UpdateCaptureStatus(h);             // 初始化截图工具状态标签
            return 0;
        }

        case WM_GETMINMAXINFO: {
            // 允许拖动调整大小，但限制最小尺寸，避免控件挤成一团
            MINMAXINFO* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = S(560);
            mmi->ptMinTrackSize.y = S(430);
            return 0;
        }

        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) {
                LayoutSettings(h);
                InvalidateRect(h, nullptr, TRUE);
            }
            return 0;

        case WM_TIMER:
            // 「清空权重数据」两步确认超时解除
            if (wp == 9001 && sWipeArmed) {
                KillTimer(h, 9001);
                sWipeArmed = false;
                SetWindowTextW(GetDlgItem(h, IDC_BTN_WIPE), L"清空权重数据…");
                InvalidateRect(GetDlgItem(h, IDC_BTN_WIPE), nullptr, TRUE);
            }
            return 0;

        case WM_ERASEBKGND:
            return 1;  // 背景由 WM_PAINT 统一填充，避免闪白

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(hdc, &rc,
                     g.brSettingsBg ? g.brSettingsBg : (HBRUSH)GetStockObject(BLACK_BRUSH));
            // 左侧 Tab 栏背景（深色），与内容区分隔
            if (g.theme) {
                HBRUSH sbBg = CreateSolidBrush(g.theme->menuBg);
                RECT sb = {0, 0, S(140), rc.bottom};
                FillRect(hdc, &sb, sbBg);
                DeleteObject(sbBg);
                HBRUSH dv = CreateSolidBrush(g.theme->divider);
                RECT dvr = {S(139), 0, S(140), rc.bottom};
                FillRect(hdc, &dvr, dv);
                DeleteObject(dv);
            }

            // 编辑框自绘 1px 描边（仅当前 Tab 可见时绘制，避免其它页残留边框）
            const int kEditIds[] = {IDC_EDT_RULES, IDC_EDT_LAUNCH, IDC_EDT_KILL,
                                    IDC_EDT_EXCLUDE, IDC_EDT_SHELLPATH, IDC_EDT_SHELLARGS};
            HBRUSH brFrame = CreateSolidBrush(RGB(70, 70, 70));
            for (int eid : kEditIds) {
                HWND ed = GetDlgItem(h, eid);
                if (!ed || !IsWindowVisible(ed)) continue;
                RECT er;
                GetWindowRect(ed, &er);
                MapWindowPoints(ed, h, (LPPOINT)&er, 2);
                FrameRect(hdc, &er, brFrame);
            }
            DeleteObject(brFrame);
            EndPaint(h, &ps);
            return 0;
        }

        case WM_CTLCOLORLISTBOX:
            // 组合框下拉列表（COMBOLBOX）未被自绘项覆盖的区域：底色与文字色必须一起给。
            // 只返回黑色画刷而不 SetTextColor，系统会用默认黑色文字 → 黑底黑字完全不可见。
            {
                HDC hl = (HDC)wp;
                if (g.theme) {
                    const Theme& t = *g.theme;
                    SetTextColor(hl, t.textBody);
                    SetBkColor(hl, t.menuBg);
                    if (!g.brMenuBg) g.brMenuBg = CreateSolidBrush(t.menuBg);
                    return (LRESULT)g.brMenuBg;
                }
                return (LRESULT)GetStockObject(BLACK_BRUSH);
            }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wp;
            // 标签文字与背景跟随主题，浅色主题下不会变成「浅字压浅底」
            SetTextColor(hdc, g.theme ? g.theme->text : RGB(235, 235, 235));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)(g.brSettingsBg ? g.brSettingsBg
                                            : (HBRUSH)GetStockObject(BLACK_BRUSH));
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            HWND w = (HWND)lp;
            if (w == GetDlgItem(h, IDC_EDT_RULES) || w == GetDlgItem(h, IDC_EDT_LAUNCH) ||
                w == GetDlgItem(h, IDC_EDT_KILL) || w == GetDlgItem(h, IDC_EDT_EXCLUDE) ||
                w == GetDlgItem(h, IDC_EDT_SHELLPATH) || w == GetDlgItem(h, IDC_EDT_SHELLARGS)) {
                const Theme& te = *g.theme;
                if (!g.brEditBg) g.brEditBg = CreateSolidBrush(te.editBg);
                SetTextColor(hdc, te.text);
                SetBkColor(hdc, te.editBg);
                return (LRESULT)g.brEditBg;
            }
            break;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
            if (mis && mis->CtlType == ODT_MENU) { OwnerMeasureMenuItem(h, mis); return TRUE; }
            if (mis && mis->CtlType == ODT_COMBOBOX) mis->itemHeight = S(26);
            else if (mis && mis->CtlType == ODT_LISTBOX) mis->itemHeight = S(34);
            return TRUE;
        }

        case WM_HSCROLL: {
            // 主题微调滑块（trackbar 发 WM_HSCROLL，不进 WM_COMMAND）
            HWND tb = (HWND)lp;
            int tid = GetDlgCtrlID(tb);
            if (tid == IDC_TRK_ALPHA || tid == IDC_TRK_RADIUS) {
                if (!g.beautify || !g.theme) break;
                int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
                if (tid == IDC_TRK_ALPHA) g.theme->alpha = (BYTE)v;
                else {
                    g.theme->radiusWindow = v;
                    g.theme->radiusCard = (int)(v * 0.8);
                    g.theme->radiusButton = (int)(v * 0.6);
                    g.theme->radiusInput = (int)(v * 0.6);
                }
                ApplyTheme();  // 即时预览（含圆角 Rgn / 材质浓度）；保存才写盘
                UpdateTuneLabel(h, tid, v);
            }
            break;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (!dis) break;
            if (dis->CtlType == ODT_MENU) { OwnerDrawMenuItem(h, dis); return TRUE; }
            if (dis->CtlType == ODT_COMBOBOX) {
                // 设置页 7 个下拉：完全自绘。闭合显示区与下拉列表都用主题色显式绘制，
                // 不依赖 DarkMode_Explorer / WM_CTLCOLORLISTBOX 的系统配色
                // （那套在深色主题下会「黑底黑字」→ 看得见框、看不见字）。
                const Theme& t = *g.theme;
                HWND cb = dis->hwndItem;
                bool isField = (dis->itemState & ODS_COMBOBOXEDIT) != 0;  // 闭合显示区
                bool hot = (dis->itemState & ODS_SELECTED) != 0;          // 列表项高亮/当前项

                COLORREF bg = isField ? t.editBg
                                      : (hot ? Blend(t.menuBg, t.accent, 0.18f) : t.menuBg);
                HBRUSH bk = CreateSolidBrush(bg);
                FillRect(dis->hDC, &dis->rcItem, bk);
                DeleteObject(bk);

                WCHAR txt[160] = {};
                if (isField) {
                    LRESULT cur = SendMessageW(cb, CB_GETCURSEL, 0, 0);
                    if (cur >= 0) SendMessageW(cb, CB_GETLBTEXT, (WPARAM)cur, (LPARAM)txt);
                } else if (dis->itemID >= 0) {
                    SendMessageW(cb, CB_GETLBTEXT, (WPARAM)dis->itemID, (LPARAM)txt);
                }
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, isField ? t.textTitle
                                               : (hot ? t.textTitle : t.textBody));
                SelectObject(dis->hDC, g.fInput);
                RECT tr = dis->rcItem;
                tr.left += S(8);
                tr.right -= S(22);  // 右侧留给原生下拉箭头按钮 / 对勾
                DrawTextW(dis->hDC, txt, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

                if (!isField && hot) {  // 列表当前项：右侧对勾
                    RECT chk{dis->rcItem.right - S(24),
                             (dis->rcItem.top + dis->rcItem.bottom - S(18)) / 2,
                             dis->rcItem.right - S(6),
                             (dis->rcItem.top + dis->rcItem.bottom - S(18)) / 2 + S(18)};
                    DrawCheckGlyph(dis->hDC, chk, t.accent);
                }
                return TRUE;
            }
            if (dis->CtlType == ODT_LISTBOX) {
                int idx = (int)dis->itemID;
                int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                if (idx < 0 || idx >= n) return TRUE;
                const Theme& t = *g.theme;
                const Theme& th = kThemes[idx];
                bool sel = (dis->itemState & ODS_SELECTED) != 0;
                bool hot = (dis->itemState & ODS_HOTLIGHT) != 0;
                HBRUSH bk = CreateSolidBrush(sel ? Blend(t.menuBg, t.accent, 0.14f)
                                                 : (hot ? Blend(t.menuBg, t.accent2, 0.08f)
                                                        : t.menuBg));
                FillRect(dis->hDC, &dis->rcItem, bk);
                DeleteObject(bk);
                // 左侧色板：上 accent / 中 bgCard / 下 bg 三色块
                int sw = S(22), sh = S(22);
                RECT sr{dis->rcItem.left + S(10),
                        (dis->rcItem.top + dis->rcItem.bottom - sh) / 2,
                        dis->rcItem.left + S(10) + sw,
                        (dis->rcItem.top + dis->rcItem.bottom - sh) / 2 + sh};
                int third = sh / 3;
                RECT b1{sr.left, sr.top, sr.right, sr.top + third};
                RECT b2{sr.left, sr.top + third, sr.right, sr.top + 2 * third};
                RECT b3{sr.left, sr.top + 2 * third, sr.right, sr.bottom};
                HBRUSH c1 = CreateSolidBrush(th.accent);
                FillRect(dis->hDC, &b1, c1);
                DeleteObject(c1);
                HBRUSH c2 = CreateSolidBrush(th.bgCard);
                FillRect(dis->hDC, &b2, c2);
                DeleteObject(c2);
                HBRUSH c3 = CreateSolidBrush(th.bg);
                FillRect(dis->hDC, &b3, c3);
                DeleteObject(c3);
                // 名称
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, sel ? t.text : t.textBody);
                SelectObject(dis->hDC, g.fInput);
                RECT tr = dis->rcItem;
                tr.left = sr.right + S(10);
                DrawTextW(dis->hDC, th.name, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                // 当前项：右侧对勾
                if (sel) {
                    RECT chk{dis->rcItem.right - S(26),
                             (dis->rcItem.top + dis->rcItem.bottom - S(18)) / 2,
                             dis->rcItem.right - S(8),
                             (dis->rcItem.top + dis->rcItem.bottom - S(18)) / 2 + S(18)};
                    DrawCheckGlyph(dis->hDC, chk, t.accent);
                }
                if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
                return TRUE;
            }
            if (dis->CtlType == ODT_BUTTON) {
                int id = (int)dis->CtlID;
                const Theme& t = *g.theme;
                if (id == IDC_CHK_CMD || id == IDC_CHK_WIN || id == IDC_CHK_CAPTURE ||
                    id == IDC_CHK_START || id == IDC_CHK_BEAUTIFY ||
                    id == IDC_CHK_WEIGHTON || id == IDC_CHK_FILEDLGJUMP ||
                    id == IDC_CHK_TOP || id == IDC_CHK_GHOST ||
                    id == IDC_CHK_SHOWWIN || id == IDC_CHK_WINGROUP ||
                    id == IDC_CHK_WINUWP || id == IDC_CHK_WINPROC) {
                    // 复选框：自绘方框 + 对勾 + 文字
                    FillRect(dis->hDC, &dis->rcItem, g.brMenuBg);
                    RECT box = dis->rcItem;
                    box.right = box.left + S(18);
                    box.top = (dis->rcItem.top + dis->rcItem.bottom - S(18)) / 2;
                    box.bottom = box.top + S(18);
                    HBRUSH fb = CreateSolidBrush(t.editBg);
                    FillRect(dis->hDC, &box, fb);
                    DeleteObject(fb);
                    HBRUSH fb2 = CreateSolidBrush(t.divider);
                    FrameRect(dis->hDC, &box, fb2);
                    DeleteObject(fb2);
                    bool checked = (id == IDC_CHK_START) ? g.startupWanted
                                 : (id == IDC_CHK_BEAUTIFY) ? g.beautify
                                 : (id == IDC_CHK_FILEDLGJUMP) ? g.fdjEnabled
                                 : (id == IDC_CHK_TOP) ? g.topEnabled
                                 : (id == IDC_CHK_CMD) ? g.cmdEnabled
                                 : (id == IDC_CHK_WIN) ? g.winEnabled
                                 : (id == IDC_CHK_CAPTURE) ? g.captureEnabled
                                 : (id == IDC_CHK_GHOST) ? g.antiGhost
                                 : (id == IDC_CHK_SHOWWIN) ? g.shellShowWindow
                                 : (id == IDC_CHK_WINGROUP) ? g.winGroupProc
                                 : (id == IDC_CHK_WINUWP) ? g.winShowUwp
                                 : (id == IDC_CHK_WINPROC) ? g.winShowProc
                                                            : g.weightEnabled;
                    if (checked)
                        DrawCheckGlyph(dis->hDC, box, t.text);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, t.text);
                    SelectObject(dis->hDC, g.fInput);
                    WCHAR label[128]{};
                    GetWindowTextW(dis->hwndItem, label, 128);
                    RECT tr = dis->rcItem;
                    tr.left = box.right + S(10);
                    DrawTextW(dis->hDC, label, -1, &tr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    if (dis->itemState & ODS_FOCUS) {
                        RECT fr = tr;
                        DrawTextW(dis->hDC, label, -1, &fr,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
                        fr.right += S(4);
                        fr.top += S(3);
                        fr.bottom -= S(3);
                        DrawFocusRect(dis->hDC, &fr);
                    }
                    return TRUE;
                }
                if (id == IDC_CMB_WAKE || id == IDC_CMB_THEME || id == IDC_CMB_HOTKEY ||
                    id == IDC_CMB_FLUSH || id == IDC_CMB_MAXENT ||
                    id == IDC_CMB_SHELLTYPE || id == IDC_CMB_SHELLCWD ||
                    id == IDC_CMB_WINCACHE) {
                    // 下拉按钮：深底 + 描边 + 当前项文字 + ▾ 箭头（唤醒位置/主题/快捷键方案/权重参数共用）
                    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
                    HBRUSH bks = CreateSolidBrush(pressed ? t.editBg : t.menuBg);
                    FillRect(dis->hDC, &dis->rcItem, bks);
                    DeleteObject(bks);
                    HBRUSH bf = CreateSolidBrush(t.divider);
                    FrameRect(dis->hDC, &dis->rcItem, bf);
                    DeleteObject(bf);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, (id == IDC_CMB_THEME && !g.beautify) ? t.sub : t.text);
                    SelectObject(dis->hDC, g.fInput);
                    WCHAR maxBuf[32];
                    const WCHAR* cur =
                        id == IDC_CMB_WAKE ? (g.centerWake ? L"屏幕居中" : L"跟随鼠标")
                      : id == IDC_CMB_THEME ? g.theme->name
                      : id == IDC_CMB_HOTKEY ? HotkeyModeText(g.hotkeyMode)
                      : id == IDC_CMB_FLUSH ? WeightFlushText(g.weightFlush)
                      : id == IDC_CMB_SHELLTYPE ? ShellTypeName(g.shellType)
                      : id == IDC_CMB_SHELLCWD ? ShellCwdName(g.shellDefaultCwd)
                      : id == IDC_CMB_WINCACHE ? (swprintf_s(maxBuf, L"%d 秒", g.winCacheSec), maxBuf)
                      : (swprintf_s(maxBuf, L"%d 条", g.weightMaxEntries), maxBuf);
                    RECT tr = dis->rcItem;
                    tr.left += S(10);
                    DrawTextW(dis->hDC, cur, -1, &tr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    // 右侧下拉箭头：几何小三角，不依赖字体
                    int arrowR = dis->rcItem.right - S(14);
                    int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                    int a = S(5);
                    POINT tri[3] = {{arrowR - a, cy - a / 2}, {arrowR + a, cy - a / 2},
                                    {arrowR, cy + a / 2 + S(1)}};
                    HGDIOBJ oldPen = SelectObject(dis->hDC, GetStockObject(NULL_PEN));
                    HBRUSH aw = CreateSolidBrush(t.sub);
                    HGDIOBJ oldBr = SelectObject(dis->hDC, aw);
                    Polygon(dis->hDC, tri, 3);
                    SelectObject(dis->hDC, oldBr);
                    SelectObject(dis->hDC, oldPen);
                    DeleteObject(aw);
                    if (dis->itemState & ODS_FOCUS) DrawFocusRect(dis->hDC, &dis->rcItem);
                    return TRUE;
                }
                if (id == IDC_TAB_GENERAL || id == IDC_TAB_WEB || id == IDC_TAB_THEME ||
                    id == IDC_TAB_GROUP || id == IDC_TAB_WEIGHT || id == IDC_TAB_EXCLUDE ||
                    id == IDC_TAB_SHELL || id == IDC_TAB_CMD || id == IDC_TAB_CAPTURE) {
                    // 左侧 Tab 按钮：激活项用强调色高亮，并加左侧竖条
                    int idx = (id == IDC_TAB_GENERAL) ? 0
                            : (id == IDC_TAB_WEB)     ? 1
                            : (id == IDC_TAB_THEME)   ? 2
                            : (id == IDC_TAB_GROUP)   ? 3
                            : (id == IDC_TAB_WEIGHT)  ? 4
                            : (id == IDC_TAB_EXCLUDE) ? 5
                            : (id == IDC_TAB_SHELL)   ? 6
                            : (id == IDC_TAB_CMD)     ? 7
                                                      : 8;
                    bool active = (g.settingsTab == idx);
                    bool hover = (dis->itemState & ODS_HOTLIGHT) != 0;
                    HBRUSH bk = CreateSolidBrush(active ? t.menuHi : (hover ? t.editBg : t.menuBg));
                    FillRect(dis->hDC, &dis->rcItem, bk);
                    DeleteObject(bk);
                    HBRUSH bf = CreateSolidBrush(t.divider);
                    FrameRect(dis->hDC, &dis->rcItem, bf);
                    DeleteObject(bf);
                    if (active) {
                        RECT bar = dis->rcItem;
                        bar.right = bar.left + S(3);
                        HBRUSH ab = CreateSolidBrush(t.text);
                        FillRect(dis->hDC, &bar, ab);
                        DeleteObject(ab);
                    }
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, active ? t.text : t.sub);
                    SelectObject(dis->hDC, g.fInput);
                    const WCHAR* lbl = (id == IDC_TAB_GENERAL) ? L"常规"
                                     : (id == IDC_TAB_WEB)     ? L"网页规则"
                                     : (id == IDC_TAB_THEME)   ? L"主题"
                                     : (id == IDC_TAB_GROUP)   ? L"一键"
                                     : (id == IDC_TAB_WEIGHT)  ? L"搜索权重"
                                     : (id == IDC_TAB_EXCLUDE) ? L"排除路径"
                                     : (id == IDC_TAB_SHELL)   ? L"Shell 与窗口"
                                     : (id == IDC_TAB_CMD)     ? L"命令"
                                                                : L"截图工具";
                    RECT tr = dis->rcItem;
                    tr.left += S(10);
                    DrawTextW(dis->hDC, lbl, -1, &tr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    return TRUE;
                }
                // 普通按钮：深色底，保存按钮用主题强调色
                bool accent = (id == IDC_BTN_SAVE);
                bool pressed2 = (dis->itemState & ODS_SELECTED) != 0;
                HBRUSH bg = CreateSolidBrush(pressed2 ? t.selBg : (accent ? t.menuHi : t.menuBg));
                FillRect(dis->hDC, &dis->rcItem, bg);
                DeleteObject(bg);
                HBRUSH bf2 = CreateSolidBrush(t.divider);
                FrameRect(dis->hDC, &dis->rcItem, bf2);
                DeleteObject(bf2);
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, t.text);
                SelectObject(dis->hDC, g.fInput);
                WCHAR text[32]{};
                GetWindowTextW(dis->hwndItem, text, 32);
                RECT tr2 = dis->rcItem;
                DrawTextW(dis->hDC, text, -1, &tr2,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (dis->itemState & ODS_FOCUS) {
                    InflateRect(&tr2, -S(4), -S(3));
                    DrawFocusRect(dis->hDC, &tr2);
                }
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == IDC_BTN_SAVE) {
                // g.centerWake 由 IDC_CMB_WAKE 点击菜单直接更新，此处只需持久化
                DWORD v = g.centerWake ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CenterWake",
                                REG_DWORD, &v, sizeof(v));
                // g.hotkeyMode / g.startupWanted 已在点击时实时更新，直接持久化即可
                DWORD vm = (DWORD)g.hotkeyMode;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"HotkeyMode",
                                REG_DWORD, &vm, sizeof(vm));
                DWORD vt = (DWORD)g.themeIdx;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Theme", REG_DWORD,
                                &vt, sizeof(vt));
                // 每个主题的微调（透明度/圆角）以 blob 持久化
                {
                    int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                    sTune.assign(n, ThemeTune());
                    for (int i = 0; i < n; ++i) {
                        sTune[i].alpha = kThemes[i].alpha;
                        sTune[i].radius = kThemes[i].radiusWindow;
                    }
                    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ThemeTune",
                                    REG_BINARY, sTune.data(),
                                    (DWORD)(sTune.size() * sizeof(ThemeTune)));
                }
                DWORD vb = g.beautify ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Beautify",
                                REG_DWORD, &vb, sizeof(vb));
                DWORD vg = g.antiGhost ? 1 : 0;  // 抗残影双缓冲（实验性）持久化
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"AntiGhost",
                                REG_DWORD, &vg, sizeof(vg));
                DWORD vwe = g.weightEnabled ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightEnabled",
                                REG_DWORD, &vwe, sizeof(vwe));
                DWORD vwf = (DWORD)g.weightFlush;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightFlush",
                                REG_DWORD, &vwf, sizeof(vwf));
                DWORD vwm = (DWORD)g.weightMaxEntries;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WeightMaxEntries",
                                REG_DWORD, &vwm, sizeof(vwm));
                fdj_set_enabled(g.fdjEnabled);  // 文件对话框跳转开关持久化到注册表
                SetStartup(g.startupWanted);
                DWORD vtop = g.topEnabled ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"TopCmd",
                                REG_DWORD, &vtop, sizeof(vtop));
                DWORD vcmd = g.cmdEnabled ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CmdCmd",
                                REG_DWORD, &vcmd, sizeof(vcmd));
                DWORD vwin = g.winEnabled ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinCmd",
                                REG_DWORD, &vwin, sizeof(vwin));
                DWORD vcap = g.captureEnabled ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureCmd",
                                REG_DWORD, &vcap, sizeof(vcap));
                int len = GetWindowTextLengthW(GetDlgItem(h, IDC_EDT_RULES));
                std::wstring rulesText(len + 1, 0);
                GetWindowTextW(GetDlgItem(h, IDC_EDT_RULES), &rulesText[0], len + 1);
                rulesText.resize(len);
                SaveWebRules(rulesText);
                // 一键启动 / 一键关闭组：分别持久化并即时重建索引
                HWND eL = GetDlgItem(h, IDC_EDT_LAUNCH);
                int lenL = GetWindowTextLengthW(eL);
                std::wstring launchText(lenL + 1, 0);
                GetWindowTextW(eL, &launchText[0], lenL + 1);
                launchText.resize(lenL);
                SaveRegText(L"GroupLaunch", launchText);
                g.groupsLaunch = ParseGroups(launchText);

                HWND eK = GetDlgItem(h, IDC_EDT_KILL);
                int lenK = GetWindowTextLengthW(eK);
                std::wstring killText(lenK + 1, 0);
                GetWindowTextW(eK, &killText[0], lenK + 1);
                killText.resize(lenK);
                SaveRegText(L"GroupKill", killText);
                g.groupsKill = ParseGroups(killText);
                // 排除路径：解析编辑器文本并落盘（注释/空行被忽略），失败回退到当前内存值
                HWND eE = GetDlgItem(h, IDC_EDT_EXCLUDE);
                int lenE = GetWindowTextLengthW(eE);
                std::wstring exclText(lenE + 1, 0);
                GetWindowTextW(eE, &exclText[0], lenE + 1);
                exclText.resize(lenE);
                auto parsedExcl = ParseExcludePaths(exclText);
                if (!parsedExcl.empty()) g.excludePaths = parsedExcl;
                RebuildExcludeNorm();
                SaveRegText(L"ExcludePaths", ExcludePathsToText(g.excludePaths));
                // —— Shell 与窗口设置持久化 ——
                if (g.shellType == 3) {  // 仅自定义时读取并保存路径/参数
                    HWND eP = GetDlgItem(h, IDC_EDT_SHELLPATH);
                    int lenP = GetWindowTextLengthW(eP);
                    std::wstring sp(lenP + 1, 0);
                    GetWindowTextW(eP, &sp[0], lenP + 1);
                    sp.resize(lenP);
                    g.shellCustomPath = sp;
                    HWND eA = GetDlgItem(h, IDC_EDT_SHELLARGS);
                    int lenA = GetWindowTextLengthW(eA);
                    std::wstring sa(lenA + 1, 0);
                    GetWindowTextW(eA, &sa[0], lenA + 1);
                    sa.resize(lenA);
                    g.shellCustomArgs = sa;
                } else {
                    g.shellCustomPath.clear();
                    g.shellCustomArgs.clear();
                }
                DWORD vs = (DWORD)g.shellType;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellType",
                               REG_DWORD, &vs, sizeof(vs));
                SaveRegText(L"ShellCustomPath", g.shellCustomPath);
                SaveRegText(L"ShellCustomArgs", g.shellCustomArgs);
                vs = (DWORD)g.shellDefaultCwd;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellDefaultCwd",
                               REG_DWORD, &vs, sizeof(vs));
                vs = g.shellShowWindow ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellShowWindow",
                               REG_DWORD, &vs, sizeof(vs));
                vs = g.winGroupProc ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinGroupProc",
                               REG_DWORD, &vs, sizeof(vs));
                vs = g.winShowUwp ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowUwp",
                               REG_DWORD, &vs, sizeof(vs));
                vs = g.winShowProc ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowProc",
                               REG_DWORD, &vs, sizeof(vs));
                vs = (DWORD)g.winCacheSec;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinCacheSec",
                               REG_DWORD, &vs, sizeof(vs));
                if (IsWindowVisible(g.hwnd)) LayoutAndRepaint();
                DestroyWindow(h);
            } else if (id == IDC_CHK_START && HIWORD(wp) == BN_CLICKED) {
                g.startupWanted = !g.startupWanted;
                InvalidateRect(GetDlgItem(h, IDC_CHK_START), nullptr, TRUE);
            } else if (id == IDC_CHK_BEAUTIFY && HIWORD(wp) == BN_CLICKED) {
                g.beautify = !g.beautify;
                ApplyBeautify();  // 即时预览：标题栏 / 圆角 / 菜单深浅立即切换
                InvalidateRect(GetDlgItem(h, IDC_CHK_BEAUTIFY), nullptr, TRUE);
                InvalidateRect(GetDlgItem(h, IDC_LST_THEME), nullptr, TRUE);  // 同步置灰/恢复
                // 微调滑块（透明度/圆角）依赖界面美化；关闭时一并置灰
                bool en = g.beautify;
                EnableWindow(GetDlgItem(h, IDC_TRK_ALPHA), en);
                EnableWindow(GetDlgItem(h, IDC_TRK_RADIUS), en);
            } else if (id == IDC_CHK_GHOST && HIWORD(wp) == BN_CLICKED) {
                // 抗残影双缓冲（实验性）：实时切换 WS_EX_COMPOSITED 整窗双缓冲
                g.antiGhost = !g.antiGhost;
                LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
                if (g.antiGhost) ex |= WS_EX_COMPOSITED;
                else ex &= ~WS_EX_COMPOSITED;
                SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
                SetWindowPos(h, nullptr, 0, 0, 0, 0,
                             SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                 SWP_NOACTIVATE);
                RefreshComposition(h);  // 立即整窗重绘，清除切换瞬间可能残留的残影
                InvalidateRect(GetDlgItem(h, IDC_CHK_GHOST), nullptr, TRUE);
            } else if (id == IDC_CMB_HOTKEY && HIWORD(wp) == CBN_SELCHANGE) {
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 3) {
                    g.hotkeyMode = s;
                    if (IsWindowVisible(g.hwnd)) RepaintNow();
                }
            } else if (id == IDC_CHK_WEIGHTON && HIWORD(wp) == BN_CLICKED) {
                g.weightEnabled = !g.weightEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WEIGHTON), nullptr, TRUE);
            } else if (id == IDC_CHK_FILEDLGJUMP && HIWORD(wp) == BN_CLICKED) {
                g.fdjEnabled = !g.fdjEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_FILEDLGJUMP), nullptr, TRUE);
            } else if (id == IDC_CHK_TOP && HIWORD(wp) == BN_CLICKED) {
                g.topEnabled = !g.topEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_TOP), nullptr, TRUE);
            } else if (id == IDC_CHK_CMD && HIWORD(wp) == BN_CLICKED) {
                g.cmdEnabled = !g.cmdEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_CMD), nullptr, TRUE);
            } else if (id == IDC_CHK_WIN && HIWORD(wp) == BN_CLICKED) {
                g.winEnabled = !g.winEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WIN), nullptr, TRUE);
            } else if (id == IDC_CHK_CAPTURE && HIWORD(wp) == BN_CLICKED) {
                g.captureEnabled = !g.captureEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_CAPTURE), nullptr, TRUE);
            } else if (id == IDC_CHK_SHOWWIN && HIWORD(wp) == BN_CLICKED) {
                g.shellShowWindow = !g.shellShowWindow;
                InvalidateRect(GetDlgItem(h, IDC_CHK_SHOWWIN), nullptr, TRUE);
            } else if (id == IDC_CHK_WINGROUP && HIWORD(wp) == BN_CLICKED) {
                g.winGroupProc = !g.winGroupProc;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WINGROUP), nullptr, TRUE);
            } else if (id == IDC_CHK_WINUWP && HIWORD(wp) == BN_CLICKED) {
                g.winShowUwp = !g.winShowUwp;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WINUWP), nullptr, TRUE);
            } else if (id == IDC_CHK_WINPROC && HIWORD(wp) == BN_CLICKED) {
                g.winShowProc = !g.winShowProc;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WINPROC), nullptr, TRUE);
            } else if (id == IDC_CMB_FLUSH && HIWORD(wp) == CBN_SELCHANGE) {
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 3) g.weightFlush = s;
            } else if (id == IDC_CMB_MAXENT && HIWORD(wp) == CBN_SELCHANGE) {
                static const int kMaxEntOpts[] = {1000, 5000, 20000, 50000};
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 4) g.weightMaxEntries = kMaxEntOpts[s];
            } else if (id == IDC_BTN_WIPE && HIWORD(wp) == BN_CLICKED) {
                // 清空权重数据：两步确认（3 秒内再点一次才执行，避免误触）
                HWND bw = GetDlgItem(h, IDC_BTN_WIPE);
                if (!sWipeArmed) {
                    sWipeArmed = true;
                    SetWindowTextW(bw, L"再次点击确认清空");
                    SetTimer(h, 9001, 3000, nullptr);
                } else {
                    KillTimer(h, 9001);
                    sWipeArmed = false;
                    SetWindowTextW(bw, L"清空权重数据…");
                    ClearClickWeights();
                    std::wstring cnt = L"当前已记忆 0 条权重数据";
                    SetWindowTextW(GetDlgItem(h, IDC_LBL_WCOUNT), cnt.c_str());
                }
                InvalidateRect(GetDlgItem(h, IDC_BTN_WIPE), nullptr, TRUE);
} else if ((id == IDC_TAB_GENERAL || id == IDC_TAB_WEB || id == IDC_TAB_THEME ||
                          id == IDC_TAB_GROUP || id == IDC_TAB_WEIGHT ||
                          id == IDC_TAB_EXCLUDE || id == IDC_TAB_SHELL ||
                          id == IDC_TAB_CMD || id == IDC_TAB_CAPTURE) &&
                         HIWORD(wp) == BN_CLICKED) {
                ShowSettingsTab(h, (id == IDC_TAB_GENERAL) ? 0
                                 : (id == IDC_TAB_WEB)    ? 1
                                 : (id == IDC_TAB_THEME)  ? 2
                                 : (id == IDC_TAB_GROUP)  ? 3
                                 : (id == IDC_TAB_WEIGHT) ? 4
                                 : (id == IDC_TAB_EXCLUDE) ? 5
                                 : (id == IDC_TAB_SHELL)   ? 6
                                 : (id == IDC_TAB_CMD)     ? 7
                                                           : 8);
             } else if (id == IDC_CMB_WAKE && HIWORD(wp) == CBN_SELCHANGE) {
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 2) g.centerWake = (s == 0);
            } else if (id == IDC_CMB_SHELLTYPE && HIWORD(wp) == CBN_SELCHANGE) {
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 4) {
                    g.shellType = s;
                    bool custom = (g.shellType == 3);
                    ShowWindow(GetDlgItem(h, IDC_LBL_SHELLPATH), custom ? SW_SHOW : SW_HIDE);
                    ShowWindow(GetDlgItem(h, IDC_EDT_SHELLPATH), custom ? SW_SHOW : SW_HIDE);
                    ShowWindow(GetDlgItem(h, IDC_LBL_SHELLARGS), custom ? SW_SHOW : SW_HIDE);
                    ShowWindow(GetDlgItem(h, IDC_EDT_SHELLARGS), custom ? SW_SHOW : SW_HIDE);
                }
            } else if (id == IDC_CMB_SHELLCWD && HIWORD(wp) == CBN_SELCHANGE) {
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 3) g.shellDefaultCwd = s;
            } else if (id == IDC_CMB_WINCACHE && HIWORD(wp) == CBN_SELCHANGE) {
                static const int secs[] = {1, 2, 5, 10, 30, 60};
                int s = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                if (s >= 0 && s < 6) g.winCacheSec = secs[s];
            } else if (id == IDC_LST_THEME && HIWORD(wp) == LBN_SELCHANGE) {
                // 主题选择器：自绘列表框选中即切换（实时预览全部界面）
                if (!g.beautify) break;  // 美化关闭时主题不可选（列表已置灰）
                HWND lst = GetDlgItem(h, IDC_LST_THEME);
                int cur = (int)SendMessageW(lst, LB_GETCURSEL, 0, 0);
                int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                if (cur >= 0 && cur < n && cur != g.themeIdx) {
                    g.themeIdx = cur;
                    g.theme = &kThemes[cur];
                    ApplyTheme();
                }
                // 切换主题后把 3 个滑块同步到该主题的微调值
                SyncTuneSliders(h);
                InvalidateRect(lst, nullptr, TRUE);
            } else if (id == IDC_BTN_CANCEL) {
                // 取消：回退未保存的主题/微调/复选框，恢复打开时的状态
                int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                for (int i = 0; i < n && i < (int)sTuneSaved.size(); ++i) {
                    kThemes[i].alpha = (BYTE)sTuneSaved[i].alpha;
                    kThemes[i].radiusWindow = sTuneSaved[i].radius;
                    kThemes[i].radiusCard = (int)(sTuneSaved[i].radius * 0.8);
                    kThemes[i].radiusButton = (int)(sTuneSaved[i].radius * 0.6);
                    kThemes[i].radiusInput = (int)(sTuneSaved[i].radius * 0.6);
                }
                g.themeIdx = g.themeSaved;
                g.theme = &kThemes[g.themeIdx];
                if (g.beautify != g.beautifySaved) { g.beautify = g.beautifySaved; ApplyBeautify(); }
                ApplyTheme();  // 复原主题 + 微调（圆角）
                g.startupWanted = g.startupSaved;
                g.hotkeyMode = g.hotkeyModeSaved;
                if (IsWindowVisible(g.hwnd)) RepaintNow();
                // 搜索权重页：回退未保存的开关与性能参数
                g.weightEnabled = g.weightEnabledSaved;
                g.weightFlush = g.weightFlushSaved;
                g.weightMaxEntries = g.weightMaxSaved;
                g.fdjEnabled = g.fdjEnabledSaved;    // 文件对话框跳转：取消即回退
                g.topEnabled = g.topEnabledSaved;    // top 命令开关：取消即回退
                // 抗残影双缓冲（实验性）：取消即回退 WS_EX_COMPOSITED 状态
                g.antiGhost = g.antiGhostSaved;
                {   LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
                    if (g.antiGhost) ex |= WS_EX_COMPOSITED; else ex &= ~WS_EX_COMPOSITED;
                    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
                    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                     SWP_NOACTIVATE); }
                // Shell 与窗口：取消即回退未保存的配置
                g.shellType = g.shellTypeSaved;
                g.shellCustomPath = g.shellCustomPathSaved;
                g.shellCustomArgs = g.shellCustomArgsSaved;
                g.shellDefaultCwd = g.shellDefaultCwdSaved;
                g.shellShowWindow = g.shellShowWindowSaved;
                g.winGroupProc = g.winGroupProcSaved;
                g.winShowUwp = g.winShowUwpSaved;
                g.winShowProc = g.winShowProcSaved;
                g.winCacheSec = g.winCacheSecSaved;
                // 命令开关：取消即回退未保存的配置
                g.topEnabled = g.topEnabledSaved;
                g.cmdEnabled = g.cmdEnabledSaved;
                g.winEnabled = g.winEnabledSaved;
                DestroyWindow(h);
            } else if (id == IDC_BTN_RESET) {
                SetWindowTextW(GetDlgItem(h, IDC_EDT_RULES), DefaultRulesText().c_str());
            } else if (id == IDC_BTN_EXCLUDE_DEFAULT && HIWORD(wp) == BN_CLICKED) {
                // 「恢复默认」：把排除路径编辑器重置为系统默认（含所有盘符的回收站等）。
                // 注意：此处只更新编辑器文本与计数标签，不立即落盘——保存按钮按下时再持久化。
                SetWindowTextW(GetDlgItem(h, IDC_EDT_EXCLUDE),
                               DefaultExcludePathsText().c_str());
                std::wstring cnt = L"当前已记忆 " +
                                   std::to_wstring(DefaultExcludePaths().size()) +
                                   L" 条排除项";
                SetWindowTextW(GetDlgItem(h, IDC_LBL_EXCLUDECOUNT), cnt.c_str());
            } else if (id == IDC_BTN_DL_SCREENCAPTURE && HIWORD(wp) == BN_CLICKED) {
                // 下载 ScreenCapture.exe：从 GitHub Release 获取最新版
                HWND btn = GetDlgItem(h, IDC_BTN_DL_SCREENCAPTURE);
                SetWindowTextW(btn, L"下载中…");
                EnableWindow(btn, FALSE);
                std::wstring dir = GetFlowtaryDir();
                std::wstring dest = dir + L"ScreenCapture.exe";
                bool ok = DownloadFile(h,
                    L"https://github.com/xland/ScreenCapture/releases/latest/download/ScreenCapture.exe",
                    dest.c_str());
                if (ok) {
                    g.captureExe = dest;
                    MessageBoxW(h, L"ScreenCapture.exe 下载完成！\r\n现在可以使用 空格+ss 截图命令了。",
                                L"下载成功", MB_ICONINFORMATION);
                }
                SetWindowTextW(btn, L"下载 ScreenCapture.exe（约 1MB）");
                EnableWindow(btn, TRUE);
                UpdateCaptureStatus(h);
            } else if (id == IDC_BTN_DL_IMAGEREADER && HIWORD(wp) == BN_CLICKED) {
                // 下载 ImageReader.exe：OCR 插件
                HWND btn = GetDlgItem(h, IDC_BTN_DL_IMAGEREADER);
                SetWindowTextW(btn, L"下载中…（约 25MB，请稍候）");
                EnableWindow(btn, FALSE);
                std::wstring dir = GetFlowtaryDir();
                std::wstring dest = dir + L"ImageReader.exe";
                bool ok = DownloadFile(h,
                    L"https://github.com/xland/ImageReader/releases/latest/download/ImageReader.exe",
                    dest.c_str());
                if (ok) {
                    MessageBoxW(h, L"ImageReader.exe 下载完成！\r\n现在可以使用 空格+ss ocr 文字识别功能了。",
                                L"下载成功", MB_ICONINFORMATION);
                }
                SetWindowTextW(btn, L"下载 ImageReader.exe（约 25MB，文字识别必需）");
                EnableWindow(btn, TRUE);
                UpdateCaptureStatus(h);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            g.hSettings = nullptr;
            sWipeArmed = false;
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void OpenSettings() {
    if (g.hSettings) {
        ShowWindow(g.hSettings, SW_SHOW);
        SetForegroundWindow(g.hSettings);
        return;
    }
    g.themeSaved = g.themeIdx;  // 保存当前主题，取消时用于回退
    g.beautifySaved = g.beautify;
    g.antiGhostSaved = g.antiGhost;  // 抗残影开关：取消时回退
    {   // 快照每个主题的微调值：取消时用它回退滑块的实时预览改动
        int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
        sTuneSaved.assign(n, ThemeTune());
        for (int i = 0; i < n; ++i) {
            sTuneSaved[i].alpha = kThemes[i].alpha;
            sTuneSaved[i].radius = kThemes[i].radiusWindow;
        }
    }
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    UpdateScale(mon);  // 窗口与控件尺寸按当前屏幕比例创建
    int W = S(600), H = S(490);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    // 使用系统原生标题栏：标题栏与正文共用同一主题配色，圆角/暗色由 ApplyBeautify 控制。
    // 抗残影双缓冲（实验性）：开启 WS_EX_COMPOSITED 让整窗子控件双缓冲合成，消除切换 Tab 的残影。
    DWORD exStyleSettings = g.antiGhost ? WS_EX_COMPOSITED : 0;
    g.hSettings = CreateWindowExW(exStyleSettings, L"FlowtarySettings", L"Flowtary 设置",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN |
                                      WS_THICKFRAME,  // 原生标题栏（含关闭按钮）+ 可拖拽改尺寸
                                  x,
                                  y, W, H, nullptr, nullptr, g.inst, nullptr);
    if (g.hSettings) {
        ApplyBeautify();  // 圆角/暗色标题栏随美化开关
        ShowWindow(g.hSettings, SW_SHOW);
        SetForegroundWindow(g.hSettings);
    }
}

// ---------------- 窗口过程 ----------------

// 结果项快捷键：执行对应徽章的结果行（输入框聚焦时最高优先级；可在设置关闭）。
// 返回 true 表示该按键已被消费：匹配到结果，或匹配到 Alt+ 键但无对应项（吞键避免蜂鸣）。
static bool TryAltHotkey(WPARAM wParam) {
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt  = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    if (!alt || ctrl || g.hotkeyMode == 2) return false;
    WCHAR k = 0;
    if (g.hotkeyMode == 0) {
        // Alt+数字（主键盘 1..9,0 或数字小键盘）
        if (wParam >= '0' && wParam <= '9') k = (WCHAR)wParam;
        else if (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)
            k = (WCHAR)('0' + (wParam - VK_NUMPAD0));
    } else if (g.hotkeyMode == 1) {
        // Alt+字母（A–J，大小写均可）
        if (wParam >= 'A' && wParam <= 'Z') k = (WCHAR)wParam;
        else if (wParam >= 'a' && wParam <= 'z') k = (WCHAR)(wParam - 32);
    }
    if (!k) return false;  // 非本方案按键，交给系统/其它逻辑
    for (int i = 0; i < (int)g.items.size(); ++i) {
        if (HotkeyChar(i) == k) {
            g.sel = i;
            ExecuteSelected();
            return true;
        }
    }
    return true;  // Alt+键但无匹配项：吞掉，避免系统蜂鸣
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g.msgTaskbarCreated && msg == g.msgTaskbarCreated) {
        TrayAdd();  // Explorer 重启后恢复托盘图标
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            // 字体由字体池提供（见 UpdateScale / ApplyTheme）
            if (g.theme) g.brDivider = CreateSolidBrush(g.theme->divider);
            UINT blink = GetCaretBlinkTime();
            if (blink == 0 || blink == INFINITE) blink = 530;
            SetTimer(hwnd, kTimerBlink, blink, nullptr);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, kTimerBlink);
            SaveClickWeightsNow();  // 退出前把未写盘的点击权重落盘（含「仅退出时写入」模式）
            TrayRemove();
            if (g.hSettings) DestroyWindow(g.hSettings);
            if (g.hTrayIcon) {
                DestroyIcon(g.hTrayIcon);
                g.hTrayIcon = nullptr;
            }
            for (int i = 0; i < kFontMax - kFontMin + 1; ++i) {
                if (g.fontPool[i]) { DeleteObject(g.fontPool[i]); g.fontPool[i] = nullptr; }
            }
            if (g.brDivider) { DeleteObject(g.brDivider); g.brDivider = nullptr; }
            if (g.brMenuBg) { DeleteObject(g.brMenuBg); g.brMenuBg = nullptr; }
            if (g.brEditBg) { DeleteObject(g.brEditBg); g.brEditBg = nullptr; }
            if (g.brSettingsBg) { DeleteObject(g.brSettingsBg); g.brSettingsBg = nullptr; }
            PostQuitMessage(0);
            return 0;

        case WM_HOTKEY:
            if (wParam == 1) {
                // 输入框聚焦时屏蔽已注册的 Alt 热键（Alt+Space / Alt+Q / Ctrl+Alt+Space），
                // 使其不再隐藏/切换窗口，仅 Alt+数字 生效（见 WM_SYSKEYDOWN）。
                if (GetFocus() == g.hwnd) return 0;
                if (IsWindowVisible(hwnd)) Hide();
                else Show();
            } else if (wParam == 2) {
                // Ctrl+Alt+G：文件对话框激活时，把前台资源管理器当前目录同步到对话框
                fdj_sync_from_explorer();
            }
            return 0;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(hwnd)) Hide();
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            Paint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MEASUREITEM: {
            // 黑暗自绘菜单：计算条目尺寸（itemData 即文本指针，空文本=分隔线）
            MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lParam;
            if (!mis || mis->CtlType != ODT_MENU) break;
            const WCHAR* text = (const WCHAR*)mis->itemData;
            if (text && *text) {
                HDC hdc = GetDC(hwnd);
                HGDIOBJ old = SelectObject(hdc, g.fList);
                SIZE sz{};
                GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &sz);
                SelectObject(hdc, old);
                ReleaseDC(hwnd, hdc);
                mis->itemWidth = sz.cx + S(44);
                mis->itemHeight = S(30);
            } else {
                mis->itemWidth = S(60);
                mis->itemHeight = S(9);  // 分隔线高度
            }
            return TRUE;
        }

        case WM_DRAWITEM: {
            // 主题自绘菜单：背景/选中/文字均取当前主题色，与主界面一致
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (!dis || dis->CtlType != ODT_MENU) break;
            const WCHAR* text = (const WCHAR*)dis->itemData;
            const Theme& t = *g.theme;
            HBRUSH bg = CreateSolidBrush(t.menuBg);
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);
            if (!text || !*text) {  // 分隔线
                RECT lr{dis->rcItem.left + S(10), dis->rcItem.top + S(4),
                        dis->rcItem.right - S(10), dis->rcItem.top + S(5)};
                HBRUSH lb = CreateSolidBrush(t.divider);
                FillRect(dis->hDC, &lr, lb);
                DeleteObject(lb);
                return TRUE;
            }
            if (dis->itemState & ODS_SELECTED) {
                HBRUSH hb = CreateSolidBrush(t.menuHi);
                FillRect(dis->hDC, &dis->rcItem, hb);
                DeleteObject(hb);
            }
            if (dis->itemState & ODS_CHECKED) {
                // 自绘勾选标记（设置页下拉的当前项指示），置于文字左侧留白处
                int cx = dis->rcItem.left + S(8);
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                int s = S(4);
                HPEN pen = CreatePen(PS_SOLID, S(2), t.text);
                HGDIOBJ oldPen = SelectObject(dis->hDC, pen);
                MoveToEx(dis->hDC, cx - s, cy, nullptr);
                LineTo(dis->hDC, cx - s / 2, cy + s / 2);
                LineTo(dis->hDC, cx + s, cy - s / 2);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(pen);
            }
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, t.text);
            SelectObject(dis->hDC, g.fList);
            RECT tr = dis->rcItem;
            tr.left += S(16);
            DrawTextW(dis->hDC, text, -1, &tr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }

        case WM_TIMER:
            if (wParam == kTimerDebounce) {
                KillTimer(hwnd, kTimerDebounce);
                if (g.mode == Mode::Everything) ExecuteEverythingQuery();
            } else if (wParam == kTimerWeightSave) {
                KillTimer(hwnd, kTimerWeightSave);
                SaveClickWeightsNow();  // 延迟合并写盘到期：一次写出全部权重
            } else if (wParam == kTimerBlink) {
                g.caretOn = !g.caretOn;
                RECT r{0, 0, S(kBaseW), S(kBaseInputH)};
                InvalidateRect(hwnd, &r, FALSE);
            } else if (wParam == kTimerBalloon) {
                KillTimer(hwnd, kTimerBalloon);
                // 清除气泡文本，防止后续 NIM_MODIFY（如提示更新）再次弹出旧气泡
                g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                g.nid.szInfo[0] = L'\0';
                Shell_NotifyIconW(NIM_MODIFY, &g.nid);
            } else if (wParam == kTimerTween) {
                TickTweens();
            } else if (wParam == kTimerHotkeyRetry) {
                // 唤起快捷键被占用后的自动恢复：优先抢注首选组合 Alt+Space
                if (!g.hotkeyWake || g.wakeIndex == 0) {
                    KillTimer(hwnd, kTimerHotkeyRetry);
                } else {
                    UnregisterHotKey(g.hwnd, 1);  // 先让出当前备用组合
                    if (RegisterHotKey(g.hwnd, 1, kWakeHotkeys[0].mod, kWakeHotkeys[0].vk)) {
                        g.hotkeyName = kWakeHotkeys[0].name;
                        g.wakeIndex = 0;
                        g.hotkeyRegistered = true;
                        TrayUpdateTip();
                        TrayBalloon(L"Flowtary", std::wstring(L"唤起快捷键已恢复：") +
                                                     kWakeHotkeys[0].name);
                    } else {
                        RegisterWakeHotkey();  // 首选仍被占：重挂备用组合（可能仍失败，下轮再试）
                    }
                    UpdateHotkeyRetryTimer();  // 依结果决定是否继续定时
                }
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_SETTINGS:
                    OpenSettings();
                    return 0;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;

        case WM_COPYDATA: {
            const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
            if (cds && cds->dwData >= ev::kReplyBase) HandleEverythingReply(cds);
            return TRUE;
        }

        case WM_APP_PROGRAMS_READY: {
            // 工作线程扫描完成：在主线程接管结果（工作线程从不写 g.programs）
            std::vector<Program>* p = (std::vector<Program>*)lParam;
            if (p) {
                g.programs = std::move(*p);
                delete p;
            }
            g.startingUp = false;
            TrayUpdateTip();  // 提示文案从「正在启动中」切回热键名
            if (IsWindowVisible(hwnd)) LayoutAndRepaint();
            return 0;
        }

        case WM_APP_QUIT:
            // 新版本接管：干净退出（WM_DESTROY 会移除托盘、注销热键）
            DestroyWindow(hwnd);
            return 0;

        case WM_APP_TRAY: {
            UINT ev = (UINT)lParam;
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd)) Hide();
                else Show();
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                SetForegroundWindow(hwnd);
                HMENU menu = CreatePopupMenu();
                if (g.startingUp) {
                    // 扫描未完成：菜单照样弹出来（有响应），但只显示状态项与退出
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                                (LPCWSTR)L"正在启动中…");
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, 0, (LPCWSTR)L"");  // 自绘分隔线
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_EXIT, L"退出(&X)");
                } else {
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_SETTINGS, L"设置(&S)");
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_REFRESH, L"刷新缓存(&R)");
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, 0, (LPCWSTR)L"");  // 自绘分隔线
                    // 唤起快捷键开关：文字随状态切换（开启→“关闭…”，关闭→“开启…”）
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WAKE_HOTKEY,
                                g.hotkeyWake ? L"关闭唤起快捷键(&H)" : L"开启唤起快捷键(&H)");
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, 0, (LPCWSTR)L"");  // 自绘分隔线
                    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_EXIT, L"退出(&X)");
                }
                StyleDarkMenu(menu);
                POINT p;
                GetCursorPos(&p);
                int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         p.x, p.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (cmd == IDM_SETTINGS) OpenSettings();
                else if (cmd == IDM_REFRESH) {
                    BuildPrograms();
                    TrayBalloon(L"已刷新", std::to_wstring(g.programs.size()) + L" 个应用已重新索引");
                }
                else if (cmd == IDM_WAKE_HOTKEY) ToggleWakeHotkey();
                else if (cmd == IDM_EXIT) DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            // Alt+数字 由 WM_SYSKEYDOWN（Alt 组合键消息）统一处理，详见下方 case。
            switch (wParam) {
                case VK_RETURN:
                    ExecuteSelected(ResolveExecKind(ctrl, shift));
                    return 0;
                case VK_ESCAPE:
                    Hide();
                    return 0;
                case VK_UP:
                    if (!g.items.empty()) {
                        g.sel = (g.sel - 1 + (int)g.items.size()) % (int)g.items.size();
                        RepaintNow();
                    }
                    return 0;
                case VK_DOWN:
                    if (!g.items.empty()) {
                        g.sel = (g.sel + 1) % (int)g.items.size();
                        RepaintNow();
                    }
                    return 0;
case VK_LEFT:
                    if (shift) {
                        if (g.selStart == g.selEnd) {
                            g.selStart = g.selEnd = g.caret;
                        }
                        if (g.caret > 0) g.caret--;
                        g.selEnd = g.caret;
                    } else {
                        if (g.selStart != g.selEnd) {
                            g.caret = (g.selStart < g.selEnd) ? g.selStart : g.selEnd;
                            g.selStart = g.selEnd = g.caret;
                        } else if (g.caret > 0) {
                            g.caret--;
                        }
                    }
                    AfterEdit();
                    return 0;
                case VK_RIGHT:
                    if (shift) {
                        if (g.selStart == g.selEnd) {
                            g.selStart = g.selEnd = g.caret;
                        }
                        if (g.caret < g.text.size()) g.caret++;
                        g.selEnd = g.caret;
                    } else {
                        if (g.selStart != g.selEnd) {
                            g.caret = (g.selStart > g.selEnd) ? g.selStart : g.selEnd;
                            g.selStart = g.selEnd = g.caret;
                        } else if (g.caret < g.text.size()) {
                            g.caret++;
                        }
                    }
                    AfterEdit();
                    return 0;
                case VK_HOME:
                    if (shift) {
                        if (g.selStart == g.selEnd) g.selStart = g.caret;
                        g.selEnd = 0;
                    } else {
                        g.selStart = g.selEnd = 0;
                    }
                    g.caret = 0;
                    AfterEdit();
                    return 0;
                case VK_END:
                    if (shift) {
                        if (g.selStart == g.selEnd) g.selStart = g.caret;
                        g.selEnd = g.text.size();
                    } else {
                        g.selStart = g.selEnd = g.text.size();
                    }
                    g.caret = g.text.size();
                    AfterEdit();
                    return 0;
                case VK_BACK:
                    if (DeleteSelection()) {
                        AfterEdit();
                    } else if (g.caret > 0) {
                        g.text.erase(g.caret - 1, 1);
                        g.caret--;
                        AfterEdit();
                    }
                    return 0;
                case VK_DELETE:
                    if (DeleteSelection()) {
                        AfterEdit();
                    } else if (g.caret < g.text.size()) {
                        g.text.erase(g.caret, 1);
                        AfterEdit();
                    }
                    return 0;
                case 'A':
                    if (ctrl) SelectAll();
                    return 0;
                case 'V':
                    if (ctrl) Paste();
                    return 0;
                case 'C':
                    if (ctrl) CopySelection();
                    return 0;
                case 'X':
                    if (ctrl) {
                        CopySelection();
                        if (DeleteSelection()) AfterEdit();
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }

        case WM_SYSKEYDOWN: {
            // 仅处理本软件自带的 Alt+数字 / Alt+字母 快捷键：输入框聚焦时最高优先级、独占执行。
            // 其它 Alt+ 组合（Alt+F4 / Alt 单独等）不拦截，交给系统默认行为。
            if (GetFocus() == g.hwnd && g.hotkeyMode != 2) {
                if (TryAltHotkey(wParam)) return 0;
            }
            break;
        }

        case WM_CHAR:
            if (wParam >= 32) {
                WCHAR ch = (WCHAR)wParam;
                InsertChars(&ch, 1);
            }
            return 0;

        case WM_IME_SETCONTEXT:
            // 组合串由主窗口自绘内联渲染：屏蔽系统浮动组合窗（浅色"临时字"浮窗）
            if (wParam) SetImePos();
            return DefWindowProcW(hwnd, msg, wParam,
                                  lParam & ~(LPARAM)ISC_SHOWUICOMPOSITIONWINDOW);

        case WM_IME_STARTCOMPOSITION:
            SetImePos();
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_IME_CHAR:
            // 上屏文本已通过 WM_IME_COMPOSITION(GCS_RESULTSTR) 插入，
            // 此处吞掉 IME 产生的 WM_IME_CHAR，避免结果文字重复插入。
            return 0;

        case WM_IME_ENDCOMPOSITION:
            g.compText.clear();
            InvalidateRect(hwnd, &RECT{0, 0, S(kBaseW), S(kBaseInputH)}, FALSE);
            return DefWindowProcW(hwnd, msg, wParam, lParam);

        case WM_IME_COMPOSITION:
            SetImePos();
            if (lParam & GCS_RESULTSTR) {
                g.compText.clear();  // 先清组合串再上屏，避免同步重绘时画出旧文本
                HIMC hIMC = ImmGetContext(hwnd);
                if (hIMC) {
                    LONG bytes = ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0) {
                        std::vector<WCHAR> buf(bytes / sizeof(WCHAR) + 1, 0);
                        ImmGetCompositionStringW(hIMC, GCS_RESULTSTR, buf.data(), bytes);
                        InsertChars(buf.data(), wcslen(buf.data()));
                    }
                    ImmReleaseContext(hwnd, hIMC);
                }
                return 0;
            }
            if (lParam & GCS_COMPSTR) {
                // 取组合中文本，交由 Paint 内联渲染
                HIMC hIMC = ImmGetContext(hwnd);
                if (hIMC) {
                    LONG bytes = ImmGetCompositionStringW(hIMC, GCS_COMPSTR, nullptr, 0);
                    if (bytes > 0) {
                        std::vector<WCHAR> buf(bytes / sizeof(WCHAR) + 1, 0);
                        ImmGetCompositionStringW(hIMC, GCS_COMPSTR, buf.data(), bytes);
                        g.compText.assign(buf.data(), wcslen(buf.data()));
                    } else {
                        g.compText.clear();
                    }
                    ImmReleaseContext(hwnd, hIMC);
                }
                InvalidateRect(hwnd, &RECT{0, 0, S(kBaseW), S(kBaseInputH)}, FALSE);
                return 0;
            }
            if (!lParam) {  // 组合被整体清除
                g.compText.clear();
                InvalidateRect(hwnd, &RECT{0, 0, S(kBaseW), S(kBaseInputH)}, FALSE);
            }
            break;

        case WM_CONTEXTMENU: {
            // 结果列表右键菜单（lParam==-1 表示键盘 Shift+F10 / 应用键）
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            bool keyboard = (p.x == -1 && p.y == -1);
            if (!keyboard) ScreenToClient(hwnd, &p);
            int idx = keyboard ? g.sel : -1;
            int inputH = S(kBaseInputH), rowH = S(kBaseRowH);
            if (!keyboard && p.y >= inputH && !g.items.empty()) {
                idx = (p.y - inputH) / rowH;
                if (idx >= (int)g.items.size()) idx = -1;
            }
            if (idx >= 0 && idx < (int)g.items.size()) {
                g.sel = idx;
                RepaintNow();
                ShowRowMenu(hwnd);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int inputH = S(kBaseInputH), rowH = S(kBaseRowH);
            // 拖拽选区中：随鼠标移动扩展 selEnd，光标跟到末尾
            if (g.dragging) {
                size_t pos = CaretFromX(x);
                g.selEnd = pos;
                g.caret = pos;
                RepaintNow();
                return 0;
            }
            int idx = -1;
            if (y >= inputH && !g.items.empty()) {
                idx = (y - inputH) / rowH;
                if (idx < 0 || idx >= (int)g.items.size()) idx = -1;
            }
            // 悬停行变化：触发 hover 补间动画（进入→提亮，离开→回落）
            if (idx != g.hoverRow) {
                g.hoverRow = idx;
                StartTween(&g.hoverT, idx >= 0 ? 1.0 : 0.0, g.theme ? g.theme->animMs : 200);
            }
            if (idx >= 0 && idx != g.sel) {
                g.sel = idx;
                RepaintNow();
            }
            // 武装 WM_MOUSELEAVE：鼠标移出结果列表区时清悬停态
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (g.hoverRow != -1) {
                g.hoverRow = -1;
                StartTween(&g.hoverT, 0.0, g.theme ? g.theme->animMs : 200);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            int inputH = S(kBaseInputH), rowH = S(kBaseRowH);
            if (y >= inputH && !g.items.empty()) {
                int idx = (y - inputH) / rowH;
                if (idx >= 0 && idx < (int)g.items.size()) {
                    g.sel = idx;
                    g.pressRow = idx;
                    StartTween(&g.pressT, 1.0, 90);  // 按下瞬间轻微下沉（点击即执行，仅一闪）
                    ExecuteSelected();
                }
            } else {
                // 输入行：放置光标，准备拖拽选区
                SetFocus(hwnd);
                size_t pos = CaretFromX(x);
                g.caret = pos;
                if (!(GetKeyState(VK_SHIFT) & 0x8000)) {
                    g.selStart = g.selEnd = pos;
                } else {
                    g.selEnd = pos;  // 锚点 selStart 不变，扩展到 pos
                }
                g.dragging = true;
                SetCapture(hwnd);
                Refresh();
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            int y = GET_Y_LPARAM(lParam);
            if (y < S(kBaseInputH)) {
                SetFocus(hwnd);
                size_t pos = CaretFromX(GET_X_LPARAM(lParam));
                size_t a, b;
                WordBoundsAt(pos, a, b);
                g.selStart = a;
                g.selEnd = b;
                g.caret = b;
                Refresh();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g.pressRow != -1) {
                g.pressRow = -1;
                StartTween(&g.pressT, 0.0, 120);
            }
            if (g.dragging) {
                g.dragging = false;
                ReleaseCapture();
            }
            return 0;
        }

        case WM_SETCURSOR: {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (p.y < S(kBaseInputH)) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 主题化提示弹窗 ----------------
// 「重复启动」「检测到新版本」等场景使用：与主界面同款圆角 + 主题配色，
// 不受系统主题影响，也比系统 MessageBox 好看。
struct NoticeData {
    std::wstring title;
    std::wstring body;
    std::wstring btn;
    int baseX = 0, baseY = 0;   // 初始位置（淡入上移用）
    DWORD t0 = 0;               // 淡入起始时间
};
constexpr int IDC_NOTICE_OK = 5001;
constexpr int kNoticeW = 400, kNoticeH = 184;

static LRESULT CALLBACK NoticeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const NoticeData* nd = (const NoticeData*)((CREATESTRUCTW*)lp)->lpCreateParams;
            NoticeData* self = nd ? new NoticeData(*nd) : new NoticeData();
            if (self) self->t0 = GetTickCount();
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            ApplyRoundCorners(h);
            SetTimer(h, 2, 16, nullptr);  // 启动淡入动画（透明度 + 上移）
            RECT rc;
            GetClientRect(h, &rc);
            int bw = S(112), bh = S(32);
            HWND b = CreateWindowExW(0, L"BUTTON", nd ? nd->btn.c_str() : L"好",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     (rc.right - bw) / 2, rc.bottom - S(22) - bh, bw, bh, h,
                                     (HMENU)(INT_PTR)IDC_NOTICE_OK, g.inst, nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            const Theme& t = *g.theme;
            HBRUSH bg = CreateSolidBrush(t.bg);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            RECT accent{0, 0, rc.right, S(3)};  // 顶部一条强调色，呼应主题
            HBRUSH ab = CreateSolidBrush(t.menuHi);
            FillRect(hdc, &accent, ab);
            DeleteObject(ab);
            NoticeData* nd = (NoticeData*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (nd) {
                SetBkMode(hdc, TRANSPARENT);
                SelectObject(hdc, g.fontPool[FontSlot(t.fontInput + 2)]);
                SetTextColor(hdc, t.text);
                RECT tr{S(22), S(18), rc.right - S(22), S(50)};
                DrawTextW(hdc, nd->title.c_str(), -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(hdc, g.fList);
                SetTextColor(hdc, t.sub);
                RECT br{S(22), S(52), rc.right - S(22), rc.bottom - S(68)};
                DrawTextW(hdc, nd->body.c_str(), -1, &br,
                          DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (!dis || dis->CtlType != ODT_BUTTON) break;
            const Theme& t = *g.theme;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            HBRUSH bg = CreateSolidBrush(pressed ? t.selBg : t.menuHi);
            FillRect(dis->hDC, &dis->rcItem, bg);
            DeleteObject(bg);
            HBRUSH bf = CreateSolidBrush(t.divider);
            FrameRect(dis->hDC, &dis->rcItem, bf);
            DeleteObject(bf);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, t.text);
            SelectObject(dis->hDC, g.fInput);
            WCHAR text[32]{};
            GetWindowTextW(dis->hwndItem, text, 32);
            DrawTextW(dis->hDC, text, -1, &dis->rcItem,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wp) == IDC_NOTICE_OK) DestroyWindow(h);
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE || wp == VK_RETURN) {
                DestroyWindow(h);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wp == 2) {       // 淡入动画：透明度 0→目标 + 轻微上移
                NoticeData* nd = (NoticeData*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (nd) {
                    DWORD dt = GetTickCount() - nd->t0;
                    int ms = (g.theme ? g.theme->animMs : 200);
                    double p = (double)dt / ms; if (p > 1) p = 1;
                    double e = 1 - pow(1 - p, 3);                 // ease-out cubic
                    BYTE target = (BYTE)((std::max)((int)(g.theme ? g.theme->alpha : 255), 240));
                    BYTE a2 = (BYTE)(target * e); if (a2 < 1) a2 = 1;
                    int off = (int)(S(8) * (1 - e));              // 从下方 8px 滑入
                    SetLayeredWindowAttributes(h, 0, a2, LWA_ALPHA);
                    SetWindowPos(h, nullptr, nd->baseX, nd->baseY + off, 0, 0,
                                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                    if (p >= 1) KillTimer(h, 2);
                }
                return 0;
            }
            KillTimer(h, 1);     // 自动关闭（wp==1）
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            NoticeData* nd = (NoticeData*)GetWindowLongPtrW(h, GWLP_USERDATA);
            delete nd;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 显示弹窗并等待其关闭；autoCloseMs > 0 时到点自动关闭
static void ShowNotice(const std::wstring& title, const std::wstring& body,
                       const std::wstring& btn, int autoCloseMs = 0) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW nc{};
        nc.cbSize = sizeof(nc);
        nc.lpfnWndProc = NoticeProc;
        nc.hInstance = g.inst;
        nc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        nc.lpszClassName = L"FlowtaryNotice";
        if (RegisterClassExW(&nc)) registered = true;
    }
    if (!g.theme) return;
    int W = S(kNoticeW), H = S(kNoticeH);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    NoticeData nd{title, body, btn};
    nd.baseX = x; nd.baseY = y;   // 供淡入动画定位（从初始位置上移滑入）
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED, L"FlowtaryNotice", title.c_str(),
                             WS_POPUP, x, y + S(8), W, H, nullptr, nullptr, g.inst, &nd);
    if (!h) return;
    // 初始完全透明，由 NoticeProc 的淡入定时器平滑升到目标透明度（避免一闪而出）
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    if (autoCloseMs > 0) SetTimer(h, 1, autoCloseMs, nullptr);
    MSG msg;
    while (IsWindow(h) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// ---------------- top 命令轻量 toast ----------------
// 在目标窗口（顶部居中）位置显示一个快速淡入淡出、自动消失的小弹窗。
// 相比系统托盘通知：非阻塞（不开嵌套消息循环）、无按钮、不抢焦点、停留期间不跑定时器，更省资源。
struct TopToastData {
    std::wstring text;
    int baseX = 0, baseY = 0;   // 初始位置（淡入上移用）
    DWORD t0 = 0;               // 淡入起始时间
    int phase = 0;              // 0=淡入, 1=停留, 2=淡出
    DWORD phaseT0 = 0;          // 当前阶段起始时间
};

static LRESULT CALLBACK TopToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const TopToastData* nd = (const TopToastData*)((CREATESTRUCTW*)lp)->lpCreateParams;
            TopToastData* self = nd ? new TopToastData(*nd) : new TopToastData();
            if (self) { self->t0 = GetTickCount(); self->phase = 0; }
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            ApplyRoundCorners(h);
            SetTimer(h, 2, 16, nullptr);   // 淡入动画
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc; GetClientRect(h, &rc);
            const Theme& t = *g.theme;
            HBRUSH bg = CreateSolidBrush(t.bg);
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            RECT accent{0, 0, rc.right, S(3)};   // 顶部强调色条，呼应主题
            HBRUSH ab = CreateSolidBrush(t.menuHi);
            FillRect(hdc, &accent, ab);
            DeleteObject(ab);
            TopToastData* nd = (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (nd) {
                SetBkMode(hdc, TRANSPARENT);
                SelectObject(hdc, g.fontPool[FontSlot(t.fontInput + 1)]);
                SetTextColor(hdc, t.text);
                RECT tr{S(14), 0, rc.right - S(14), rc.bottom};
                DrawTextW(hdc, nd->text.c_str(), -1, &tr,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {  // 停留结束 → 进入淡出
                KillTimer(h, 1);
                TopToastData* nd = (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (nd) { nd->phase = 2; nd->phaseT0 = GetTickCount(); }
                SetTimer(h, 2, 16, nullptr);
                return 0;
            }
            if (wp == 2) {  // 淡入 / 淡出动画驱动
                TopToastData* nd = (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (!nd) break;
                DWORD now = GetTickCount();
                BYTE target = (BYTE)((std::max)((int)(g.theme ? g.theme->alpha : 255), 240));
                if (nd->phase == 0) {
                    double p = (double)(now - nd->t0) / 120.0; if (p > 1) p = 1;
                    double e = 1 - pow(1 - p, 3);              // ease-out cubic
                    BYTE a = (BYTE)(target * e); if (a < 1) a = 1;
                    SetLayeredWindowAttributes(h, 0, a, LWA_ALPHA);
                    int off = (int)(S(6) * (1 - e));           // 从下方 6px 滑入
                    SetWindowPos(h, nullptr, nd->baseX, nd->baseY + off, 0, 0,
                                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                    if (p >= 1) {
                        KillTimer(h, 2);                       // 停留期间不跑动画定时器（省资源）
                        nd->phase = 1; nd->phaseT0 = now;
                        SetTimer(h, 1, 1300, nullptr);         // 停留 1.3s
                    }
                } else if (nd->phase == 2) {
                    double p = (double)(now - nd->phaseT0) / 150.0; if (p > 1) p = 1;
                    BYTE a = (BYTE)(target * (1 - p));
                    SetLayeredWindowAttributes(h, 0, a, LWA_ALPHA);
                    if (p >= 1) DestroyWindow(h);
                }
                return 0;
            }
            break;
        case WM_DESTROY:
            KillTimer(h, 1); KillTimer(h, 2);
            delete (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 在目标窗口顶部居中弹出轻量 toast（目标窗口为 null 时退回主屏居中），非阻塞、自动消失
static void ShowTopToast(HWND target, const std::wstring& text) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW nc{};
        nc.cbSize = sizeof(nc);
        nc.lpfnWndProc = TopToastProc;
        nc.hInstance = g.inst;
        nc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        nc.lpszClassName = L"FlowtaryTopToast";
        if (RegisterClassExW(&nc)) registered = true;
    }
    if (!g.theme) return;
    // 按文本实测宽度决定弹窗尺寸（设上限防过宽）
    HDC dc = GetDC(nullptr);
    HFONT oldFont = (HFONT)SelectObject(dc, g.fontPool[FontSlot(g.theme->fontInput + 1)]);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);
    int W = sz.cx + S(28);
    int maxW = S(420);
    if (W > maxW) W = maxW;
    if (W < S(80)) W = S(80);
    int H = S(40);

    // 定位：目标窗口顶部居中（贴近窗口位置）；无目标时退回主屏居中
    int x = 0, y = 0;
    RECT wr{};
    if (target && IsWindow(target) && GetWindowRect(target, &wr)) {
        x = wr.left + ((wr.right - wr.left) - W) / 2;
        y = wr.top + S(8);
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = target ? MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
    }
    TopToastData nd{text, x, y};
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                             L"FlowtaryTopToast", L"", WS_POPUP,
                             x, y + S(6), W, H, nullptr, nullptr, g.inst, &nd);
    if (!h) return;
    SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    ShowWindow(h, SW_SHOWNOACTIVATE);  // 不抢焦点、不进入任务栏/Alt-Tab
}

// ---------------- 单实例：区分「同版本重复启动」与「新版本替换启动」 ----------------
// 版本戳 = 本 exe 路径 + 最后修改时间。运行中的实例把戳写进注册表；
// 后启动的进程比对：一致 → 同一个构建，属重复启动；不一致 → 新版本，接管。
struct BuildStamp {
    std::wstring path;
    unsigned long long stamp = 0;
};

static BuildStamp CurrentBuildStamp() {
    BuildStamp s;
    WCHAR buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    s.path = buf;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(buf, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER ul;
        ul.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        ul.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        s.stamp = (unsigned long long)ul.QuadPart;
    }
    return s;
}

static bool IsSameRunningBuild() {
    BuildStamp me = CurrentBuildStamp();
    std::wstring path = LoadRegText(L"RunningPath");
    unsigned long long stamp = 0;
    DWORD cb = sizeof(stamp);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"RunningStamp", RRF_RT_REG_QWORD,
                 nullptr, &stamp, &cb);
    return !path.empty() && path == me.path && stamp != 0 && stamp == me.stamp;
}

static void RecordRunningBuild() {
    BuildStamp me = CurrentBuildStamp();
    SaveRegText(L"RunningPath", me.path);
    unsigned long long stamp = me.stamp;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"RunningStamp", REG_QWORD,
                    &stamp, sizeof(stamp));
}

// ---------------- 入口 ----------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // DPI 感知
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    SetCtxFn setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetProcessDpiAwarenessContext");
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();
    g.inst = hInst;
    // 通用控件初始化（设置窗微调滑块 TRACKBAR_CLASS 需要；InitCommonControlsEx 仅需一次）
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
    }
    LoadSettings();  // 先加载设置：确定主题/快捷键
    // 查找 ScreenCapture.exe（优先同目录，其次注册表自定义路径）
    {
        WCHAR exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir.resize(pos + 1);
        std::wstring candidate = dir + L"ScreenCapture.exe";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g.captureExe = candidate;
        } else {
            // 尝试注册表自定义路径
            DWORD type = 0, cb = 0;
            if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureExe",
                             RRF_RT_REG_SZ, &type, nullptr, &cb) == ERROR_SUCCESS && cb > sizeof(WCHAR)) {
                std::vector<WCHAR> buf(cb / sizeof(WCHAR));
                if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureExe",
                                 RRF_RT_REG_SZ, nullptr, buf.data(), &cb) == ERROR_SUCCESS) {
                    std::wstring customPath(buf.data());
                    if (GetFileAttributesW(customPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                        g.captureExe = customPath;
                    }
                }
            }
        }
    }
    UpdateScale(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));  // 建字体池（比例 = max(DPI, 物理高/1080)）
    ApplyTheme(false);  // 建立主题字体与画刷：此时还没有窗口，只为弹窗准备绘制资源

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 单实例：区分「同版本重复启动」与「新版本替换启动」
    // （放在主题/字体初始化之后，这样弹窗才能用上主题配色与字体）
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\Flowtary.Singleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (IsSameRunningBuild()) {
            ShowNotice(L"Flowtary 已在运行",
                       L"程序已经在后台运行了。\r\n在任务栏托盘找到 Flowtary，左键单击即可唤出。",
                       L"知道了");
            return 0;
        }
        // 新版本：提示后结束旧实例，由本进程接管继续启动
        ShowNotice(L"检测到新版本", L"正在关闭旧版本并启动新版本…", L"好", 1500);
        if (HWND oldWnd = FindWindowW(L"FlowtaryLauncher", L"Flowtary"))
            PostMessageW(oldWnd, WM_APP_QUIT, 0, 0);
        WaitForSingleObject(mtx, 5000);  // 等旧实例退出并接管互斥体
    }
    RecordRunningBuild();

    LoadWebRules();
    LoadGroupRules();
    LoadExcludePaths();  // 排除路径（设置可编辑），作用于 Everything + 程序搜索结果
    LoadClickWeights();  // 点击权重数据（%APPDATA%\Flowtary\weights.dat）
    EnableDarkMenus();  // 按主题深浅强制弹出菜单（托盘/右键） 绘制

    // 初始化拼音库
    {
        WCHAR exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring dir(exePath);
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) dir.resize(pos + 1);
        std::wstring dictPath = dir + L"dict";
        Pinyin::setDictionaryPath(dictPath);
        g_pinyin = std::make_unique<Pinyin::Pinyin>();
    }
    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 注册窗口类
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 窗口图标从内置 .ico 资源加载（托盘/窗口/alt-tab 共用）
    if (!g.hAppIcon) g.hAppIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON));
    wc.hIcon = g.hAppIcon;
    wc.lpszClassName = L"FlowtaryLauncher";
    RegisterClassExW(&wc);

    // 设置窗口类（黑暗模式自绘观感）
    WNDCLASSEXW sc{};
    sc.cbSize = sizeof(sc);
    sc.lpfnWndProc = SettingsProc;
    sc.hInstance = hInst;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hIcon = g.hAppIcon;
    sc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    sc.lpszClassName = L"FlowtarySettings";
    RegisterClassExW(&sc);

    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName,
                             L"Flowtary", WS_POPUP, 0, 0, S(kBaseW), S(kBaseInputH), nullptr,
                             nullptr, hInst, nullptr);
    if (!g.hwnd) return 1;
    ApplyRoundCorners(g.hwnd);
    ApplyTheme();  // 应用主题：透明度、字体、刷子、菜单深色、星点
    fdj_init(g.hwnd);  // 文件对话框“文件夹原地跳转”增强（类 Listary Quick-Switch）

    // 唤起快捷键：仅当开关开启时注册，依次尝试 Alt+Space -> Alt+Q -> Ctrl+Alt+Space
    // （避免与其他启动器冲突导致完全不可用）；关闭状态下只保留托盘左键唤出
    if (g.hotkeyWake) {
        if (!RegisterWakeHotkey()) {
            MessageBoxW(nullptr, L"Alt+Space / Alt+Q / Ctrl+Alt+Space 热键均注册失败，可能被其他程序占用。",
                        L"Flowtary", MB_ICONWARNING);
        }
    }
    // 辅助热键：Ctrl+Alt+G —— 文件对话框激活时，把前台资源管理器当前目录同步到对话框
    RegisterHotKey(g.hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G');
    TrayAdd();  // 优先让托盘图标就位（气泡提示使用最终选定的热键名）

    // 程序扫描较慢：放到工作线程，托盘图标已先就位且可响应（右键显示「正在启动中」）
    g.startingUp = true;
    _beginthreadex(nullptr, 0, ScanProgramsThread, nullptr, 0, nullptr);
    g.everythingExe = FindEverythingExe();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 设置框：回车一律保存（规则编辑框多行，会吞掉 Return，不走默认按钮）
        if (g.hSettings && msg.hwnd && IsChild(g.hSettings, msg.hwnd) &&
            msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            SendMessageW(g.hSettings, WM_COMMAND, IDC_BTN_SAVE, 0);
            continue;
        }
        if (g.hSettings && IsDialogMessageW(g.hSettings, &msg)) continue;  // 设置框 Tab/方向键导航
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    fdj_uninit();  // 卸载文件对话框增强的钩子与 32 位助手
    RemoveWakeHook();
    UnregisterHotKey(g.hwnd, 1);
    UnregisterHotKey(g.hwnd, 2);
    CoUninitialize();
    return 0;
}
