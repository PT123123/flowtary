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
#include <imm.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <process.h>
#include <new>

#include <algorithm>
#include <cwctype>
#include <string>
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
    COLORREF border;     // GDI 描边回退
    bool stars;          // 星空点点缀
};

static const Theme kThemes[] = {
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
     RGB(228, 228, 228), RGB(243, 243, 243), RGB(196, 196, 196), false},
};

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
    HBRUSH brEditBg = nullptr;   // 设置窗口编辑框深色背景
    HINSTANCE inst = nullptr;
    float scale = 1.0f;          // 全局比例：max(DPI, 屏幕物理高度/1080)，不写死像素
    bool centerWake = true;    // 唤醒位置：true=屏幕居中，false=跟随鼠标
    bool dwmBorderOk = false;  // DWM 描边可用（否则 Paint 回退 GDI 描边）
    int hotkeyMode = 0;        // 结果项快捷键方案：0=Alt+数字, 1=Alt+字母, 2=关闭
    bool startupWanted = false;// 设置窗“开机自动启动”勾选状态（自绘复选框用变量驱动，
                               // 因为 BS_OWNERDRAW 按钮的 Button_GetCheck/SetCheck 不生效）
    bool startupSaved = false; // 设置窗打开时的初始值（取消时回退）
    int hotkeyModeSaved = 0;   // 同上，结果项快捷键方案（取消时回退）
    int settingsTab = 0;        // 设置窗当前 Tab：0=常规, 1=网页规则（关闭后仍记住上次选择）
    int themeIdx = 0;          // 当前主题索引（设置窗切换后、保存前为暂存值）
    int themeSaved = 0;        // 设置窗打开时的初始主题（取消时回退）
    bool beautify = true;      // 界面美化：暗色标题栏 + 圆角窗口 + 强制暗色菜单（默认开）
    bool beautifySaved = true; // 设置窗打开时的初始值（取消时回退）
    bool glass = true;         // 毛玻璃（亚克力）背景，仅在 beautify 开启时生效（默认开）
    bool glassSaved = true;    // 设置窗打开时的初始值（取消时回退）
    bool startingUp = true;    // 程序扫描尚未完成：托盘提示/右键菜单显示「正在启动中」
    const Theme* theme = nullptr;
    std::vector<Star> stars;   // 星空主题星点坐标

    HFONT fontPool[15] = {};   // 逻辑字号 10..24 预建字体池（11-24），永不中途删除
    HBRUSH brMenuBg = nullptr;   // 弹出菜单背景（跟随主题重建）

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
constexpr int kDebounceMs = 120;
constexpr int WM_APP_TRAY = WM_APP + 1;
constexpr int WM_APP_PROGRAMS_READY = WM_APP + 2;  // 工作线程扫描完成，回主线程接管结果
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
static BYTE EffectiveAlpha(BYTE a);
static const CmdGroup* FindGroup(const std::vector<CmdGroup>& gs, const std::wstring& key);
static int KillProcessesByName(const std::wstring& name);
static void LayoutSettings(HWND h);
static void Layout();
static void RepaintNow();
static void GenerateStars();
static BOOL CALLBACK RefreshChildFont(HWND child, LPARAM lp);

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

