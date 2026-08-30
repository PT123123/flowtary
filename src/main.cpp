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
#include <imm.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <process.h>
#include <new>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <unordered_map>
#include <vector>

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

    // —— 4. 材质（真实 DWM，Win11；Win10 回退 Matte）——
    enum class Material { Matte, Acrylic, Metal, Mica };
    Material material = Material::Acrylic;  // 默认真实亚克力（仅 Win11；Win10 回退 Matte）；新主题用 Mica

    // —— 7. 动效 ——
    int animMs = 200;    // 过渡时长 150-350，ease-out
    int animCurve = 0;   // 0=ease-out-cubic，1=ease-out-quad

    // —— 9. 图标与装饰 ——
    bool accentStrip = true;   // 顶部强调色条
    bool cornerGlow  = false;  // 低透明度角部辉光
    bool iconFilled  = false;  // 图标风格：false=线性描边 / true=填充

    int blurStrength = 0x55;   // 亚克力浓度（ACCENT GradientColor 高 8 位；滑块可调）
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
     6, Theme::Material::Mica, 220, 0, true, false, false, 0x55},

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
     6, Theme::Material::Mica, 220, 0, true, false, false, 0x55},

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
     6, Theme::Material::Mica, 200, 0, true, true, false, 0x55},

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
     6, Theme::Material::Mica, 200, 0, true, false, false, 0x55},
};

// 主题微调快照（透明度/毛玻璃浓度/圆角半径），随主题索引一一对应，持久化到注册表 ThemeTune blob
struct ThemeTune {
    int alpha = 255;     // 透明度 0-255（影响弹窗等离屏层；主窗为真实 DWM 材质，不依赖此项）
    int blur = 0x55;     // 亚克力浓度（DWM GradientColor 高 8 位）
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
    bool startMenu = false;
    HICON icon = nullptr;
    bool iconTried = false;
};

struct Row {
    enum Kind { File, Folder, Web, Prog, EvFallback, Hint, Group };
    Kind kind = Hint;
    std::wstring title;
    std::wstring sub;     // 路径 / URL 说明
    std::wstring action;  // 打开目标；EvFallback 时为 Everything 命令行参数
    Program* prog = nullptr;
    // 一键组（Row::Group）：groupKill=false 启动 groupTargets 里的文件；
    // groupKill=true 结束 groupTargets 里的进程名
    bool groupKill = false;
    std::vector<std::wstring> groupTargets;
};

// 一键组：关键字 → 目标列表（启动组存文件路径，关闭组存进程名）
struct CmdGroup {
    std::wstring key;                   // 关键字（小写存储，比较时忽略大小写）
    std::vector<std::wstring> targets;
};

enum class Mode { None, Everything, Web, Programs };

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
    int hotkeyModeSaved = 0;   // 同上，结果项快捷键方案（取消时回退）
    int settingsTab = 0;        // 设置窗当前 Tab：0=常规, 1=网页规则, 2=主题, 3=一键, 4=搜索权重（关闭后仍记住上次选择）
    int themeIdx = 0;          // 当前主题索引（设置窗切换后、保存前为暂存值）
    int themeSaved = 0;        // 设置窗打开时的初始主题（取消时回退）
    bool beautify = true;      // 界面美化：暗色标题栏 + 圆角窗口 + 强制暗色菜单（默认开）
    bool beautifySaved = true; // 设置窗打开时的初始值（取消时回退）
    bool glass = true;         // 毛玻璃（亚克力）背景，仅在 beautify 开启时生效（默认开）
    bool glassSaved = true;    // 设置窗打开时的初始值（取消时回退）
    bool fdjEnabled = true;    // 文件对话框跳转总开关（默认开；UI 在设置「常规」Tab，不再放托盘菜单）
    bool fdjEnabledSaved = true;  // 设置窗打开时的初始值（取消时回退）
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

    std::wstring text;
    size_t caret = 0;
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

    NOTIFYICONDATAW nid{};     // 托盘图标
    HICON hTrayIcon = nullptr;
    HICON hAppIcon = nullptr;  // 窗口图标（与托盘同款，字体绘制，不依赖 .ico 资源）
    HWND hSettings = nullptr;  // 设置窗口
    UINT msgTaskbarCreated = 0;
} g;

constexpr int kBaseW = 600, kBaseInputH = 56, kBaseRowH = 30, kBasePad = 12;
constexpr int kFontMin = 11, kFontMax = 24;  // 字体池逻辑字号范围
constexpr UINT_PTR kTimerDebounce = 1;
constexpr UINT_PTR kTimerBlink = 2;
constexpr UINT_PTR kTimerBalloon = 3;
constexpr UINT_PTR kTimerWeightSave = 4;   // 点击权重延迟合并写盘
constexpr UINT_PTR kTimerTween = 5;        // 补间动画（与光标闪烁 kTimerBlink 区分，互不干扰）
constexpr int kDebounceMs = 120;
constexpr int kWeightSaveDelayMs = 3000;   // 延迟合并写盘的等待时间
constexpr int WM_APP_TRAY = WM_APP + 1;
constexpr int WM_APP_PROGRAMS_READY = WM_APP + 2;  // 工作线程扫描完成，回主线程接管结果
constexpr int WM_APP_QUIT = WM_APP + 3;  // 新版本接管：通知旧实例退出
constexpr int IDM_SETTINGS = 2001;
constexpr int IDM_EXIT = 2002;
constexpr int IDM_REFRESH = 2003;   // 托盘菜单：刷新应用缓存
constexpr int IDM_OPEN = 2011;      // 结果右键菜单：打开
constexpr int IDM_OPENLOC = 2012;   // 结果右键菜单：打开所在文件夹
constexpr int IDM_COPYPATH = 2013;  // 结果右键菜单：复制路径
constexpr int IDM_RUNAS = 2014;     // 结果右键菜单：以管理员模式打开

