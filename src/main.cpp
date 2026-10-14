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
#include "themes.h"  // struct Theme / kThemes / ThemeTune（与设置 sidecar 共用）
#include "settings_ipc.h"  // sidecar ↔ 主进程消息协议
#include "settings_model.h"  // 设置项的唯一真源：注册表值名、编辑器语法、默认文本
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
constexpr DWORD kMaxResults     = 10;   // 展示上限：过滤后最多保留的结果行数
constexpr DWORD kMaxFetch       = 50;   // 单次向 Everything 索取上限：过滤噪音后仍能凑满展示上限

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

// ---------------- 设置数据模型（与 flowtary-settings.exe 共用） ----------------
// 文本工具、通配符匹配、四个编辑器的解析/序列化、注册表原语、weights/learned 文件路径
// 都在 settings_model.cpp 里实现一份；下面把这些名字引回本文件的全局作用域，
// 使既有的数百处调用点保持不变。
using ft::TrimW;
using ft::ToLowerW;
using ft::EndsWithI;
using ft::MatchWildcardI;
using ft::NormalizePathForExclude;
using ft::NormalizePatternForExclude;
using ft::LoadRegText;
using ft::SaveRegText;
using ft::RegGetDword;
using ft::RegSetDword;
using ft::WebCmd;
using ft::CmdGroup;
using ft::ParseRules;
using ft::RulesToText;
using ft::ParseGroups;
using ft::GroupsToText;
using ft::ParseExcludePaths;
using ft::ExcludePathsToText;
using ft::DefaultExcludePaths;
using ft::DefaultExcludePathsText;
using ft::HasExcludePattern;
using ft::IsLegacyDotDefault;
using ft::DefaultRulesText;
using ft::RulesTextEqual;
using ft::kWebRulesVer;
using ft::kExcludeDefaultsVer;
using ft::kEntrySep;
using ft::StartupEnabled;
using ft::SetStartupPath;


static std::vector<ThemeTune> sTune;      // 当前生效值（滑块实时预览会改 kThemes，这里只作回退/读写中转）
static std::vector<ThemeTune> sTuneSaved;  // 打开设置窗口时的快照（取消时用它回退）
// 设置窗 sidecar 会话的运行态快照：预览期间未提交就靠它整体回退
static ft::Settings sLiveSaved;
static int sThemeIdxSaved = 0;
static bool sBeautifySaved = true;

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


// 自学习条目：f/d 搜索打开过的文件/文件夹，记为「搜索词 → 路径」的长期记忆
struct LearnVal {
    int count = 0;          // 打开次数（排序用，越大越前）
    long long last = 0;     // 最近一次打开（Unix 秒，过期清理用）
    std::wstring lnk;       // 镜像快捷方式文件名（learned 目录内，空=尚未创建）
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
    HINSTANCE inst = nullptr;
    float scale = 1.0f;          // 全局比例：max(DPI, 屏幕物理高度/1080)，不写死像素
    bool centerWake = true;    // 唤醒位置：true=屏幕居中，false=跟随鼠标
    bool dwmBorderOk = false;  // DWM 描边可用（否则 Paint 回退 GDI 描边）
    int hotkeyMode = 0;        // 结果项快捷键方案：0=Alt+数字, 1=Alt+字母, 2=关闭
    bool hotkeyWake = true;    // 唤起快捷键总开关（托盘菜单切换；关闭时不再注册 Alt+Space 等）
    int themeIdx = 0;          // 当前主题索引
    bool beautify = true;      // 界面美化：强制暗色菜单（设置窗风格由 sidecar 自己施加）
    // 点开头过滤（默认开，UI 在设置「排除路径」Tab）：搜索文件/文件夹时，
    // 路径中任一层名以 '.' 开头（目录 .git / .vscode，或文件 .gitignore）即过滤该行。
    bool skipDotFolders = true;
    bool fdjEnabled = true;    // 文件对话框跳转总开关（默认开；UI 在设置「常规」Tab，不再放托盘菜单）
    bool topEnabled = true;    // top 命令开关（默认开：空格+top 回车置顶/取消置顶当前窗口）
    bool cmdEnabled = true;    // cmd 命令开关（默认开：空格+cmd 执行 Shell 命令，原 "> 命令"）
    bool winEnabled = true;    // w 命令开关（默认开：空格+w 切换窗口，原 "< 关键词"）
    // ---- 点击加权排序（性能参数在设置「搜索权重」Tab） ----
    bool weightEnabled = true; // 点击权重记忆总开关（默认开）
    int weightFlush = 1;       // 写盘时机：0=每次点击立即写入, 1=延迟合并写入, 2=仅退出时写入
    int weightMaxEntries = 5000;  // 权重条目上限（内存 + 磁盘均按此裁剪）
    std::wstring evTermKey;    // 当前 Everything 查询对应的标准化有效搜索词（回复到达时用于按权重重排）
    // 本次刷新的排除词（-关键字 语法，已小写）：SearchPrograms / PrependLearnedRows 读取它过滤结果，
    // f/d 分支同时用它把排除词翻成 Everything 原生 !file:/!folder: 语法。Refresh() 每轮开头清空。
    std::vector<std::wstring> excludes;
    // 权重存储：有效搜索词（去首尾空格+小写） → 文件完整绝对路径 → 点击次数。
    // 不同搜索词之间互相独立；以完整路径区分同名文件。
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, int>> weights;
    size_t weightCount = 0;    // weights 中（词,路径）对总数
    bool weightsDirty = false; // 有未写盘的权重变更
    // ---- f/d 自学习（打开即记忆为快捷方式，下次同词搜索直接置顶；设置「自学习」Tab） ----
    bool learnEnabled = true;   // 自学习总开关（默认开）
    int learnScope = 1;         // 记录范围：0=仅 exe, 1=exe+文件夹, 2=所有文件+文件夹
    int learnMaxEntries = 500;  // 学习条目上限（超出淘汰打开次数最少的一条）
    int learnExpire = 0;        // 过期清理：0=永不过期（长期保留），否则为「N 天未使用即清理」
    // 学习存储：有效搜索词（f/d 前缀后的关键词，标准化小写） → 路径 → 条目
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, LearnVal>> learned;
    size_t learnCount = 0;     // learned 中（词,路径）对总数
    bool learnedDirty = false; // 有未写盘的学习变更
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
    std::wstring evFallbackQuery;  // 路径级兜底查询：主查询（按名字）零结果时自动追发一次；发出即清空
    bool evFallbackPending = false;  // 兜底查询已排期、等待 WM_APP_EV_FALLBACK 发送
    std::wstring evSentQuery;  // 上一次实际发出的查询串（回复到达时核对仍处于同一阶段）
    DWORD expectReply = 0;     // 期待的结果消息 dwData
    DWORD serial = 0;

    std::vector<Program> programs;
    // 文件/文件夹行图标缓存：路径（小写）→ 小图标。File/Folder 结果行（含自学习置顶项）
    // 与程序行一样显示图标；首次经 shell 抽取后缓存，退出时统一销毁。
    std::unordered_map<std::wstring, HICON> pathIcons;
    std::wstring everythingExe;
    std::wstring hotkeyName = L"Alt+Space";
    HHOOK hWakeHook = nullptr;     // 低级键盘钩子：让首选唤起组合优先于其他程序的 RegisterHotKey
    bool hotkeyRegistered = false; // 唤起热键是否已成功 RegisterHotKey（失败由重试定时器回收）
    int wakeIndex = -1;            // 当前注册的唤起组合下标（-1=未注册；0=首选 Alt+Space）

    NOTIFYICONDATAW nid{};     // 托盘图标
    HICON hTrayIcon = nullptr;
    HICON hAppIcon = nullptr;  // 窗口图标（与托盘同款，字体绘制，不依赖 .ico 资源）
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
    // 窗口枚举缓存（避免频繁 EnumWindows 卡顿）
    std::vector<WinInfo> windowCache;
    DWORD windowCacheTick = 0;

    // —— 截图命令（ScreenCapture.exe）——
    bool captureEnabled = true;        // ss 命令开关（默认开）
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
constexpr int WM_APP_EV_FALLBACK = WM_APP + 4;  // Everything 路径级兜底查询：延迟到消息循环外发（见下）
constexpr int WM_APP_SETTINGS_GONE = WM_APP + 5;  // 设置窗 sidecar 进程结束（线程池回调 PostMessage 进来）
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
static void Layout();
static void RepaintNow();
static void GenerateStars();
// 点击加权排序（定义在「点击权重」小节）
static std::wstring NormalizeSearchTerm(const std::wstring& s);
static int GetClickWeight(const std::wstring& term, const std::wstring& path);
static void RecordClickWeight(const Row& r);
static void SaveClickWeightsNow();
// f/d 自学习（定义在「自学习」小节）
static void RecordLearned(const Row& r);
static void PrependLearnedRows(const std::wstring& term);
static void SaveLearnedNow();
static void LoadLearned();
static void ClearLearned();
static std::wstring LearnedDirPath();
static void TrayBalloon(const std::wstring& title, const std::wstring& msg);
static HWND TopTargetWindow();  // top 命令的目标窗口（唤醒前的前台窗口）
static void ShowTopToast(HWND target, const std::wstring& text);  // 屏幕居中半透明 toast（打开提示/top 命令共用）

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
    if (t.stars) GenerateStars();
    if (g.hwnd && repaint) {
        Layout();
        RepaintNow();
    }
}

