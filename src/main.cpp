// ============================================================
// Flowtary — Windows 极简全局搜索启动器
//   Alt+Space 唤醒 | d/f 本地索引搜索 | 网页前缀跳转 | 程序启动
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
#include "slint_bridge.h"  // Slint 渲染器 ↔ 主进程 共享内存桥
#include "settings_model.h"  // 设置项的唯一真源：注册表值名、编辑器语法、默认文本
#include "search_history.h"  // 搜索历史台账（诊断用纯文本，行格式与落盘都在这一份）
#include <atomic>
#include "search_policy.h"
#include "df_search.h"  // d/f 搜索后端：已有 Windows Search 索引查询
#include "indexer_client.h"
#include "app_ver.h"    // 「<应用名>-<版本号>」发布目录解析（结果排序把最新版抬到前面）
#include "app_catalog.h"
#include "app_icons.h"
#include "app_catalog_watch.h"
#include "workshop_catalog.h"
#include <imm.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <process.h>
#include <new>

#include <algorithm>
#include <condition_variable>
#include <map>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")

constexpr DWORD kMaxVisibleResults = 10;

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
using ft::SyncStartupPath;


static std::vector<ThemeTune> sTune;      // 当前生效值（滑块实时预览会改 kThemes，这里只作回退/读写中转）
static std::vector<ThemeTune> sTuneSaved;  // 打开设置窗口时的快照（取消时用它回退）
// 设置窗 sidecar 会话的运行态快照：预览期间未提交就靠它整体回退
static ft::Settings sLiveSaved;
static int sThemeIdxSaved = 0;

// ---------------- 数据结构 ----------------
struct Program {
    std::wstring name;    // 显示名称
    std::wstring path;    // .lnk 或 .exe 完整路径（执行用）
    std::wstring target;  // 快捷方式解析出的目标（工作目录用）
    bool startMenu = false;
};

struct Row {
    // Reveal：搜索框里直接粘贴了「文件」路径时的直达项 —— 回车不启动该文件，
    // 而是在资源管理器中打开其所在文件夹并选中它（action 为文件完整路径）。
    enum Kind { File, Folder, Web, Prog, Hint, Group, Shell, Window, Top, Capture, Reveal };
    Kind kind = Hint;
    std::wstring title;
    std::wstring sub;     // 路径 / URL 说明
    std::wstring action;  // 打开目标；Shell 时为命令行文本；Window 时为 HWND 十六进制串
    Program* prog = nullptr;
    bool workshopHit = false; // Flowtary-owned C:\\workshop\\*.exe priority metadata
    int usage = 0;            // 历史总使用次数（跨词点击权重 + 自学习打开次数），Layout 时统一回填
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

// 点击权重条目：从纯次数升级为「次数 + 最近使用时间」。
// last 只参与「最近使用」加分，旧的 3 列 weights.dat 读入后 last=0（视为时间未知，不加分），
// 保证老数据行为平滑退回纯次数排序。保留 int 隐式构造，既有 `weights[t][p] = 5` 写法照常工作。
struct WeightVal {
    int count = 0;
    long long last = 0;
    WeightVal() = default;
    WeightVal(int c, long long l = 0) : count(c), last(l) {}
    WeightVal& operator+=(int d) { count += d; return *this; }
};

enum class Mode { None, Files, Web, Programs, Shell, Window, Top, Capture };

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
    HINSTANCE inst = nullptr;
    float scale = 1.0f;          // 全局比例：max(DPI, 屏幕物理高度/1080)，不写死像素
    bool centerWake = true;    // 唤醒位置：true=屏幕居中，false=跟随鼠标
    int hotkeyMode = 0;        // 结果项快捷键方案：0=Alt+数字, 1=Alt+字母, 2=关闭
    bool hotkeyWake = true;    // 唤起快捷键总开关（托盘菜单切换；关闭时不再注册 Alt+Space 等）
    int themeIdx = 0;          // 当前主题索引
    // 点开头过滤（默认开，UI 在设置「排除路径」Tab）：搜索文件/文件夹时，
    // 路径中任一层名以 '.' 开头（目录 .git / .vscode，或文件 .gitignore）即过滤该行。
    bool skipDotFolders = true;
    int dfBackend = 0;          // d/f 后端：0=自动（Flowtary 优先）1=仅 Flowtary 2=仅 Windows Search
    bool fdjEnabled = true;    // 文件对话框跳转总开关（默认开；UI 在设置「常规」Tab，不再放托盘菜单）
    bool topEnabled = true;    // top 命令开关（默认开：空格+top 回车置顶/取消置顶当前窗口）
    bool cmdEnabled = true;    // cmd 命令开关（默认开：空格+cmd 执行 Shell 命令，原 "> 命令"）
    bool winEnabled = true;    // w 命令开关（默认开：空格+w 切换窗口，原 "< 关键词"）
    // ---- 点击加权排序（性能参数在设置「搜索权重」Tab） ----
    bool weightEnabled = true; // 点击权重记忆总开关（默认开）
    int weightFlush = 1;       // 写盘时机：0=每次点击立即写入, 1=延迟合并写入, 2=仅退出时写入
    int weightMaxEntries = 5000;  // 权重条目上限（内存 + 磁盘均按此裁剪）
    std::wstring evTermKey;    // 当前 d/f 查询对应的标准化有效搜索词
    // 本次刷新的排除词（-关键字 语法，已小写）：SearchPrograms / PrependLearnedRows 读取它过滤结果，
    // f/d 查询在 Flowtary / Windows Search 结果返回后统一应用排除词。
    std::vector<std::wstring> excludes;
    // 权重存储：有效搜索词（去首尾空格+小写） → 文件完整绝对路径 → 点击次数/最近时间。
    // 不同搜索词之间互相独立；以完整路径区分同名文件。升级后按应用身份聚合跨版本次数。
    std::unordered_map<std::wstring, std::unordered_map<std::wstring, WeightVal>> weights;
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
    // ---- 搜索历史台账（诊断：一轮查询写一条 Q，一次打开写一条 O；设置「搜索历史」页）----
    bool historyEnabled = true;    // 总开关（默认开）
    int historyMaxLines = 2000;    // 文件保留行数（超出只留尾部）
    std::wstring qText;            // 本轮查询的输入原文（Q/O 行都记这一份）
    std::wstring qLayer;           // 本轮层标签：D / F / P（程序搜索）
    ULONGLONG qT0 = 0;             // 本轮派发时刻
    int qFirstMs = -1;             // 首屏上屏延迟（-1=结果还没上屏）
    bool qLogged = false;          // 本轮 Q 行已成型（先攒着，编辑会话结束才落盘）
    std::wstring qPendingLine;     // 攒着还没写的 Q 行原文（空=没有）
    std::wstring qPendingText;     // 攒着那条的输入，用来判断「还在接着打字」
    std::wstring qPendingLayer;
    std::wstring qMissed;          // 本轮到最近一次上屏为止还没报齐的层（空=报齐）
    int qWsRows = 0;               // Windows Search 层行数
    int qSettledMs = -1;           // 本轮报齐时定格的总耗时（-1=还没报齐）；底栏非秒出时显示
    bool startingUp = true;    // 程序扫描尚未完成：托盘提示/右键菜单显示「正在启动中」
    Theme* theme = nullptr;

    HFONT fontPool[15] = {};   // 逻辑字号 10..24 预建字体池（11-24），供提示弹窗/轻提示使用

    std::vector<WebCmd> webCmds;  // 网页跳转规则（设置可编辑）
    std::vector<CmdGroup> groupsLaunch;  // 一键启动组：关键字 → 文件列表
    std::vector<CmdGroup> groupsKill;    // 一键关闭组：关键字 → 进程名列表

    // 排除路径（设置可编辑）：用户原文，每行一条（保留给设置页编辑框原样回显）。
    // 命中文件路径或程序 .lnk/.exe 完整路径时丢弃该项。
    // 仅在文件搜索与程序模式生效；网页/一键组不参与过滤。
    std::vector<std::wstring> excludePaths;
    // excludePaths 的归一化镜像（反斜杠统一 + 去尾部分隔符 + 全小写），与前者下标一一对应。
    // 匹配时只看这一份，避免用户填的大小写/尾部反斜杠写法导致静默失效。
    std::vector<std::wstring> excludePathsNorm;

    std::wstring text;

    Mode mode = Mode::None;
    std::vector<Row> items;
    int sel = 0;

    // Query existing indexes only; no owned filesystem index or traversal backend.
    df::Query dfQuery;
    std::atomic<DWORD> dfGen{0};
    bool dfWsDone = true;
    bool dfWsFailed = false;
    ft::search::Route searchRoute = ft::search::Route::Unavailable;
    std::vector<Row> dfBaseRows;


    std::vector<Program> programs;
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
    // —— Slint 渲染器（共享内存桥）——
    slintbridge::Bridge* bridge = nullptr;  // 共享内存映射（懒创建）
} g;

// —— 主窗版式（Raycast 风格，逻辑像素）——
// 视觉结构：窗口内一枚独立圆角搜索框 + 下方结果列表直接铺在玻璃上（无独立卡片），
// 每行大 squircle 图标 + 标题/副标题上下双行，选中行为通栏纯色胶囊。
// 关键等式：kBaseInputH(=pad+searchH) + kBaseCardGapTop = 列表顶边 rowsTop，
// 命中测试/Layout/动画都复用这个等式，所以改这里即可整体适配，无需逐个改调用点。
constexpr int kBaseW = 680;
constexpr int kBasePad = 14;        // 窗口四周留白：搜索框与列表的左右边界
constexpr int kSearchH = 52;        // 搜索框高度
constexpr int kBaseInputH = 66;     // = kBasePad + kSearchH，搜索框底边 y
constexpr int kBaseRowH = 54;       // 行高（容纳双行文字 + 大图标）
constexpr int kBaseCardGapTop = 14; // 搜索框底 → 列表顶
constexpr int kBaseCardPadBottom = 0;   // 新布局无结果卡片，列表底即内容底
constexpr int kBaseWinPadBottom = 14;   // 列表底 → 窗口底
constexpr int kBaseEmptyH = 200;
constexpr int kBaseFooterH = 22;
// 造型半径 / 尺寸
constexpr int kWinRadius = 16, kSearchRadius = 12, kSelRadius = 11;
constexpr int kIconSize = 38, kIconRadius = 11, kIconGapL = 13, kIconTextGap = 13;
constexpr int kBadgeW = 30, kBadgeH = 19, kBadgeR = 5, kBadgeGapR = 15;
constexpr int kSlowMs = 120;      // 超过这个毫秒数才报耗时；秒出的查询保持干净
constexpr int kFontMin = 11, kFontMax = 24;  // 字体池逻辑字号范围
constexpr UINT_PTR kTimerDebounce = 1;
constexpr UINT_PTR kTimerBlink = 2;        // 目录变更轮询（沿用旧光标闪烁定时器 ID）
constexpr UINT_PTR kTimerBalloon = 3;
constexpr UINT_PTR kTimerWeightSave = 4;   // 点击权重延迟合并写盘
constexpr UINT_PTR kTimerHotkeyRetry = 6;  // 唤起快捷键被占用时定时重试回收
constexpr UINT_PTR kTimerHistFlush = 8;    // 搜索历史：编辑静置后把攒着的 Q 行落盘
constexpr UINT_PTR kTimerLatency = 9;      // 耗时底栏：pending 期间实时跳毫秒（~30fps）
constexpr UINT kLatencyTickMs = 33;

// 纯空态：零输入、零待上屏组合串、零结果（有输入但无结果时不算，避免搜不到时误显引导页）
static inline bool IsWelcomeState() {
    return g.text.empty() && g.items.empty();
}
constexpr UINT kHistFlushMs = 4000;        // 静置 4s 视为这一轮打完了（程序搜索逐字同步出结果，不落盘会一行一个前缀）
constexpr int kDebounceMs = 120;
constexpr int kWeightSaveDelayMs = 3000;   // 延迟合并写盘的等待时间
constexpr int WM_APP_TRAY = WM_APP + 1;
constexpr int WM_APP_PROGRAMS_READY = WM_APP + 2;  // 工作线程扫描完成，回主线程接管结果
constexpr int WM_APP_QUIT = WM_APP + 3;  // 新版本接管：通知旧实例退出
constexpr int WM_APP_SETTINGS_GONE = WM_APP + 5;  // 设置窗 sidecar 进程结束（线程池回调 PostMessage 进来）
constexpr int WM_APP_DF_READY = WM_APP + 6;  // d/f 双后端结果就绪：lParam = DfReply*（堆上，主线程接管后 delete）
constexpr int WM_APP_APPS_CHANGED = WM_APP + 7;
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
constexpr int IDM_ADDALIAS = 2029;    // 结果右键菜单：添加别名（在自学习目录建同名快捷方式）

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
static std::wstring GetExePath();
static void EnableDarkMenus();
static const CmdGroup* FindGroup(const std::vector<CmdGroup>& gs, const std::wstring& key);
static int KillProcessesByName(const std::wstring& name);
static void Layout();
static void RepaintNow();
// 点击加权排序（定义在「点击权重」小节）
static std::wstring NormalizeSearchTerm(const std::wstring& s);
static int GetClickWeight(const std::wstring& term, const std::wstring& path);
static void RecordClickWeight(const Row& r);
static void SaveClickWeightsNow();
static long long NowUnix();  // 定义在自学习小节；评分函数用它算「距今多久」
// f/d 自学习（定义在「自学习」小节）
static void RecordLearned(const Row& r);
static void RerankByTier();  // 热度档统一重排（定义在自学习段，Refresh 先用到）
static void PrependLearnedRows(const std::wstring& term);
static bool ApplyAliasForRow(const std::wstring& target, const std::wstring& name);
static void SaveLearnedNow();
static void LoadLearned();
static void ClearLearned();
static std::wstring LearnedDirPath();
// 搜索历史台账（定义在「点击权重」小节末尾，落盘实现在 src/search_history.cpp）
static void ArmHistoryClock(const WCHAR* layer);
static std::wstring HistoryMissingLayers();
static void LogHistoryQuery();
static void SnapshotHistoryPending();
static void FlushHistoryQuery();
static void LogHistoryOpen(const Row& r);
static void NoteHistoryFirstPaint();
static void TrayBalloon(const std::wstring& title, const std::wstring& msg);
static HWND TopTargetWindow();  // top 命令的目标窗口（唤醒前的前台窗口）
static void ShowTopToast(const std::wstring& text);  // 屏幕居中轻提示（打开提示/top 命令共用）
static void ShowNotice(const std::wstring& title, const std::wstring& body,
                       const std::wstring& btn, int autoCloseMs = 0);