// 结果项快捷键方案：
//   方案0 Alt+数字：按结果优先级自上而下分配 1..9,0（列表最多 10 行，1=最高优先级）
//   方案1 Alt+字母：按结果优先级自上而下分配 A..J（列表最多 10 行，A=最高优先级）
static const WCHAR* kHotkeyOrder = L"1234567890";
static const WCHAR* kHotkeyLetterOrder = L"ABCDEFGHIJ";

// 前置声明（后文定义，ApplyTheme 需要）
static void EnableDarkMenus();
static void ApplyGlass();
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

// 主窗已非分层窗口，毛玻璃走 DWM 系统亚克力（见 ApplyGlassTo）；透明键色 kGlassKey 不再需要。

// 强制整窗重绘并触发 DWM 重新合成：切换毛玻璃/主题/美化时，DWMWA_SYSTEMBACKDROP_TYPE 与
// 可能残留的 accent 都改的是 DWM 合成层，主动刷新可避免切换瞬间旧材质/旧模糊的残影。
// 先 RDW_ERASE|RDW_INVALIDATE 让 GDI 位图与新材质对齐，再 SWP_FRAMECHANGED nudge DWM 重合成。
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
        // 主窗已非分层窗口：毛玻璃由 ApplyGlass 走 DWMWA_SYSTEMBACKDROP_TYPE（系统亚克力），
        // 整体透明度/COLORKEY 已不适用；仅刷新分隔线刷子。
        if (g.brDivider) { DeleteObject(g.brDivider); g.brDivider = nullptr; }
        g.brDivider = CreateSolidBrush(t.divider);
    }
    // 设置窗已非分层窗口，透明度由 DWM 亚克力材质负责，这里不再调用
    // SetLayeredWindowAttributes（对无 WS_EX_LAYERED 的窗口无效）。
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
    ApplyGlass();  // 毛玻璃背景随美化开关/玻璃开关与主题底色刷新
    // 切换主题后同步标题栏明暗（深色↔浅色主题时标题栏要跟着变）
    if (g.hSettings && g.beautify) ApplyDarkTitlebar(g.hSettings);
    if (t.stars) GenerateStars();
    if (g.hSettings && repaint) {
        EnumChildWindows(g.hSettings, RefreshChildFont, 0);
        RefreshComposition(g.hSettings);  // 设置窗亚克力切换也强制重合成（含 SWP_FRAMECHANGED），消除残影
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

    // 按显示名称去重（保留优先级更高的来源）
    for (auto& p : raw) {
        std::wstring k = ToLowerW(p.name);
        bool dup = false;
        for (auto& q : out) {
            if (ToLowerW(q.name) == k) { dup = true; break; }
        }
        if (!dup) out.push_back(std::move(p));
    }
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

static void SearchPrograms(const std::wstring& query) {
    std::wstring ql = NormalizeSearchTerm(query);  // 与权重 key 同款标准化（去首尾空格+小写）
    struct Cand { Program* p; int score; int w; };
    std::vector<Cand> cands;
    for (auto& p : g.programs) {
        int s = MatchScore(ToLowerW(p.name), ql);
        if (s > 0) {
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
    // 若正在组词，先取消 IME 组合，避免残留未上屏状态
    HIMC hIMC = ImmGetContext(g.hwnd);
    if (hIMC) {
        ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(g.hwnd, hIMC);
    }
    g.compText.clear();
    ShowWindow(g.hwnd, SW_HIDE);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);  // 归还物理内存
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
    for (DWORD i = 0; i < n; ++i) {
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
        g.items.push_back(std::move(r));
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

static void Refresh() {
    KillTimer(g.hwnd, kTimerDebounce);
    g.items.clear();
    g.sel = 0;
    g.mode = Mode::None;
    g.expectReply = 0;
    g.evTermKey.clear();

    const std::wstring& t = g.text;
    if (!t.empty()) {
        size_t sp = t.find(L' ');
        std::wstring tok = ToLowerW(sp == std::wstring::npos ? t : t.substr(0, sp));
        std::wstring rest = sp == std::wstring::npos ? L"" : TrimW(t.substr(sp + 1));

        if (sp != std::wstring::npos && (tok == L"d" || tok == L"f")) {
            g.mode = Mode::Everything;
            if (rest.empty()) {
                AddHint(tok == L"f" ? L"输入关键词搜索文件" : L"输入关键词搜索文件夹");
            } else {
                g.evQuery = BuildModifierQuery(tok == L"f" ? L"file:" : L"folder:", rest);
                g.evTermKey = NormalizeSearchTerm(rest);  // 本次查询的有效搜索词（权重 key）
                AddHint(L"正在搜索…");
                SetTimer(g.hwnd, kTimerDebounce, kDebounceMs, nullptr);
            }
        } else if (sp != std::wstring::npos && FindWebCmd(tok)) {
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
static bool ExecuteRow(Row& r) {
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
        case Row::Hint:
            break;
    }
    return false;
}

static void ExecuteSelected() {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    RecordClickWeight(g.items[g.sel]);  // 点击/回车/快捷键选中即记权重（内部过滤非本地条目与不保存的前缀）
    if (ExecuteRow(g.items[g.sel])) Hide();
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

static void ShowRowMenu(HWND hwnd) {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    Row& r = g.items[g.sel];
    if (r.kind == Row::Hint) return;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPEN, (LPCWSTR)L"打开");
    if (r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog)
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPENLOC, (LPCWSTR)L"打开所在文件夹");
    if (r.kind == Row::File || r.kind == Row::Prog)
        AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_RUNAS, (LPCWSTR)L"以管理员模式打开");
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_COPYPATH,
                (LPCWSTR)(r.kind == Row::Web ? L"复制链接" : L"复制路径"));
    SetMenuDefaultItem(menu, IDM_OPEN, FALSE);
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
        case IDM_RUNAS:
            ExecuteRowAdmin(r);
            break;
        case IDM_COPYPATH:
            CopyTextToClipboard(r.action);  // Web 行复制的是链接
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

static void InsertChars(const WCHAR* s, size_t n) {
    if (n == 0) return;
    g.text.insert(g.caret, s, n);
    g.caret += n;
    AfterEdit();
}

static void CopyAll() {
    CopyTextToClipboard(g.text);
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

// ---------------- 托盘 / 注册表 / 圆角 ----------------
// Win11 DWM 圆角 + 描边；旧系统调用失败则保持直角（Paint 回退 GDI 描边）
static void ApplyRoundCorners(HWND h) {
    DWORD pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    COLORREF border = (g.theme ? g.theme->border : RGB(80, 80, 80));
    if (SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border))))
        g.dwmBorderOk = true;
}

// ---------------- 毛玻璃（亚克力）背景 ----------------
// 毛玻璃实现说明：主窗与设置窗现在都是「非分层窗口」，统一走文档化
// DWMWA_SYSTEMBACKDROP_TYPE(38) 系统亚克力材质（Win11 有效）：3=TransientWindow(亚克力) 生效、1=None 关闭。
// 客户区背景由 Paint 在玻璃开启时不铺底而自然透出亚克力；OS 原生合成，切换主题/开关均无残留。
// 历史未公开的 SetWindowCompositionAttribute/BLURBEHIND 方案在分层窗上会与亚克力冲突且切换残留，已弃用，
// 仅保留「清残留 accent」的兜底调用（见 ApplyGlassTo）。
struct AccentPolicy {
    int AccentState;
    int AccentFlags;
    int GradientColor;
    int AnimationId;
};
struct WinCompAttrData {
    int Attribute;
    void* Data;
    ULONG SizeOfData;
};
typedef BOOL(WINAPI* SetWindowCompositionAttributeFn)(HWND, WinCompAttrData*);

static void ApplyGlassTo(HWND h) {
    if (!h) return;
    const bool on = g.beautify && g.glass;
    // 主窗与设置窗现已统一为「非分层窗口」，均走文档化 DWM 亚克力材质：
    // DWMWA_SYSTEMBACKDROP_TYPE=3(TransientWindow/亚克力) 生效，1=None 关闭。
    // 由 OS 原生合成，切换主题/开关毛玻璃均无残留。
    DWORD backdrop = 1;  // 默认 None（哑光 / 关毛玻璃 / Win10）
    if (on && g.theme) {
        // 仅 Win11 的 DWMWA_SYSTEMBACKDROP_TYPE 支持 Mica/Acrylic；Win10 下该属性被忽略，
        // 窗口走 Paint 实铺底色（即 Matte 回退），无需额外处理。
        switch (g.theme->material) {
            case Theme::Material::Mica:    backdrop = 2; break;  // 2=Mica（哑光，汲取桌面壁纸色调）
            case Theme::Material::Acrylic: backdrop = 3; break;  // 3=TransientWindow/亚克力（半透）
            case Theme::Material::Metal:   backdrop = 3; break;  // Metal 暂用亚克力近似（Paint 另加微弱渐变）
            case Theme::Material::Matte:   backdrop = 1; break;  // 无材质
        }
    }
    DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
    // 清掉可能残留的未公开 accent（历史 BLURBEHIND 缓存会叠加在亚克力上，必须清掉）
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        auto fn = (SetWindowCompositionAttributeFn)GetProcAddress(u, "SetWindowCompositionAttribute");
        if (fn) {
            AccentPolicy ap{};
            ap.AccentState = 0;  // ACCENT_DISABLED
            WinCompAttrData d{19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap)};
            fn(h, &d);
        }
    }
    RefreshComposition(h);
}