// 界面美化开关对主进程的唯一影响：弹出菜单深浅。
// 设置窗（flowtary-settings.exe）的暗色标题栏/圆角由 sidecar 自己施加。
static void ApplyBeautify() { EnableDarkMenus(); }

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
#pragma comment(lib, "comctl32.lib") // 通用控件（现代主题下的编辑框/滚动条）

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

// 重建排除模式的归一化镜像。凡是改写 g.excludePaths 的地方（加载 / 恢复默认 / 设置页保存）
// 都必须调一次，否则匹配会退化成拿用户原文和小写路径比较。
static void RebuildExcludeNorm() {
    g.excludePathsNorm.clear();
    g.excludePathsNorm.reserve(g.excludePaths.size());
    for (const auto& p : g.excludePaths) g.excludePathsNorm.push_back(NormalizePatternForExclude(p));
}

// 路径中是否含有「以 . 开头的目录层」（.git / .vscode / .cache 这类隐藏目录）。
// lastIsDir = true 时最后一层也按目录算（搜索文件夹：.git 这类目录自身也要跳过）；
// false 时忽略最后一层（搜索文件：`C:\proj\.gitignore` 在本层就是个文件，不算命中）。
// 盘符（C:）与 UNC（\\server\share）前缀不参与判断；相对路径的非绝对成分照常按层处理。
static bool PathHasDotFolder(const std::wstring& path, bool lastIsDir) {
    if (path.empty()) return false;
    std::wstring np = NormalizePathForExclude(path);  // 统一 '\\'，大小写对 '.' 判断无影响
    size_t i = 0;
    if (np.size() >= 2 && np[1] == L':') i = 2;       // 跳过盘符
    while (i < np.size() && np[i] == L'\\') ++i;      // 跳过盘符后的分隔符 / UNC 前置
    while (i < np.size()) {
        size_t j = np.find(L'\\', i);
        bool last = (j == std::wstring::npos);
        if (last && !lastIsDir) break;                // 最后一层是文件：不参与判断
        if (np[i] == L'.') return true;
        if (last) break;
        i = j + 1;
        while (i < np.size() && np[i] == L'\\') ++i;  // 连续分隔符折叠
    }
    return false;
}