// Slint 渲染器（共享内存桥）：定义在窗口过程之前，本文件多处要调用
static void SlintBridgeEnsure();
static void SlintPushState();
static void SlintSendCmd(int cmd);
static void SlintPushRowMenu();
static void SlintPushAliasPrompt(const std::wstring& defaultName);
static void SlintHandleRequests();
static bool TryAltHotkeyDirect(WPARAM wParam);
static HWND SlintIpcWindow();
static HWND SlintUiWindow();
static std::wstring g_slintAliasTarget;  // Slint 别名覆盖层正在编辑的目标路径

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
static HFONT MakeFont(int logSize, int weight = FW_NORMAL) {
    return CreateFontW(-S(logSize), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
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

// 主题应用：字体指针与菜单深浅按预设重置（界面由 Slint 渲染器主题属性控制）
static void ApplyTheme(bool repaint = true) {
    if (!g.theme) return;
    const Theme& t = *g.theme;
    g.fInput = g.fontPool[FontSlot(t.fontInput)];
    g.fList = g.fontPool[FontSlot(t.fontList)];
    EnableDarkMenus();
    if (g.hwnd && repaint) {
        Layout();
        RepaintNow();
    }
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
                // Read the stored target only. Resolve may search disks/network shares
                // or update a broken shortcut; selecting an entry must not start that work.
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
struct ProgramScanState {
    std::atomic<bool> stop{false},running{false};
    std::mutex deliveryMutex;
    HWND window=nullptr;
};
static auto s_programScanState=std::make_shared<ProgramScanState>();
struct ProgramScanContext { std::shared_ptr<ProgramScanState> state; HWND destination; };
static ft::apps::Stats s_programCatalogStats;
static ft::apps::CatalogWatch s_programWatch;

struct ProgramBuildReply { std::vector<Program> programs; ft::apps::Stats stats; };
static void CopyAppCatalog(ft::apps::Result catalog,ProgramBuildReply& reply) {
    for(auto& entry:catalog.entries){
        Program p;
        p.name=std::move(entry.name);p.path=std::move(entry.path);p.target=std::move(entry.target);p.startMenu=entry.startMenu;
        reply.programs.push_back(std::move(p));
    }
    reply.stats=std::move(catalog.stats);
}
static void BuildProgramsInto(ProgramBuildReply& reply,const std::atomic<bool>* stop) {
    CopyAppCatalog(ft::apps::Collect({},stop),reply);
    if(stop && stop->load(std::memory_order_acquire))return;
    // C:\workshop 是 Flowtary 自研应用的发布目录，里面的 exe 是「程序」而非普通文件。
    // 复用 f-search 的同一个有界扫描器把这些 exe 也并进程序目录，默认模式搜名字即可
    // 启动，不用先敲 f 进文件搜索。同名按 exe 名分组只留最新正式版（四个 Flowtary
    // 版本目录不会刷屏），辅助 exe（indexer/settings…）各占一行、用全名可搜。
    df::WorkshopScan scan=df::ScanWorkshopExecutables(L"C:\\workshop",{},stop);
    if(scan.paths.empty())return;
    std::unordered_set<std::wstring> known;
    known.reserve(reply.programs.size()+scan.paths.size());
    for(const auto& p:reply.programs)known.insert(ToLowerW(p.path));
    for(auto& entry:df::WorkshopProgramEntries(scan.paths)){
        Program p;
        p.name=std::move(entry.name);
        p.path=std::move(entry.path);
        if(known.count(ToLowerW(p.path)))continue;
        p.startMenu=false;  // 直接 exe：CreateProcess 启动，不交 Shell 解析快捷方式
        known.insert(ToLowerW(p.path));
        reply.programs.push_back(std::move(p));
    }
}

// 目录扫描受预算截断时，权重表里点过多次的条目可能压根没被枚举到，
// 这时"常用"只影响排序、救不回一个不在表里的程序。用权重/自学习的
// 跨词累计次数把高频且仍存在的 exe/lnk 补回程序表，让搜索真正能兜底。
// 只在主线程调用（读 g.weights / g.learned，并写 reply.programs）。
static void RestoreFrequentPrograms(ProgramBuildReply& reply,bool catalogTruncated) {
    if(!catalogTruncated)return;
    std::vector<std::pair<std::wstring,int>> candidates;
    candidates.reserve(g.weightCount+g.learnCount);
    for(auto& kv:g.weights)
        for(auto& pv:kv.second)candidates.emplace_back(pv.first,pv.second.count);
    for(auto& kv:g.learned)
        for(auto& pv:kv.second)candidates.emplace_back(pv.first,pv.second.count);
    auto hot=ft::search::TopWeightedPaths(candidates,2,64);
    if(hot.empty())return;
    std::unordered_set<std::wstring> known;
    known.reserve(reply.programs.size());
    for(const auto& p:reply.programs)known.insert(ToLowerW(p.path));
    for(const auto& hit:hot){
        if(known.count(ToLowerW(hit.first)))continue;
        DWORD attr=GetFileAttributesW(hit.first.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_DIRECTORY))continue;
        const bool isLnk=EndsWithI(hit.first,L".lnk");
        if(!isLnk && !EndsWithI(hit.first,L".exe"))continue;
        size_t slash=hit.first.find_last_of(L"\\/");
        std::wstring name=(slash==std::wstring::npos)?hit.first:hit.first.substr(slash+1);
        if(isLnk && name.size()>4)name.resize(name.size()-4);
        if(name.empty())continue;
        Program p;
        p.name=std::move(name);
        p.path=hit.first;
        p.startMenu=isLnk;  // 与目录扫描一致：快捷方式交给 Shell 打开，工作目录留空
        known.insert(ToLowerW(p.path));
        reply.programs.push_back(std::move(p));
    }
}

// 启动扫描放到工作线程：托盘图标先就位并可响应（右键显示「正在启动中」），
// 扫描结果通过 WM_APP_PROGRAMS_READY 交回主线程接管，避免与搜索并发读写 g.programs。
static unsigned __stdcall ScanProgramsThread(void* context) {
    std::unique_ptr<ProgramScanContext> request((ProgramScanContext*)context);
    auto state=request->state;HWND destination=request->destination;
    const HRESULT comResult=CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::unique_ptr<ProgramBuildReply> out(new(std::nothrow)ProgramBuildReply());
    if(out)BuildProgramsInto(*out,&state->stop);
    if(SUCCEEDED(comResult))CoUninitialize();
    // Warm the bounded, Flowtary-owned C:\workshop rule on this same startup
    // worker, after app discovery, so the first keystroke never starts its scan.
    if(out && !state->stop.load(std::memory_order_acquire))df::WorkshopCatalogWarmup(&state->stop);
    std::lock_guard<std::mutex> lock(state->deliveryMutex);
    const bool delivered=!state->stop.load() && state->window==destination &&
       PostMessageW(destination,WM_APP_PROGRAMS_READY,0,(LPARAM)out.get());
    if(delivered)out.release();
    else state->running=false;
    // Keep multi-million-entry snapshot loading off this catalog/UI delivery
    // worker. The indexer performs a machine-headroom preflight before loading.
    if(delivered && df::FlowtaryIndexAvailable()) {
        try { std::thread([] { (void)df::FlowtaryIndexPrewarm(); }).detach(); }
        catch (...) { /* The normal first-query path still starts the child lazily. */ }
    }
    return 0;
}
static bool StartProgramsRefresh() {
    auto state=s_programScanState;
    if(state->running.exchange(true))return false;
    state->stop=false;
    {std::lock_guard<std::mutex> lock(state->deliveryMutex);state->window=g.hwnd;}
    auto* context=new(std::nothrow)ProgramScanContext{state,g.hwnd};
    HANDLE thread=context?(HANDLE)_beginthreadex(nullptr,0,ScanProgramsThread,context,0,nullptr):nullptr;
    if(!thread){delete context;state->running=false;g.startingUp=false;s_programWatch.Stop();return false;}
    CloseHandle(thread); // The worker is owned by its cancellation/delivery state, not a leaked thread handle.
    s_programWatch.AcknowledgeRefresh();
    g.startingUp=true;
    return true;
}
static void StopProgramsRefresh(HWND hwnd) {
    auto state=s_programScanState;
    state->stop=true;
    std::lock_guard<std::mutex> lock(state->deliveryMutex);
    state->window=nullptr;
    MSG message;
    while(PeekMessageW(&message,hwnd,WM_APP_PROGRAMS_READY,WM_APP_PROGRAMS_READY,PM_REMOVE))
        delete (ProgramBuildReply*)message.lParam;
    s_programWatch.Stop();
}
static std::wstring ProgramWorkingDirectory(Program* p) {
    if(!p)return {};
    // Let the Shell honor a shortcut's own working-directory/arguments when opening it.
    return p->startMenu ? std::wstring() : DirOf(p->target);
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

// d/f 结构化词表：与文件查询分词规则相同（空格分隔，"短语"整体一词），
// 但去掉引号本身；排除词已由 ParseExcludes 剥离，不在此处理。
// 供 df::Query.words 使用（Windows Search SQL 按需转义）。
static std::vector<std::wstring> SplitQueryWords(const std::wstring& q) {
    std::vector<std::wstring> out;
    size_t i = 0, n = q.size();
    while (i < n) {
        while (i < n && iswspace(q[i])) ++i;
        if (i >= n) break;
        if (q[i] == L'"') {
            size_t j = q.find(L'"', i + 1);
            if (j == std::wstring::npos) {
                if (i + 1 < n) out.push_back(q.substr(i + 1));
                break;
            }
            if (j > i + 1) out.push_back(q.substr(i + 1, j - i - 1));
            i = j + 1;
        } else {
            size_t j = i;
            while (j < n && !iswspace(q[j])) ++j;
            out.push_back(q.substr(i, j - i));
            i = j;
        }
    }
    return out;
}

// textLower 已小写；命中任一排除词返回 true（子串包含，大小写不敏感）
static bool HasExcludeTerm(const std::wstring& textLower) {
    for (const auto& e : g.excludes)
        if (!e.empty() && textLower.find(e) != std::wstring::npos) return true;
    return false;
}

// ---------------- 使用分档与热度 ----------------
// 常用阈值：历史总使用次数达到该行数才进入醒目样式；
// 没用过/偶尔用的行保持原样（哪怕同在开始菜单这类规定目录下）。
constexpr int kHotUsageMin = 3;

// 使用分档：0=原样(0-2 次)，2=常用(3-9)，3=高频(10-29)，4=重度(>=30)。
// 档决定字体/徽章形态；档内再由 UsageHeat 连续驱动颜色强度，
// 同档里 15 次和 25 次看起来也不一样，避免一刀切的粗粒度。
static int UsageTier(int usage) {
    if (usage < kHotUsageMin) return 0;
    if (usage < 10) return 2;
    if (usage < 30) return 3;
    return 4;
}
// 热度 0..1：log 曲线，次数边际递减（1→0.17，3→0.33，15→0.66，29→0.82，64+→1）
static float UsageHeat(int usage) {
    if (usage <= 0) return 0.f;
    float h = logf(1.f + (float)usage) / logf(65.f);
    return h > 1.f ? 1.f : h;
}

// ---------------- 全局使用档案（徽章与排序共用同一口径） ----------------
// 徽章上的「×N」是「跨全部搜索词 + 自学习」的累计次数；如果排序仍按「当前词桶」
// 的使用分排，就会出现「徽章标着重度 ×81、却被零使用的行压在下面」的矛盾。
// 这里把全局累计（次数 + 最近使用时间）单独聚一份，排序与徽章都从它取数。
struct AppUsage { int count = 0; long long last = 0; };

static std::unordered_map<std::wstring, AppUsage> g_globalUsage;
static bool g_globalUsageDirty = true;

// 权重/自学习发生任何增删改都要打脏标，否则排序会读到旧档案。
static void MarkUsageDirty() { g_globalUsageDirty = true; }

static const std::unordered_map<std::wstring, AppUsage>& GlobalUsage() {
    if (g_globalUsageDirty) {
        g_globalUsage.clear();
        // 按应用身份聚合：taskinformer 升级到新版本目录后，历史各版本的使用次数合并计数，
        // 常用档加粗不会因为升级而丢失。文件/文件夹不在版本目录里，身份=路径，行为不变。
        for (auto& kv : g.weights)
            for (auto& pv : kv.second) {
                AppUsage& a = g_globalUsage[ft::search::AppIdentityKey(pv.first)];
                a.count += pv.second.count;
                if (pv.second.last > a.last) a.last = pv.second.last;
            }
        for (auto& kv : g.learned)
            for (auto& pv : kv.second) {
                AppUsage& a = g_globalUsage[ft::search::AppIdentityKey(pv.first)];
                a.count += pv.second.count;
                if (pv.second.last > a.last) a.last = pv.second.last;
            }
        g_globalUsageDirty = false;
    }
    return g_globalUsage;
}

// 某条目标的全局累计次数（徽章口径）与分档。
static int GlobalUsageCount(const std::wstring& path) {
    const auto& m = GlobalUsage();
    auto it = m.find(ft::search::AppIdentityKey(path));
    return it == m.end() ? 0 : it->second.count;
}

// 当前查询下的「使用档案」：把精确词命中 + 前缀词推荐的历史权重，统一按应用身份聚合。
// 一次遍历算好，排序时每个候选只需一次查表。跨版本合并让升级不丢历史使用。
//   - 精确词 term==ql：用户敲完整/当前词时点过的
//   - 前缀词 term 以 ql 开头且更长：以前敲完整词点过、现在只敲前缀的自适应推荐
static void CollectUsageByApp(const std::wstring& ql,
                              std::unordered_map<std::wstring, AppUsage>& out) {
    out.clear();
    if (!g.weightEnabled || ql.empty()) return;
    for (auto& kv : g.weights) {
        const std::wstring& term = kv.first;
        const bool exact = (term == ql);
        const bool prefix = term.size() > ql.size() &&
                            wcsncmp(term.c_str(), ql.c_str(), ql.size()) == 0;
        if (!exact && !prefix) continue;
        for (auto& pv : kv.second) {
            AppUsage& a = out[ft::search::AppIdentityKey(pv.first)];
            a.count += pv.second.count;
            if (pv.second.last > a.last) a.last = pv.second.last;
        }
    }
    // 自学习只贡献「最近使用时间」：weights.dat 旧格式没有时间戳，而自学习记录了真实打开时刻。
    // 次数不重复累加（避免点击权重被二次计数），只把 last 合并进来驱动「最近使用」加分。
    // 例：weights 里 taskinformer 各版本共 21 次但无时间，learned 里 0.1.48 有最近时间戳，
    // 合并后它既有高频次数又有近期时间，能排过路径固定、只有次数没有时间的常驻程序。
    if (g.learnEnabled) {
        for (auto& kv : g.learned) {
            const std::wstring& term = kv.first;
            const bool exact = (term == ql);
            const bool prefix = term.size() > ql.size() &&
                                wcsncmp(term.c_str(), ql.c_str(), ql.size()) == 0;
            if (!exact && !prefix) continue;
            for (auto& pv : kv.second) {
                AppUsage& a = out[ft::search::AppIdentityKey(pv.first)];
                if (pv.second.last > a.last) a.last = pv.second.last;
            }
        }
    }
}

// 路径在当前查询下的综合使用分：只做一次应用身份查表 + 分档评分。
static int UsageScoreOf(const std::wstring& path, const std::wstring& ql,
                        const std::unordered_map<std::wstring, AppUsage>& usage,
                        long long now) {
    if (!g.weightEnabled) return 0;
    auto it = usage.find(ft::search::AppIdentityKey(path));
    if (it == usage.end()) return 0;
    (void)ql;
    return ft::search::UsageScore(it->second.count, it->second.last, now);
}

static std::wstring UrlHostOf(const std::wstring& url);
static std::wstring ShortLocationLabel(const std::wstring& path);

// query 由 Refresh() 传入「已剥离排除词的正向查询」；排除词在 g.excludes（本轮解析一次，共用）。
static void SearchPrograms(const std::wstring& query) {
    std::wstring ql = NormalizeSearchTerm(query);  // 与权重 key 同款标准化（去首尾空格+小写）
    std::unordered_map<std::wstring, AppUsage> usage;  // 应用身份 → 累计次数/最近时间
    CollectUsageByApp(ql, usage);
    const long long now = NowUnix();
    const bool hasEx = !g.excludes.empty();
    const auto& globalUse = GlobalUsage();
    struct Cand { Program* p; int score; int w; int tier; };
    std::vector<Cand> cands;
    // 单字符查询只保留精确(4)/前缀(3)：带上「包含/子序列」时命中面过宽
    // （实测 d 命中全部程序的 32%），刚敲第一个字母就撑满 10 行噪音结果。
    // 有使用历史的条目例外：是用户此前真实点击过的，属于自适应推荐，不算噪音。
    const bool prefixOnly = (ql.size() < 2);
    for (auto& p : g.programs) {
        int s = MatchScore(ToLowerW(p.name), ql);
        int w = UsageScoreOf(p.path, ql, usage, now);
        if (!(s > 0 && !(prefixOnly && s < 3)) && w <= 0) continue;
        if (hasEx) {  // -关键字：名称/路径任一含排除词即丢弃
            std::wstring hay = ToLowerW(p.name) + L' ' + ToLowerW(p.path);
            if (HasExcludeTerm(hay)) continue;
        }
        // 全局热度档：与行上徽章同一口径（跨全部搜索词累计），重度档必须排在最前
        int tier = 0;
        auto git = globalUse.find(ft::search::AppIdentityKey(p.path));
        if (git != globalUse.end()) tier = UsageTier(git->second.count);
        cands.push_back({&p, s, w, tier});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.tier != b.tier) return a.tier > b.tier;  // 热度档优先：重度 > 高频 > 常用 > 原样
        if (a.w != b.w) return a.w > b.w;  // 再比当前词桶的综合使用分（跨版本累计次数+最近使用）
        if (a.score != b.score) return a.score > b.score;  // 同分再比文本匹配度
        if (a.p->startMenu != b.p->startMenu) return a.p->startMenu;
        return _wcsicmp(a.p->name.c_str(), b.p->name.c_str()) < 0;
    });
    size_t n = (std::min)(cands.size(), (size_t)kMaxVisibleResults);
    for (size_t i = 0; i < n; ++i) {
        Row r;
        r.kind = Row::Prog;
        r.prog = cands[i].p;
        r.title = cands[i].p->name;
        r.sub = ShortLocationLabel(cands[i].p->path);
        r.action = cands[i].p->path;
        if (IsPathExcluded(r.action)) continue;  // 排除命中：跳过该程序
        g.items.push_back(std::move(r));
    }
}

// 副标题减字：完整路径在启动器列表里是纯噪音（半屏灰字、尾部还和标题重复）。
// 只保留 2-4 字的「来源」标签，让一行回到「图标 + 名称 + 极简注脚」的 iOS 层级。
// 没有明确来源的条目返回空串——留白比塞满更高级。
static std::wstring UrlHostOf(const std::wstring& url) {
    size_t a = url.find(L"://");
    if (a == std::wstring::npos) return L"";
    a += 3;
    size_t b = url.find_first_of(L"/?", a);
    return b == std::wstring::npos ? url.substr(a) : url.substr(a, b - a);
}
static std::wstring ShortLocationLabel(const std::wstring& path) {
    if (path.empty()) return L"";
    if (path.size() > 8 && _wcsnicmp(path.c_str(), L"http", 4) == 0)
        return UrlHostOf(path);
    std::wstring p;
    p.reserve(path.size());
    for (WCHAR ch : path) {
        if (ch == L'/') ch = L'\\';
        if (ch >= L'A' && ch <= L'Z') ch = (WCHAR)(ch - L'A' + L'a');
        p.push_back(ch);
    }
    auto contains = [&](const WCHAR* s) { return p.find(s) != std::wstring::npos; };
    if (contains(L"\\start menu\\"))  return L"开始菜单";
    if (contains(L"\\desktop\\"))     return L"桌面";
    if (contains(L"\\downloads\\"))   return L"下载";
    if (contains(L"\\documents\\"))   return L"文档";
    if (contains(L"\\workshop\\"))    return L"工作区";
    if (contains(L"\\program files")) return L"程序";
    return L"";
}

static void AnnotateRowUsage() {
    bool any = false;
    for (auto& r : g.items) {
        r.usage = 0;
        if (r.kind == Row::Prog || r.kind == Row::File || r.kind == Row::Folder) any = true;
    }
    if (!any) return;
    const auto& total = GlobalUsage();
    for (auto& r : g.items) {
        if (r.kind != Row::Prog && r.kind != Row::File && r.kind != Row::Folder) continue;
        auto it = total.find(ft::search::AppIdentityKey(r.action));
        if (it != total.end()) r.usage = it->second.count;
    }
}

// ---------------- 搜索耗时底栏 ----------------
// 「秒出」的查询不占地方；一旦不是秒出（还在等 / 等完但超过阈值），底栏实时报已经多少毫秒。
static bool SearchPending() {
    if (g.mode == Mode::Files && !g.dfWsDone) return true;
    if (g.mode == Mode::Programs && g.startingUp) return true;
    return false;
}

// 返回底栏是否显示；ms=要报的毫秒数，live=是否还在跳（pending 实时累计 vs 报齐后定格）。
static bool LatencyFooter(int& ms, bool& live) {
    ms = 0; live = false;
    if (g.qT0 == 0 || g.text.empty()) return false;
    if (SearchPending()) {
        ms = (int)(GetTickCount64() - g.qT0);
        live = true;
        return ms >= kSlowMs;
    }
    if (g.qSettledMs >= 0) { ms = g.qSettledMs; return ms >= kSlowMs; }
    return false;
}

// 本轮报齐（没有还在等的层）时定格总耗时；只记第一次，后续重排不刷新它。
static void NoteQuerySettled() {
    if (g.qSettledMs < 0 && g.qT0 != 0 && !SearchPending())
        g.qSettledMs = (int)(GetTickCount64() - g.qT0);
}

// 每次上屏后收尾：定格耗时；还在等就开跳毫秒的定时器，报齐就停（定格数字不需要重画）。
static void FinishRefreshLatency() {
    NoteQuerySettled();
    if (SearchPending()) SetTimer(g.hwnd, kTimerLatency, kLatencyTickMs, nullptr);
    else KillTimer(g.hwnd, kTimerLatency);
}

static std::wstring PendingLayerText() {
    if (g.mode == Mode::Programs) return L"应用列表加载中";
    switch (g.searchRoute) {
        case ft::search::Route::FlowtaryIndex: return L"Flowtary 索引未回";
        case ft::search::Route::WorkshopCatalog: return L"workshop 目录未回";
        default: return L"Windows Search 未回";
    }
}

// ---------------- 退出/唤出追踪日志（诊断「无声消失」与热键不响应） ----------------
// 追加写 %APPDATA%\Flowtary\trace.log，单行 = 时间 + pid + 事件。只记低频关键事件。
// 事件标签保持 ASCII，避免 CRT 宽字符→MBCS 转换的编码歧义。
static void AppendTrace(const char* tag) {
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("APPDATA", path, MAX_PATH)) return;
    lstrcatA(path, "\\Flowtary\\trace.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[256];
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\r\n",
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                        st.wMilliseconds, GetCurrentProcessId(), tag);
    DWORD written = 0;
    if (n > 0) WriteFile(f, line, (DWORD)n, &written, nullptr);
    CloseHandle(f);
}