static void ApplyGlass() {
    ApplyGlassTo(g.hwnd);
    ApplyGlassTo(g.hSettings);
}

// 注：主窗已非分层窗口，不再有整体 alpha 透明度，故无 EffectiveAlpha 压暗逻辑。

// 图标一律用系统自带字体现场绘制：优先 Segoe MDL2 Assets / Segoe Fluent Icons 的
// 放大镜字形（U+E721），图标字体缺失时回退 Segoe UI 粗体字母「F」。
// 不引入任何 .ico 资源，零额外图标开销。
static HICON MakeFontIcon(int size) {
    if (size < 8) size = 16;
    HDC sdc = GetDC(nullptr);
    HBITMAP color = CreateCompatibleBitmap(sdc, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HDC dc = CreateCompatibleDC(sdc);

    // 颜色位图：黑色圆底
    HGDIOBJ oldBmp = SelectObject(dc, color);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    HGDIOBJ oldBr = SelectObject(dc, black);
    Ellipse(dc, 0, 0, size, size);
    SelectObject(dc, oldBr);
    DeleteObject(black);

    // 白色字形：用 GetGlyphIndices 探测码位是否真实存在，避免字体缺失画成方框
    const WCHAR* kGlyph = L"\xE721";  // 放大镜（Search）
    const WCHAR* kFams[] = {L"Segoe MDL2 Assets", L"Segoe Fluent Icons", L"Segoe UI Symbol"};
    int fh = -(size * 58 / 100);
    HFONT f = nullptr;
    bool useGlyph = false;
    for (const WCHAR* fam : kFams) {
        HFONT cand = CreateFontW(fh, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, fam);
        if (!cand) continue;
        HGDIOBJ oldF = SelectObject(dc, cand);
        WORD gi = 0;
        DWORD gr = GetGlyphIndicesW(dc, kGlyph, 1, &gi, GGI_MARK_NONEXISTING_GLYPHS);
        SelectObject(dc, oldF);
        if (gr != GDI_ERROR && gi != 0xFFFF) {
            f = cand;
            useGlyph = true;
            break;
        }
        DeleteObject(cand);
    }
    if (!f) {  // 图标字体不可用：回退字母 F
        f = CreateFontW(fh, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (f) {
        HGDIOBJ oldF = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT r{0, 0, size, size};
        DrawTextW(dc, useGlyph ? kGlyph : L"F", -1, &r,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldF);
        DeleteObject(f);
    }
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBmp);

    // 掩码：单色位图，白=透明、黑=不透明 → 圆外透明
    oldBmp = SelectObject(dc, mask);
    oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    oldBr = SelectObject(dc, GetStockObject(BLACK_BRUSH));
    PatBlt(dc, 0, 0, size, size, WHITENESS);
    Ellipse(dc, 0, 0, size, size);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBmp);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(nullptr, sdc);
    return ic;
}

static void TrayAdd() {
    if (!g.hwnd) return;
    if (!g.hTrayIcon) g.hTrayIcon = MakeFontIcon(GetSystemMetrics(SM_CXSMICON));
    if (!g.hTrayIcon) return; // Guard against icon creation failure
    ZeroMemory(&g.nid, sizeof(g.nid));
    g.nid.cbSize = sizeof(g.nid);
    g.nid.hWnd = g.hwnd;
    g.nid.uID = 1;
    g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    g.nid.uCallbackMessage = WM_APP_TRAY;
    g.nid.hIcon = g.hTrayIcon;
    std::wstring tip = g.startingUp ? L"Flowtary — 正在启动中…"
                                    : (L"Flowtary — " + g.hotkeyName + L" 唤出");
    lstrcpynW(g.nid.szTip, tip.c_str(), ARRAYSIZE(g.nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

static void TrayUpdateTip() {
    if (!g.hwnd) return;
    std::wstring tip = g.startingUp ? L"Flowtary — 正在启动中…"
                                    : (L"Flowtary — " + g.hotkeyName + L" 唤出");
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
    cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Glass", RRF_RT_REG_DWORD,
                     nullptr, &v, &cb) == ERROR_SUCCESS)
        g.glass = v != 0;
    else
        g.glass = true;  // 默认开
    // 主题微调（透明度/毛玻璃浓度/圆角）：按主题读回 blob；主题数量变化时整体忽略（用预设默认）
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
                    int b = (std::max)(0, (std::min)(255, sTune[i].blur));
                    int r = (std::max)(0, (std::min)(14, sTune[i].radius));
                    kThemes[i].alpha = (BYTE)a;
                    kThemes[i].blurStrength = b;
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
    if (sp != std::wstring::npos) {
        std::wstring tok = ToLowerW(t.substr(0, sp));
        std::wstring rest = TrimW(t.substr(sp + 1));
        if (tok == L"d" || tok == L"f") {
            termOut = NormalizeSearchTerm(rest);
            return !termOut.empty();
        }
        if (FindWebCmd(tok)) return false;  // gg 等网页前缀：本次点击不记录权重
    }
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

    bool glassOn = g.beautify && g.glass;
    HDC mem;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    if (glassOn) {
        // 毛玻璃开启：直接画到窗口 DC 且不铺背景，让 DWM 系统亚克力材质透出；
        // 文字/列表/边框照常绘制（整体 alpha/COLORKEY 在主窗已不适用）。
        mem = hdc;
    } else {
        mem = CreateCompatibleDC(hdc);
        bmp = CreateCompatibleBitmap(hdc, W, H);
        oldBmp = SelectObject(mem, bmp);
        // 非玻璃态按主题铺底（双缓冲离屏，再 BitBlt 到窗口 DC）
        if (t.bg2 != t.bg) FillVGradient(mem, 0, 0, W, H, t.bg, t.bg2);
        else { HBRUSH bgBr = CreateSolidBrush(t.bg); FillRect(mem, &rc, bgBr); DeleteObject(bgBr); }
    }
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

    if (!glassOn) {
        // 仅非玻璃态（离屏双缓冲）需把位图拷到窗口 DC；玻璃态直接画在窗口 DC 上
        BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }
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
constexpr int IDC_CHK_GLASS = 3018;     // 主题页：毛玻璃背景开关
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
// 主题页：自绘主题选择器（色板列表）+ 3 个微调滑块（实时预览，保存后生效）
constexpr int IDC_LST_THEME = 3040;  // 主题选择器：自绘列表框
constexpr int IDC_LBL_TUNE = 3041;   // 「微调」说明标签
constexpr int IDC_TRK_ALPHA = 3042;  // 透明度滑块 0-255
constexpr int IDC_LBL_ALPHA = 3043;  // 透明度滑块标签
constexpr int IDC_TRK_BLUR = 3044;   // 毛玻璃浓度滑块 0-255
constexpr int IDC_LBL_BLUR = 3045;   // 毛玻璃浓度滑块标签
constexpr int IDC_TRK_RADIUS = 3046; // 圆角半径滑块 0-14
constexpr int IDC_LBL_RADIUS = 3047; // 圆角半径滑块标签
constexpr int IDM_THEME_BASE = 4200;  // 主题下拉菜单指令基值
constexpr int IDM_FLUSH_BASE = 4410;  // 写入时机下拉菜单指令基值
constexpr int IDM_MAXENT_BASE = 4420; // 条目上限下拉菜单指令基值

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
    vis(IDC_CHK_GLASS, theme);
    vis(IDC_LBL_THEME, theme);
    vis(IDC_LST_THEME, theme);
    vis(IDC_LBL_TUNE, theme);
    vis(IDC_TRK_ALPHA, theme);
    vis(IDC_LBL_ALPHA, theme);
    vis(IDC_TRK_BLUR, theme);
    vis(IDC_LBL_BLUR, theme);
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
    ApplyGlass();       // 毛玻璃随美化开关一起开关
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
        case IDC_CHK_GLASS:
        case IDC_CHK_WEIGHTON:
        case IDC_CHK_FILEDLGJUMP:
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
        case IDC_LST_THEME:
        case IDC_LBL_TUNE:
        case IDC_TRK_ALPHA:
        case IDC_TRK_BLUR:
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

// 主题微调滑块：把当前主题的 alpha/blur/radius 同步到滑块位置与标签
static void UpdateTuneLabel(HWND h, int tid, int v) {
    int lid = (tid == IDC_TRK_ALPHA) ? IDC_LBL_ALPHA
            : (tid == IDC_TRK_BLUR)  ? IDC_LBL_BLUR
                                     : IDC_LBL_RADIUS;
    HWND lab = GetDlgItem(h, lid);
    if (!lab) return;
    const WCHAR* name = (tid == IDC_TRK_ALPHA) ? L"透明度"
                       : (tid == IDC_TRK_BLUR)  ? L"毛玻璃浓度"
                                                : L"圆角半径";
    WCHAR buf[48];
    swprintf_s(buf, L"%s：%d", name, v);
    SetWindowTextW(lab, buf);
}
static void SyncTuneSliders(HWND h) {
    if (!g.theme) return;
    HWND a = GetDlgItem(h, IDC_TRK_ALPHA), b = GetDlgItem(h, IDC_TRK_BLUR),
          r = GetDlgItem(h, IDC_TRK_RADIUS);
    if (a) SendMessageW(a, TBM_SETPOS, TRUE, g.theme->alpha);
    if (b) SendMessageW(b, TBM_SETPOS, TRUE, g.theme->blurStrength);
    if (r) SendMessageW(r, TBM_SETPOS, TRUE, g.theme->radiusWindow);
    UpdateTuneLabel(h, IDC_TRK_ALPHA, g.theme->alpha);
    UpdateTuneLabel(h, IDC_TRK_BLUR, g.theme->blurStrength);
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
            c = CreateWindowExW(0, L"BUTTON", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin + S(120), S(44), contentW - S(120), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_HOTKEY, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"唤醒位置：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(72), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_WAKE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 唤醒位置下拉框：系统 ComboBox 的圆角边框在浅色系统主题下无法变暗，
            // 改为自绘按钮 + 黑暗弹出菜单（复用 StyleDarkMenu），外观完全可控。
            c = CreateWindowExW(0, L"BUTTON", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin + S(90), S(74), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_WAKE, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

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

            c = CreateWindowExW(0, L"STATIC", L"毛玻璃浓度",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(314), S(64),
                                S(20), h, (HMENU)(INT_PTR)IDC_LBL_BLUR, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            c = CreateWindowExW(0, TRACKBAR_CLASS, nullptr,
                                WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_BOTH,
                                margin + S(68), S(314), contentW - S(68), S(20), h,
                                (HMENU)(INT_PTR)IDC_TRK_BLUR, g.inst, nullptr);
            SendMessageW(c, TBM_SETRANGE, TRUE, MAKELONG(0, 255));
            SendMessageW(c, TBM_SETPOS, TRUE, g.theme ? g.theme->blurStrength : 0x55);

            c = CreateWindowExW(0, L"STATIC", L"圆角半径",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(338), S(64),
                                S(20), h, (HMENU)(INT_PTR)IDC_LBL_RADIUS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
            c = CreateWindowExW(0, TRACKBAR_CLASS, nullptr,
                                WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS | TBS_BOTH,
                                margin + S(68), S(338), contentW - S(68), S(20), h,
                                (HMENU)(INT_PTR)IDC_TRK_RADIUS, g.inst, nullptr);
            SendMessageW(c, TBM_SETRANGE, TRUE, MAKELONG(0, 14));
            SendMessageW(c, TBM_SETPOS, TRUE, g.theme ? g.theme->radiusWindow : 10);
            SyncTuneSliders(h);  // 标签显示「名称：当前值」，与滑块位置对齐

            // 微调滑块依赖界面美化（毛玻璃/材质）：初始按美化开关置灰/恢复
            if (!g.beautify) {
                EnableWindow(GetDlgItem(h, IDC_TRK_ALPHA), FALSE);
                EnableWindow(GetDlgItem(h, IDC_TRK_BLUR), FALSE);
                EnableWindow(GetDlgItem(h, IDC_TRK_RADIUS), FALSE);
            }

            // 界面美化开关（自绘复选框，状态由 g.beautify 驱动；移入「主题」Tab）
            c = CreateWindowExW(0, L"BUTTON",
                                L"启用界面美化（暗色标题栏 / 圆角窗口 / 暗色菜单）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(20), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_BEAUTIFY, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            // 毛玻璃背景开关（仅在界面美化开启时可勾选）
            c = CreateWindowExW(0, L"BUTTON", L"毛玻璃背景（亚克力模糊）",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin, S(52), contentW, S(24), h,
                                (HMENU)(INT_PTR)IDC_CHK_GLASS, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

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
            c = CreateWindowExW(0, L"BUTTON", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin + S(90), S(54), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_FLUSH, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"STATIC", L"条目上限：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(88), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_MAXENT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
            c = CreateWindowExW(0, L"BUTTON", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin + S(90), S(90), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_MAXENT, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

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

            RecordSettingsLayout(h);            // 记录初始几何，之后可随窗口缩放重排
            ShowSettingsTab(h, g.settingsTab);  // 按当前 Tab 初始化分组可见性并重排
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
            // 毛玻璃开启时：不铺底，让 DWM 亚克力材质（含主题色调）透出，
            // 原生标题栏与正文共用同一材质 / 同色，整窗统一。
            // 关闭毛玻璃时：用主题底色铺满客户区，外观依旧协调。
            bool glassOn = g.beautify && g.glass;
            if (!glassOn) {
                FillRect(hdc, &rc,
                         g.brSettingsBg ? g.brSettingsBg : (HBRUSH)GetStockObject(BLACK_BRUSH));
            }
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
            const int kEditIds[] = {IDC_EDT_RULES, IDC_EDT_LAUNCH, IDC_EDT_KILL};
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
            // 组合框下拉列表（COMBOLBOX）背景染黑，消除最后一条下方的白条
            return (LRESULT)GetStockObject(BLACK_BRUSH);

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
                w == GetDlgItem(h, IDC_EDT_KILL)) {
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
            if (mis && mis->CtlType == ODT_COMBOBOX) mis->itemHeight = S(26);
            else if (mis && mis->CtlType == ODT_LISTBOX) mis->itemHeight = S(34);
            return TRUE;
        }

        case WM_HSCROLL: {
            // 主题微调滑块（trackbar 发 WM_HSCROLL，不进 WM_COMMAND）
            HWND tb = (HWND)lp;
            int tid = GetDlgCtrlID(tb);
            if (tid == IDC_TRK_ALPHA || tid == IDC_TRK_BLUR || tid == IDC_TRK_RADIUS) {
                if (!g.beautify || !g.theme) break;
                int v = (int)SendMessageW(tb, TBM_GETPOS, 0, 0);
                if (tid == IDC_TRK_ALPHA) g.theme->alpha = (BYTE)v;
                else if (tid == IDC_TRK_BLUR) g.theme->blurStrength = v;
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
                if (id == IDC_CHK_START || id == IDC_CHK_BEAUTIFY || id == IDC_CHK_GLASS ||
                    id == IDC_CHK_WEIGHTON || id == IDC_CHK_FILEDLGJUMP) {
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
                                 : (id == IDC_CHK_GLASS) ? g.glass
                                 : (id == IDC_CHK_FILEDLGJUMP) ? g.fdjEnabled
                                                               : g.weightEnabled;
                    if (checked)
                        DrawCheckGlyph(dis->hDC, box, t.text);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    // 毛玻璃依附于界面美化：美化关闭时整项置灰
                    SetTextColor(dis->hDC, (id == IDC_CHK_GLASS && !g.beautify) ? t.sub : t.text);
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
                    id == IDC_CMB_FLUSH || id == IDC_CMB_MAXENT) {
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
                    id == IDC_TAB_GROUP || id == IDC_TAB_WEIGHT) {
                    // 左侧 Tab 按钮：激活项用强调色高亮，并加左侧竖条
                    int idx = (id == IDC_TAB_GENERAL) ? 0
                            : (id == IDC_TAB_WEB)     ? 1
                            : (id == IDC_TAB_THEME)   ? 2
                            : (id == IDC_TAB_GROUP)   ? 3
                                                      : 4;
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
                                                               : L"搜索权重";
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
                // 每个主题的微调（透明度/毛玻璃浓度/圆角）以 blob 持久化
                {
                    int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                    sTune.assign(n, ThemeTune());
                    for (int i = 0; i < n; ++i) {
                        sTune[i].alpha = kThemes[i].alpha;
                        sTune[i].blur = kThemes[i].blurStrength;
                        sTune[i].radius = kThemes[i].radiusWindow;
                    }
                    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ThemeTune",
                                    REG_BINARY, sTune.data(),
                                    (DWORD)(sTune.size() * sizeof(ThemeTune)));
                }
                DWORD vb = g.beautify ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Beautify",
                                REG_DWORD, &vb, sizeof(vb));
                DWORD vg = g.glass ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Glass",
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
                InvalidateRect(GetDlgItem(h, IDC_CHK_GLASS), nullptr, TRUE);
                // 微调滑块依赖界面美化（毛玻璃/材质）；关闭时一并置灰
                bool en = g.beautify;
                EnableWindow(GetDlgItem(h, IDC_TRK_ALPHA), en);
                EnableWindow(GetDlgItem(h, IDC_TRK_BLUR), en);
                EnableWindow(GetDlgItem(h, IDC_TRK_RADIUS), en);
            } else if (id == IDC_CHK_GLASS && HIWORD(wp) == BN_CLICKED) {
                if (!g.beautify) return 0;  // 美化关闭时毛玻璃不可切换
                g.glass = !g.glass;
                ApplyTheme();  // 内部会调 ApplyGlass，并重算窗口透明度让模糊透出来
                InvalidateRect(GetDlgItem(h, IDC_CHK_GLASS), nullptr, TRUE);
            } else if (id == IDC_CMB_HOTKEY && HIWORD(wp) == BN_CLICKED) {
                // 结果项快捷键方案下拉：列 数字/字母/关闭，当前项打勾；选择后即时预览
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING | (g.hotkeyMode == 0 ? MF_CHECKED : 0), 4310,
                            L"Alt + 数字（1–9,0）");
                AppendMenuW(m, MF_STRING | (g.hotkeyMode == 1 ? MF_CHECKED : 0), 4311,
                            L"Alt + 字母（A–J）");
                AppendMenuW(m, MF_STRING | (g.hotkeyMode == 2 ? MF_CHECKED : 0), 4312,
                            L"关闭");
                StyleDarkMenu(m);
                RECT r;
                GetWindowRect(GetDlgItem(h, IDC_CMB_HOTKEY), &r);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd == 4310) g.hotkeyMode = 0;
                else if (cmd == 4311) g.hotkeyMode = 1;
                else if (cmd == 4312) g.hotkeyMode = 2;
                else return 0;
                InvalidateRect(GetDlgItem(h, IDC_CMB_HOTKEY), nullptr, TRUE);
                if (IsWindowVisible(g.hwnd)) RepaintNow();
            } else if (id == IDC_CHK_WEIGHTON && HIWORD(wp) == BN_CLICKED) {
                g.weightEnabled = !g.weightEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_WEIGHTON), nullptr, TRUE);
            } else if (id == IDC_CHK_FILEDLGJUMP && HIWORD(wp) == BN_CLICKED) {
                g.fdjEnabled = !g.fdjEnabled;
                InvalidateRect(GetDlgItem(h, IDC_CHK_FILEDLGJUMP), nullptr, TRUE);
            } else if (id == IDC_CMB_FLUSH && HIWORD(wp) == BN_CLICKED) {
                // 写入时机下拉：立即 / 延迟合并 / 退出时，当前项打勾
                HMENU m = CreatePopupMenu();
                for (int i = 0; i < 3; ++i)
                    AppendMenuW(m, MF_STRING | (g.weightFlush == i ? MF_CHECKED : 0),
                                IDM_FLUSH_BASE + i, WeightFlushText(i));
                StyleDarkMenu(m);
                RECT r;
                GetWindowRect(GetDlgItem(h, IDC_CMB_FLUSH), &r);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd >= IDM_FLUSH_BASE && cmd < IDM_FLUSH_BASE + 3)
                    g.weightFlush = cmd - IDM_FLUSH_BASE;
                else
                    return 0;
                InvalidateRect(GetDlgItem(h, IDC_CMB_FLUSH), nullptr, TRUE);
            } else if (id == IDC_CMB_MAXENT && HIWORD(wp) == BN_CLICKED) {
                // 记忆条目上限下拉（内存与磁盘均按此裁剪，权重升序淘汰）
                static const int kMaxEntOpts[] = {1000, 5000, 20000, 50000};
                HMENU m = CreatePopupMenu();
                WCHAR opt[32];
                for (int i = 0; i < 4; ++i) {
                    swprintf_s(opt, L"%d 条", kMaxEntOpts[i]);
                    AppendMenuW(m, MF_STRING | (g.weightMaxEntries == kMaxEntOpts[i]
                                                    ? MF_CHECKED : 0),
                                IDM_MAXENT_BASE + i, opt);
                }
                StyleDarkMenu(m);
                RECT r;
                GetWindowRect(GetDlgItem(h, IDC_CMB_MAXENT), &r);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd >= IDM_MAXENT_BASE && cmd < IDM_MAXENT_BASE + 4)
                    g.weightMaxEntries = kMaxEntOpts[cmd - IDM_MAXENT_BASE];
                else
                    return 0;
                InvalidateRect(GetDlgItem(h, IDC_CMB_MAXENT), nullptr, TRUE);
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
                         id == IDC_TAB_GROUP || id == IDC_TAB_WEIGHT) &&
                        HIWORD(wp) == BN_CLICKED) {
                ShowSettingsTab(h, (id == IDC_TAB_GENERAL) ? 0
                                 : (id == IDC_TAB_WEB)    ? 1
                                 : (id == IDC_TAB_THEME)  ? 2
                                 : (id == IDC_TAB_GROUP)  ? 3
                                                          : 4);
             } else if (id == IDC_CMB_WAKE && HIWORD(wp) == BN_CLICKED) {
                // 下拉弹出黑暗菜单（复用 StyleDarkMenu 同一套自绘/染色）
                HMENU m = CreatePopupMenu();
                AppendMenuW(m, MF_STRING | (g.centerWake ? MF_CHECKED : 0), 4101,
                            L"屏幕居中");
                AppendMenuW(m, MF_STRING | (!g.centerWake ? MF_CHECKED : 0), 4102,
                            L"跟随鼠标");
                StyleDarkMenu(m);
                RECT r;
                GetWindowRect(GetDlgItem(h, IDC_CMB_WAKE), &r);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd == 4101) g.centerWake = true;
                else if (cmd == 4102) g.centerWake = false;
                InvalidateRect(GetDlgItem(h, IDC_CMB_WAKE), nullptr, TRUE);
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
                    kThemes[i].blurStrength = sTuneSaved[i].blur;
                    kThemes[i].radiusWindow = sTuneSaved[i].radius;
                    kThemes[i].radiusCard = (int)(sTuneSaved[i].radius * 0.8);
                    kThemes[i].radiusButton = (int)(sTuneSaved[i].radius * 0.6);
                    kThemes[i].radiusInput = (int)(sTuneSaved[i].radius * 0.6);
                }
                g.themeIdx = g.themeSaved;
                g.theme = &kThemes[g.themeIdx];
                if (g.beautify != g.beautifySaved) { g.beautify = g.beautifySaved; ApplyBeautify(); }
                if (g.glass != g.glassSaved) g.glass = g.glassSaved;
                ApplyTheme();  // 复原主题 + 微调（圆角/材质浓度）
                g.startupWanted = g.startupSaved;
                g.hotkeyMode = g.hotkeyModeSaved;
                if (IsWindowVisible(g.hwnd)) RepaintNow();
                // 搜索权重页：回退未保存的开关与性能参数
                g.weightEnabled = g.weightEnabledSaved;
                g.weightFlush = g.weightFlushSaved;
                g.weightMaxEntries = g.weightMaxSaved;
                g.fdjEnabled = g.fdjEnabledSaved;    // 文件对话框跳转：取消即回退
                DestroyWindow(h);
            } else if (id == IDC_BTN_RESET) {
                SetWindowTextW(GetDlgItem(h, IDC_EDT_RULES), DefaultRulesText().c_str());
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
    g.glassSaved = g.glass;
    {   // 快照每个主题的微调值：取消时用它回退滑块的实时预览改动
        int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
        sTuneSaved.assign(n, ThemeTune());
        for (int i = 0; i < n; ++i) {
            sTuneSaved[i].alpha = kThemes[i].alpha;
            sTuneSaved[i].blur = kThemes[i].blurStrength;
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
    // 使用系统原生标题栏，并让其走 DWM 亚克力材质（ApplyGlassTo 已对设置窗设置
    // DWMWA_SYSTEMBACKDROP_TYPE=Acrylic + WCA_ACCENT 亚克力模糊）：标题栏与正文共用同一
    // 材质、同色，告别「系统纯色平板」，且天然带亚克力模糊。
    // 关键：窗口不再用 WS_EX_LAYERED + LWA_ALPHA —— 分层+Alpha 会让 DWM 材质在标题栏失效。
    g.hSettings = CreateWindowExW(0, L"FlowtarySettings", L"Flowtary 设置",
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
                else if (cmd == IDM_EXIT) DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // Alt+数字 由 WM_SYSKEYDOWN（Alt 组合键消息）统一处理，详见下方 case。
            switch (wParam) {
                case VK_RETURN:
                    ExecuteSelected();
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
                    if (g.caret > 0) g.caret--;
                    AfterEdit();
                    return 0;
                case VK_RIGHT:
                    if (g.caret < g.text.size()) g.caret++;
                    AfterEdit();
                    return 0;
                case VK_HOME:
                    g.caret = 0;
                    AfterEdit();
                    return 0;
                case VK_END:
                    g.caret = g.text.size();
                    AfterEdit();
                    return 0;
                case VK_BACK:
                    if (g.caret > 0) {
                        g.text.erase(g.caret - 1, 1);
                        g.caret--;
                        AfterEdit();
                    }
                    return 0;
                case VK_DELETE:
                    if (g.caret < g.text.size()) {
                        g.text.erase(g.caret, 1);
                        AfterEdit();
                    }
                    return 0;
                case 'V':
                    if (ctrl) Paste();
                    return 0;
                case 'C':
                    if (ctrl) CopyAll();
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
            int y = GET_Y_LPARAM(lParam);
            int inputH = S(kBaseInputH), rowH = S(kBaseRowH);
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
                SetFocus(hwnd);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g.pressRow != -1) {
                g.pressRow = -1;
                StartTween(&g.pressT, 0.0, 120);
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
    LoadClickWeights();  // 点击权重数据（%APPDATA%\Flowtary\weights.dat）
    EnableDarkMenus();  // 按主题深浅强制弹出菜单（托盘/右键） 绘制
    g.msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 注册窗口类
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 图标用系统字体现场绘制（无 .ico 资源），托盘与两个窗口共用同一枚
    if (!g.hAppIcon) g.hAppIcon = MakeFontIcon(GetSystemMetrics(SM_CXICON));
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

    // 热键依次尝试：Alt+Space -> Alt+Q -> Ctrl+Alt+Space（避免与其他启动器冲突导致完全不可用）
    static const struct { UINT mod, vk; const WCHAR* name; } kHotkeys[] = {
        {MOD_ALT | MOD_NOREPEAT, VK_SPACE, L"Alt+Space"},
        {MOD_ALT | MOD_NOREPEAT, 'Q', L"Alt+Q"},
        {MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE, L"Ctrl+Alt+Space"},
    };
    bool hotkeyOk = false;
    for (int i = 0; i < 3; ++i) {
        if (RegisterHotKey(g.hwnd, 1, kHotkeys[i].mod, kHotkeys[i].vk)) {
            hotkeyOk = true;
            g.hotkeyName = kHotkeys[i].name;
            break;
        }
    }
    if (!hotkeyOk) {
        MessageBoxW(nullptr, L"Alt+Space / Alt+Q / Ctrl+Alt+Space 热键均注册失败，可能被其他程序占用。",
                    L"Flowtary", MB_ICONWARNING);
    }
    // 辅助热键：Ctrl+Alt+G —— 文件对话框激活时，把前台资源管理器当前目录同步到对话框
    RegisterHotKey(g.hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G');
    TrayAdd();  // 优先让托盘图标就位（气泡提示使用最终选定的热键名）

    // 程序扫描较慢：放到工作线程，托盘图标已先就位且可响应（右键显示「正在启动中」）
    g.startingUp = true;
    _beginthreadex(nullptr, 0, ScanProgramsThread, nullptr, 0, nullptr);
    g.everythingExe = FindEverythingExe();
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

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
    UnregisterHotKey(g.hwnd, 1);
    UnregisterHotKey(g.hwnd, 2);
    CoUninitialize();
    return 0;
}