// 路径是否被排除：点开头文件夹开关（全局）+ g.excludePaths 任一模式命中
// （模式不含通配符时按前缀匹配）
// 匹配一律大小写不敏感：np 已小写，模式取 g.excludePathsNorm 里对应的归一化副本
// （曾经直接拿用户原文比较，导致 `C:\$RECYCLE.BIN` 这类含大写的默认项永远不命中）。
static bool IsPathExcluded(const std::wstring& path, bool lastIsDir = false) {
    if (path.empty()) return false;
    if (g.skipDotFolders && PathHasDotFolder(path, lastIsDir)) return true;
    if (g.excludePaths.empty()) return false;
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

// ---------------- 排除语法：-关键字 从结果中去掉含该关键字的项 ----------------
// 以 '-' 开头且长度>1 的词视为排除词，其余词（含引号短语）按原顺序拼回正向查询。
// 正向查询保留原始大小写（仅折叠空白），调用方再各自 NormalizeSearchTerm/ToLowerW，
// 保证权重 key、自学习 key 与匹配文本用的是同一份正向词。
struct ParsedQuery {
    std::wstring positive;               // 去掉排除词后的正向查询（空格分隔，已 trim）
    std::vector<std::wstring> excludes;  // 排除词（已小写，不含前导 '-'）
};

static ParsedQuery ParseExcludes(const std::wstring& q) {
    ParsedQuery r;
    size_t i = 0, n = q.size();
    while (i < n) {
        while (i < n && iswspace(q[i])) ++i;
        if (i >= n) break;
        size_t j;
        if (q[i] == L'"') {
            j = q.find(L'"', i + 1);
            j = (j == std::wstring::npos) ? n : j + 1;
        } else {
            j = i;
            while (j < n && !iswspace(q[j])) ++j;
        }
        std::wstring w = q.substr(i, j - i);
        if (w.size() > 1 && w[0] == L'-')
            r.excludes.push_back(ToLowerW(w.substr(1)));
        else {
            if (!r.positive.empty()) r.positive += L' ';
            r.positive += w;
        }
        i = j;
    }
    return r;
}

// textLower 已小写；命中任一排除词返回 true（子串包含，大小写不敏感）
static bool HasExcludeTerm(const std::wstring& textLower) {
    for (const auto& e : g.excludes)
        if (!e.empty() && textLower.find(e) != std::wstring::npos) return true;
    return false;
}

// 自适应推荐：把「已记录搜索词以当前查询为前缀」的历史权重按条目路径聚合。
// 用户以前敲完整词（如 bilibili）点击过的条目，现在只敲前缀（b / bi）也能被顶到最前。
// 只取严格比查询长的词：与查询等长的词由 GetClickWeight 精确命中，避免重复计权。
// 权重按真实点击次数原样累加（与精确词同量纲）：点得多的自然排前面。
static void CollectPrefixWeights(const std::wstring& ql, std::unordered_map<std::wstring, int>& out) {
    out.clear();
    if (!g.weightEnabled || ql.empty()) return;
    for (auto& kv : g.weights) {
        const std::wstring& term = kv.first;
        if (term.size() <= ql.size()) continue;
        if (wcsncmp(term.c_str(), ql.c_str(), ql.size()) != 0) continue;
        for (auto& pv : kv.second) out[pv.first] += pv.second;
    }
}

// query 由 Refresh() 传入「已剥离排除词的正向查询」；排除词在 g.excludes（本轮解析一次，共用）。
static void SearchPrograms(const std::wstring& query) {
    std::wstring ql = NormalizeSearchTerm(query);  // 与权重 key 同款标准化（去首尾空格+小写）
    std::unordered_map<std::wstring, int> hist;    // 前缀历史权重：条目路径 → Σ
    CollectPrefixWeights(ql, hist);
    const bool hasEx = !g.excludes.empty();
    struct Cand { Program* p; int score; int w; };
    std::vector<Cand> cands;
    // 单字符查询只保留精确(4)/前缀(3)：带上「包含/子序列」时命中面过宽
    // （实测 d 命中全部程序的 32%），刚敲第一个字母就撑满 10 行噪音结果。
    // 有历史权重的条目例外：是用户此前真实点击过的，属于自适应推荐，不算噪音。
    const bool prefixOnly = (ql.size() < 2);
    for (auto& p : g.programs) {
        int s = MatchScoreWithPinyin(ToLowerW(p.name), p.pinyin, ql);
        int hw = 0;
        auto hit = hist.find(p.path);
        if (hit != hist.end()) hw = hit->second;
        if (!(s > 0 && !(prefixOnly && s < 3)) && hw <= 0) continue;
        if (hasEx) {  // -关键字：名称/路径/拼音任一含排除词即丢弃
            std::wstring hay = ToLowerW(p.name) + L' ' + ToLowerW(p.path) + L' ' + p.pinyin;
            if (HasExcludeTerm(hay)) continue;
        }
        int w = g.weightEnabled ? GetClickWeight(ql, p.path) + hw : 0;
        cands.push_back({&p, s, w});
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

// 取任意文件/文件夹的小图标（File/Folder 结果行用）：按路径缓存，首次经 shell 抽取。
// 路径暂不可达（网络盘/刚移动）时退回 SHGFI_USEFILEATTRIBUTES，按扩展名/目录属性给关联图标。
static HICON GetPathIcon(const std::wstring& path) {
    if (path.empty()) return nullptr;
    std::wstring key = ToLowerW(path);
    auto it = g.pathIcons.find(key);
    if (it != g.pathIcons.end()) return it->second;
    // 缓存容量上限（很宽）：超出先整批销毁重建，防止长期运行无限增长
    if (g.pathIcons.size() >= 1024) {
        for (auto& kv : g.pathIcons)
            if (kv.second) DestroyIcon(kv.second);
        g.pathIcons.clear();
    }
    SHFILEINFOW sfi{};
    HICON ic = nullptr;
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON))
        ic = sfi.hIcon;
    if (!ic) {
        DWORD attr = GetFileAttributesW(path.c_str());
        attr = (attr == INVALID_FILE_ATTRIBUTES) ? 0 : (attr & FILE_ATTRIBUTE_DIRECTORY);
        if (SHGetFileInfoW(path.c_str(), attr, &sfi, sizeof(sfi),
                           SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES))
            ic = sfi.hIcon;
    }
    if (ic) g.pathIcons.emplace(std::move(key), ic);
    return ic;
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
    // Everything 未连接时自学习结果照常可用：置顶插入
    PrependLearnedRows(g.evTermKey);
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
    q->max_results = ev::kMaxFetch;
    memcpy(q->search_string, g.evQuery.c_str(), (len + 1) * sizeof(WCHAR));
    g.evSentQuery = g.evQuery;

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
    DWORD n = (std::min)(list->numitems, ev::kMaxFetch);
    size_t headerBytes = sizeof(ev::List) - sizeof(ev::Item) + (size_t)n * sizeof(ev::Item);
    if (headerBytes > cds->cbData) n = 0;
    int kept = 0;  // 已保留的展示行数（过滤后凑满 kMaxResults 即停）
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
        // 排除命中：直接丢弃。点开头检测覆盖整条路径的每一层——
        // 目录层（.git/.vscode）与 . 开头的文件名（.gitignore/.env）都过滤。
        if (IsPathExcluded(r.action, true)) continue;
        g.items.push_back(std::move(r));
        ++kept;
    }
    // 主查询（关键词匹配名字）零结果：排期一次「路径级」兜底查询——
    // 关键词改匹配整条路径（含所在目录），如 d aw-qtui software 命中 C:\software\aw-qtui。
    // 注意：本函数运行在 WM_COPYDATA 回调里，Everything 的 IPC 线程可能仍阻塞在
    // 发给我们这条消息的 SendMessage 上；此处绝不能再同步 SendMessageTimeout 回去
    // （重入会让 Everything 超时不应答，被误判成「IPC 未连接」）——只置标志 + PostMessage，
    // 由消息循环里的 WM_APP_EV_FALLBACK 真正把查询发出去。
    if (g.items.empty() && !g.evFallbackQuery.empty() && !g.startingUp &&
        g.evSentQuery == g.evQuery) {
        g.evQuery = g.evFallbackQuery;
        g.evFallbackQuery.clear();
        g.evFallbackPending = true;
        PostMessageW(g.hwnd, WM_APP_EV_FALLBACK, 0, 0);
        AddHint(L"正在搜索…");
        g.sel = 0;
        LayoutAndRepaint();
        return;
    }
    // 点击加权重排：同一有效搜索词下点过的文件/文件夹排前面（权重相同保持 Everything 原序）；
    // 同样聚合前缀历史权重（自适应推荐：f bilibili 点过的文件，敲 f b / f bi 时也排前面）
    if (g.weightEnabled && !g.evTermKey.empty() && g.items.size() > 1) {
        const std::wstring& tk = g.evTermKey;
        std::unordered_map<std::wstring, int> hist;
        CollectPrefixWeights(tk, hist);
        auto rowW = [&](const Row& r) {
            int w = GetClickWeight(tk, r.action);
            auto it = hist.find(r.action);
            if (it != hist.end()) w += it->second;
            return w;
        };
        std::stable_sort(g.items.begin(), g.items.end(),
                         [&rowW](const Row& a, const Row& b) { return rowW(a) > rowW(b); });
    }
    // f/d 自学习：命中的学习项摘出原行后置顶（exe 优先）；有学习结果时不再显示「无结果」
    PrependLearnedRows(g.evTermKey);
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
// -关键字 → 翻成 Everything 原生排除语法 !file:word / !folder:word（索引侧过滤，非本地丢弃）。
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
        if (kw[i] == L'-' && j > i + 1) {  // 排除词：! + 修饰符 + 去掉前导 '-' 的词
            out += L'!';
            out += mod;
            out.append(kw, i + 1, j - i - 1);
        } else {
            out += mod;
            out.append(kw, i, j - i);
        }
        out += L' ';
        i = j;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

// 路径级兜底查询：typeFilter（folder: / file: 单独成词，作类型过滤）+ 每个关键词 path:word。
// Everything 里 path: 匹配整条路径文本（所在目录 + 名字），关键词因此可以分布在不同层级：
// d aw-qtui software -> "folder: path:aw-qtui path:software" 命中 C:\software\aw-qtui。
static std::wstring BuildPathFallbackQuery(const WCHAR* typeFilter, const std::wstring& kw) {
    std::wstring out = BuildModifierQuery(L"path:", kw);
    if (out.empty()) return out;
    out.insert(0, typeFilter);
    out.insert(std::wcslen(typeFilter), L" ");
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
    std::wstring q = ToLowerW(TrimW(query));  // query 为 Refresh() 剥离排除词后的正向词
    const bool hasEx = !g.excludes.empty();
    struct Cand { int score; size_t idx; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < g.windowCache.size(); ++i) {
        const WinInfo& w = g.windowCache[i];
        if (!g.winShowUwp && w.isUwp) continue;
        if (hasEx) {  // -关键字：标题/进程名/类名任一含排除词即丢弃
            std::wstring hay = ToLowerW(w.title) + L' ' + ToLowerW(w.processName) + L' ' + ToLowerW(w.className);
            if (HasExcludeTerm(hay)) continue;
        }
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
                    ParsedQuery pq = ParseExcludes(rest);  // -关键字：排除含该词的窗口
                    g.excludes = pq.excludes;
                    SearchWindows(pq.positive);
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
                ParsedQuery pq = ParseExcludes(body);
                g.excludes = pq.excludes;
                SearchPrograms(pq.positive);
            }
        } else if (tok == L"d" || tok == L"f") {
            g.mode = Mode::Everything;
            g.evFallbackPending = false;  // 新输入作废旧的兜底排期（已投递的 WM_APP 会因标志为 false 直接返回）
            if (rest.empty()) {
                g.evFallbackQuery.clear();
                AddHint(tok == L"f" ? L"输入关键词搜索文件" : L"输入关键词搜索文件夹");
            } else {
                bool isFile = tok == L"f";
                ParsedQuery pq = ParseExcludes(rest);  // -关键字：Everything 侧原生 !mod: 过滤，本地学习项按 g.excludes 过滤
                g.excludes = pq.excludes;
                // BuildModifierQuery/BuildPathFallbackQuery 接收含 -词 的原始 rest，自行翻译成 !file:/!folder:
                g.evQuery = BuildModifierQuery(isFile ? L"file:" : L"folder:", rest);
                // 主查询无结果时自动追发的路径级兜底（见 HandleEverythingReply）
                g.evFallbackQuery = BuildPathFallbackQuery(isFile ? L"file:" : L"folder:", rest);
                g.evTermKey = NormalizeSearchTerm(pq.positive);  // 有效搜索词（权重/学习 key，不含排除词）
                AddHint(L"正在搜索…");
                // f/d 自学习：以前打开过的条目先置顶（Everything 结果到达后再合并）
                PrependLearnedRows(g.evTermKey);
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
            ParsedQuery pq = ParseExcludes(t);      // -关键字：本地过滤程序名/路径/拼音
            g.excludes = pq.excludes;
            std::wstring term = NormalizeSearchTerm(pq.positive);
            SearchPrograms(pq.positive);
            // 自学习结果同样参与普通搜索（覆盖不在程序索引里的条目）
            PrependLearnedRows(term);
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

// 打开成功后的提示文案（空串=不提示）：程序/文件/文件夹给「已打开 xx」，
// 一键启动/关闭组给对应计数。窗口切换、top、命令行等没有「打开」语义的场景不提示。
static std::wstring OpenedToastText(const Row& r) {
    switch (r.kind) {
        case Row::Prog:
        case Row::File:
        case Row::Folder:
            return L"已打开 " + r.title;
        case Row::Group:
            if (!r.groupTargets.empty())
                return r.groupKill ? L"已关闭 " + std::to_wstring(r.groupTargets.size()) + L" 个进程"
                                   : L"已启动 " + std::to_wstring(r.groupTargets.size()) + L" 项";
            return L"";
        default:
            return L"";
    }
}

static void ExecuteSelected(ExecKind ek = ExecKind::Normal) {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    RecordClickWeight(g.items[g.sel]);  // 点击/回车/快捷键选中即记权重（内部过滤非本地条目与不保存的前缀）
    if (ExecuteRow(g.items[g.sel], ek)) {
        RecordLearned(g.items[g.sel]);  // f/d 打开成功 → 记为自学习条目（含 .lnk 快捷方式）
        // 打开成功的轻量提示：贴在启动器原位顶部，淡入淡出自动消失（不抢焦点、不阻塞）
        std::wstring toast = OpenedToastText(g.items[g.sel]);
        if (!toast.empty()) ShowTopToast(g.hwnd, toast);
        Hide();
    }
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
// 托盘菜单与主界面右键菜单共用，保证任何 Windows 版本下文字都可见。
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
            if (ExecuteRow(r)) {
                RecordLearned(r);  // 右键打开成功同样记自学习
                std::wstring toast = OpenedToastText(r);
                if (!toast.empty()) ShowTopToast(g.hwnd, toast);
            }
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

// 托盘悬浮文案：主标题 — 状态/热键 + 版本号（v<FT_VER_DOT>）。
// FT_VER_DOT 是 ASCII 窄字面量，逐字符转宽后拼接（szTip 上限 128 宽字符，绰绰有余）。
static std::wstring TrayTipText() {
    std::wstring tip = g.startingUp ? L"Flowtary — 正在启动中…"
                                    : (g.hotkeyWake ? (L"Flowtary — " + g.hotkeyName + L" 唤出")
                                                    : L"Flowtary — 左键点击唤出");
    std::wstring ver;
    for (const char* p = FT_VER_DOT; *p; ++p) ver += (wchar_t)(unsigned char)*p;
    return tip + L" v" + ver;
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
    std::wstring tip = TrayTipText();
    lstrcpynW(g.nid.szTip, tip.c_str(), ARRAYSIZE(g.nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g.nid);
}

static void TrayUpdateTip() {
    if (!g.hwnd) return;
    std::wstring tip = TrayTipText();
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

static bool GetStartupEnabled() { return ft::StartupEnabled(); }

static void SetStartup(bool on) { ft::SetStartupPath(GetExePath(), on); }

// 设置项的唯一读法在 ft::Settings::Load()（settings_model.cpp）；这里只做
// 「Settings → g 运行态」的搬运。三个入口共用它：启动时读注册表、sidecar 保存后
// 重放、sidecar 取消时回退快照 —— 保证运行态只有一种被写法。
static ft::Settings sSnap;
static void ApplySettingsToRuntime(const ft::Settings& s) {
    g.centerWake = s.centerWake != 0;
    g.hotkeyMode = s.hotkeyMode;
    g.hotkeyWake = s.hotkeyWake;
    g.themeIdx = s.themeIdx;
    g.theme = &kThemes[g.themeIdx];
    g.beautify = s.beautify;
    g.skipDotFolders = s.skipDotFolders;
    sTune = s.tune;
    s.ApplyTuneToThemes();
    g.weightEnabled = s.weightEnabled;
    g.weightFlush = s.weightFlush;
    g.weightMaxEntries = s.weightMaxEntries;
    g.learnEnabled = s.learnEnabled;
    g.learnScope = s.learnScope;
    g.learnMaxEntries = s.learnMaxEntries;
    g.learnExpire = s.learnExpire;
    g.topEnabled = s.topEnabled;
    g.cmdEnabled = s.cmdEnabled;
    g.winEnabled = s.winEnabled;
    g.captureEnabled = s.captureEnabled;
    g.shellType = s.shellType;
    g.shellCustomPath = s.shellCustomPath;
    g.shellCustomArgs = s.shellCustomArgs;
    g.shellDefaultCwd = s.shellDefaultCwd;
    g.shellShowWindow = s.shellShowWindow;
    g.winGroupProc = s.winGroupProc;
    g.winShowUwp = s.winShowUwp;
    g.winShowProc = s.winShowProc;
    g.winCacheSec = s.winCacheSec;
}
static void LoadSettings() {
    sSnap.Load();
    ApplySettingsToRuntime(sSnap);
}
// ---------------- 设置项的解析与持久化 ----------------
// 编辑器语法、内置默认文本、注册表值名/钳制范围全部收敛到 settings_model.h（namespace ft），
// 主程序与 flowtary-settings.exe 共用同一份实现。下面只是把结果灌进 g 的薄封装。
static void LoadWebRules() {
    auto web = ft::LoadWebRules();
    g.webCmds = std::move(web.cmds);
}

static void SaveWebRules(const std::wstring& editorText) {
    ft::SaveWebRules(editorText);
    g.webCmds = ft::ParseRules(editorText);
}

// 一键组：关键字 → 目标列表（启动组存文件路径，关闭组存进程名）
// 文本格式：每行「关键字 + 空白 + 目标;目标;…」，`#` 开头为注释，重复关键字取第一条。
static void LoadGroupRules() {
    g.groupsLaunch = ft::ParseGroups(ft::LoadRegText(L"GroupLaunch"));
    g.groupsKill = ft::ParseGroups(ft::LoadRegText(L"GroupKill"));
}

static void LoadExcludePaths() {
    g.excludePaths = ft::LoadExcludePaths();
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

// 自学习设置项的显示文字（下拉当前值与列表项共用）
static const WCHAR* LearnScopeText(int s) {
    return s == 0 ? L"仅 exe"
         : s == 1 ? L"exe 与文件夹"
                  : L"所有文件与文件夹";
}
static const WCHAR* LearnExpireText(int days) {
    return days == 0 ? L"永不过期（长期保留）"
         : days == 90 ? L"90 天未使用即清理"
         : days == 180 ? L"180 天未使用即清理"
                       : L"365 天未使用即清理";
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
        termOut = NormalizeSearchTerm(ParseExcludes(rest).positive);  // 与搜索 key 对齐：剔除 -关键字 排除词
        return !termOut.empty();
    }
    if (FindWebCmd(tok)) return false;  // gg 等网页前缀：本次点击不记录权重
    // 输入本身就是一条路径（路径直达条，含 %VAR% 写法）：整条路径当搜索词存进权重表没有意义
    if (t.find(L'\\') != std::wstring::npos || t.find(L'%') != std::wstring::npos) return false;
    termOut = NormalizeSearchTerm(ParseExcludes(t).positive);  // 剔除 -关键字 排除词，与搜索 key 对齐
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

// ---------------- f/d 自学习（打开即记忆，下次同词搜索直接置顶） ----------------
// 用户在 f / d 前缀搜索里成功打开一个文件/文件夹后，把「本次有效搜索词 → 完整路径」
// 记为长期记忆（learned.dat），并在 %APPDATA%\Flowtary\learned 镜像一个 .lnk 快捷方式。
// 之后敲同一词（或它的前缀）时，学习项无视 Everything 是否命中，直接置顶展示：
// 精确词组在前，exe 优先，再按打开次数与最近使用时间。
// learned.dat 是权威数据；.lnk 目录可人工查看/增删：启动时双向同步一次
// （被删掉的 .lnk 会遗忘对应条目；手动放进来的 .lnk 会以次数 1 被吸收为学习项）。

static long long FileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (long long)(u.QuadPart / 10000000ULL - 11644473600ULL);
}

static long long NowUnix() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return FileTimeToUnix(ft);
}

static std::wstring LearnedDirPath() {
    WCHAR buf[MAX_PATH]{};
    std::wstring dir;
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH))
        dir = std::wstring(buf) + L"\\Flowtary";
    else
        dir = DirOf(GetExePath());
    return dir + L"\\learned";
}

static std::wstring LearnedDataFilePath() {
    WCHAR buf[MAX_PATH]{};
    std::wstring dir;
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH))
        dir = std::wstring(buf) + L"\\Flowtary";
    else
        dir = DirOf(GetExePath());
    return dir + L"\\learned.dat";
}

// 搜索词 → 安全文件名：替换 Windows 非法字符，去尾部空格/点，截断长度
static std::wstring SanitizeTermForFile(const std::wstring& term) {
    std::wstring out;
    for (WCHAR ch : term) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) out += L'_';
        else out += ch;
    }
    if (out.size() > 100) {
        out.resize(100);
        WCHAR last = out.back();
        if (last >= 0xD800 && last <= 0xDBFF) out.pop_back();  // 别把代理对截半
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    if (out.empty()) out = L"learned";
    return out;
}