// UI 由 Slint 渲染器排版，主进程只负责把行数据算好。
static void Layout() {
    AnnotateRowUsage();
}

static void RepaintNow() { SlintPushState(); }

static void LayoutAndRepaint() {
    Layout();
    RepaintNow();
}

// 唤起时拉起同目录的 flowtary-slint.exe（渲染器）。
// 系统层（全局热键 Alt+Space / 搜索管线 / 自学习）留在主程序，Slint 只负责画界面。
// 返回 false 表示渲染器不可用（缺 exe 或启动失败）。
static bool LaunchSlintEngine() {
    SlintBridgeEnsure();
    if (HWND ipc = SlintIpcWindow()) {
        // 渲染器已在运行：下发状态 + 命令它显示
        SlintPushState();
        SlintSendCmd(slintbridge::kCmdShow);
        return true;
    }
    std::wstring exe = GetExePath();
    size_t slash = exe.find_last_of(L"\\/");
    std::wstring path = (slash == std::wstring::npos) ? L"flowtary-slint.exe"
                                                      : exe.substr(0, slash + 1) + L"flowtary-slint.exe";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        AppendTrace("Slint renderer missing");
        return false;
    }
    // 渲染器起来后会发 kReqReady，那时才真正显示（此刻还没有窗口）。
    // 用 CreateProcess 而不是 ShellExecute：不经过 Explorer，启动更快也更可控。
    std::wstring cmdLine = L"\"" + path + L"\"" + (g.centerWake ? L" --center" : L" --mouse");
    std::wstring workDir = (slash == std::wstring::npos) ? std::wstring() : exe.substr(0, slash + 1);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(path.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
                        nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi)) {
        AppendTrace("Slint renderer launch failed");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static void Show() {
    AppendTrace("SHOW(wake)");
    g.text.clear();
    g.items.clear();
    g.sel = 0;
    g.mode = Mode::None;
    g.evTermKey.clear();
    ++g.dfGen;
    g.searchRoute = ft::search::Route::Unavailable;
    g.qT0 = 0;
    g.qSettledMs = -1;
    KillTimer(g.hwnd, kTimerDebounce);
    KillTimer(g.hwnd, kTimerLatency);
    g.prevForeground = GetForegroundWindow();  // top 命令的目标窗口
    if (!LaunchSlintEngine())
        ShowNotice(L"Flowtary", L"界面组件缺失：请确认 flowtary-slint.exe 与主程序在同一目录。", L"好");
}

static void Hide() {
    AppendTrace("HIDE");
    KillTimer(g.hwnd, kTimerDebounce);
    KillTimer(g.hwnd, kTimerLatency);
    ++g.dfGen;
    g.searchRoute = ft::search::Route::Unavailable;
    SlintSendCmd(slintbridge::kCmdHide);
}

// ---------------- 文件索引查询 ----------------
static void AddHint(const std::wstring& text) {
    Row r;
    r.kind = Row::Hint;
    r.title = text;
    g.items.push_back(std::move(r));
}

static void StartWindowsSearch(ft::search::Route route = ft::search::Route::WindowsSearch);

// d/f queries consume existing indexes; name/path fallback stays within the same catalog.
struct DfReply {
    DWORD gen;
    ft::search::Route route;
    bool failed;
    std::vector<df::Hit> hits;
};

static void DfWsWorker(DWORD gen, const df::Query& q, ft::search::Route route) {
    auto reply = std::make_unique<DfReply>();
    reply->gen = gen;
    reply->route = route;
    if (gen != g.dfGen.load()) return;
    int result = 0;
    auto appendWorkshop = [&]() {
        std::vector<df::Hit> workshopHits;
        const int workshopResult = df::WorkshopCatalogQuery(q, workshopHits);
        for (auto& workshopHit : workshopHits) {
            const bool duplicate = std::any_of(reply->hits.begin(), reply->hits.end(),
                [&](const df::Hit& hit) { return CompareStringOrdinal(hit.path.c_str(), (int)hit.path.size(),
                    workshopHit.path.c_str(), (int)workshopHit.path.size(), TRUE) == CSTR_EQUAL; });
            if (!duplicate) reply->hits.push_back(std::move(workshopHit));
        }
        if (result < 0 && workshopResult >= 0) result = workshopResult;
    };
    if (route == ft::search::Route::FlowtaryIndex) {
        result = df::FlowtaryIndexQuery(q, ft::search::HasExplicitPath(q.words), reply->hits);
        if (result >= 0) appendWorkshop();
    } else if (route == ft::search::Route::WorkshopCatalog) {
        result = df::WorkshopCatalogQuery(q, reply->hits);
    } else {
        result = df::QueryWithPathFallback(q, reply->hits, df::WindowsSearchQuery,
            [gen] { return gen == g.dfGen.load(); });
        if (route == ft::search::Route::WindowsSearch) appendWorkshop();
    }
    reply->failed = result < 0;
    if (PostMessageW(g.hwnd, WM_APP_DF_READY, 0, (LPARAM)reply.get())) reply.release();
}

// 常驻 WS 工作线程（latest-wins）：df_search 里的 COM/索引会话只在首次查询时建一次，
// 后续查询毫秒级——每条查询开新线程会反复重建会话（实测 ~0.8s 开销）。
static std::mutex s_wsMu;
static std::condition_variable s_wsCv;
static DWORD s_wsJobGen = 0;
static df::Query s_wsJob;
static ft::search::Route s_wsRoute = ft::search::Route::WindowsSearch;
static bool s_wsHasJob = false;
static bool s_wsQuit = false;

static unsigned __stdcall WsThreadProc(LPVOID) {
    for (;;) {
        df::Query q;
        DWORD gen = 0;
        ft::search::Route route = ft::search::Route::WindowsSearch;
        {
            std::unique_lock<std::mutex> lk(s_wsMu);
            s_wsCv.wait(lk, [] { return s_wsHasJob || s_wsQuit; });
            if (s_wsQuit) break;
            gen = s_wsJobGen;
            route = s_wsRoute;
            q = std::move(s_wsJob);
            s_wsHasJob = false;
        }
        DfWsWorker(gen, q, route);   // 空闲时阻塞在条件变量上，不占 CPU
    }
    return 0;
}

static bool WsEnqueue(DWORD gen, const df::Query& q, ft::search::Route route = ft::search::Route::WindowsSearch) {
    static bool started = false;   // StartDfSearch 仅主线程调用：懒启动无竞争
    {
        std::lock_guard<std::mutex> lk(s_wsMu);
        s_wsJobGen = gen;
        s_wsRoute = route;
        s_wsJob = q;
        s_wsHasJob = true;
        if (!started) {
            HANDLE thread = (HANDLE)_beginthreadex(nullptr, 0, WsThreadProc, nullptr, 0, nullptr);
            if (!thread) { s_wsHasJob = false; return false; }
            started = true;
            CloseHandle(thread);
        }
    }
    s_wsCv.notify_one();
    return true;
}

static void RenderDfResults();
static void StartFlowtaryIndexSearch() {
    g.searchRoute = ft::search::Route::FlowtaryIndex;
    g.dfWsDone = false;
    g.dfWsFailed = false;
    g.dfBaseRows.clear();
    ArmHistoryClock(L"FTI");
    if (!WsEnqueue(g.dfGen.load(), g.dfQuery, ft::search::Route::FlowtaryIndex)) {
        g.dfWsDone = g.dfWsFailed = true;
        RenderDfResults();
    }
}

// 把一条索引命中转换成展示行（与 HandleDfReply 同一套规则：标题=末段、副标题=父目录、
// 排除词与全局排除路径过滤）。预填与异步回复共用，保证两路结果形态一致。
static bool MakeDfRow(const df::Hit& h, Row& row) {
    if (HasExcludeTerm(ToLowerW(h.path))) return false;
    row = Row{};
    row.kind = h.isFolder ? Row::Folder : Row::File;
    row.action = h.path;
    row.workshopHit = h.workshop;
    size_t p = h.path.find_last_of(L'\\');
    if (p == std::wstring::npos || p + 1 == h.path.size()) {
        row.title = h.path;
    } else {
        row.title = h.path.substr(p + 1);
        row.sub = h.path.substr(0, p);
        if (row.sub.size() == 2 && row.sub[1] == L':') row.sub += L'\\';
    }
    if (row.title.empty()) row.title = h.path;
    return !IsPathExcluded(row.action, true);
}

static void StartWindowsSearch(ft::search::Route route) {
    g.searchRoute = route;
    g.dfWsDone = false;
    g.dfWsFailed = false;
    g.dfBaseRows.clear();
    ArmHistoryClock(L"WS");

    // 自动路由且没有 Flowtary 全盘快照时，WS 查询实测 2–13 秒。C:\workshop 是本机
    // 自有、已 warmed 的内存目录：先把它的命中同步上屏（毫秒级），慢速 WS 结果
    // 回来后 HandleDfReply 会按路径去重合并——这才是「workshop 秒出」的原意。
    // 只对 Route::WindowsSearch 生效：显式「仅 WS」用户不要本地目录，其它路由自带后端。
    if (route == ft::search::Route::WindowsSearch && df::WorkshopCatalogAvailable() &&
        !g.dfQuery.words.empty()) {
        std::vector<df::Hit> prefilled;
        if (df::WorkshopCatalogQuery(g.dfQuery, prefilled) > 0) {
            for (const auto& h : prefilled) {
                Row row;
                if (MakeDfRow(h, row)) g.dfBaseRows.push_back(std::move(row));
            }
        }
        if (!g.dfBaseRows.empty()) RenderDfResults();  // 首屏立即呈现，dwWsDone 仍 false
    }

    if (!WsEnqueue(g.dfGen.load(), g.dfQuery, route)) {
        g.dfWsDone = g.dfWsFailed = true;
        RenderDfResults();
    }
}

static void StartDfSearch() {
    ++g.dfGen;

    g.dfWsFailed = false;
    g.dfBaseRows.clear();
    g.searchRoute = ft::search::SelectRoute(g.dfBackend, df::FlowtaryIndexAvailable(), df::WorkshopCatalogAvailable());
    if (g.searchRoute == ft::search::Route::WindowsSearch ||
        g.searchRoute == ft::search::Route::WindowsSearchOnly) {
        StartWindowsSearch(g.searchRoute);
    } else if (g.searchRoute == ft::search::Route::FlowtaryIndex) {
        StartFlowtaryIndexSearch();
    } else if (g.searchRoute == ft::search::Route::WorkshopCatalog) {
        g.dfWsDone = false;
        g.dfWsFailed = false;
        g.dfBaseRows.clear();
        if (!WsEnqueue(g.dfGen.load(), g.dfQuery, ft::search::Route::WorkshopCatalog)) {
            g.dfWsDone = g.dfWsFailed = true;
            RenderDfResults();
        }
    } else {
        g.items.clear();
        PrependLearnedRows(g.evTermKey);
        AddHint(L"Flowtary 索引不可用；未启动文件扫描");
        g.sel = 0;
        LayoutAndRepaint();
    }
}

// 点击加权重排（与 Handle索引Reply 内同一套规则；含自适应前缀推荐 + 跨版本聚合）
// 首要键是「全局热度档」——与行上徽章同一口径（跨全部搜索词累计），
// 否则会出现「徽章标着重度 ×81、却被零使用的行压在下面」。
static void RerankDfByWeight() {
    if (g.evTermKey.empty() || g.items.size() <= 1) return;
    const std::wstring& tk = g.evTermKey;
    std::unordered_map<std::wstring, AppUsage> usage;
    if (g.weightEnabled) CollectUsageByApp(tk, usage);
    const long long now = NowUnix();
    const auto& globalUse = GlobalUsage();
    auto rowTier = [&](const Row& r) -> int {
        if (r.kind != Row::File && r.kind != Row::Folder) return 0;
        auto it = globalUse.find(ft::search::AppIdentityKey(r.action));
        if (it == globalUse.end()) return 0;
        return UsageTier(it->second.count);
    };
    auto rowW = [&](const Row& r) {
        if (r.kind != Row::File && r.kind != Row::Folder) return 0;
        return UsageScoreOf(r.action, tk, usage, now);
    };
    std::stable_sort(g.items.begin(), g.items.end(),
                     [&rowTier, &rowW](const Row& a, const Row& b) {
                         int ta = rowTier(a), tb = rowTier(b);
                         if (ta != tb) return ta > tb;   // 热度档优先：重度 > 高频 > 常用 > 原样
                         if (a.workshopHit != b.workshopHit) return a.workshopHit;
                         return rowW(a) > rowW(b);
                     });
}

// ---------------- 应用版本目录排序 ----------------
// 发布目录（workshop / software 等）的布局是「<应用名>-<版本>」，同一应用常有多版并存，
// 外加 -backup-<日期>-xxx / -verifyfix 这类快照副本。搜「aura」时第一行必须是最新版的
// 正式目录：文本命中质量同分时，按解析出的版本号降序，快照沉到同版本正式版之后。
// 解析规则在 src/app_ver.h（与 tools/appver_test.cpp 共用，规则只写一遍）。
using appver::Info;

static void RenderDfResults() {
    g.items = g.dfBaseRows;
    // 基础排序：逐词按文件名命中质量（精确 > 前缀 > 包含 > 仅路径）——
    // 两个后端的原始顺序都是遍历序，不重排会让深层目录的包含命中压过浅层的精确命中。
    // 权重重排随后再叠加（点过的条目最前）。
    const std::vector<std::wstring>& words = g.dfQuery.words;
    auto scoreOf = [&words](const Row& r) {
        std::wstring name = ToLowerW(r.title);
        std::wstring path = ToLowerW(r.action);
        int s = 0;
        for (const auto& w : words) {
            std::wstring wl = ToLowerW(w);
            if (name == wl) s += 8;
            else if (name.size() >= wl.size() && name.compare(0, wl.size(), wl) == 0) s += 4;
            else if (name.find(wl) != std::wstring::npos) s += 2;
            else if (path.find(wl) != std::wstring::npos) s += 1;
        }
        return s;
    };
    if (!words.empty() && g.items.size() > 1) {
        // 排序键一次算好（文本分 + 热度档 + 版本档）：比较器里现算路径解析会被排序放大几倍。
        struct DfKey { int tier; int workshop; int score; int vrank; unsigned long long ver; int plain; size_t idx; };
        std::vector<DfKey> keys(g.items.size());
        std::vector<Info> av(g.items.size());
        appver::Table tab;
        const auto& globalUse = GlobalUsage();
        for (size_t i = 0; i < g.items.size(); ++i) {
            keys[i].score = scoreOf(g.items[i]);
            keys[i].workshop = g.items[i].workshopHit ? 1 : 0;
            keys[i].idx = i;
            // 热度档与行上徽章同源：重度档必须排在最前，不能被零使用的行压住
            keys[i].tier = 0;
            if (g.items[i].kind == Row::File || g.items[i].kind == Row::Folder) {
                auto it = globalUse.find(ft::search::AppIdentityKey(g.items[i].action));
                if (it != globalUse.end()) keys[i].tier = UsageTier(it->second.count);
            }
            av[i] = appver::Parse(g.items[i].action);
            tab.Feed(av[i]);
        }
        for (size_t i = 0; i < g.items.size(); ++i) {
            keys[i].vrank = tab.Rank(av[i]);
            keys[i].plain = av[i].has ? (av[i].plain ? 1 : 0) : 1;
            keys[i].ver = av[i].ver;
        }
        std::stable_sort(keys.begin(), keys.end(), [](const DfKey& a, const DfKey& b) {
            if (a.tier != b.tier) return a.tier > b.tier;  // 热度档优先
            if (a.workshop != b.workshop) return a.workshop > b.workshop;
            if (a.score != b.score) return a.score > b.score;
            if (a.vrank != b.vrank) return a.vrank < b.vrank;
            if (a.ver != b.ver) return a.ver > b.ver;
            if (a.plain != b.plain) return a.plain > b.plain;
            return a.idx < b.idx;   // 同键保持后端返回顺序
        });
        std::vector<Row> sorted;
        sorted.reserve(g.items.size());
        for (const auto& k : keys) sorted.push_back(std::move(g.items[k.idx]));
        g.items = std::move(sorted);
    }
    // f/d 自学习置顶（与 索引 展示路径同一套：摘出与结果重复的学习行后插到最前）
    // 先插学习项再重排：学习项也参与热度排序，重度档学习行仍然排在最前，
    // 而只有一两次使用的学习项不会硬压过热得多的目标。
    PrependLearnedRows(g.evTermKey);
    RerankDfByWeight();
    NoteHistoryFirstPaint();
    if (g.items.empty()) {
        if (!g.dfWsDone) AddHint(g.searchRoute == ft::search::Route::FlowtaryIndex ? L"正在查询 Flowtary 索引…" : g.searchRoute == ft::search::Route::WorkshopCatalog ? L"正在查询 C:\\workshop 应用…" : L"正在查询 Windows Search…");
        else if (g.dfWsFailed) AddHint(g.searchRoute == ft::search::Route::WorkshopCatalog ? L"C:\\workshop 目录索引查询失败" : g.searchRoute == ft::search::Route::FlowtaryIndex ? L"Flowtary 索引查询失败；未启动文件扫描" : L"Windows Search 不可用；未启动文件扫描");
        else AddHint(g.searchRoute == ft::search::Route::WorkshopCatalog ? L"无结果（C:\\workshop 应用）" : g.searchRoute == ft::search::Route::FlowtaryIndex ? L"无结果（Flowtary 索引）" : L"无结果（仅搜索 Windows Search 已索引的位置）");
    }
    if (g.dfWsDone) {
        g.qMissed.clear();
        LogHistoryQuery();
    } else {
        g.qMissed = HistoryMissingLayers();   // 本轮还开着：把此刻的现场攒着，换查询/退出时落盘
        SnapshotHistoryPending();
    }
    g.sel = 0;
    FinishRefreshLatency();
    LayoutAndRepaint();
}

static void HandleDfReply(DfReply* r) {
    if (!r) return;
    std::unique_ptr<DfReply> hold(r);
    if (r->gen != g.dfGen || g.mode != Mode::Files ||
        g.searchRoute != r->route) return;
    g.dfWsDone = true;
    g.dfWsFailed = r->failed;
    g.qWsRows = (int)r->hits.size();
    std::vector<Row> rows;
    if (!r->failed) {
        for (const auto& h : r->hits) {
            if (HasExcludeTerm(ToLowerW(h.path))) continue;
            Row row;
            row.kind = h.isFolder ? Row::Folder : Row::File;
            row.action = h.path;
            row.workshopHit = h.workshop;
            // 与 索引 行同构：title=名字, sub=父目录（盘根 C:\ 整行一个 title）
            size_t p = h.path.find_last_of(L'\\');
            if (p == std::wstring::npos || p + 1 == h.path.size()) {
                row.title = h.path;
                row.sub.clear();
            } else {
                row.title = h.path.substr(p + 1);
                row.sub = h.path.substr(0, p);
                if (row.sub.size() == 2 && row.sub[1] == L':') row.sub += L'\\';  // 盘根目录补分隔符
            }
            if (row.title.empty()) row.title = h.path;
            // 排除路径 / 点开头过滤：与 索引 结果同一套规则
            if (IsPathExcluded(row.action, true)) continue;
            rows.push_back(std::move(row));
        }
    }
    for (auto& nr : rows) {          // 同一路径只展示一次
        bool dup = false;
        for (const auto& er : g.dfBaseRows)
            if (_wcsicmp(er.action.c_str(), nr.action.c_str()) == 0) { dup = true; break; }
        if (!dup) g.dfBaseRows.push_back(std::move(nr));
    }
    RenderDfResults();
}

// ---------------- 输入解析与刷新 ----------------
static const WebCmd* FindWebCmd(const std::wstring& tok) {
    for (auto& w : g.webCmds)
        if (tok == w.prefix) return &w;
    return nullptr;
}

// 文件查询由结构化分词、路径回退和本地排除规则组成；不依赖外部 IPC 语法。
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
            if (g.items.size() >= kMaxVisibleResults) break;
        }
    } else {
        for (auto& c : cands) {
            const WinInfo& w = g.windowCache[c.idx];
            Row r; r.kind = Row::Window; r.winHwnd = w.hwnd;
            r.title = w.title; r.procName = w.processName;
            r.sub = g.winShowProc ? w.processName : L"";
            r.action = std::to_wstring((uintptr_t)w.hwnd);
            g.items.push_back(std::move(r));
            if (g.items.size() >= kMaxVisibleResults) break;
        }
    }
}