// 主题应用：字体指针、透明度、各刷子、菜单深色模式、星点，全部按预设重置
static void ApplyTheme(bool repaint = true) {
    if (!g.theme) return;
    const Theme& t = *g.theme;
    g.fInput = g.fontPool[FontSlot(t.fontInput)];
    g.fList = g.fontPool[FontSlot(t.fontList)];
    if (g.hwnd) {
        SetLayeredWindowAttributes(g.hwnd, 0, EffectiveAlpha(t.alpha), LWA_ALPHA);
        if (g.brDivider) { DeleteObject(g.brDivider); g.brDivider = nullptr; }
        g.brDivider = CreateSolidBrush(t.divider);
    }
    if (g.hSettings) SetLayeredWindowAttributes(g.hSettings, 0, EffectiveAlpha(t.alpha),
                                                LWA_ALPHA);
    if (g.brMenuBg) { DeleteObject(g.brMenuBg); g.brMenuBg = nullptr; }
    g.brMenuBg = CreateSolidBrush(t.menuBg);
    if (g.brEditBg) { DeleteObject(g.brEditBg); g.brEditBg = nullptr; }
    g.brEditBg = CreateSolidBrush(t.editBg);
    EnableDarkMenus();
    ApplyGlass();  // 毛玻璃背景随美化开关/玻璃开关与主题底色刷新
    if (t.stars) GenerateStars();
    if (g.hSettings && repaint) {
        EnumChildWindows(g.hSettings, RefreshChildFont, 0);
        RedrawWindow(g.hSettings, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
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
    std::wstring ql = ToLowerW(query);
    struct Cand { Program* p; int score; };
    std::vector<Cand> cands;
    for (auto& p : g.programs) {
        int s = MatchScore(ToLowerW(p.name), ql);
        if (s > 0) cands.push_back({&p, s});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
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
    g.mode = Mode::None;
    g.expectReply = 0;
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
        case Row::File:
        case Row::Folder:
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
    COLORREF border = RGB(80, 80, 80);
    if (SUCCEEDED(DwmSetWindowAttribute(h, DWMWA_BORDER_COLOR, &border, sizeof(border))))
        g.dwmBorderOk = true;
}

// ---------------- 毛玻璃（亚克力）背景 ----------------
// 优先用文档化属性 DWMWA_SYSTEMBACKDROP_TYPE(38)：3=TransientWindow(亚克力)、1=None(关闭)；
// 旧系统回退未公开的 SetWindowCompositionAttribute + ACCENT_ENABLE_ACRYLICBLURBEHIND(4)。
// 两者都不可用时静默跳过，不影响其它功能。
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
    DWORD backdrop = on ? 3 : 1;  // 3=TransientWindow(亚克力) / 1=None
    DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
    // 同步未公开接口：旧系统靠它生效；关闭时也能把残留的亚克力清干净
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    auto fn = (SetWindowCompositionAttributeFn)GetProcAddress(u, "SetWindowCompositionAttribute");
    if (!fn) return;
    AccentPolicy ap{};
    if (on) {
        ap.AccentState = 4;  // ACCENT_ENABLE_ACRYLICBLURBEHIND
        ap.AccentFlags = 2;  // 作用到整个窗口（含客户区）
        COLORREF bc = g.theme ? g.theme->bg : RGB(0, 0, 0);
        // ABGR：高 8 位 = 雾面浓度（越小越通透），低 24 位 = 主题底色（BGR 顺序）
        ap.GradientColor =
            (0x99 << 24) | (GetBValue(bc) << 16) | (GetGValue(bc) << 8) | GetRValue(bc);
    } else {
        ap.AccentState = 0;  // ACCENT_DISABLED
    }
    WinCompAttrData d{19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap)};
    fn(h, &d);
}

static void ApplyGlass() {
    ApplyGlassTo(g.hwnd);
    ApplyGlassTo(g.hSettings);
}

// 毛玻璃开启时略微降低窗口不透明度，让背后的亚克力模糊透出来；
// 本程序窗口是「分层窗口 + 统一透明度」，玻璃的可见程度取决于主题自身的 alpha。
static BYTE EffectiveAlpha(BYTE a) {
    if (g.beautify && g.glass) return (BYTE)(std::min)((int)a, 232);
    return a;
}

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

// ---------------- 绘制 ----------------
static void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = S(kBasePad), inputH = S(kBaseInputH), rowH = S(kBaseRowH);
    const Theme& t = *g.theme;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    // 背景：纯色或垂直渐变（透明蓝色/星空风格），星空主题再点缀星点
    if (t.bg2 != t.bg) FillVGradient(mem, 0, 0, W, H, t.bg, t.bg2);
    else {
        HBRUSH bgBr = CreateSolidBrush(t.bg);
        FillRect(mem, &rc, bgBr);
        DeleteObject(bgBr);
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
    SetBkMode(mem, TRANSPARENT);

    // 输入行
    SelectObject(mem, g.fInput);
    {
        int textX = pad - g.scrollX;
        RECT r{textX, 0, W + 4096, inputH};
        SaveDC(mem);
        IntersectClipRect(mem, 0, 0, W, inputH);
        if (g.text.empty() && g.compText.empty()) {
            SetTextColor(mem, t.hintText);
            DrawTextW(mem, L"f/d 搜文件 · bd/bili/zhihu… 搜网页 · 直接输入启动程序", -1, &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            SetTextColor(mem, t.text);
            DrawTextW(mem, g.text.c_str(), (int)g.text.size(), &r,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        // 内联渲染 IME 组合串（未上屏临时文字）：跟在光标后，浅灰字 + 下划线
        if (!g.compText.empty()) {
            int cx = pad - g.scrollX + CaretTextWidth(mem);
            SetTextColor(mem, t.sub);
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
        HBRUSH carBr = CreateSolidBrush(t.text);
        FillRect(mem, &cr, carBr);
        DeleteObject(carBr);
    }
    // 分隔线
    if (!g.items.empty() && g.brDivider) {
        RECT lr{0, inputH, W, inputH + 1};
        FillRect(mem, &lr, g.brDivider);
    }

    // 结果列表
    SelectObject(mem, g.fList);
    int rows = (std::min)((int)g.items.size(), (int)ev::kMaxResults);
    HBRUSH selBr = CreateSolidBrush(t.selBg);
    for (int i = 0; i < rows; ++i) {
        Row& row = g.items[i];
        int y = inputH + i * rowH;
        if (i == g.sel) {
            RECT r{0, y, W, y + rowH};
            FillRect(mem, &r, selBr);
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
            SetTextColor(mem, t.sub);
            DrawTextW(mem, row.sub.c_str(), (int)row.sub.size(), &sr,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            subReserved = subW + S(8);
        }
        // 标题
        RECT tr{x, y, W - pad - subReserved - hkReserved, y + rowH};
        SetTextColor(mem, row.kind == Row::Hint ? t.hintText : t.text);
        DrawTextW(mem, row.title.c_str(), (int)row.title.size(), &tr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    DeleteObject(selBr);

    // 描边（DWM 边框不可用时回退 GDI 1px 描边）
    if (!g.dwmBorderOk) {
        HBRUSH obr = CreateSolidBrush(t.border);
        RECT fr{0, 0, W, H};
        FrameRect(mem, &fr, obr);
        DeleteObject(obr);
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
constexpr int IDM_THEME_BASE = 4200;  // 主题下拉菜单指令基值

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
    bool web = (tab == 1);
    vis(IDC_LBL_RULES, web);
    vis(IDC_EDT_RULES, web);
    vis(IDC_LBL_RULEHINT, web);
    vis(IDC_BTN_RESET, web);
    bool theme = (tab == 2);
    vis(IDC_CHK_BEAUTIFY, theme);
    vis(IDC_CHK_GLASS, theme);
    vis(IDC_LBL_THEME, theme);
    vis(IDC_CMB_THEME, theme);
    bool group = (tab == 3);
    vis(IDC_LBL_LAUNCH, group);
    vis(IDC_EDT_LAUNCH, group);
    vis(IDC_LBL_KILL, group);
    vis(IDC_EDT_KILL, group);
    vis(IDC_LBL_GROUPHINT, group);
    // 保存/取消/恢复默认：始终显示，贴底并整行居中（由 LayoutSettings 统一处理）
    LayoutSettings(h);
    InvalidateRect(h, nullptr, TRUE);
}

// Win11 / Win10 20H1+：标题栏跟随暗色
static void ApplyDarkTitlebar(HWND h) {
    BOOL dark = TRUE;
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
        case IDC_CMB_HOTKEY:
        case IDC_CMB_WAKE:
        case IDC_CMB_THEME:
            cg.stretchW = true;  // 内容区控件：宽度跟随
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

            c = CreateWindowExW(0, L"STATIC", L"主题样式：",
                                WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(92), S(90),
                                S(28), h, (HMENU)(INT_PTR)IDC_LBL_THEME, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

            c = CreateWindowExW(0, L"BUTTON", nullptr,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                margin + S(90), S(94), contentW - S(90), S(28), h,
                                (HMENU)(INT_PTR)IDC_CMB_THEME, g.inst, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

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

        case WM_ERASEBKGND:
            return 1;  // 背景由 WM_PAINT 统一填充，避免闪白

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
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
                // 侧栏顶部品牌标题（Tab 列表之上，用强调色）
                RECT ttr = {S(10), S(2), S(140), S(21)};
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, g.theme->menuHi);
                SelectObject(hdc, g.fList);
                DrawTextW(hdc, L"Flowtary", -1, &ttr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
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
            SetTextColor(hdc, RGB(235, 235, 235));
            SetBkMode(hdc, TRANSPARENT);
            return (LRESULT)GetStockObject(BLACK_BRUSH);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wp;
            HWND w = (HWND)lp;
            if (w == GetDlgItem(h, IDC_EDT_RULES) || w == GetDlgItem(h, IDC_EDT_LAUNCH) ||
                w == GetDlgItem(h, IDC_EDT_KILL)) {
                if (!g.brEditBg) g.brEditBg = CreateSolidBrush(RGB(24, 24, 24));
                SetTextColor(hdc, RGB(235, 235, 235));
                SetBkColor(hdc, RGB(24, 24, 24));
                return (LRESULT)g.brEditBg;
            }
            break;
        }

        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lp;
            if (mis && mis->CtlType == ODT_COMBOBOX) mis->itemHeight = S(26);
            return TRUE;
        }

        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
            if (!dis) break;
            if (dis->CtlType == ODT_BUTTON) {
                int id = (int)dis->CtlID;
                const Theme& t = *g.theme;
                if (id == IDC_CHK_START || id == IDC_CHK_BEAUTIFY || id == IDC_CHK_GLASS) {
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
                                 : (id == IDC_CHK_BEAUTIFY ? g.beautify : g.glass);
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
                if (id == IDC_CMB_WAKE || id == IDC_CMB_THEME || id == IDC_CMB_HOTKEY) {
                    // 下拉按钮：深底 + 描边 + 当前项文字 + ▾ 箭头（唤醒位置/主题/快捷键方案共用）
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
                    const WCHAR* cur = id == IDC_CMB_WAKE
                                           ? (g.centerWake ? L"屏幕居中" : L"跟随鼠标")
                                           : id == IDC_CMB_THEME ? g.theme->name
                                                                : HotkeyModeText(g.hotkeyMode);
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
                    id == IDC_TAB_GROUP) {
                    // 左侧 Tab 按钮：激活项用强调色高亮，并加左侧竖条
                    int idx = (id == IDC_TAB_GENERAL) ? 0
                            : (id == IDC_TAB_WEB)     ? 1
                            : (id == IDC_TAB_THEME)   ? 2
                                                      : 3;
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
                                                               : L"一键";
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
                DWORD vb = g.beautify ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Beautify",
                                REG_DWORD, &vb, sizeof(vb));
                DWORD vg = g.glass ? 1 : 0;
                RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"Glass",
                                REG_DWORD, &vg, sizeof(vg));
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
                InvalidateRect(GetDlgItem(h, IDC_CMB_THEME), nullptr, TRUE);  // 同步置灰/恢复
                InvalidateRect(GetDlgItem(h, IDC_CHK_GLASS), nullptr, TRUE);
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
             } else if ((id == IDC_TAB_GENERAL || id == IDC_TAB_WEB || id == IDC_TAB_THEME ||
                         id == IDC_TAB_GROUP) &&
                        HIWORD(wp) == BN_CLICKED) {
                ShowSettingsTab(h, (id == IDC_TAB_GENERAL) ? 0
                                 : (id == IDC_TAB_WEB)    ? 1
                                 : (id == IDC_TAB_THEME)  ? 2
                                                          : 3);
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
            } else if (id == IDC_CMB_THEME && HIWORD(wp) == BN_CLICKED) {
                // 主题下拉：列全部预设，当前项打勾；选择后即时预览全部界面
                if (!g.beautify) return 0;  // 美化关闭时主题样式不可选（下拉置灰）
                int n = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
                HMENU m = CreatePopupMenu();
                for (int i = 0; i < n; ++i)
                    AppendMenuW(m, MF_STRING | (i == g.themeIdx ? MF_CHECKED : 0),
                                IDM_THEME_BASE + i, kThemes[i].name);
                StyleDarkMenu(m);
                RECT r;
                GetWindowRect(GetDlgItem(h, IDC_CMB_THEME), &r);
                SetForegroundWindow(h);
                int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                         r.left, r.bottom, 0, h, nullptr);
                DestroyMenu(m);
                if (cmd >= IDM_THEME_BASE && cmd < IDM_THEME_BASE + n) {
                    int ni = cmd - IDM_THEME_BASE;
                    if (ni != g.themeIdx) {
                        g.themeIdx = ni;
                        g.theme = &kThemes[ni];
                        ApplyTheme();
                    }
                }
                InvalidateRect(GetDlgItem(h, IDC_CMB_THEME), nullptr, TRUE);
            } else if (id == IDC_BTN_CANCEL) {
                // 取消：回退未保存的主题/复选框，恢复打开时的预设
                if (g.themeIdx != g.themeSaved) {
                    g.themeIdx = g.themeSaved;
                    g.theme = &kThemes[g.themeIdx];
                    ApplyTheme();
                }
                if (g.beautify != g.beautifySaved) {
                    g.beautify = g.beautifySaved;
                    ApplyBeautify();
                }
                if (g.glass != g.glassSaved) {
                    g.glass = g.glassSaved;
                    ApplyTheme();  // 复原透明度与毛玻璃背景
                }
                if (g.startupWanted != g.startupSaved) g.startupWanted = g.startupSaved;
                if (g.hotkeyMode != g.hotkeyModeSaved) {
                    g.hotkeyMode = g.hotkeyModeSaved;
                    if (IsWindowVisible(g.hwnd)) RepaintNow();
                }
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
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    UpdateScale(mon);  // 窗口与控件尺寸按当前屏幕比例创建
    // 466/490 为逻辑尺寸（含标题栏余量），实际像素随比例缩放
    int W = S(600), H = S(490);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
    int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
    g.hSettings = CreateWindowExW(WS_EX_LAYERED, L"FlowtarySettings", L"Flowtary 设置",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN |
                                      WS_THICKFRAME,  // 可拖动调整大小
                                  x,
                                  y, W, H, nullptr, nullptr, g.inst, nullptr);
    if (g.hSettings) {
        ApplyBeautify();  // 圆角/暗色标题栏随美化开关（此前无条件圆角会覆盖「关闭美化」）
        SetLayeredWindowAttributes(g.hSettings, 0, g.theme->alpha, LWA_ALPHA);
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
            PostQuitMessage(0);
            return 0;

        case WM_HOTKEY:
            if (wParam == 1) {
                // 输入框聚焦时屏蔽已注册的 Alt 热键（Alt+Space / Alt+Q / Ctrl+Alt+Space），
                // 使其不再隐藏/切换窗口，仅 Alt+数字 生效（见 WM_SYSKEYDOWN）。
                if (GetFocus() == g.hwnd) return 0;
                if (IsWindowVisible(hwnd)) Hide();
                else Show();
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
            if (y >= inputH && !g.items.empty()) {
                int idx = (y - inputH) / rowH;
                if (idx >= 0 && idx < (int)g.items.size() && idx != g.sel) {
                    g.sel = idx;
                    RepaintNow();
                }
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
                    ExecuteSelected();
                }
            } else {
                SetFocus(hwnd);
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

// ---------------- 入口 ----------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    // 单实例
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\Flowtary.Singleton");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    // DPI 感知
    typedef BOOL(WINAPI * SetCtxFn)(HANDLE);
    SetCtxFn setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                               "SetProcessDpiAwarenessContext");
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();
    g.inst = hInst;
    LoadSettings();  // 先加载设置：确定主题/快捷键
    UpdateScale(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY));  // 建字体池（比例 = max(DPI, 物理高/1080)）

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    LoadWebRules();
    LoadGroupRules();
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

    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName,
                             L"Flowtary", WS_POPUP, 0, 0, S(kBaseW), S(kBaseInputH), nullptr,
                             nullptr, hInst, nullptr);
    if (!g.hwnd) return 1;
    ApplyRoundCorners(g.hwnd);
    ApplyTheme();  // 应用主题：透明度、字体、刷子、菜单深色、星点

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

    UnregisterHotKey(g.hwnd, 1);
    CoUninitialize();
    return 0;
}