static bool CreateShortcutFile(const std::wstring& lnkPath, const std::wstring& target,
                               const std::wstring& desc) {
    bool ok = false;
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(kCLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   kIID_IShellLinkW, (void**)&psl))) {
        psl->SetPath(target.c_str());
        std::wstring dir = DirOf(target);
        if (!dir.empty()) psl->SetWorkingDirectory(dir.c_str());
        if (!desc.empty()) psl->SetDescription(desc.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(kIID_IPersistFile, (void**)&pf))) {
            ok = SUCCEEDED(pf->Save(lnkPath.c_str(), TRUE));
            pf->Release();
        }
        psl->Release();
    }
    return ok;
}

// 给条目分配未被占用的 .lnk 文件名（同词多目标时追加 " (2)" 序号）
static std::wstring AllocLnkName(const std::wstring& term) {
    std::vector<std::wstring> used;
    for (auto& kv : g.learned)
        for (auto& pv : kv.second)
            if (!pv.second.lnk.empty()) used.push_back(pv.second.lnk);
    std::wstring base = SanitizeTermForFile(term);
    for (int i = 0; i < 10000; ++i) {
        std::wstring name = i ? base + L" (" + std::to_wstring(i + 1) + L").lnk"
                              : base + L".lnk";
        bool dup = false;
        for (auto& u : used)
            if (_wcsicmp(u.c_str(), name.c_str()) == 0) { dup = true; break; }
        if (!dup) return name;
    }
    return L"";
}

static void DeleteEntryLnk(const LearnVal& v) {
    if (v.lnk.empty()) return;
    DeleteFileW((LearnedDirPath() + L"\\" + v.lnk).c_str());
}

// 确保条目有对应的 .lnk（缺失或指向不对时重建；失败不致命，下次启动同步会再试）
static void EnsureEntryLnk(const std::wstring& term, const std::wstring& path, LearnVal& v) {
    if (v.lnk.empty()) v.lnk = AllocLnkName(term);
    if (v.lnk.empty()) return;
    std::wstring full = LearnedDirPath() + L"\\" + v.lnk;
    if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES &&
        _wcsicmp(ResolveLnkTarget(full).c_str(), path.c_str()) == 0)
        return;  // 已存在且指向正确，不重写
    CreateDirectoryW(LearnedDirPath().c_str(), nullptr);
    CreateShortcutFile(full, path, L"Flowtary 自学习：" + term);
}

// 到达上限时淘汰「次数最少、其次最久未用」的一条（跳过即将写入的新条目），连带删 .lnk
static void PruneOneLearned(const std::wstring& keepTerm, const std::wstring& keepPath) {
    std::wstring minTerm, minPath;
    LearnVal* minV = nullptr;
    for (auto& kv : g.learned) {
        for (auto& pv : kv.second) {
            if (kv.first == keepTerm && pv.first == keepPath) continue;
            if (!minV || pv.second.count < minV->count ||
                (pv.second.count == minV->count && pv.second.last < minV->last)) {
                minV = &pv.second;
                minTerm = kv.first;
                minPath = pv.first;
            }
        }
    }
    if (!minV) return;
    DeleteEntryLnk(*minV);
    auto it = g.learned.find(minTerm);
    it->second.erase(minPath);
    if (it->second.empty()) g.learned.erase(it);
    if (g.learnCount > 0) --g.learnCount;
}

static void ScheduleLearnedSave() {
    // 写盘时机沿用「搜索权重」页的延迟合并策略
    if (g.weightFlush == 0) {
        SaveLearnedNow();
    } else if (g.weightFlush == 1) {
        SetTimer(g.hwnd, kTimerWeightSave, kWeightSaveDelayMs, nullptr);
    }
    // weightFlush == 2：仅退出时写入（主窗 WM_DESTROY 一并落盘）
}