static void Refresh() {
    KillTimer(g.hwnd, kTimerDebounce);
    ++g.dfGen;
    g.searchRoute = ft::search::Route::Unavailable;
    g.items.clear();
    g.sel = 0;
    g.mode = Mode::None;
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
                ArmHistoryClock(L"P");
                SearchPrograms(pq.positive);
                LogHistoryQuery();   // 程序搜索在主线程同步完成，派发即有结果
            }
        } else if (tok == L"d" || tok == L"f") {
                    g.mode = Mode::Files;
            ++g.dfGen;
            g.searchRoute = ft::search::Route::Unavailable;
            if (rest.empty()) {
                AddHint(tok == L"f" ? L"输入关键词搜索文件" : L"输入关键词搜索文件夹");
            } else {
                bool isFile = tok == L"f";
                ParsedQuery pq = ParseExcludes(rest);  // -关键字：Flowtary / Windows Search 结果统一过滤
                g.excludes = pq.excludes;

                g.evTermKey = NormalizeSearchTerm(pq.positive);  // 有效搜索词（权重/学习 key，不含排除词）
                // 双后端结构化查询（正向词表 + 排除词）：防抖到期由 StartDfSearch() 派发
                g.dfQuery = df::Query{};
                g.dfQuery.isFile = isFile;
                g.dfQuery.words = SplitQueryWords(pq.positive);
                g.dfQuery.excludes = pq.excludes;
                AddHint(L"正在搜索…");
                // f/d 自学习：以前打开过的条目先置顶（索引 结果到达后再合并）
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
                r.sub = UrlHostOf(r.action);  // 只留域名，不铺整条 URL
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
                r.sub = std::to_wstring(gp->targets.size()) + L" 个目标";  // 行上只给数量，清单留给右键复制
                r.action = sub;  // 右键「复制路径」时复制目标清单
                g.items.push_back(std::move(r));
            };
            addGroup(FindGroup(g.groupsLaunch, key), false);
            addGroup(FindGroup(g.groupsKill, key), true);
            ParsedQuery pq = ParseExcludes(t);      // -关键字：本地过滤程序名/路径
            g.excludes = pq.excludes;
            std::wstring term = NormalizeSearchTerm(pq.positive);
            ArmHistoryClock(L"P");
            SearchPrograms(pq.positive);
            // 自学习结果同样参与普通搜索（覆盖不在程序索引里的条目）
            PrependLearnedRows(term);
            // 热度档统一重排：重度/高频/常用必须在最前，学习项也一起参与
            RerankByTier();
            if(g.items.empty()){
                if(g.startingUp)AddHint(L"正在加载应用…");
                else if(s_programCatalogStats.truncated)AddHint(L"应用列表尚未完整，可稍后刷新");
                else if(!s_programCatalogStats.errors.empty())AddHint(L"部分应用入口暂时无法读取，可刷新重试");
            }
            LogHistoryQuery();   // 程序搜索在主线程同步完成，派发即有结果
        }
    }
    FinishRefreshLatency();
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
            std::wstring dir = ProgramWorkingDirectory(r.prog);
            ShellExecuteW(nullptr, L"open", r.action.c_str(), nullptr,
                          dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
            return true;
        }
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
                ShowTopToast(L"未找到可置顶的窗口");
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
            ShowTopToast(toast);
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
        LogHistoryOpen(g.items[g.sel]); // 台账记「这次实际点了哪条」，与权重分开：关了权重也留痕
        // 打开成功的轻量提示：淡入淡出自动消失（不抢焦点、不阻塞）
        std::wstring toast = OpenedToastText(g.items[g.sel]);
        if (!toast.empty()) ShowTopToast(toast);
        Hide();
    }
}