// 打开成功后记录学习项。来源两类：
//   - f/d（Everything）模式下的本地文件/文件夹行 → 词 = f/d 前缀后的有效搜索词
//   - 普通程序搜索模式下的程序行（元宝这类不在开始菜单索引里的程序，就是从这学的）
//     → 词 = 整条输入剥离 -关键字 后的正向词；范围仍受 learnScope 控制
static void RecordLearned(const Row& r) {
    if (!g.learnEnabled) return;
    std::wstring term;
    if (g.mode == Mode::Everything && (r.kind == Row::File || r.kind == Row::Folder)) {
        term = g.evTermKey;  // f/d 前缀后的有效搜索词（已标准化）
    } else if (g.mode == Mode::Programs && r.kind == Row::Prog) {
        if (g.text.empty() || g.text[0] == L' ') return;  // 空格命令派生的程序搜索不学
        term = NormalizeSearchTerm(ParseExcludes(g.text).positive);  // 与搜索 key 对齐：剔除 -关键字 排除词
        if (term.find(L'\\') != std::wstring::npos ||
            term.find(L'/') != std::wstring::npos ||
            term.find(L'%') != std::wstring::npos) return;  // 输入本身是路径：学了没意义
    } else {
        return;
    }
    if (term.empty()) return;
    std::wstring path = r.action;
    if (path.empty()) return;
    // 不学习 learned 快捷方式目录里的条目本身（避免把记忆产物当成记忆对象）
    std::wstring ldir = LearnedDirPath() + L"\\";
    if (path.size() > ldir.size() &&
        _wcsnicmp(path.c_str(), ldir.c_str(), ldir.size()) == 0) return;
    if (IsPathExcluded(path, r.kind == Row::Folder)) return;

    bool isFolder = (r.kind == Row::Folder);
    bool isExe = !isFolder && EndsWithI(path, L".exe");
    if (g.learnScope == 0 && !isExe) return;
    if (g.learnScope == 1 && !isExe && !isFolder) return;

    auto& m = g.learned[term];
    auto it = m.find(path);
    long long now = NowUnix();
    if (it == m.end()) {
        if (g.learnCount >= (size_t)(std::max)(10, g.learnMaxEntries))
            PruneOneLearned(term, path);
        LearnVal v;
        v.count = 1;
        v.last = now;
        it = m.emplace(path, v).first;
        ++g.learnCount;
    } else {
        ++it->second.count;
        it->second.last = now;
    }
    EnsureEntryLnk(term, path, it->second);
    g.learnedDirty = true;
    ScheduleLearnedSave();
}

// 立即写盘：按排序键裁剪到上限（连带删除被挤出条目的 .lnk）后整体写出
static void SaveLearnedNow() {
    if (!g.learnedDirty) return;
    g.learnedDirty = false;

    struct E { const std::wstring* term; const std::wstring* path; LearnVal* v; };
    std::vector<E> all;
    all.reserve(g.learnCount);
    for (auto& kv : g.learned)
        for (auto& pv : kv.second) all.push_back({&kv.first, &pv.first, &pv.second});
    std::sort(all.begin(), all.end(), [](const E& a, const E& b) {
        if (a.v->count != b.v->count) return a.v->count > b.v->count;
        return a.v->last > b.v->last;
    });
    size_t cap = (size_t)(std::max)(10, g.learnMaxEntries);
    if (all.size() > cap) {
        for (size_t i = cap; i < all.size(); ++i) {
            DeleteEntryLnk(*all[i].v);
            auto t = g.learned.find(*all[i].term);
            if (t != g.learned.end()) {
                t->second.erase(*all[i].path);
                if (t->second.empty()) g.learned.erase(t);
            }
            if (g.learnCount > 0) --g.learnCount;
        }
        all.resize(cap);
    }

    std::wstring text;
    for (auto& e : all) {
        text += *e.term; text += kWeightSep;
        text += *e.path; text += kWeightSep;
        text += std::to_wstring(e.v->count); text += kWeightSep;
        text += std::to_wstring(e.v->last); text += kWeightSep;
        text += e.v->lnk;
        text += L"\r\n";
    }
    std::wstring file = LearnedDataFilePath();
    CreateDirectoryW(DirOf(file).c_str(), nullptr);
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { g.learnedDirty = true; return; }
    DWORD wr = 0;
    WriteFile(f, text.data(), (DWORD)(text.size() * sizeof(WCHAR)), &wr, nullptr);
    CloseHandle(f);
}

// 启动时加载 learned.dat，并与 .lnk 目录双向同步 + 按过期策略清理
static void LoadLearned() {
    g.learned.clear();
    g.learnCount = 0;
    g.learnedDirty = false;

    HANDLE f = CreateFileW(LearnedDataFilePath().c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(f, nullptr);
        if (sz != INVALID_FILE_SIZE && sz >= sizeof(WCHAR) && sz <= 64u * 1024 * 1024) {
            std::vector<WCHAR> buf(sz / sizeof(WCHAR) + 1, 0);
            DWORD rd = 0;
            BOOL ok = ReadFile(f, buf.data(), (sz / sizeof(WCHAR)) * sizeof(WCHAR), &rd,
                               nullptr);
            if (ok) {
                std::wstring text(buf.data(), rd / sizeof(WCHAR));
                size_t cap = (size_t)(std::max)(10, g.learnMaxEntries);
                size_t i = 0, n = text.size();
                while (i < n && g.learnCount < cap) {
                    size_t j = text.find_first_of(L"\r\n", i);
                    if (j == std::wstring::npos) j = n;
                    std::wstring line = text.substr(i, j - i);
                    i = j + 1;
                    size_t s1 = line.find(kWeightSep);
                    size_t s2 = (s1 == std::wstring::npos) ? std::wstring::npos
                                                           : line.find(kWeightSep, s1 + 1);
                    size_t s3 = (s2 == std::wstring::npos) ? std::wstring::npos
                                                           : line.find(kWeightSep, s2 + 1);
                    if (s1 == std::wstring::npos || s2 == std::wstring::npos ||
                        s3 == std::wstring::npos) continue;
                    std::wstring term = line.substr(0, s1);
                    std::wstring path = line.substr(s1 + 1, s2 - s1 - 1);
                    LearnVal v;
                    v.count = _wtoi(line.c_str() + s2 + 1);
                    v.last = _wtoi64(line.c_str() + s3 + 1);
                    v.lnk = line.substr(s3 + 1);
                    if (term.empty() || path.empty() || v.count <= 0) continue;
                    if (g.learned[term].emplace(path, v).second) ++g.learnCount;
                }
            }
        }
        CloseHandle(f);
    }

    // 与快捷方式目录双向同步（目录存在才检查：删除同步 + 手动添加吸收）
    std::wstring dir = LearnedDirPath();
    {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> present;  // 目录里现存的 .lnk 文件名
            std::vector<std::pair<std::wstring, FILETIME>> stamps;  // 对应写入时间（吸收时当 last）
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                present.push_back(fd.cFileName);
                stamps.push_back({fd.cFileName, fd.ftLastWriteTime});
            } while (FindNextFileW(h, &fd));
            FindClose(h);

            auto referenced = [&present](const std::wstring& name) {
                if (name.empty()) return true;  // 尚未建出 .lnk 的条目不参与删除同步
                for (auto& p : present)
                    if (_wcsicmp(p.c_str(), name.c_str()) == 0) return true;
                return false;
            };
            // 用户手动删了 .lnk：对应条目一并遗忘
            for (auto tk = g.learned.begin(); tk != g.learned.end();) {
                for (auto pk = tk->second.begin(); pk != tk->second.end();) {
                    if (!referenced(pk->second.lnk)) {
                        pk = tk->second.erase(pk);
                        if (g.learnCount > 0) --g.learnCount;
                        g.learnedDirty = true;
                    } else ++pk;
                }
                if (tk->second.empty()) tk = g.learned.erase(tk);
                else ++tk;
            }
            // 用户手动放进来的 .lnk：以次数 1 吸收为学习项（词 = 文件名去扩展名与序号）
            size_t cap = (size_t)(std::max)(10, g.learnMaxEntries);
            for (size_t pi = 0; pi < present.size() && g.learnCount < cap; ++pi) {
                const std::wstring& name = present[pi];
                bool known = false;
                for (auto& kv : g.learned)
                    for (auto& pv : kv.second)
                        if (!_wcsicmp(pv.second.lnk.c_str(), name.c_str())) { known = true; break; }
                if (known) continue;
                std::wstring target = ResolveLnkTarget(dir + L"\\" + name);
                if (target.empty() ||
                    GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                std::wstring stem = name.substr(0, name.size() - 4);  // 去 .lnk
                if (stem.size() > 4) {  // 去 " (N)" 序号尾巴
                    size_t rp = stem.find_last_of(L')');
                    if (rp != std::wstring::npos && rp + 1 == stem.size() && rp >= 3) {
                        size_t lp = stem.rfind(L'(', rp);
                        if (lp != std::wstring::npos && lp + 1 < rp) {
                            bool digits = true;
                            for (size_t k = lp + 1; k < rp; ++k)
                                if (stem[k] < L'0' || stem[k] > L'9') { digits = false; break; }
                            if (digits && lp > 0 && stem[lp - 1] == L' ')
                                stem.resize(lp - 1);
                        }
                    }
                }
                std::wstring term = NormalizeSearchTerm(stem);
                if (term.empty()) continue;
                LearnVal v;
                v.count = 1;
                v.last = FileTimeToUnix(stamps[pi].second);
                v.lnk = name;
                if (g.learned[term].emplace(target, v).second) {
                    ++g.learnCount;
                    g.learnedDirty = true;
                }
            }
        }
    }

    // 过期清理：N 天未使用即遗忘（0 = 永不过期，长期保留）
    if (g.learnExpire > 0) {
        long long cutoff = NowUnix() - (long long)g.learnExpire * 86400;
        for (auto tk = g.learned.begin(); tk != g.learned.end();) {
            for (auto pk = tk->second.begin(); pk != tk->second.end();) {
                if (pk->second.last < cutoff) {
                    DeleteEntryLnk(pk->second);
                    pk = tk->second.erase(pk);
                    if (g.learnCount > 0) --g.learnCount;
                    g.learnedDirty = true;
                } else ++pk;
            }
            if (tk->second.empty()) tk = g.learned.erase(tk);
            else ++tk;
        }
    }
    if (g.learnedDirty) SaveLearnedNow();
}

// 清空全部学习数据（设置页两步确认后调用）：删除目录里的 .lnk，立即写空文件
static void ClearLearned() {
    WIN32_FIND_DATAW fd{};
    std::wstring dir = LearnedDirPath();
    HANDLE h = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g.learned.clear();
    g.learnCount = 0;
    g.learnedDirty = true;
    SaveLearnedNow();
}

// 把命中的学习项插到结果最前面（exe 优先；与当前结果按路径去重后重新置顶）
static void PrependLearnedRows(const std::wstring& term) {
    if (!g.learnEnabled || term.empty()) return;
    struct Cand {
        std::wstring path;
        int count;
        long long last;
        bool exact;   // 学习词与查询词完全一致（优先于前缀命中）
        bool isExe;
        bool isDir;
    };
    std::vector<Cand> cs;
    size_t plen = term.size();
    for (auto& kv : g.learned) {
        bool exact = kv.first == term;
        if (!exact && (kv.first.size() <= plen ||
                       wcsncmp(kv.first.c_str(), term.c_str(), plen) != 0))
            continue;
        for (auto& pv : kv.second) {
            if (!g.excludes.empty() && HasExcludeTerm(ToLowerW(pv.first))) continue;  // -关键字：学习项路径含排除词也不置顶
            DWORD attr = GetFileAttributesW(pv.first.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) continue;  // 目标暂不在：本次不展示
            bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            cs.push_back({pv.first, pv.second.count, pv.second.last, exact,
                          !isDir && EndsWithI(pv.first, L".exe"), isDir});
        }
    }
    if (cs.empty()) return;
    std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) {
        if (a.exact != b.exact) return a.exact;
        if (a.isExe != b.isExe) return a.isExe;   // exe 优先
        if (a.count != b.count) return a.count > b.count;
        return a.last > b.last;
    });
    size_t n = (std::min)(cs.size(), (size_t)10);
    for (size_t i = 0; i < n; ++i) {
        const Cand& c = cs[i];
        // 同一目标已在结果里（Everything 命中或权重重排）：摘掉原行，改由学习位置顶
        for (size_t k = 0; k < g.items.size();) {
            if (g.items[k].kind != Row::Hint && _wcsicmp(g.items[k].action.c_str(),
                                                         c.path.c_str()) == 0)
                g.items.erase(g.items.begin() + k);
            else ++k;
        }
        Row r;
        r.kind = c.isDir ? Row::Folder : Row::File;
        size_t s = c.path.find_last_of(L"\\/");
        r.title = (s == std::wstring::npos) ? c.path : c.path.substr(s + 1);
        r.sub = c.path;
        r.action = c.path;
        // 插在「一键组」行之后（显式关键字命中的组动作保持最高位）
        size_t pos = 0;
        while (pos < g.items.size() && g.items[pos].kind == Row::Group) ++pos;
        g.items.insert(g.items.begin() + pos, std::move(r));
    }
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
        } else if ((row.kind == Row::File || row.kind == Row::Folder ||
                    row.kind == Row::Reveal) &&
                   !row.action.empty()) {
            // 文件/文件夹行（自学习置顶项、Everything 结果、粘贴路径直达项均属此类）：
            // 按路径经 shell 取图标并缓存，与程序行保持同一直视对齐
            HICON ic = GetPathIcon(row.action);
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

// 截图工具解析：同目录优先，其次注册表自定义路径。启动时和 sidecar 下载完成时各跑一次
// （下载逻辑在 flowtary-settings.exe，主进程只能靠 MsgCaptureInstalled 重新解析）。
static void ResolveCaptureExe() {
    g.captureExe.clear();
    std::wstring exe = GetExePath();
    size_t pos = exe.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? L"" : exe.substr(0, pos + 1);
    std::wstring candidate = dir + L"ScreenCapture.exe";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g.captureExe = candidate;
        return;
    }
    DWORD type = 0, cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureExe", RRF_RT_REG_SZ,
                     &type, nullptr, &cb) == ERROR_SUCCESS &&
        cb > sizeof(WCHAR)) {
        std::vector<WCHAR> buf(cb / sizeof(WCHAR));
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"CaptureExe", RRF_RT_REG_SZ,
                         nullptr, buf.data(), &cb) == ERROR_SUCCESS) {
            std::wstring customPath(buf.data());
            if (GetFileAttributesW(customPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                g.captureExe = customPath;
        }
    }
}

// =====================================================================
// 设置窗 sidecar（flowtary-settings.exe，FLTK）
//
// 分工：sidecar 负责界面与「写注册表」；主进程负责运行态与派生缓存。
//   · 打开时主进程先快照运行态 → sidecar 期间的实时预览只改主进程内存
//   · 保存：sidecar 自己写注册表，再发 MsgCommit，主进程全量重读 + 重放
//   · 取消 / X 关闭 / 进程消失：未提交则用快照回退，语义等同旧的「取消」按钮
// =====================================================================
static HWND sSidecarHwnd = nullptr;      // sidecar 主窗（由 MsgHello 告知）
static HANDLE sSidecarProc = nullptr;    // sidecar 进程句柄
static HANDLE sSidecarWait = nullptr;    // 进程退出等待（线程池回调 → PostMessage）
static bool sSidecarCommitted = false;   // 本次会话是否已提交（提交后不再回退）

static void TakeSettingsSnapshot() {
    sLiveSaved = sSnap;  // sSnap = 最近一次「已落盘」的设置；预览不会碰它
    sThemeIdxSaved = g.themeIdx;
    sBeautifySaved = g.beautify;
    sTuneSaved.assign(kThemeCount, ThemeTune());
    for (int i = 0; i < kThemeCount; ++i) {
        sTuneSaved[i].alpha = kThemes[i].alpha;
        sTuneSaved[i].radius = kThemes[i].radiusWindow;
    }
}

static void RestoreThemeTune() {
    for (int i = 0; i < kThemeCount && i < (int)sTuneSaved.size(); ++i) {
        int r = sTuneSaved[i].radius;
        kThemes[i].alpha = (BYTE)sTuneSaved[i].alpha;
        kThemes[i].radiusWindow = r;
        kThemes[i].radiusCard = (int)(r * 0.8);
        kThemes[i].radiusButton = (int)(r * 0.6);
        kThemes[i].radiusInput = (int)(r * 0.6);
    }
}

static void RestoreSettingsSnapshot() {
    ApplySettingsToRuntime(sLiveSaved);
    g.themeIdx = sThemeIdxSaved;
    g.theme = &kThemes[g.themeIdx];
    RestoreThemeTune();
    g.beautify = sBeautifySaved;
    ApplyTheme();
}

// 实时预览：只写内存，一个字节都不落盘。
static void ApplySettingsPreview(const ipc::Preview& p) {
    g.centerWake = p.centerWake != FALSE;
    g.hotkeyMode = p.hotkeyMode;
    g.skipDotFolders = p.skipDotFolders != FALSE;
    g.fdjEnabled = p.fdjEnabled != FALSE;  // 仅为镜像；判定处现读注册表
    g.weightEnabled = p.weightEnabled != FALSE;
    g.weightFlush = p.weightFlush;
    g.weightMaxEntries = p.weightMaxEntries;
    g.learnEnabled = p.learnEnabled != FALSE;
    g.learnScope = p.learnScope;
    g.learnMaxEntries = p.learnMaxEntries;
    g.learnExpire = p.learnExpire;
    g.topEnabled = p.topEnabled != FALSE;
    g.cmdEnabled = p.cmdEnabled != FALSE;
    g.winEnabled = p.winEnabled != FALSE;
    g.captureEnabled = p.captureEnabled != FALSE;
    g.shellType = p.shellType;
    g.shellDefaultCwd = p.shellDefaultCwd;
    g.shellShowWindow = p.shellShowWindow != FALSE;
    g.winGroupProc = p.winGroupProc != FALSE;
    g.winShowUwp = p.winShowUwp != FALSE;
    g.winShowProc = p.winShowProc != FALSE;
    g.winCacheSec = p.winCacheSec;

    int n = (std::min)(p.tuneCount, kThemeCount);
    for (int i = 0; i < n; ++i) {
        int r = (std::max)(0, (std::min)(14, p.tune[i].radius));
        kThemes[i].alpha = (BYTE)(std::max)(0, (std::min)(255, p.tune[i].alpha));
        kThemes[i].radiusWindow = r;
        kThemes[i].radiusCard = (int)(r * 0.8);
        kThemes[i].radiusButton = (int)(r * 0.6);
        kThemes[i].radiusInput = (int)(r * 0.6);
    }
    if (p.themeIdx >= 0 && p.themeIdx < kThemeCount) {
        g.themeIdx = p.themeIdx;
        g.theme = &kThemes[g.themeIdx];
    }
    bool beautifyChanged = (g.beautify != (p.beautify != FALSE));
    g.beautify = p.beautify != FALSE;
    ApplyTheme();  // 重建画刷/字体/星点 + 暗色菜单 + 主窗 Layout/Repaint
    if (beautifyChanged) ApplyBeautify();
}