// 以管理员（提升权限）模式打开：仅对可执行项（程序 / 文件）有意义，使用 runas 动词提权
static bool ExecuteRowAdmin(Row& r) {
    std::wstring path, dir;
    if (r.kind == Row::Prog) {
        path = r.action;
        dir = ProgramWorkingDirectory(r.prog);
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
    // 深色主题→ForceDark(2)，浅色主题→AllowDark(1)
    int mode = (g.theme && g.theme->dark) ? 2 : 1;
    if (setMode) setMode(mode);
    if (flush) flush();
}

static bool RunRowMenuCommand(int cmd) {
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return false;
    Row& r = g.items[g.sel];
    bool done = true;
    switch (cmd) {
        case IDM_OPEN:
            RecordClickWeight(r);  // 右键打开同样记权重
            if (ExecuteRow(r)) {
                RecordLearned(r);  // 右键打开成功同样记自学习
                LogHistoryOpen(r);
                std::wstring toast = OpenedToastText(r);
                if (!toast.empty()) ShowTopToast(toast);
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
        case IDM_ADDALIAS: {
            // 先拷出目标再弹输入覆盖层：期间 g.items 可能被刷新改动
            std::wstring target = r.action, name = r.title;
            done = false;  // 加完别名留在启动器里，不自动隐藏
            // 输入覆盖层在渲染器里画，主进程只记住目标等回传
            g_slintAliasTarget = target;
            SlintSendCmd(slintbridge::kCmdHoldShow);
            SlintPushAliasPrompt(name);
            break;
        }
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
    return done;
}

// ---------------- 托盘 / 注册表 / 圆角 ----------------
static void ApplyRoundCorners(HWND h) {
    DWORD pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    COLORREF border = (g.theme ? g.theme->border : RGB(80, 80, 80));
    DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

// ---------------- 托盘 / 注册表 / 圆角 ----------------
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
    g.skipDotFolders = s.skipDotFolders;
    g.dfBackend = s.dfBackend;
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
    g.historyEnabled = s.historyEnabled;
    g.historyMaxLines = s.historyMaxLines;
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
//   f/d 前缀（索引 文件/文件夹搜索）→ 只有前缀后的关键词参与权重
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
    return jt == it->second.end() ? 0 : jt->second.count;
}

// 到达条目上限时，淘汰全局权重最低的一条（跳过即将写入的新条目），为新记录腾位置
static void PruneOneMinWeight(const std::wstring& keepTerm, const std::wstring& keepPath) {
    std::wstring minTerm, minPath;
    int minW = INT_MAX;
    for (auto& kv : g.weights) {
        for (auto& pv : kv.second) {
            if (kv.first == keepTerm && pv.first == keepPath) continue;
            if (pv.second.count < minW) {
                minW = pv.second.count;
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
    MarkUsageDirty();
}

// 记录一次点击权重（仅本地文件 / 文件夹 / 程序条目）
static void RecordClickWeight(const Row& r) {
    if (!g.weightEnabled) return;
    if (r.kind != Row::File && r.kind != Row::Folder && r.kind != Row::Prog) return;
    std::wstring term;
    if (!WeightTermFromInput(term)) return;  // 无效词或命中不保存的前缀（如 gg）：完全不记录
    std::wstring path = r.action;           // 完整绝对路径作为条目唯一标识
    if (path.empty()) return;

    const long long now = NowUnix();
    auto& m = g.weights[term];
    auto it = m.find(path);
    if (it == m.end()) {
        if (g.weightCount >= (size_t)(std::max)(1, g.weightMaxEntries))
            PruneOneMinWeight(term, path);
        m[path] = WeightVal(1, now);
        ++g.weightCount;
    } else {
        ++it->second.count;
        it->second.last = now;  // 最近使用时间随每次点击刷新
    }
    g.weightsDirty = true;
    MarkUsageDirty();

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

    // 4 列：term <US> path <US> count <US> last（last=0 表示旧数据时间未知）
    struct E { const std::wstring* term; const std::wstring* path; int w; long long last; };
    std::vector<E> all;
    all.reserve(g.weightCount);
    for (auto& kv : g.weights)
        for (auto& pv : kv.second) all.push_back({&kv.first, &pv.first, pv.second.count, pv.second.last});
    std::sort(all.begin(), all.end(), [](const E& a, const E& b) {
        if (a.w != b.w) return a.w > b.w;
        return a.last > b.last;
    });
    size_t cap = (size_t)(std::max)(1, g.weightMaxEntries);
    if (all.size() > cap) all.resize(cap);

    std::wstring text;
    for (auto& e : all) {
        text += *e.term; text += kWeightSep;
        text += *e.path; text += kWeightSep;
        text += std::to_wstring(e.w); text += kWeightSep;
        text += std::to_wstring(e.last);
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
    MarkUsageDirty();
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
        // 兼容新旧两种格式：旧 3 列 term<US>path<US>count（无 last）；
        // 新 4 列再带一个 <US>last。缺少/非法 last 一律记 0（时间未知，不加分）。
        std::wstring rest = line.substr(s2 + 1);
        size_t s3 = rest.find(kWeightSep);
        int w = _wtoi(rest.c_str());
        long long last = 0;
        if (s3 != std::wstring::npos) {
            last = _wcstoi64(rest.c_str() + s3 + 1, nullptr, 10);
            rest = rest.substr(0, s3);
            w = _wtoi(rest.c_str());
        }
        if (term.empty() || path.empty() || w <= 0) continue;
        if (g.weights[term].emplace(path, WeightVal(w, last)).second) ++g.weightCount;
    }
}

// 清空全部权重（设置页两步确认后调用）：立即写空文件
static void ClearClickWeights() {
    g.weights.clear();
    g.weightCount = 0;
    g.weightsDirty = true;
    MarkUsageDirty();
    SaveClickWeightsNow();
}

// ---------------- 搜索历史台账 ----------------
// 一轮查询（防抖后真正派发的那一轮）攒一条 Q 行：首屏延迟、各层给了几条、总耗时、第一行结果；
// 每次成功打开本地条目写一条 O 行。打完字静置 kHistFlushMs 才落盘，逐字打字不重复记。
// 台账只用于事后检查，不参与排序；rows=0 和带「未回」的行最值钱——搜不出来的现场只有这里留得下来。
static size_t HistoryRowCount() {
    size_t n = 0;
    for (const auto& r : g.items)
        if (r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog) ++n;
    return n;
}

static std::wstring HistoryTopPath() {
    for (const auto& r : g.items)
        if (r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog) return r.action;
    return std::wstring();
}

static bool IsPrefixOf(const std::wstring& a, const std::wstring& b) {
    return a.size() <= b.size() && std::equal(a.begin(), a.end(), b.begin());
}

// 派发时刻复位本轮计数（Q 行的时间基准从这里起算）
static void ArmHistoryClock(const WCHAR* layer) {
    if (!g.qPendingLine.empty()) {
        // 同层、且新旧输入互为前缀 = 用户还在接着打字：上一条作废，
        // 只留他真正看到结果的最后一轮（程序搜索逐字同步出结果，否则一个字落一行）。
        bool cont = g.qPendingLayer == layer &&
                    (IsPrefixOf(g.qPendingText, g.text) || IsPrefixOf(g.text, g.qPendingText));
        // 没报齐、又不足 1 秒：他还在打字，这轮不值得留（卡住的那类交给下面的落盘时机）
        bool draft = !g.qLogged && GetTickCount64() - g.qT0 < 1000;
        if (cont || draft) {
            g.qPendingLine.clear();
            KillTimer(g.hwnd, kTimerHistFlush);
        } else {
            FlushHistoryQuery();   // 换了查询：上一轮先写完再起新一轮，文件里顺序不颠倒
        }
    }
    g.qLayer = layer;
    g.qText = g.text;
    g.qT0 = GetTickCount64();
    g.qFirstMs = -1;
    g.qSettledMs = -1;
    g.qLogged = false;
    g.qMissed.clear();
    g.qWsRows = 0;
}

// 结果第一次上屏：记下首屏延迟（「秒出」要的就是这个数，不是总耗时）
static void NoteHistoryFirstPaint() {
    if (g.qFirstMs >= 0 || HistoryRowCount() == 0) return;
    g.qFirstMs = (int)(GetTickCount64() - g.qT0);
}

// 本轮还没报齐的层（只有 d/f 这条路是分层的；其余模式返回空）
static std::wstring HistoryMissingLayers() {
    if (g.mode != Mode::Files) return std::wstring();
    std::wstring m;
    if (g.searchRoute == ft::search::Route::WindowsSearch && !g.dfWsDone) m = L"WS";
    return m;
}

// 用「此刻的现场」拼一行：rows/top 必须在结果上屏时就拍快照，
// 等落盘时再读 g.items 会读到下一轮的空列表。
static std::wstring BuildHistoryLine(const std::wstring& missed) {
    ft::hist::QueryLine q;
    q.ts = ft::hist::NowText();
    q.layer = g.qLayer;
    q.query = g.qText;
    q.firstMs = g.qFirstMs;
    q.totalMs = (int)(GetTickCount64() - g.qT0);
    q.rows = (int)HistoryRowCount();
    q.wsRows = g.qWsRows;
    q.top = HistoryTopPath();
    q.missed = missed;
    return ft::hist::FormatQueryLine(q);
}

// 本轮成型：先攒着，静置 kHistFlushMs 后落盘（时间戳取成型那一刻，不是落盘那一刻）
static void LogHistoryQuery() {
    if (!g.historyEnabled || g.qLogged) return;
    g.qLogged = true;
    NoteHistoryFirstPaint();
    g.qPendingLine = BuildHistoryLine(g.qMissed);
    g.qPendingText = g.qText;
    g.qPendingLayer = g.qLayer;
    SetTimer(g.hwnd, kTimerHistFlush, kHistFlushMs, nullptr);
}

// 层还没报齐：每次上屏都把现场重攒一遍（不 arm 计时器——一轮真正打完字才值得落一条），
// 换查询（ArmHistoryClock）或退出（WM_DESTROY）时由 FlushHistoryQuery 把最后这版写出去。
static void SnapshotHistoryPending() {
    if (!g.historyEnabled || g.qLogged) return;
    g.qPendingLine = BuildHistoryLine(g.qMissed);
    g.qPendingText = g.qText;
    g.qPendingLayer = g.qLayer;
}

static void FlushHistoryQuery() {
    if (g.qPendingLine.empty()) return;
    std::wstring line = g.qPendingLine;
    g.qPendingLine.clear();
    KillTimer(g.hwnd, kTimerHistFlush);
    if (!g.historyEnabled) return;   // 关掉了就把攒着的丢掉，不留到下次开机
    ft::hist::Write(line, (size_t)g.historyMaxLines);
}

// 与 RecordClickWeight 同一批调用点：只记本地文件/文件夹/程序条目
static void LogHistoryOpen(const Row& r) {
    if (!g.historyEnabled) return;
    if (r.kind != Row::File && r.kind != Row::Folder && r.kind != Row::Prog) return;
    FlushHistoryQuery();   // Q 行先落，O 行紧跟：事后读台账才看得出「这条是哪一轮搜出来的」
    ft::hist::Write(ft::hist::FormatOpenLine(ft::hist::NowText(), g.qLayer, g.qText, r.action),
                    (size_t)g.historyMaxLines);
}

// ---------------- f/d 自学习（打开即记忆，下次同词搜索直接置顶） ----------------
// 用户在 f / d 前缀搜索里成功打开一个文件/文件夹后，把「本次有效搜索词 → 完整路径」
// 记为长期记忆（learned.dat），并在 %APPDATA%\Flowtary\learned 镜像一个 .lnk 快捷方式。
// 之后敲同一词（或它的前缀）时，学习项无视 索引 是否命中，直接置顶展示：
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
    MarkUsageDirty();
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
//   - f/d（索引）模式下的本地文件/文件夹行 → 词 = f/d 前缀后的有效搜索词
//   - 普通程序搜索模式下的程序行（元宝这类不在开始菜单索引里的程序，就是从这学的）
//     → 词 = 整条输入剥离 -关键字 后的正向词；范围仍受 learnScope 控制
static void RecordLearned(const Row& r) {
    if (!g.learnEnabled) return;
    std::wstring term;
    if (g.mode == Mode::Files && (r.kind == Row::File || r.kind == Row::Folder)) {
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
    // 程序快捷方式（.lnk）与 exe 等价：开始菜单里绝大多数入口都是 lnk，
    // 旧逻辑只认 exe，导致"经常从 lnk 启动的程序"永远学不进记忆。
    bool isProgramLink = !isFolder && EndsWithI(path, L".lnk");
    if (g.learnScope == 0 && !isExe) return;  // 「仅 exe」维持原义
    if (g.learnScope == 1 && !isExe && !isFolder && !isProgramLink) return;

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
    MarkUsageDirty();
    ScheduleLearnedSave();
}

// 给条目加一个别名：在自学习目录里建「别名.lnk」，并登记为学习项（与自学习结果同一套数据）。
// 之后搜别名（或其前缀）即可直接置顶打开。
// 真正落库：把「别名 → 目标」登记为学习项并镜像 .lnk。
// Slint 覆盖层提交后走到这里，保存逻辑只有这一份。
static bool ApplyAliasForRow(const std::wstring& target, const std::wstring& name) {
    if (target.empty()) return false;
    std::wstring term = NormalizeSearchTerm(name);
    // 别名会被当成搜索词：路径/百分号形式学了没意义，直接拒绝
    if (term.empty() || term.find(L'\\') != std::wstring::npos ||
        term.find(L'/') != std::wstring::npos || term.find(L'%') != std::wstring::npos)
        return false;

    auto& m = g.learned[term];
    auto it = m.find(target);
    long long now = NowUnix();
    if (it == m.end()) {
        if (g.learnCount >= (size_t)(std::max)(10, g.learnMaxEntries))
            PruneOneLearned(term, target);
        LearnVal v;
        v.count = 1;
        v.last = now;
        it = m.emplace(target, v).first;
        ++g.learnCount;
    } else {
        it->second.last = now;
    }

    // 快捷方式用别名本身命名；同名已被其他条目占用时追加 " (2)" 序号
    std::wstring base = SanitizeTermForFile(name);
    auto usedByOther = [&](const std::wstring& n) {
        if (n.empty()) return false;
        for (auto& kv : g.learned)
            for (auto& pv : kv.second) {
                if (&pv.second == &it->second) continue;
                if (!_wcsicmp(pv.second.lnk.c_str(), n.c_str())) return true;
            }
        return false;
    };
    std::wstring lnk = base + L".lnk";
    for (int i = 2; usedByOther(lnk) && i < 10000; ++i)
        lnk = base + L" (" + std::to_wstring(i) + L").lnk";

    std::wstring dir = LearnedDirPath();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring full = dir + L"\\" + lnk;
    if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES ||
        _wcsicmp(ResolveLnkTarget(full).c_str(), target.c_str()) != 0) {
        if (!CreateShortcutFile(full, target, L"Flowtary 别名：" + name)) {
            // 快捷方式没建出来就不留半条新数据
            if (it->second.lnk.empty()) {
                m.erase(it);
                if (g.learnCount > 0) --g.learnCount;
                if (m.empty()) g.learned.erase(term);
            }
            return false;
        }
    }
    if (!it->second.lnk.empty() && _wcsicmp(it->second.lnk.c_str(), lnk.c_str()) != 0)
        DeleteFileW((dir + L"\\" + it->second.lnk).c_str());  // 旧镜像让位给新别名
    it->second.lnk = lnk;
    g.learnedDirty = true;
    MarkUsageDirty();
    SaveLearnedNow();  // 立即落盘：别名马上可用，也和目录里的 .lnk 保持一致
    ShowTopToast(L"已添加别名：" + name);
    return true;
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
            MarkUsageDirty();
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
    MarkUsageDirty();  // 下面整段都会增删学习项，标记全局使用档案失效

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
    MarkUsageDirty();
    SaveLearnedNow();
}

// 把命中的学习项插到结果最前面（exe 优先；与当前结果按路径去重后重新置顶）
static void PrependLearnedRows(const std::wstring& term) {
    if (!g.learnEnabled || term.empty()) return;
    struct Cand {
        std::wstring path;
        int count;
        long long last;
        int rank;     // 学习词与查询词的匹配等级：精确 > 学习词前缀 > 查询词前缀
        bool isExe;
        bool isDir;
    };
    std::vector<Cand> cs;
    for (auto& kv : g.learned) {
        int rank = ft::search::LearnedTermMatchRank(kv.first, term);
        if (rank < 0) continue;
        for (auto& pv : kv.second) {
            if (!g.excludes.empty() && HasExcludeTerm(ToLowerW(pv.first))) continue;  // -关键字：学习项路径含排除词也不置顶
            DWORD attr = GetFileAttributesW(pv.first.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES) continue;  // 目标暂不在：本次不展示
            bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
            cs.push_back({pv.first, pv.second.count, pv.second.last, rank,
                          !isDir && EndsWithI(pv.first, L".exe"), isDir});
        }
    }
    if (cs.empty()) return;
    std::sort(cs.begin(), cs.end(), [](const Cand& a, const Cand& b) {
        if (a.rank != b.rank) return a.rank < b.rank;   // 匹配等级优先（精确 > 各类前缀）
        if (a.isExe != b.isExe) return a.isExe;   // exe 优先
        if (a.count != b.count) return a.count > b.count;
        return a.last > b.last;
    });
    size_t n = (std::min)(cs.size(), (size_t)10);
    for (size_t i = 0; i < n; ++i) {
        const Cand& c = cs[i];
        // 同一目标已在结果里（索引 命中或权重重排）：摘掉原行，改由学习位置顶
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
        if (!c.isDir && EndsWithI(r.title, L".lnk")) r.title.resize(r.title.size() - 4);  // 程序快捷方式显示应用名
        r.sub = ShortLocationLabel(c.path);
        r.action = c.path;
        // 插在「一键组」行之后（显式关键字命中的组动作保持最高位）
        size_t pos = 0;
        while (pos < g.items.size() && g.items[pos].kind == Row::Group) ++pos;
        g.items.insert(g.items.begin() + pos, std::move(r));
    }
}

// 热度档统一重排：把「重度/高频/常用」档的行整体提到前面，档内保持原顺序。
// 只在可打分的行（程序/文件/文件夹）之间换位——组、网页、提示、直达项留在原位，
// 免得「一键启动」这类显式动作被热度挤下去。
// 必须排这一步的原因：自学习项（PrependLearnedRows）总是插在最前，
// 只用过一两次的学习项会硬压过热得多的重度项——这正是「重度没排在最上面」的另一半。
static void RerankByTier() {
    // slots：可打分行的原始位置（升序）。写回时必须按这个原始顺序落位，
    // 否则「排好序的行 → 排序后的 slots」会互相抵消（恒等置换，等于没排）。
    std::vector<size_t> slots;
    for (size_t i = 0; i < g.items.size(); ++i) {
        Row::Kind k = g.items[i].kind;
        if (k == Row::Prog || k == Row::File || k == Row::Folder) slots.push_back(i);
    }
    if (slots.size() <= 1) return;
    const auto& globalUse = GlobalUsage();
    auto tierOf = [&](const Row& r) -> int {
        auto it = globalUse.find(ft::search::AppIdentityKey(r.action));
        if (it == globalUse.end()) return 0;
        return UsageTier(it->second.count);
    };
    // 排的是「行的先后」，不是「位置」：先按档位把行取出，再依次写回原始坑位。
    std::vector<size_t> order = slots;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return tierOf(g.items[a]) > tierOf(g.items[b]);
    });
    std::vector<Row> picked;
    picked.reserve(order.size());
    for (size_t idx : order) picked.push_back(g.items[idx]);
    for (size_t i = 0; i < slots.size(); ++i) g.items[slots[i]] = std::move(picked[i]);
}

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
// 设置窗 sidecar（flowtary-settings.exe，Slint）
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
    ApplyTheme();
}

// 实时预览：只写内存，一个字节都不落盘。
static void ApplySettingsPreview(const ipc::Preview& p) {
    g.centerWake = p.centerWake != FALSE;
    g.hotkeyMode = p.hotkeyMode;
    g.skipDotFolders = p.skipDotFolders != FALSE;
    g.dfBackend = p.dfBackend;
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
    ApplyTheme();  // 重建画刷/字体/星点 + 暗色菜单 + 主窗 Layout/Repaint
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
    LayoutAndRepaint();
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
        LayoutAndRepaint();
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
            LayoutAndRepaint();
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
        TrayBalloon(L"Flowtary", L"未找到设置程序（与主程序同目录）");
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

// Alt+数字/字母 —— 渲染器转发过来的版本。
// GetKeyState 是线程级的：渲染器进程里按着 Alt，本进程查不到，
// 所以修饰键由渲染器侧判断（Ctrl 组合在那边就拦掉了），这里只按键位判定。
static bool TryAltHotkeyDirect(WPARAM wParam) {
    if (g.hotkeyMode == 2) return false;
    WCHAR k = 0;
    if (g.hotkeyMode == 0) {
        if (wParam >= '0' && wParam <= '9') k = (WCHAR)wParam;
        else if (wParam >= VK_NUMPAD0 && wParam <= VK_NUMPAD9)
            k = (WCHAR)('0' + (wParam - VK_NUMPAD0));
    } else if (g.hotkeyMode == 1) {
        if (wParam >= 'A' && wParam <= 'Z') k = (WCHAR)wParam;
        else if (wParam >= 'a' && wParam <= 'z') k = (WCHAR)(wParam - 32);
    }
    if (!k) return false;
    for (int i = 0; i < (int)g.items.size(); ++i) {
        if (HotkeyChar(i) == k) {
            g.sel = i;
            ExecuteSelected();
            return true;
        }
    }
    return true;  // Alt+键但无匹配项：吞掉，避免系统蜂鸣
}

// ---------------- Slint 渲染器（大脑 ↔ 皮，共享内存桥） ----------------
// 渲染器把用户意图写进请求环，主进程消费后把结果写进状态槽；
// 两侧只用 PostMessage 做「有更新了」的纯通知，因此不存在互等 SendMessage 的死锁。

static void SlintBridgeEnsure() {
    if (g.bridge) return;
    std::wstring path = slintbridge::BridgeFilePath();
    if (path.empty()) {
        AppendTrace("SlintBridge: no bridge path");
        return;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        AppendTrace("SlintBridge: open bridge file failed");
        return;
    }
    LARGE_INTEGER sz{};
    sz.QuadPart = (LONGLONG)sizeof(slintbridge::Bridge);
    SetFilePointerEx(f, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(f);
    HANDLE h = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0,
                                  (DWORD)sizeof(slintbridge::Bridge), nullptr);
    if (!h) {
        CloseHandle(f);
        AppendTrace("SlintBridge: CreateFileMapping failed");
        return;
    }
    g.bridge = (slintbridge::Bridge*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sizeof(slintbridge::Bridge));
    CloseHandle(h);
    CloseHandle(f);
    if (!g.bridge) {
        AppendTrace("SlintBridge: MapView failed");
        return;
    }
    // 启动时没有渲染器在跑：清空上次运行遗留的请求/命令环
    if (!SlintIpcWindow()) memset(g.bridge, 0, sizeof(slintbridge::Bridge));
    AppendTrace("SlintBridge: mapped");
}

// 渲染器的 IPC 窗口：大脑用它定位渲染器 + 通知「状态更新了」
static HWND SlintIpcWindow() {
    return FindWindowW(slintbridge::kIpcClass, nullptr);
}

// 渲染器的 Slint 界面窗口（标题固定）：用于判断「启动器当前开着没有」
static HWND SlintUiWindow() {
    return FindWindowW(nullptr, L"flowtary-slint");
}

// 启动器当前是否处于打开状态（渲染器界面窗口可见）
static bool LauncherVisible() {
    HWND ui = SlintUiWindow();
    return ui && IsWindowVisible(ui);
}

// 主题调色板 → 桥：COLORREF 是 0x00BBGGRR，附 8 位 alpha 方便渲染器直接当 #RRGGBBAA 用
static unsigned SlintColorWithAlpha(COLORREF c, int a) {
    return (unsigned)GetRValue(c) | ((unsigned)GetGValue(c) << 8) |
           ((unsigned)GetBValue(c) << 16) | ((unsigned)(a & 0xFF) << 24);
}
static unsigned SlintColorScale(COLORREF c, double k) {
    int r = (int)(GetRValue(c) * k), g = (int)(GetGValue(c) * k), b = (int)(GetBValue(c) * k);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (unsigned)r | ((unsigned)g << 8) | ((unsigned)b << 16);
}
static void SlintPushTheme() {
    if (!g.bridge || !g.theme) return;
    const Theme& t = *g.theme;
    slintbridge::ThemeWire w{};
    w.panelTop = SlintColorWithAlpha(t.bg, t.alpha);
    w.panelBottom = SlintColorWithAlpha(t.bg2, t.alpha);
    w.glow = SlintColorWithAlpha(t.accent, 0x12);
    w.menu = SlintColorWithAlpha(t.menuBg, 0xF5);
    w.input = t.editBg;
    w.text = t.textTitle;
    w.sub = t.sub;
    w.accent = t.accent;
    w.accent2 = t.accent2;
    w.accentDark = SlintColorScale(t.accent, 0.72);
    w.selA = SlintColorWithAlpha(t.accent, 0x40);
    w.selB = SlintColorWithAlpha(t.accent2, 0x22);
    w.accentLine = SlintColorWithAlpha(t.accent, 0x55);
    w.accentBorder = SlintColorWithAlpha(t.accent2, 0x66);
    w.accentSoft = SlintColorWithAlpha(t.accent, 0x1F);
    const int lum = (GetRValue(t.accent) * 299 + GetGValue(t.accent) * 587 +
                     GetBValue(t.accent) * 114) / 1000;
    w.onAccent = lum > 150 ? 0x000000u : 0xFFFFFFu;
    // 液态玻璃至少 20px 圆角：主题微调里常驻的 10px 太生硬，允许继续往大调
    w.radius = t.radiusWindow < 20 ? 20 : t.radiusWindow;
    w.seq = (unsigned)InterlockedIncrement((volatile LONG*)&g.bridge->themeSeq);
    g.bridge->theme = w;
}

// 行区序列化：把 g.items 的可见行打包成「RowHead + utf8 字段」序列
static void SlintBuildRows(std::vector<char>& out) {
    int n = (int)((std::min)((size_t)kMaxVisibleResults, g.items.size()));
    for (int i = 0; i < n; ++i) {
        const Row& r = g.items[i];
        std::string title = WideToUtf8(r.title);
        std::string sub = WideToUtf8(r.sub);
        std::string icon;
        switch (r.kind) {
            case Row::Prog:
                if (r.prog) icon = WideToUtf8(r.prog->path);
                break;
            case Row::File:
            case Row::Folder:
            case Row::Reveal:
                icon = WideToUtf8(r.action);
                break;
            default:
                break;
        }
        slintbridge::RowHead h {};
        h.kind = (int)r.kind;
        h.hotkey = (int)HotkeyChar(i);
        h.usage = r.usage;
        h.titleLen = (unsigned)title.size();
        h.subLen = (unsigned)sub.size();
        h.iconLen = (unsigned)icon.size();
        out.insert(out.end(), (const char*)&h, (const char*)&h + sizeof(h));
        out.insert(out.end(), title.begin(), title.end());
        out.insert(out.end(), sub.begin(), sub.end());
        out.insert(out.end(), icon.begin(), icon.end());
    }
}

// 大脑 → 渲染器：下发完整状态（文本 + 行列表）
static void SlintPushState() {
    SlintBridgeEnsure();
    if (!g.bridge) return;
    SlintPushTheme();  // 主题与状态一起下发（换主题/微调都会走到这里）
    LONG seq = g.bridge->stateSeq + 1;
    slintbridge::StateSlot& slot = g.bridge->state[seq % slintbridge::kStateSlots];
    std::string text = WideToUtf8(g.text);
    std::vector<char> rows;
    SlintBuildRows(rows);
    if (text.size() > (size_t)slintbridge::kTextMax) text.resize(slintbridge::kTextMax);
    if (rows.size() > (size_t)slintbridge::kRowsMax) rows.resize(slintbridge::kRowsMax);
    slot.selected = g.sel;
    slot.hotkeyMode = g.hotkeyMode;
    slot.rowCount = (int)((std::min)((size_t)kMaxVisibleResults, g.items.size()));
    slot.centerWake = g.centerWake ? 1 : 0;
    slot.textLen = (unsigned)text.size();
    slot.rowsLen = (unsigned)rows.size();
    if (!text.empty()) memcpy(slot.payload, text.data(), text.size());
    if (!rows.empty()) memcpy(slot.payload + text.size(), rows.data(), rows.size());
    slot.seq = (unsigned)seq;
    InterlockedExchange(&g.bridge->stateSeq, seq);  // 写完负载再发布（渲染器按 seq 取槽）
    if (HWND ipc = SlintIpcWindow()) PostMessageW(ipc, slintbridge::kKickToRender, 0, 0);
}

// 大脑 → 渲染器：投递一条命令（显示 / 隐藏 / 退出）
static void SlintSendCmd(int cmd) {
    SlintBridgeEnsure();
    if (!g.bridge) return;
    if (cmd == slintbridge::kCmdShow) {
        // 唤起时主进程刚收到热键，拥有前台授权；把它转授给渲染器，
        // 否则渲染器 SetForegroundWindow 会被系统前台锁拒绝（窗口显示但打不了字）。
        if (HWND ui = SlintUiWindow()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(ui, &pid);
            if (pid) AllowSetForegroundWindow(pid);
        }
    }
    LONG head = g.bridge->cmdHead;
    LONG tail = g.bridge->cmdTail;
    if (head - tail >= (LONG)(sizeof(g.bridge->cmd) / sizeof(g.bridge->cmd[0]))) return;  // 满了就丢
    g.bridge->cmd[head % (LONG)(sizeof(g.bridge->cmd) / sizeof(g.bridge->cmd[0]))] = cmd;
    InterlockedExchange(&g.bridge->cmdHead, head + 1);
    HWND ipc = SlintIpcWindow();
    {
        char tb[96];
        snprintf(tb, sizeof(tb), "KICK: cmd=%d ipc=%p", cmd, (void*)ipc);
        AppendTrace(tb);
    }
    if (ipc) PostMessageW(ipc, slintbridge::kKickToRender, 0, 0);
}

// 大脑 → 渲染器：把一段文本写进单段文本槽并发布序号
static void SlintWriteTextSlot(volatile LONG* seq, slintbridge::TextSlot& slot,
                               const std::string& text) {
    if (!g.bridge) return;
    size_t n = text.size();
    if (n > (size_t)slintbridge::kTextSlotMax - 1) n = (size_t)slintbridge::kTextSlotMax - 1;
    LONG s = *seq + 1;
    slot.len = (unsigned)n;
    if (n) memcpy(slot.text, text.data(), n);
    slot.text[n] = 0;
    slot.seq = (unsigned)s;
    InterlockedExchange(seq, s);  // 先写内容再发布
}

// 让渲染器弹出别名输入覆盖层（预填默认名）
static void SlintPushAliasPrompt(const std::wstring& defaultName) {
    SlintBridgeEnsure();
    if (!g.bridge) return;
    SlintWriteTextSlot(&g.bridge->aliasSeq, g.bridge->alias, WideToUtf8(defaultName));
    SlintSendCmd(slintbridge::kCmdAliasPrompt);
}

// 当前菜单锚定的行：菜单开着时结果可能被别处刷新，执行命令时回到这一行
static int g_slintMenuRow = -1;

// 把当前行的右键菜单命令写进桥，由渲染器用 Slint 自绘展示。
// 命令条件与执行逻辑都在 RunRowMenuCommand 这一份。
static void SlintPushRowMenu()
{
    if (!g.bridge) return;
    if (g.sel < 0 || g.sel >= (int)g.items.size()) return;
    const Row& r = g.items[g.sel];
    if (r.kind == Row::Hint) return;

    slintbridge::MenuSlot& m = g.bridge->menu;
    m.row = g.sel;
    m.count = 0;
    auto add = [&](int id, const wchar_t* label, bool def = false) {
        if (m.count >= slintbridge::kMenuMaxItems) return;
        slintbridge::MenuItemWire& it = m.items[m.count++];
        it.id = id;
        it.flags = def ? 1 : 0;
        int n = WideCharToMultiByte(CP_UTF8, 0, label, -1, it.label,
                                    slintbridge::kMenuLabelMax, nullptr, nullptr);
        it.labelLen = n > 0 ? (unsigned)(n - 1) : 0;
        if (n <= 0) it.label[0] = 0;
    };
    if (r.kind == Row::Window) {
        add(IDM_WIN_SWITCH, L"切换到此窗口", true);
        add(IDM_WIN_CLOSE, L"关闭窗口");
        add(IDM_WIN_KILL, L"结束进程");
        add(IDM_COPYTITLE, L"复制标题");
        add(IDM_COPYPROC, L"复制进程名");
    } else {
        add(IDM_OPEN, r.kind == Row::Reveal ? L"打开所在文件夹" : L"打开", true);
        if (r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog)
            add(IDM_OPENLOC, L"打开所在文件夹");
        if (r.kind == Row::Reveal) add(IDM_OPENFILE, L"打开文件");
        if ((r.kind == Row::File || r.kind == Row::Folder || r.kind == Row::Prog) &&
            GetFileAttributesW(r.action.c_str()) != INVALID_FILE_ATTRIBUTES)
            add(IDM_ADDALIAS, L"添加别名…");
        if (r.kind == Row::File || r.kind == Row::Prog)
            add(IDM_RUNAS, L"以管理员模式打开");
        if (r.kind == Row::Shell) add(IDM_SHELL_ADMIN, L"以管理员运行");
        if (r.kind == Row::Folder || r.kind == Row::File || r.kind == Row::Reveal ||
            r.kind == Row::Prog)
            add(IDM_OPENCMD, r.kind == Row::Folder ? L"用此路径打开命令行"
                                                   : L"在所在目录打开命令行");
        add(IDM_COPYPATH, r.kind == Row::Web ? L"复制链接"
                          : r.kind == Row::Shell ? L"复制命令" : L"复制路径");
    }
    if (!m.count) return;
    g_slintMenuRow = m.row;
    unsigned seq = ++m.seq;
    InterlockedExchange(&g.bridge->menuSeq, (LONG)seq);  // 先写数据再发布序号
    SlintSendCmd(slintbridge::kCmdMenuShow);
}

// 大脑消费渲染器的请求环
static void SlintHandleRequests() {
    SlintBridgeEnsure();   // 首次唤起时映射可能还没建（渲染器先建也一样）
    if (!g.bridge) return;
    LONG head = InterlockedCompareExchange(&g.bridge->reqHead, 0, 0);
    LONG tail = g.bridge->reqTail;
    bool needPush = false;
    bool showAfterPush = false;
    while (tail < head) {
        const slintbridge::Req& r = g.bridge->req[tail % slintbridge::kReqRingSize];
        switch (r.op) {
            case slintbridge::kReqText: {
                unsigned n = r.textLen;
                if (n > (unsigned)slintbridge::kTextMax) n = (unsigned)slintbridge::kTextMax;
                std::wstring w = Utf8ToWide(std::string(r.text, r.text + n));
                if (g.text != w) g.text = w;
                Refresh();  // 重新出结果（内部会推状态）
                needPush = true;
                break;
            }
            case slintbridge::kReqNav: {
                if (!g.items.empty()) {
                    int n = (int)g.items.size();
                    g.sel = (g.sel + r.arg + n) % n;
                    needPush = true;
                }
                break;
            }
            case slintbridge::kReqActivate: {
                int idx = r.arg & slintbridge::kActRowMask;
                bool ctrl = (r.arg & slintbridge::kActCtrl) != 0;
                bool shift = (r.arg & slintbridge::kActShift) != 0;
                {
                    char tb[96];
                    snprintf(tb, sizeof(tb), "EXEC: row=%d ctrl=%d shift=%d", idx, (int)ctrl, (int)shift);
                    AppendTrace(tb);
                }
                if (idx >= 0 && idx < (int)g.items.size()) g.sel = idx;
                // 与 GDI 版一致：Ctrl=关闭 / Ctrl+Shift=提权（窗口行另有语义）
                ExecuteSelected(ResolveExecKind(ctrl, shift));
                break;
            }
            case slintbridge::kReqRowMenu: {
                int idx = r.arg;
                if (idx >= 0 && idx < (int)g.items.size()) {
                    g.sel = idx;
                    SlintPushState();  // 先把高亮挪到右键那一行
                }
                SlintPushRowMenu();    // 命令表写进桥，渲染器用 Slint 自绘菜单
                break;
            }
            case slintbridge::kReqMenuPick: {
                // 菜单选择：与 GDI 原生菜单共用同一份命令执行逻辑
                if (g_slintMenuRow >= 0 && g_slintMenuRow < (int)g.items.size())
                    g.sel = g_slintMenuRow;
                g_slintMenuRow = -1;
                if (RunRowMenuCommand(r.arg)) Hide();
                break;
            }
            case slintbridge::kReqMenuDismiss:
                g_slintMenuRow = -1;
                break;
            case slintbridge::kReqAliasCommit: {
                unsigned n = r.textLen;
                if (n > (unsigned)slintbridge::kTextMax) n = (unsigned)slintbridge::kTextMax;
                std::wstring name = Utf8ToWide(std::string(r.text, r.text + n));
                std::wstring target = g_slintAliasTarget;
                g_slintAliasTarget.clear();
                SlintSendCmd(slintbridge::kCmdReleaseShow);
                ApplyAliasForRow(target, name);  // 校验 + 落库 + toast 都在这份逻辑里
                needPush = true;                 // 别名立刻参与排序
                break;
            }
            case slintbridge::kReqAliasCancel:
                g_slintAliasTarget.clear();
                SlintSendCmd(slintbridge::kCmdReleaseShow);
                break;
            case slintbridge::kReqHide:
                Hide();
                break;
            case slintbridge::kReqAltKey:
                TryAltHotkeyDirect((WPARAM)r.arg);
                break;
            case slintbridge::kReqReady:
                // 渲染器刚起来：先下发状态，再让它显示（避免旧结果闪一下）
                AppendTrace("REQ: renderer ready");
                needPush = true;
                showAfterPush = true;
                break;
        }
        ++tail;
    }
    InterlockedExchange(&g.bridge->reqTail, tail);
    if (needPush) SlintPushState();
    if (showAfterPush) SlintSendCmd(slintbridge::kCmdShow);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g.msgTaskbarCreated && msg == g.msgTaskbarCreated) {
        TrayAdd();  // Explorer 重启后恢复托盘图标
        return 0;
    }
    switch (msg) {
        case WM_CREATE: {
            // 字体由字体池提供（见 UpdateScale / ApplyTheme）
            return 0;
        }
        case WM_DESTROY:
            AppendTrace("WM_DESTROY (main window)");
            StopProgramsRefresh(hwnd);
            FlushHistoryQuery();    // 退出前把攒着的最后一轮查询落盘（含没报齐就退出的那类现场）
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
            for (int i = 0; i < kFontMax - kFontMin + 1; ++i) {
                if (g.fontPool[i]) { DeleteObject(g.fontPool[i]); g.fontPool[i] = nullptr; }
            }
            SlintSendCmd(slintbridge::kCmdQuit);  // 通知 Slint 渲染器自行退出
            PostQuitMessage(0);
            return 0;

        case WM_HOTKEY:
            if (wParam == 1) {
                // 输入框聚焦时屏蔽已注册的 Alt 热键（Alt+Space / Alt+Q / Ctrl+Alt+Space），
                // 使其不再隐藏/切换窗口。启动器窗口属于渲染器进程，用前台窗口判断。
                bool launcherFocused = (GetForegroundWindow() == SlintUiWindow());
                if (launcherFocused) {
                    AppendTrace("HOTKEY swallowed: launcher focused while hotkey fired");
                    return 0;
                }
                AppendTrace(IsWindowVisible(hwnd) ? "HOTKEY toggle -> hide" : "HOTKEY toggle -> show");
                if (LauncherVisible()) Hide();
                else Show();
            } else if (wParam == 2) {
                // Ctrl+Alt+G：文件对话框激活时，把前台资源管理器当前目录同步到对话框
                fdj_sync_from_explorer();
            }
            return 0;

        case WM_TIMER:
            if (wParam == kTimerDebounce) {
                KillTimer(hwnd, kTimerDebounce);
                if (g.mode == Mode::Files) StartDfSearch();
            } else if (wParam == kTimerWeightSave) {
                KillTimer(hwnd, kTimerWeightSave);
                SaveClickWeightsNow();  // 延迟合并写盘到期：一次写出全部权重
                SaveLearnedNow();       // 自学习数据同步落盘
            } else if (wParam == kTimerHistFlush) {
                KillTimer(hwnd, kTimerHistFlush);
                FlushHistoryQuery();   // 编辑静置到期：把这一轮的 Q 行落盘
            } else if (wParam == kTimerBlink) {
                // 目录变更轮询（原来兼作光标闪烁定时器；主窗不再自绘，只保留轮询）
                if (s_programWatch.Poll()) StartProgramsRefresh();
            } else if (wParam == kTimerBalloon) {
                KillTimer(hwnd, kTimerBalloon);
                // 清除气泡文本，防止后续 NIM_MODIFY（如提示更新）再次弹出旧气泡
                g.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                g.nid.szInfo[0] = L'\0';
                Shell_NotifyIconW(NIM_MODIFY, &g.nid);
            } else if (wParam == kTimerLatency) {
                // 还在等：实时跳毫秒（跨阈值时底栏出现/长高，需要重新 Layout）；报齐就停。
                if (SearchPending()) LayoutAndRepaint();
                else KillTimer(hwnd, kTimerLatency);
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
                case IDM_REFRESH:
                    StartProgramsRefresh();
                    return 0;
                case IDM_SETTINGS:
                    OpenSettings();
                    return 0;
                case IDM_EXIT:
                    AppendTrace("tray menu EXIT clicked");
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;

        case WM_COPYDATA: {
            const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
            if (cds) HandleSettingsIpc(cds);  // 设置窗 sidecar
            return TRUE;
        }

        case slintbridge::kKickToBrain:
            // Slint 渲染器：请求环里有新请求（用户输入 / 键盘 / 激活行）
            SlintHandleRequests();
            return 0;

        case WM_APP_SETTINGS_GONE:
            // sidecar 进程结束（正常关闭 / 被杀 / 崩溃）：未提交就回退实时预览
            OnSettingsSessionEnded();
            return 0;

        case WM_APP_DF_READY:
            // Windows Search 结果就绪：按查询代数和当前后端过滤过期回复
            HandleDfReply((DfReply*)lParam);
            return 0;

        case WM_APP_PROGRAMS_READY: {
            // 工作线程扫描完成：在主线程接管结果（工作线程从不写 g.programs）
            ProgramBuildReply* p = (ProgramBuildReply*)lParam;
            if (p) {
                g.items.erase(std::remove_if(g.items.begin(),g.items.end(),[](const Row& r){return r.kind==Row::Prog;}),g.items.end());
                RestoreFrequentPrograms(*p,p->stats.truncated);  // 截断时用高频权重补回漏扫的程序
                g.programs = std::move(p->programs);
                s_programCatalogStats=std::move(p->stats);
                delete p;
            }
            s_programScanState->running=false;
            g.startingUp = false;
            TrayUpdateTip();  // 提示文案从「正在启动中」切回热键名
            if(g.mode==Mode::Programs)Refresh();
            else LayoutAndRepaint();
            return 0;
        }
        case WM_APP_APPS_CHANGED:
            s_programWatch.ShellNotification((HANDLE)wParam,(DWORD)lParam);
            return 0;

        case WM_APP_QUIT:
            AppendTrace("WM_APP_QUIT: takeover by another instance");
            // 新版本接管：干净退出（WM_DESTROY 会移除托盘、注销热键）
            DestroyWindow(hwnd);
            return 0;

        case WM_APP_TRAY: {
            UINT ev = (UINT)lParam;
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                if (LauncherVisible()) Hide();
                else Show();
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                SetForegroundWindow(hwnd);
                HMENU menu = CreatePopupMenu();
                // 标准 Windows 菜单，深色由 EnableDarkMenus 的 SetPreferredAppMode 施加；
                // 不能再走自绘/MIM_BACKGROUND（StyleDarkMenu），否则混出浅底深字。
                if (g.startingUp) {
                    // 扫描未完成：菜单照样弹出来（有响应），但只显示状态项与退出
                    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, 0,
                                (LPCWSTR)L"正在启动中…");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出(&X)");
                } else {
                    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置(&S)");
                    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"刷新缓存(&R)");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    // 唤起快捷键开关：文字随状态切换（开启→“关闭…”，关闭→“开启…”）
                    AppendMenuW(menu, MF_STRING, IDM_WAKE_HOTKEY,
                                g.hotkeyWake ? L"关闭唤起快捷键(&H)" : L"开启唤起快捷键(&H)");
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出(&X)");
                }
                POINT p;
                GetCursorPos(&p);
                int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                         p.x, p.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (cmd == IDM_SETTINGS) OpenSettings();
                else if (cmd == IDM_REFRESH) {
                    SendMessageW(hwnd, WM_COMMAND, IDM_REFRESH, 0);
                }
                else if (cmd == IDM_WAKE_HOTKEY) ToggleWakeHotkey();
                else if (cmd == IDM_EXIT) DestroyWindow(hwnd);
            }
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 主题化提示弹窗 ----------------
// 「重复启动」「检测到新版本」等场景使用：与主界面同款圆角 + 主题配色，
// 不受系统主题影响，也比系统 MessageBox 好看。
//
// 透明实现与 toast 一致：软件渲染的实色背景盖不住桌面，真正的透明靠
// DWM 亚克力。弹窗本身保持 layered（淡入动画要用），背景不填实色，
// 另起一层亚克力玻璃贴在弹窗正下方透出桌面。
enum NoticeAccentState { NOTICE_ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 };
struct NoticeAccentPolicy { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
struct NoticeCompositionData { DWORD Attrib; void* pvData; SIZE_T cbData; };
using NoticeSetCompositionFn = BOOL(WINAPI*)(HWND, NoticeCompositionData*);
static void notice_enable_acrylic(HWND hwnd, unsigned tint) {
    if (!hwnd) return;
    auto fn = reinterpret_cast<NoticeSetCompositionFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    NoticeAccentPolicy p {};
    p.AccentState = NOTICE_ACCENT_ENABLE_ACRYLICBLURBEHIND;
    p.AccentFlags = 2;
    p.GradientColor = tint;  // ABGR：跟随主题底色
    NoticeCompositionData d { 19, &p, sizeof(p) };
    fn(hwnd, &d);
}

static LRESULT CALLBACK NoticeGlassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;  // 不刷底色，让 DWM 亚克力透出来
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST: return HTTRANSPARENT;  // 点击穿透
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND ensure_notice_glass(unsigned tint) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = NoticeGlassProc;
        wc.hInstance = g.inst;
        wc.lpszClassName = L"FlowtaryNoticeGlass";
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return nullptr;
    }
    HWND glass = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        L"FlowtaryNoticeGlass", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g.inst, nullptr);
    if (!glass) return nullptr;
    notice_enable_acrylic(glass, tint);
    // 圆角形状交给 SetWindowRgn，禁掉 DWM 自己的圆角/边框，免得内侧多一圈弧线
    DWORD pref = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(glass, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(glass, DWMWA_BORDER_COLOR, &none, sizeof(none));
    return glass;
}

struct NoticeData {
    std::wstring title;
    std::wstring body;
    std::wstring btn;
    int baseX = 0, baseY = 0;   // 初始位置（淡入上移用）
    DWORD t0 = 0;               // 淡入起始时间
    HWND glass = nullptr;       // 背后的亚克力玻璃层
};
constexpr int IDC_NOTICE_OK = 5001;
constexpr int kNoticeW = 400, kNoticeH = 184;

static LRESULT CALLBACK NoticeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // 不擦背景，交给背后的亚克力层
        case WM_CREATE: {
            const NoticeData* nd = (const NoticeData*)((CREATESTRUCTW*)lp)->lpCreateParams;
            NoticeData* self = nd ? new NoticeData(*nd) : new NoticeData();
            if (self) self->t0 = GetTickCount();
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
            ApplyRoundCorners(h);
            if (self) {
                unsigned tint = (g.theme ? (g.theme->bg & 0x00FFFFFFu) : 0x0016181Cu)
                              | 0x8A000000u;
                self->glass = ensure_notice_glass(tint);
                if (self->glass) {
                    int w = S(kNoticeW), hh = S(kNoticeH);
                    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, hh + 1,
                                                      S(10) * 2, S(10) * 2))
                        SetWindowRgn(self->glass, rgn, TRUE);
                    SetWindowPos(self->glass, h, self->baseX, self->baseY + S(8),
                                 w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                }
            }
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
            // 背景不填实色：底色与磨砂由背后那层亚克力玻璃提供
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
                    SetLayeredWindowAttributes(h, 0x00000000, a2, LWA_COLORKEY | LWA_ALPHA);
                    SetWindowPos(h, nullptr, nd->baseX, nd->baseY + off, 0, 0,
                                 SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
                    // 玻璃层跟着弹窗的淡入位移走，保持严丝合缝
                    if (nd->glass) {
                        RECT wr;
                        if (GetWindowRect(h, &wr))
                            SetWindowPos(nd->glass, h, wr.left, wr.top, 0, 0,
                                         SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                    if (p >= 1) KillTimer(h, 2);
                }
                return 0;
            }
            KillTimer(h, 1);     // 自动关闭（wp==1）
            DestroyWindow(h);
            return 0;
        case WM_DESTROY: {
            NoticeData* nd = (NoticeData*)GetWindowLongPtrW(h, GWLP_USERDATA);
            if (nd && nd->glass) DestroyWindow(nd->glass);
            delete nd;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 显示弹窗并等待其关闭；autoCloseMs > 0 时到点自动关闭
static void ShowNotice(const std::wstring& title, const std::wstring& body,
                       const std::wstring& btn, int autoCloseMs) {
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
    // GDI 在 layered 窗上留下的未绘制区域是纯黑实底，用 COLORKEY 把它抠掉，
    // 才能让背后的亚克力玻璃透出来；alpha 仍由淡入定时器统一管理。
    SetLayeredWindowAttributes(h, 0x00000000, 0, LWA_COLORKEY | LWA_ALPHA);
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

// ---------------- 顶部 toast：主进程原生自绘，逐像素 alpha ----------------
// 1.3.28 及之前的方案有两层「假透明」，这里一并去掉：
//   1. Slint 软件渲染画不出 alpha → 只能画纯黑底 + LWA_COLORKEY 抠色，
//      边缘二值硬边（圆角锯齿、文字无 AA）；
//   2. 背景靠独立窗口的 DWM 亚克力玻璃，而未公开接口 GradientColor 的
//      alpha 语义是反的（0=全不透明、255=全透明），0x38 实为 78% 不透明，
//      Win11 上亚克力又退化成无模糊的纯色 tint —— 整块看着像深色实板。
// 现在的做法：GDI+ 画进 32bpp ARGB 位图（圆角/徽标/文字全走抗锯齿），
// 预乘后用 UpdateLayeredWindow 逐像素 alpha 上屏，桌面直接透过位图 alpha
// 显示，不再依赖色键与 DWM 亚克力；淡入淡出用 ULW 的 SourceConstantAlpha。
static constexpr int kToastW = 360;         // 药丸宽（逻辑像素，与原 .slint 版一致）
static constexpr int kToastH = 64;
static constexpr int kToastRadius = 15;
static constexpr int kToastTopOffset = 96;  // 距工作区顶边的距离
static constexpr BYTE kToastBgAlpha = 0x8A; // 药丸底不透明度 46%：与「已在运行」弹窗玻璃一致
static constexpr int kToastFadeIn = 120, kToastHold = 2000, kToastFadeOut = 300;  // ms

struct NativeToast {
    std::wstring text;
    HDC memDC = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int pxW = 0, pxH = 0;   // 位图物理尺寸
    int x = 0, y = 0;       // 当前屏幕位置
    int phase = 0;          // 0=淡入 1=保持 2=淡出 3=已隐藏
    DWORD t0 = 0;           // 当前阶段起始时间
};
static NativeToast g_toast;  // 常驻复用：每次 show 重画内容、重定位、重启动画
static HWND g_toastWnd = nullptr;
static ULONG_PTR g_gdiplusToken = 0;

static bool EnsureGdiplus() {
    if (g_gdiplusToken) return true;
    Gdiplus::GdiplusStartupInput in;
    return Gdiplus::GdiplusStartup(&g_gdiplusToken, &in, nullptr) == Gdiplus::Ok;
}

// 圆角矩形路径（GDI+ 没有内置 AddRoundedRect）
static void AddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    const float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// 超宽文案按字符回退加「…」（toast 文本短，线性回退足够）
static std::wstring ToastElide(Gdiplus::Graphics& gfx, const Gdiplus::Font& font,
                               const std::wstring& text, float maxW) {
    auto width = [&](const std::wstring& s) {
        Gdiplus::RectF m(0, 0, 0, 0);
        if (gfx.MeasureString(s.c_str(), (int)s.size(), &font, Gdiplus::PointF(0, 0),
                              nullptr, &m) != Gdiplus::Ok)
            return (float)1e6;  // 量不出宽度就当作超宽
        return m.Width;
    };
    if (width(text) <= maxW) return text;
    const std::wstring dots = L"…";
    if (width(dots) > maxW) return L"";
    size_t n = text.size();
    while (n > 0 && width(text.substr(0, n) + dots) > maxW) --n;
    return text.substr(0, n) + dots;
}

// 重画位图：主题底色药丸（半透明）+ 绿徽标白对勾 + 主题文字色文案
static void ToastRender(NativeToast* self) {
    const Theme& t = *g.theme;
    const int W = S(kToastW), H = S(kToastH);
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return;
    }
    memset(bits, 0, (size_t)W * H * 4);  // 全透明底
    {
        using namespace Gdiplus;
        // 包裹 DIB 的 ARGB 位图：GDI+ 写入 straight alpha，画完统一预乘
        Bitmap surface(W, H, W * 4, PixelFormat32bppARGB, (BYTE*)bits);
        Graphics gfx(&surface);
        gfx.SetSmoothingMode(SmoothingModeAntiAlias);
        gfx.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);  // 灰度 AA：透明底上无彩边
        gfx.SetPixelOffsetMode(PixelOffsetModeHalf);

        const float r = (float)S(kToastRadius);
        GraphicsPath pill;
        AddRoundRect(pill, 0.5f, 0.5f, (float)W - 1.0f, (float)H - 1.0f, r);
        SolidBrush pillBrush(Color(kToastBgAlpha, GetRValue(t.bg), GetGValue(t.bg), GetBValue(t.bg)));
        gfx.FillPath(&pillBrush, &pill);
        Pen edge(Color(0x30, 0xFF, 0xFF, 0xFF), (float)S(1));  // 1px 淡描边：亮背景上给药丸轮廓
        gfx.DrawPath(&edge, &pill);

        // 左侧成功徽标：绿圆 + 白对勾（几何与原 .slint 版一致）
        const float cx = (float)S(14) + r, cy = (float)H / 2;
        SolidBrush badgeBrush(Color(0xFF, 0x2E, 0xA3, 0x6B));
        gfx.FillEllipse(&badgeBrush, cx - r, cy - r, r * 2, r * 2);
        const PointF chk[3] = {
            { cx - S(7), cy + S(0) }, { cx - S(2), cy + S(5) }, { cx + S(7), cy - S(6) },
        };
        GraphicsPath check;
        check.AddLines(chk, 3);
        Pen checkPen(Color(0xFF, 0xFF, 0xFF, 0xFF), (float)S(2));
        checkPen.SetStartCap(LineCapRound);
        checkPen.SetEndCap(LineCapRound);
        checkPen.SetLineJoin(LineJoinRound);
        gfx.DrawPath(&checkPen, &check);

        // 文案：13px 同原 .slint 版；字体池按比例建好，lfHeight 负值即 em 像素
        HFONT hf = g.fontPool[FontSlot(13)];
        LOGFONTW lf {};
        if (GetObjectW(hf, sizeof(lf), &lf)) {
            FontFamily family(lf.lfFaceName);
            FontFamily fallback(L"Microsoft YaHei UI");
            const FontFamily* use = family.GetLastStatus() == Ok ? &family : &fallback;
            const FontStyle style = lf.lfWeight >= 700 ? FontStyleBold : FontStyleRegular;
            const int em = lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight;
            Font font(use, (float)em, style, UnitPixel);
            const float tx = (float)S(56), tw = (float)W - tx - S(14);
            const std::wstring shown = ToastElide(gfx, font, self->text, tw);
            SolidBrush textBrush(Color(0xFF, GetRValue(t.text), GetGValue(t.text), GetBValue(t.text)));
            StringFormat fmt;
            fmt.SetAlignment(StringAlignmentNear);
            fmt.SetLineAlignment(StringAlignmentCenter);
            fmt.SetFormatFlags(fmt.GetFormatFlags() | StringFormatFlagsNoClip |
                               StringFormatFlagsNoWrap);
            gfx.DrawString(shown.c_str(), (int)shown.size(), &font,
                           RectF(tx, 0, tw, (float)H), &fmt, &textBrush);
        }
    }
    // 预乘 alpha（UpdateLayeredWindow 要求 premultiplied BGRA）
    BYTE* p = (BYTE*)bits;
    for (int i = 0; i < W * H; ++i, p += 4) {
        const unsigned a = p[3];
        if (a == 255 || a == 0) continue;
        p[0] = (BYTE)(p[0] * a / 255);
        p[1] = (BYTE)(p[1] * a / 255);
        p[2] = (BYTE)(p[2] * a / 255);
    }
    // 换掉旧位图（旧的可能因换屏比例变化而尺寸不同）
    if (self->memDC && self->bmp) {
        SelectObject(self->memDC, self->oldBmp);
        DeleteObject(self->bmp);
    }
    if (!self->memDC) self->memDC = CreateCompatibleDC(nullptr);
    self->bmp = bmp;
    self->oldBmp = SelectObject(self->memDC, bmp);
    self->pxW = W;
    self->pxH = H;
}

// 上屏：alpha 用于淡入淡出（内容不变，只改 SourceConstantAlpha，不重绘）；
// pt/size 非空时同时移动窗口（首次显示用）
static void ToastPresent(HWND h, const NativeToast* self, BYTE alpha,
                         const POINT* pt = nullptr, const SIZE* size = nullptr) {
    HDC screen = GetDC(nullptr);
    BLENDFUNCTION bf { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    POINT src { 0, 0 };
    UpdateLayeredWindow(h, screen, const_cast<POINT*>(pt), const_cast<SIZE*>(size),
                        self->memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// 淡入 → 保持 → 淡出 → 隐藏
static void ToastTick(HWND h) {
    NativeToast* self = &g_toast;
    if (!self->bmp) return;
    const DWORD el = GetTickCount() - self->t0;
    switch (self->phase) {
        case 0: {  // 淡入
            const int a = (int)((el * 255UL) / kToastFadeIn);
            ToastPresent(h, self, (BYTE)(a > 255 ? 255 : a));
            if (el >= (DWORD)kToastFadeIn) {
                ToastPresent(h, self, 255);  // 补一帧全显，别停在中间值
                self->phase = 1;
                self->t0 = GetTickCount();
            }
            break;
        }
        case 1:  // 保持
            if (el >= (DWORD)kToastHold) {
                self->phase = 2;
                self->t0 = GetTickCount();
            }
            break;
        case 2: {  // 淡出
            const int a = 255 - (int)((el * 255UL) / kToastFadeOut);
            ToastPresent(h, self, (BYTE)(a < 0 ? 0 : a));
            if (el >= (DWORD)kToastFadeOut) {
                KillTimer(h, 1);
                self->phase = 3;
                ShowWindow(h, SW_HIDE);
            }
            break;
        }
        default:
            KillTimer(h, 1);
            break;
    }
}

static LRESULT CALLBACK ToastNativeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {  // ULW 窗口不走 WM_PAINT：留空处理避免闪烁
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_NCHITTEST: return HTTRANSPARENT;  // 点击穿透
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == 1) ToastTick(h);
            return 0;
        case WM_DESTROY:
            if (g_toast.memDC) {
                if (g_toast.bmp) {
                    SelectObject(g_toast.memDC, g_toast.oldBmp);
                    DeleteObject(g_toast.bmp);
                    g_toast.bmp = nullptr;
                }
                DeleteDC(g_toast.memDC);
                g_toast.memDC = nullptr;
            }
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 屏幕居中轻提示：主进程 GDI+ 自绘 + UpdateLayeredWindow 逐像素 alpha
static void ShowTopToast(const std::wstring& text) {
    if (!g.theme) return;
    AppendTrace("TOAST show");
    if (!EnsureGdiplus()) return;
    if (!g_toastWnd || !IsWindow(g_toastWnd)) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc {};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = ToastNativeProc;
            wc.hInstance = g.inst;
            wc.lpszClassName = L"FlowtaryNativeToast";
            registered = RegisterClassExW(&wc) != 0;
            if (!registered) return;
        }
        g_toastWnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                WS_EX_TOPMOST,
            L"FlowtaryNativeToast", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g.inst,
            nullptr);
        if (!g_toastWnd) return;
    }
    g_toast.text = text;
    ToastRender(&g_toast);
    if (!g_toast.bmp) return;
    // 跟随鼠标所在显示器：工作区水平居中、距顶 S(96)
    POINT cur {};
    GetCursorPos(&cur);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    g_toast.x = 0;
    g_toast.y = 0;
    if (GetMonitorInfoW(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &mi)) {
        g_toast.x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - g_toast.pxW) / 2;
        g_toast.y = mi.rcWork.top + S(kToastTopOffset);
    }
    const SIZE size { g_toast.pxW, g_toast.pxH };
    const POINT pos { g_toast.x, g_toast.y };
    ToastPresent(g_toastWnd, &g_toast, 0, &pos, &size);  // 全透明就位，避免闪一帧
    ShowWindow(g_toastWnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_toastWnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_toast.phase = 0;
    g_toast.t0 = GetTickCount();
    SetTimer(g_toastWnd, 1, 16, nullptr);  // 重复调用即重启动画
}

// ---------------- 单实例：区分「同版本重复启动」与「新版本替换启动」 ----------------
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

    // 开发调试：--toast-test <文本> —— 启动即弹一条顶部 toast（不走唤起/命令链），
    // 用于肉眼调整 toast 的配色/透明度/排版。文本含空格时整体加引号传入。
    if (const wchar_t* flag = wcsstr(GetCommandLineW(), L"--toast-test")) {
        std::wstring t(flag + 12);
        const size_t b = t.find_first_not_of(L" \"");
        if (b != std::wstring::npos) {
            const size_t e = t.find_last_not_of(L" \"");
            ShowTopToast(t.substr(b, e - b + 1));
            AppendTrace("TOAST show (--toast-test)");
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);


    // 单实例：区分「同版本重复启动」与「新版本替换启动」
    // （放在主题/字体初始化之后，这样弹窗才能用上主题配色与字体）
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\Flowtary.Singleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (IsSameRunningBuild()) {
            AppendTrace("duplicate start (same build): notice + exit");
            ShowNotice(L"Flowtary 已在运行",
                       L"程序已经在后台运行了。\r\n在任务栏托盘找到 Flowtary，左键单击即可唤出。",
                       L"知道了");
            return 0;
        }
        AppendTrace("version-mismatch start: taking over old instance");
        // 新版本：提示后结束旧实例，由本进程接管继续启动
        ShowNotice(L"检测到新版本", L"正在关闭旧版本并启动新版本…", L"好", 1500);
        if (HWND oldWnd = FindWindowW(L"FlowtaryLauncher", L"Flowtary"))
            PostMessageW(oldWnd, WM_APP_QUIT, 0, 0);
        WaitForSingleObject(mtx, 5000);  // 等旧实例退出并接管互斥体
    }
    RecordRunningBuild();

    // 自启项跟着本次真正跑起来的 exe 走：部署目录带版本号，Run 里记的却是上次点
    // 「保存」那一刻的路径。发版后不点设置也会自己纠正，登录不再起在旧版本上。
    // 放在单实例接管之后：只有活下来的那个实例写，被顶掉的旧 exe 不会把路径改回去。
    ft::SyncStartupPath(GetExePath());

    LoadWebRules();
    LoadGroupRules();
    LoadExcludePaths();  // 排除路径（设置可编辑），作用于 索引 + 程序搜索结果
    LoadClickWeights();  // 点击权重数据（%APPDATA%\Flowtary\weights.dat）
    LoadLearned();       // f/d 自学习数据（learned.dat + learned\*.lnk 双向同步）
    EnableDarkMenus();  // 按主题深浅强制弹出菜单（托盘/右键） 绘制

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
    ApplyTheme();  // 应用主题：字体与菜单深浅
    fdj_init(g.hwnd);  // 文件对话框“文件夹原地跳转”增强（类 Listary Quick-Switch）

    // 唤起快捷键：仅当开关开启时注册，依次尝试 Alt+Space -> Alt+Q -> Ctrl+Alt+Space
    // （避免与其他启动器冲突导致完全不可用）；关闭状态下只保留托盘左键唤出
    if (g.hotkeyWake) {
        if (!RegisterWakeHotkey()) {
            AppendTrace("wake hotkey: all 3 RegisterHotKey failed, LL hook fallback active");
            MessageBoxW(nullptr, L"Alt+Space / Alt+Q / Ctrl+Alt+Space 热键均注册失败，可能被其他程序占用。",
                        L"Flowtary", MB_ICONWARNING);
        } else {
            char tb[64];
            _snprintf_s(tb, sizeof(tb), _TRUNCATE, "startup: wake hotkey registered (%ls)",
                        g.hotkeyName.c_str());
            AppendTrace(tb);
        }
    } else {
        AppendTrace("startup: wake hotkey disabled by settings");
    }
    // 辅助热键：Ctrl+Alt+G —— 文件对话框激活时，把前台资源管理器当前目录同步到对话框
    RegisterHotKey(g.hwnd, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'G');
    TrayAdd();  // 优先让托盘图标就位（气泡提示使用最终选定的热键名）

    // 程序扫描较慢：放到工作线程，托盘图标已先就位且可响应（右键显示「正在启动中」）
    s_programWatch.Start(g.hwnd,WM_APP_APPS_CHANGED);
    StartProgramsRefresh();


    AppendTrace("startup complete: enter message loop");
    MSG msg;
    int gr = 0;
    while ((gr = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    char eb[96];
    _snprintf_s(eb, sizeof(eb), _TRUNCATE,
                "message loop exit: GetMessage returned %d%s", gr,
                gr == 0 ? " (WM_QUIT, expected)" : " (ERROR -> loop ended WITHOUT WM_QUIT!)");
    AppendTrace(eb);

    fdj_uninit();  // 卸载文件对话框增强的钩子与 32 位助手
    RemoveWakeHook();
    UnregisterHotKey(g.hwnd, 1);
    UnregisterHotKey(g.hwnd, 2);
    CoUninitialize();
    return 0;
}