// 保存后重放：注册表已由 sidecar 写好，主进程按唯一读法全量重来一遍。
static void ReplaySettings() {
    LoadSettings();  // 含 tune → kThemes
    LoadWebRules();
    LoadGroupRules();
    LoadExcludePaths();  // 含 RebuildExcludeNorm()
    g.fdjEnabled = fdj_enabled();
    g.windowCache.clear();  // 刷新间隔可能改小了：立即失效，不等旧间隔
    g.windowCacheTick = 0;
    ApplyTheme();
    ApplyBeautify();
    if (IsWindowVisible(g.hwnd)) LayoutAndRepaint();
    TakeSettingsSnapshot();  // 已提交：把新状态当回退基线
}

static void SendToSidecar(DWORD id, const void* data, DWORD cb) {
    if (!sSidecarHwnd || !IsWindow(sSidecarHwnd)) return;
    COPYDATASTRUCT cds{};
    cds.dwData = id;
    cds.cbData = cb;
    cds.lpData = (PVOID)data;
    // 超时而非无限等待：sidecar 卡住时不能把启动器一起冻住
    SendMessageTimeoutW(sSidecarHwnd, WM_COPYDATA, (WPARAM)g.hwnd, (LPARAM)&cds,
                        SMTO_ABORTIFHUNG, 2000, nullptr);
}

static void SendStatsToSidecar() {
    ipc::Stats st{};
    st.weightCount = (ULONGLONG)g.weightCount;
    st.learnCount = (ULONGLONG)g.learnCount;
    SendToSidecar(ipc::MsgStats, &st, sizeof(st));
}

static VOID CALLBACK SidecarGone(PVOID, BOOLEAN) {
    if (g.hwnd) PostMessageW(g.hwnd, WM_APP_SETTINGS_GONE, 0, 0);  // 回主线程处理
}

// 收尾：sidecar 关闭 / 崩溃 / 主进程主动结束会话
static void EndSettingsSession() {
    if (sSidecarWait) {
        UnregisterWait(sSidecarWait);  // 不等待已在跑的回调；回调里的 PostMessage 无害
        sSidecarWait = nullptr;
    }
    if (sSidecarProc) {
        CloseHandle(sSidecarProc);
        sSidecarProc = nullptr;
    }
    sSidecarHwnd = nullptr;
}

static void OnSettingsSessionEnded() {
    bool committed = sSidecarCommitted;
    sSidecarCommitted = false;
    EndSettingsSession();
    if (!committed) {
        RestoreSettingsSnapshot();
        if (IsWindowVisible(g.hwnd)) LayoutAndRepaint();
    }
}

static void HandleSettingsIpc(const COPYDATASTRUCT* cds) {
    if (!cds || !cds->lpData || !ipc::IsOurMsg((DWORD)cds->dwData)) return;
    switch ((DWORD)cds->dwData) {
        case ipc::MsgHello: {
            if (cds->cbData < sizeof(ipc::Header)) return;
            sSidecarHwnd = ((const ipc::Header*)cds->lpData)->sender;
            sSidecarCommitted = false;
            SendStatsToSidecar();  // 权威计数：内存态可能比 weights.dat 新
            return;
        }
        case ipc::MsgPreview: {
            if (cds->cbData != sizeof(ipc::Preview)) return;
            ApplySettingsPreview(*(const ipc::Preview*)cds->lpData);
            return;
        }
        case ipc::MsgCommit:
            ReplaySettings();
            sSidecarCommitted = true;
            SendStatsToSidecar();
            return;
        case ipc::MsgCancel:
            RestoreSettingsSnapshot();
            if (IsWindowVisible(g.hwnd)) LayoutAndRepaint();
            sSidecarCommitted = false;  // 取消后仍可继续改，基线仍是打开时的状态
            return;
        case ipc::MsgClosed:
            OnSettingsSessionEnded();
            return;
        case ipc::MsgWipeWeights:
            ClearClickWeights();
            SendStatsToSidecar();
            return;
        case ipc::MsgWipeLearned:
            ClearLearned();
            SendStatsToSidecar();
            return;
        case ipc::MsgQueryStats:
            SendStatsToSidecar();
            return;
        case ipc::MsgCaptureInstalled:
            ResolveCaptureExe();
            return;
    }
}

static void OpenSettings() {
    if (sSidecarHwnd && IsWindow(sSidecarHwnd)) {
        ShowWindow(sSidecarHwnd, SW_SHOW);
        SetForegroundWindow(sSidecarHwnd);
        return;
    }
    std::wstring exe = GetExePath();
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : exe.substr(0, slash + 1);
    std::wstring sidecar = dir + L"flowtary-settings.exe";
    if (GetFileAttributesW(sidecar.c_str()) == INVALID_FILE_ATTRIBUTES) {
        TrayBalloon(L"Flowtary", L"未找到 flowtary-settings.exe（与主程序同目录）");
        return;
    }
    TakeSettingsSnapshot();
    std::wstring cmd = L"\"" + sidecar + L"\" " + ipc::kArgIpc + L" " +
                       std::to_wstring((ULONG_PTR)g.hwnd) + L" " + ipc::kArgMainExe + L" \"" +
                       exe + L"\"";
    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(sidecar.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &si, &pi)) {
        TrayBalloon(L"Flowtary", L"设置窗启动失败");
        return;
    }
    CloseHandle(pi.hThread);
    sSidecarProc = pi.hProcess;
    sSidecarCommitted = false;
    if (!RegisterWaitForSingleObject(&sSidecarWait, sSidecarProc, SidecarGone, nullptr, 1000,
                                     WT_EXECUTEONLYONCE)) {
        sSidecarWait = nullptr;
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
            SaveLearnedNow();       // 自学习数据同样落盘
            TrayRemove();
            // 主进程要走：通知设置窗 sidecar 自行关闭，再释放等待句柄
            SendToSidecar(ipc::MsgMainQuitting, nullptr, 0);
            if (sSidecarWait) {
                UnregisterWait(sSidecarWait);
                sSidecarWait = nullptr;
            }
            if (sSidecarProc) {
                CloseHandle(sSidecarProc);
                sSidecarProc = nullptr;
            }
            sSidecarHwnd = nullptr;
            if (g.hTrayIcon) {
                DestroyIcon(g.hTrayIcon);
                g.hTrayIcon = nullptr;
            }
            for (auto& kv : g.pathIcons) {
                if (kv.second) DestroyIcon(kv.second);
            }
            g.pathIcons.clear();
            for (int i = 0; i < kFontMax - kFontMin + 1; ++i) {
                if (g.fontPool[i]) { DeleteObject(g.fontPool[i]); g.fontPool[i] = nullptr; }
            }
            if (g.brDivider) { DeleteObject(g.brDivider); g.brDivider = nullptr; }
            if (g.brMenuBg) { DeleteObject(g.brMenuBg); g.brMenuBg = nullptr; }
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
                SaveLearnedNow();       // 自学习数据同步落盘
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
            else if (cds) HandleSettingsIpc(cds);  // 设置窗 sidecar
            return TRUE;
        }

        case WM_APP_SETTINGS_GONE:
            // sidecar 进程结束（正常关闭 / 被杀 / 崩溃）：未提交就回退实时预览
            OnSettingsSessionEnded();
            return 0;

        case WM_APP_EV_FALLBACK: {
            // 路径级兜底查询：必须在 WM_COPYDATA 回调之外（消息循环层）发出，
            // 否则与 Everything 的 IPC 线程互相阻塞 → 超时 → 假「IPC 未连接」。
            if (!g.evFallbackPending) return 0;
            g.evFallbackPending = false;
            if (g.mode == Mode::Everything) ExecuteEverythingQuery();
            return 0;
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

// ---------------- 屏幕居中轻量 toast（半透明无边界药丸） ----------------
// 打开成功 / top 置顶等场景共用：出现在目标窗口所在屏幕的正中间，
// 半透明药丸 + 柔和外投影、无描边无边界；文字保持完全不透明。
// 用 UpdateLayeredWindow 逐像素 alpha 合成（预乘 BGRA），
// 淡入 120ms → 停留 1.3s → 淡出 150ms，按 16ms 定时器重建位图；
// 非阻塞（不开嵌套消息循环）、WS_EX_NOACTIVATE 不抢焦点、点击穿透。
struct TopToastData {
    std::wstring text;
    COLORREF fill = 0;          // 药丸填充色（跟随主题）
    COLORREF textColor = 0;     // 文字色（跟随主题）
    int fillA = 165;            // 药丸不透明度 0..255
    int shadowA = 95;           // 投影（贴边）不透明度
    int W = 0, H = 0;           // 位图/窗口尺寸（含投影留白，逻辑像素）
    int pl = 0, pr = 0;         // 药丸左右边界（位图像素）
    int cy = 0, r = 0;          // 药丸垂直中心与半径
    int shadowR = 0;            // 投影外扩半径
    int baseX = 0, baseY = 0;   // 窗口位置（屏幕坐标）
    DWORD t0 = 0;               // 淡入起始时间
    int phase = 0;              // 0=淡入, 1=停留, 2=淡出
    DWORD phaseT0 = 0;          // 当前阶段起始时间
};

// 像素中心到水平线段的距离（药丸 = 线段 + 半径的并集）
static inline float ToastDistToSeg(float px, float py, float x0, float y0,
                                   float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.f;
    if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
    float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
    return sqrtf(ex * ex + ey * ey);
}

// 合成一帧：外投影 + 半透明药丸 + 不透明文字 → 32bpp 预乘 alpha 位图。
// m = 整体不透明度倍率（淡入淡出）。文字掩膜由 GDI 抗锯齿绘制，灰度即覆盖率。
static HBITMAP BuildToastFrame(const TopToastData& nd, HFONT font, float m) {
    if (nd.W <= 0 || nd.H <= 0) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = nd.W;
    bi.bmiHeader.biHeight = -nd.H;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return nullptr; }
    memset(bits, 0, (size_t)nd.W * nd.H * 4);

    // 文字掩膜：黑底白字（GDI 抗锯齿），灰度 = 文字覆盖率
    HBITMAP tmBmp = nullptr;
    HDC tdc = CreateCompatibleDC(nullptr);
    const BYTE* tb = nullptr;
    if (tdc) {
        void* tbits = nullptr;
        tmBmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &tbits, nullptr, 0);
        if (tmBmp && tbits) {
            memset(tbits, 0, (size_t)nd.W * nd.H * 4);
            SelectObject(tdc, tmBmp);
            HBRUSH blk = CreateSolidBrush(RGB(0, 0, 0));
            RECT full{0, 0, nd.W, nd.H};
            FillRect(tdc, &full, blk);
            DeleteObject(blk);
            SetBkMode(tdc, TRANSPARENT);
            SetTextColor(tdc, RGB(255, 255, 255));
            SelectObject(tdc, font);
            RECT tr{nd.pl + S(20), 0, nd.pr - S(20), nd.H};
            DrawTextW(tdc, nd.text.c_str(), (int)nd.text.size(), &tr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            tb = (const BYTE*)tbits;
        }
    }

    if (tb) {
        BYTE* p = (BYTE*)bits;
        float fr = (float)GetRValue(nd.fill), fg = (float)GetGValue(nd.fill), fb = (float)GetBValue(nd.fill);
        float cr = (float)GetRValue(nd.textColor), cg = (float)GetGValue(nd.textColor), cb = (float)GetBValue(nd.textColor);
        for (int y = 0; y < nd.H; ++y) {
            for (int x = 0; x < nd.W; ++x) {
                size_t i = (size_t)y * nd.W + x;
                float tm = tb[i * 4] / 255.f;              // 文字覆盖率
                float d = ToastDistToSeg(x + 0.5f, y + 0.5f, (float)nd.pl + nd.r,
                                         (float)nd.cy, (float)nd.pr - nd.r, (float)nd.cy);
                float cov = (float)nd.r + 0.5f - d;        // 药丸覆盖率（抗锯齿边缘）
                if (cov < 0.f) cov = 0.f; else if (cov > 1.f) cov = 1.f;
                // 药丸填充 + 外侧投影（二次衰减，只在药丸外）
                float a = cov * (float)nd.fillA;
                float sd = d - nd.r;
                if (sd > 0.f && sd < (float)nd.shadowR) {
                    float k = 1.f - sd / (float)nd.shadowR;
                    float sh = k * k * (float)nd.shadowA;
                    if (sh > a) a = sh;
                }
                // 颜色：文字处向文字色插值；药丸内用填充色；纯投影区黑色
                float rr, gg, bb;
                if (tm > 0.f) {
                    rr = fr + (cr - fr) * tm;
                    gg = fg + (cg - fg) * tm;
                    bb = fb + (cb - fb) * tm;
                } else if (cov > 0.f) {
                    rr = fr; gg = fg; bb = fb;
                } else {
                    rr = gg = bb = 0.f;
                }
                float ta = tm * 255.f;                    // 文字保持完全不透明
                if (ta > a) a = ta;
                a *= m;
                if (a < 1.f) continue;                    // 全透明像素不动（已清零）
                if (a > 255.f) a = 255.f;
                p[i * 4]     = (BYTE)(bb * a / 255.f);    // 预乘 BGRA
                p[i * 4 + 1] = (BYTE)(gg * a / 255.f);
                p[i * 4 + 2] = (BYTE)(rr * a / 255.f);
                p[i * 4 + 3] = (BYTE)a;
            }
        }
    }
    if (tdc) DeleteDC(tdc);
    if (tmBmp) DeleteObject(tmBmp);
    return bmp;
}

// 把当前帧推送到分层窗口（位置不变，仅内容/透明度变化）
static void ToastPushFrame(HWND h, const TopToastData& nd, HFONT font, float m) {
    HBITMAP bmp = BuildToastFrame(nd, font, m);
    if (!bmp) return;
    HDC wdc = GetDC(h);
    if (wdc) {
        HDC mem = CreateCompatibleDC(wdc);
        if (mem) {
            HGDIOBJ old = SelectObject(mem, bmp);
            POINT ptSrc{0, 0};
            SIZE sz{nd.W, nd.H};
            POINT ptDst{nd.baseX, nd.baseY};
            BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            UpdateLayeredWindow(h, wdc, &ptDst, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old);
        }
        DeleteDC(mem);
    }
    ReleaseDC(h, wdc);
    DeleteObject(bmp);
}

static LRESULT CALLBACK TopToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            const TopToastData* nd = (const TopToastData*)((CREATESTRUCTW*)lp)->lpCreateParams;
            TopToastData* self = nd ? new TopToastData(*nd) : new TopToastData();
            if (self) { self->t0 = GetTickCount(); self->phase = 0; }
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            SetTimer(h, 2, 16, nullptr);   // 淡入动画
            return 0;
        }
        case WM_NCHITTEST:                // 点击穿透：不挡下方窗口的操作
            return HTTRANSPARENT;
        case WM_TIMER:
            if (wp == 1) {  // 停留结束 → 进入淡出
                KillTimer(h, 1);
                TopToastData* nd = (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (nd) { nd->phase = 2; nd->phaseT0 = GetTickCount(); }
                SetTimer(h, 2, 16, nullptr);
                return 0;
            }
            if (wp == 2) {  // 淡入 / 淡出：重建位图推送到分层窗口
                TopToastData* nd = (TopToastData*)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (!nd) break;
                DWORD now = GetTickCount();
                float m = 1.f;
                if (nd->phase == 0) {
                    double p = (double)(now - nd->t0) / 120.0; if (p > 1) p = 1;
                    m = (float)(1 - pow(1 - p, 3));           // ease-out cubic
                    if (p >= 1) {
                        KillTimer(h, 2);                      // 停留期间不跑动画定时器（省资源）
                        nd->phase = 1;
                        SetTimer(h, 1, 1300, nullptr);        // 停留 1.3s
                    }
                } else if (nd->phase == 2) {
                    double p = (double)(now - nd->phaseT0) / 150.0; if (p > 1) p = 1;
                    m = (float)(1 - p);
                    if (p >= 1) { DestroyWindow(h); return 0; }
                } else {
                    return 0;                                 // 停留期定时器已停，防御性返回
                }
                HFONT font = g.fontPool[FontSlot((g.theme ? g.theme->fontInput : 16) + 1)];
                ToastPushFrame(h, *nd, font, m);
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

// 在目标窗口所在屏幕的正中央弹出半透明 toast（无目标时主屏居中），非阻塞、自动消失
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
    // 按文本实测宽度决定尺寸（设上限防过宽）
    HFONT font = g.fontPool[FontSlot(g.theme->fontInput + 1)];
    HDC dc = GetDC(nullptr);
    HGDIOBJ oldFont = (HFONT)SelectObject(dc, font);
    SIZE sz{};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);

    const int shadowR = S(10);
    const int pillH = S(40);
    int W = sz.cx + S(20) * 2 + shadowR * 2;
    int maxW = S(420);
    if (W > maxW) W = maxW;
    if (W < S(120)) W = S(120);
    int H = pillH + shadowR * 2;

    // 定位：目标窗口所在显示器的工件区正中央
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = target ? MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST)
                          : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoW(mon, &mi)) {
        mi.rcWork.left = 0; mi.rcWork.top = 0;
        mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;

    TopToastData nd{text};
    nd.W = W; nd.H = H;
    nd.shadowR = shadowR;
    nd.pl = shadowR; nd.pr = W - shadowR;
    nd.cy = H / 2;
    nd.r = pillH / 2;
    nd.baseX = x; nd.baseY = y;
    // 填充色跟随主题：暗色用输入框底色，浅色用卡片底色；文字用主题主文字色
    nd.fill = g.theme->dark ? g.theme->bgInput : g.theme->bgCard;
    nd.textColor = g.theme->text;
    nd.fillA = g.theme->dark ? 160 : 170;

    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             L"FlowtaryTopToast", L"", WS_POPUP,
                             x, y, W, H, nullptr, nullptr, g.inst, &nd);
    if (!h) return;
    ToastPushFrame(h, nd, font, 0.f);   // 首帧全透明，由定时器淡入
    ShowWindow(h, SW_SHOWNOACTIVATE);   // 不抢焦点、不进入任务栏/Alt-Tab
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
    // 通用控件初始化（自绘菜单/主题控件需要；InitCommonControlsEx 仅需一次）
    {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&icc);
    }
    LoadSettings();  // 先加载设置：确定主题/快捷键
    ResolveCaptureExe();
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
    LoadLearned();       // f/d 自学习数据（learned.dat + learned\*.lnk 双向同步）
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
