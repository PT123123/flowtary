// Flowtary 设置窗 —— Slint 版（flowtary-settings-slint.exe）
//   · 只做界面：数据模型、注册表读写、与主进程的 IPC 全部复用 settings_model / settings_ipc
//   · 12 个页签用「数据驱动表单」实现：C++ 把当前页签整理成 FormRow 列表，
//     settings.slint 负责画，交互通过回调回到这里改 g_s
//   · 保存 / 取消 / 预览语义与 FLTK 版完全一致
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <urlmon.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cwchar>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "private/slint_timer.h"
#include "search_history.h"
#include "settings.h"
#include "settings_ipc.h"
#include "settings_model.h"
#include "themes.h"

#pragma comment(lib, "urlmon.lib")
#pragma comment(lib, "dwmapi.lib")

// ---------------- 全局状态 ----------------
static ft::Settings g_s;
static HWND g_hMain = nullptr;
static HWND g_hSelf = nullptr;
static std::wstring g_mainExe;
static bool g_committed = false;
static bool g_quit = false;
static int g_tab = 0;
static int g_wipeArmed = 0;  // 1=权重 2=自学习 3=台账（两次点击确认）
static int g_wipeGen = 0;

static std::shared_ptr<slint::VectorModel<FormRow>> g_model;
static std::shared_ptr<slint::VectorModel<slint::SharedString>> g_tabsModel;
static std::vector<FormRow> g_rows;
static std::optional<slint::ComponentHandle<SettingsWindow>> g_win;

// 每个下拉控件 id → 实际候选（label + 存储值）
static std::map<int, std::vector<ft::Option>> g_choice;

// ---------------- 文本转换 ----------------
static std::string u8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::wstring wu8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------- 控件 id ----------------
enum Id {
    ID_UIENGINE = 1, ID_STARTUP, ID_HOTKEYMODE, ID_CENTERWAKE, ID_FDJ, ID_DFBACKEND,
    ID_RULES,
    ID_BEAUTIFY, ID_ANTIGHOST, ID_THEME, ID_ALPHA, ID_RADIUS,
    ID_LAUNCH, ID_KILL,
    ID_WEIGHT_ENABLED, ID_WEIGHT_FLUSH, ID_WEIGHT_MAX, ID_WIPE_WEIGHTS, ID_WEIGHT_COUNT,
    ID_SKIPDOT, ID_EXCLUDE, ID_RESET_EXCLUDE, ID_EXCLUDE_COUNT,
    ID_INDEXDIRS, ID_INDEXDIRS_COUNT,
    ID_SHELLTYPE, ID_SHELLPATH, ID_SHELLARGS, ID_SHELLCWD, ID_SHELLSHOW,
    ID_WINGROUP, ID_WINUWP, ID_WINPROC, ID_WINCACHE,
    ID_TOP, ID_CMD, ID_WIN, ID_CAPTURE,
    ID_CAP_STATUS, ID_DL_SC, ID_DL_IR,
    ID_LEARN_ENABLED, ID_LEARN_SCOPE, ID_LEARN_MAX, ID_LEARN_EXPIRE,
    ID_LEARN_OPEN, ID_WIPE_LEARN, ID_LEARN_COUNT,
    ID_HIST_ENABLED, ID_HIST_MAX, ID_HIST_REFRESH, ID_HIST_OPEN, ID_HIST_WIPE,
    ID_HIST_INFO, ID_HIST_VIEW,
};

// ---------------- 与主进程通信 ----------------
static void SendToMain(DWORD id, const void* data, DWORD cb) {
    if (!g_hMain || !IsWindow(g_hMain)) return;
    COPYDATASTRUCT cds{};
    cds.dwData = id;
    cds.cbData = cb;
    cds.lpData = (PVOID)data;
    DWORD_PTR ok = 0;
    SendMessageTimeoutW(g_hMain, WM_COPYDATA, (WPARAM)g_hSelf, (LPARAM)&cds, SMTO_ABORTIFHUNG,
                        3000, &ok);
}

static ipc::Preview MakePreview() {
    ipc::Preview p{};
    p.hdr.sender = g_hSelf;
    p.themeIdx = g_s.themeIdx;
    p.hotkeyMode = g_s.hotkeyMode;
    p.centerWake = g_s.centerWake ? TRUE : FALSE;
    p.beautify = g_s.beautify ? TRUE : FALSE;
    p.skipDotFolders = g_s.skipDotFolders ? TRUE : FALSE;
    p.fdjEnabled = g_s.fdjEnabled ? TRUE : FALSE;
    p.dfBackend = g_s.dfBackend;
    p.dfIndexTier = g_s.dfIndexTier;
    p.weightEnabled = g_s.weightEnabled ? TRUE : FALSE;
    p.weightFlush = g_s.weightFlush;
    p.weightMaxEntries = g_s.weightMaxEntries;
    p.learnEnabled = g_s.learnEnabled ? TRUE : FALSE;
    p.learnScope = g_s.learnScope;
    p.learnMaxEntries = g_s.learnMaxEntries;
    p.learnExpire = g_s.learnExpire;
    p.topEnabled = g_s.topEnabled ? TRUE : FALSE;
    p.cmdEnabled = g_s.cmdEnabled ? TRUE : FALSE;
    p.winEnabled = g_s.winEnabled ? TRUE : FALSE;
    p.captureEnabled = g_s.captureEnabled ? TRUE : FALSE;
    p.shellType = g_s.shellType;
    p.shellDefaultCwd = g_s.shellDefaultCwd;
    p.shellShowWindow = g_s.shellShowWindow ? TRUE : FALSE;
    p.winGroupProc = g_s.winGroupProc ? TRUE : FALSE;
    p.winShowUwp = g_s.winShowUwp ? TRUE : FALSE;
    p.winShowProc = g_s.winShowProc ? TRUE : FALSE;
    p.winCacheSec = g_s.winCacheSec;
    int n = (int)((std::min)(g_s.tune.size(), (size_t)ipc::kMaxThemes));
    p.tuneCount = n;
    for (int i = 0; i < n; ++i) {
        p.tune[i].alpha = g_s.tune[i].alpha;
        p.tune[i].radius = g_s.tune[i].radius;
    }
    return p;
}
static void PushPreview() {
    ipc::Preview p = MakePreview();
    SendToMain(ipc::MsgPreview, &p, sizeof(p));
}

// ---------------- 表单行构造 ----------------
static const int kLabel = 0, kCheck = 1, kChoice = 2, kInput = 3, kMulti = 4,
                 kButton = 5, kHint = 6, kSlider = 7, kInfo = 8;

static FormRow MakeRow(int id, int kind, const std::wstring& label, float h) {
    FormRow r{};
    r.row_id = id;
    r.kind = kind;
    r.label = slint::SharedString(u8(label).c_str());
    r.checked = false;
    r.option_index = 0;
    r.enabled = true;
    r.min = 0;
    r.max = 100;
    r.amount = 0;
    r.row_height = h;
    return r;
}

static std::shared_ptr<slint::Model<slint::SharedString>> MakeOptions(
    const std::vector<ft::Option>& opts) {
    std::vector<slint::SharedString> v;
    v.reserve(opts.size());
    for (auto& o : opts) v.push_back(slint::SharedString(u8(o.label).c_str()));
    return std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(v));
}

static FormRow RowChoice(int id, const std::wstring& label, std::vector<ft::Option> opts,
                         int value) {
    int idx = ft::LocateOption(opts, value, INT_MIN, INT_MAX);
    g_choice[id] = opts;
    FormRow r = MakeRow(id, kChoice, label, 34);
    r.options = MakeOptions(opts);
    r.option_index = idx;
    return r;
}
static FormRow RowCheck(int id, const std::wstring& label, bool on) {
    FormRow r = MakeRow(id, kCheck, label, 30);
    r.checked = on;
    return r;
}
static FormRow RowInput(int id, const std::wstring& label, const std::wstring& value,
                        bool enabled = true) {
    FormRow r = MakeRow(id, kInput, label, 36);
    r.value = slint::SharedString(u8(value).c_str());
    r.enabled = enabled;
    return r;
}
static FormRow RowMulti(int id, const std::wstring& label, const std::wstring& value, float h,
                        bool enabled = true) {
    FormRow r = MakeRow(id, kMulti, label, h);
    std::string text = u8(value);
    // 编辑器里用 CRLF，Windows 粘贴/换行才符合直觉
    std::string crlf;
    crlf.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) crlf += '\r';
        crlf += text[i];
    }
    r.value = slint::SharedString(crlf.c_str());
    r.enabled = enabled;
    return r;
}
static FormRow RowHint(int id, const std::wstring& text, float h = 42) {
    FormRow r = MakeRow(id, kHint, text, h);
    r.enabled = false;
    return r;
}
static FormRow RowButton(int id, const std::wstring& label) {
    return MakeRow(id, kButton, label, 40);
}
static FormRow RowInfo(int id, const std::wstring& text) {
    FormRow r = MakeRow(id, kInfo, text, 22);
    r.enabled = false;
    return r;
}
static FormRow RowSlider(int id, const std::wstring& label, int value, int lo, int hi) {
    FormRow r = MakeRow(id, kSlider, label, 32);
    r.amount = value;
    r.min = lo;
    r.max = hi;
    return r;
}

// 编辑器文本 → 设置字段（单行/多行统一去 CR）
static std::wstring EditorText(const std::string& s) {
    std::wstring w = wu8(s);
    std::wstring out;
    out.reserve(w.size());
    for (WCHAR c : w)
        if (c != L'\r') out += c;
    return out;
}

static std::wstring CaptureStatusText() {
    size_t pos = g_mainExe.find_last_of(L"\\/");
    std::wstring dir = (pos == std::wstring::npos) ? L"" : g_mainExe.substr(0, pos + 1);
    bool sc = GetFileAttributesW((dir + L"ScreenCapture.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool ir = GetFileAttributesW((dir + L"ImageReader.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (sc && ir) return L"ScreenCapture.exe 和 ImageReader.exe 均已就绪";
    if (sc) return L"ScreenCapture.exe 已就绪，ImageReader.exe 未下载（文字识别不可用）";
    if (ir) return L"ImageReader.exe 已就绪，ScreenCapture.exe 未下载";
    return L"ScreenCapture.exe 和 ImageReader.exe 均未下载";
}

static const size_t kHistoryViewLines = 400;
static size_t CountLines(const std::wstring& text) {
    size_t n = 0;
    for (WCHAR c : text)
        if (c == L'\n') ++n;
    return n;
}
static std::wstring HistoryViewText() {
    std::wstring all = ft::hist::ReadTail((size_t)g_s.historyMaxLines + 1000);
    std::wstring shown = ft::hist::TakeTail(all, kHistoryViewLines);
    return shown;
}
static std::wstring HistoryInfoText() {
    std::wstring all = ft::hist::ReadTail((size_t)g_s.historyMaxLines + 1000);
    std::wstring shown = ft::hist::TakeTail(all, kHistoryViewLines);
    std::wstring info = L"台账共 " + std::to_wstring(CountLines(all)) + L" 行，这里显示最近 " +
                        std::to_wstring(CountLines(shown)) + L" 行";
    if (all.empty()) info += L"（还没有记录：搜一次再回来看）";
    return info;
}

// ---------------- 每个页签的表单 ----------------
static std::vector<FormRow> BuildRows(int tab) {
    g_choice.clear();
    std::vector<FormRow> v;
    switch (tab) {
        case 0:  // 常规
            v.push_back(RowChoice(ID_UIENGINE, L"界面引擎：", ft::UiEngineOptions(), g_s.uiEngine));
            v.push_back(RowCheck(ID_STARTUP, L"开机自动启动", g_s.startupWanted));
            v.push_back(RowChoice(ID_HOTKEYMODE, L"结果项快捷键：", ft::HotkeyModeOptions(),
                                  g_s.hotkeyMode));
            v.push_back(RowChoice(ID_CENTERWAKE, L"唤醒位置：", ft::CenterWakeOptions(),
                                  g_s.centerWake));
            v.push_back(RowCheck(ID_FDJ, L"文件对话框跳转（在文件对话框中直接定位文件夹）",
                                 g_s.fdjEnabled));
            v.push_back(RowChoice(ID_DFBACKEND, L"d/f 搜索后端：", ft::DfBackendOptions(),
                                  g_s.dfBackend));
            v.push_back(MakeRow(0, kLabel,
                                L"Flowtary 快照查询已接入；当前仍需独立完成索引生成与验证。", 24));
            v.push_back(RowHint(0,
                                L"自动模式优先查询已发布的 Flowtary 快照；快照尚未就绪时查询 Windows "
                                L"Search。\nFlowtary 快照缺失或损坏时不会启动文件扫描；Windows Search "
                                L"范围由系统设置决定。",
                                52));
            v.push_back(RowHint(0,
                                L"界面引擎切换（GDI/Slint）在保存后、下次唤起时生效。\n"
                                L"唤起快捷键（Alt+Space）在托盘菜单里开关；结果项快捷键改动立即生效。",
                                46));
            break;
        case 1:  // 网页规则
            v.push_back(MakeRow(0, kLabel, L"网页搜索规则：", 22));
            v.push_back(RowMulti(ID_RULES, L"", g_s.rulesText, 330));
            v.push_back(RowHint(0, L"每行一条：前缀 + 空格 + 链接模板（{q} 为关键词，回车跳转）", 22));
            break;
        case 2:  // 主题
            if (g_s.uiEngine == 0) {
                // GDI 引擎才有的旧选项；Slint 引擎下没有「自绘 + 双缓冲」这一层
                v.push_back(RowCheck(ID_BEAUTIFY,
                                     L"启用界面美化（暗色标题栏 / 圆角窗口 / 暗色菜单）",
                                     g_s.beautify));
                v.push_back(RowCheck(ID_ANTIGHOST, L"启用抗残影双缓冲（实验性）", g_s.antiGhost));
                v.push_back(RowHint(0, L"实验性：开启整窗双缓冲消除残影；个别控件可能偶发闪烁", 20));
            } else {
                v.push_back(RowHint(
                    0,
                    L"当前为 Slint 引擎：界面由主题直接控制，配色、圆角与透明度都会实时生效。"
                    L"GDI 版的「界面美化 / 抗残影双缓冲」已不再适用，故不在此显示。",
                    44));
            }
            {
                std::vector<ft::Option> themes;
                for (int i = 0; i < kThemeCount; ++i)
                    themes.push_back({kThemes[i].name, i});
                v.push_back(RowChoice(ID_THEME, L"主题：", themes, g_s.themeIdx));
            }
            v.push_back(RowHint(0, L"微调（拖动即预览，保存后长期生效）", 20));
            if (g_s.themeIdx >= 0 && g_s.themeIdx < (int)g_s.tune.size()) {
                v.push_back(RowSlider(ID_ALPHA, L"透明度：", g_s.tune[g_s.themeIdx].alpha, 0, 255));
                v.push_back(RowSlider(ID_RADIUS, L"圆角半径：", g_s.tune[g_s.themeIdx].radius, 0, 30));
            }
            break;
        case 3:  // 一键
            v.push_back(MakeRow(0, kLabel, L"启动组（关键字 → 文件）：", 22));
            v.push_back(RowMulti(ID_LAUNCH, L"", g_s.launchText, 150));
            v.push_back(MakeRow(0, kLabel, L"关闭组（关键字 → 进程）：", 22));
            v.push_back(RowMulti(ID_KILL, L"", g_s.killText, 150));
            v.push_back(RowHint(0,
                                L"每行：关键字 + 空格 + 目标（多个用 ; 分隔）；# 开头为注释。"
                                L"启动组填文件全路径，关闭组填进程名（可省 .exe）",
                                34));
            break;
        case 4:  // 搜索权重
            v.push_back(RowCheck(ID_WEIGHT_ENABLED, L"启用点击权重记忆（相同搜索词下点过的条目排序提前）",
                                 g_s.weightEnabled));
            v.push_back(RowChoice(ID_WEIGHT_FLUSH, L"写入时机：", ft::WeightFlushOptions(),
                                  g_s.weightFlush));
            v.push_back(RowChoice(ID_WEIGHT_MAX, L"条目上限：", ft::WeightMaxOptions(),
                                  g_s.weightMaxEntries));
            v.push_back(RowButton(
                ID_WIPE_WEIGHTS, g_wipeArmed == 1 ? L"再次点击确认清空" : L"清空权重数据…"));
            v.push_back(RowInfo(ID_WEIGHT_COUNT,
                                L"当前已记忆 " +
                                    std::to_wstring(ft::CountWeightEntries()) + L" 条权重数据"));
            v.push_back(RowHint(0,
                                L"权重按「搜索词 → 文件完整路径」记录，不同搜索词互相独立；f/d 前缀只对"
                                L"后面的关键词记录，网页前缀（如 gg、bd）的点击不记录。数据文件："
                                L"%APPDATA%\\Flowtary\\weights.dat",
                                54));
            break;
        case 5:  // 排除路径
            v.push_back(RowCheck(ID_SKIPDOT, L"不搜索 . 开头的文件与文件夹（.git / .gitignore …）",
                                 g_s.skipDotFolders));
            v.push_back(RowMulti(ID_EXCLUDE, L"", g_s.excludeText, 300));
            v.push_back(RowButton(ID_RESET_EXCLUDE, L"恢复默认"));
            v.push_back(RowInfo(ID_EXCLUDE_COUNT,
                                L"当前已记忆 " +
                                    std::to_wstring((g_s.excludePaths.empty()
                                                         ? ft::ParseExcludePaths(g_s.excludeText).size()
                                                         : g_s.excludePaths.size())) +
                                    L" 条排除项"));
            v.push_back(RowHint(0,
                                L"每行一条路径，大小写不敏感，支持通配符 * ?。以排除项开头的文件/文件夹/"
                                L"程序会被过滤。网页与一键组不受影响，. 开头的文件/文件夹由上方开关统一跳过。"
                                L"保存后生效。",
                                46));
            break;
        case 6:  // 索引目录
            v.push_back(MakeRow(0, kLabel, L"旧索引目录（仅保留配置，不参与搜索）：", 22));
            v.push_back(RowMulti(ID_INDEXDIRS, L"", g_s.indexDirsText, 300, false));
            v.push_back(RowInfo(ID_INDEXDIRS_COUNT,
                                L"保留 " + std::to_wstring(ft::ParseIndexDirs(g_s.indexDirsText).size()) +
                                    L" 条旧目录配置（不生效）"));
            v.push_back(RowHint(0,
                                L"旧目录配置不触发扫描。d/f 查询 Flowtary 快照或 Windows Search。\n"
                                L"C:\\workshop\\*.exe 由 Flowtary 自己维护。",
                                46));
            break;
        case 7:  // Shell 与窗口
            v.push_back(RowChoice(ID_SHELLTYPE, L"Shell 程序：", ft::ShellTypeOptions(),
                                  g_s.shellType));
            if (g_s.shellType == 3) {
                v.push_back(RowInput(ID_SHELLPATH, L"自定义 Shell 路径：", g_s.shellCustomPath));
                v.push_back(RowInput(ID_SHELLARGS, L"自定义启动参数：", g_s.shellCustomArgs));
                v.push_back(RowHint(0, L"提示：{c} = 命令", 20));
            }
            v.push_back(RowChoice(ID_SHELLCWD, L"默认工作目录：", ft::ShellCwdOptions(),
                                  g_s.shellDefaultCwd));
            v.push_back(RowCheck(ID_SHELLSHOW, L"前台显示输出窗口（否则后台静默执行）",
                                 g_s.shellShowWindow));
            v.push_back(RowCheck(ID_WINGROUP, L"合并同进程窗口（多窗口进程显示为分组）",
                                 g_s.winGroupProc));
            v.push_back(RowCheck(ID_WINUWP, L"显示 UWP 应用窗口", g_s.winShowUwp));
            v.push_back(RowCheck(ID_WINPROC, L"结果副标题显示进程名", g_s.winShowProc));
            v.push_back(RowChoice(ID_WINCACHE, L"窗口缓存刷新间隔(秒)：", ft::WinCacheOptions(),
                                  g_s.winCacheSec));
            break;
        case 8:  // 命令
            v.push_back(RowCheck(ID_TOP, L"top 命令（空格+top 回车，置顶/取消置顶当前窗口）",
                                 g_s.topEnabled));
            v.push_back(RowCheck(ID_CMD, L"cmd 命令（空格+cmd 执行 Shell 命令，原 “> 命令”）",
                                 g_s.cmdEnabled));
            v.push_back(RowCheck(ID_WIN, L"w 命令（空格+w 切换窗口，原 “< 关键词”）",
                                 g_s.winEnabled));
            v.push_back(RowCheck(ID_CAPTURE, L"ss 命令（空格+ss 截图，空格+ss pin 贴图）",
                                 g_s.captureEnabled));
            v.push_back(RowHint(0, L"关闭后对应前缀不再被识别，搜索时按普通关键词处理。保存后生效。",
                                26));
            break;
        case 9:  // 截图工具
            v.push_back(MakeRow(0, kLabel, CaptureStatusText(), 26));
            v.push_back(RowButton(ID_DL_SC, L"下载 ScreenCapture.exe（约 1MB）"));
            v.push_back(RowButton(ID_DL_IR, L"下载 ImageReader.exe（约 25MB，文字识别必需）"));
            v.push_back(RowHint(0,
                                L"下载后将自动保存到 Flowtary.exe 同目录。OCR 功能（空格+ss ocr）需要 "
                                L"ImageReader.exe 才可使用。",
                                34));
            v.push_back(RowHint(0,
                                L"提示：文字识别结果会自动复制到剪贴板。支持的命令：空格+ss（截图）/ "
                                L"空格+ss pin（贴图）/ 空格+ss ocr（文字识别）",
                                40));
            break;
        case 10:  // 自学习
            v.push_back(RowCheck(ID_LEARN_ENABLED,
                                 L"启用自学习（f/d 搜索打开过的条目自动记忆，下次同词搜索置顶）",
                                 g_s.learnEnabled));
            v.push_back(RowChoice(ID_LEARN_SCOPE, L"记录范围：", ft::LearnScopeOptions(),
                                  g_s.learnScope));
            v.push_back(RowChoice(ID_LEARN_MAX, L"条目上限：", ft::LearnMaxOptions(),
                                  g_s.learnMaxEntries));
            v.push_back(RowChoice(ID_LEARN_EXPIRE, L"过期清理：", ft::LearnExpireOptions(),
                                  g_s.learnExpire));
            v.push_back(RowButton(ID_LEARN_OPEN, L"打开快捷方式目录"));
            v.push_back(RowButton(ID_WIPE_LEARN,
                                  g_wipeArmed == 2 ? L"再次点击确认清空" : L"清空自学习数据…"));
            v.push_back(RowInfo(ID_LEARN_COUNT,
                                L"当前已学习 " + std::to_wstring(ft::CountLearnedEntries()) +
                                    L" 条快捷方式"));
            v.push_back(RowHint(0,
                                L"仅在 f / d 前缀搜索生效：打开成功后按「搜索词 → 路径」记入 "
                                L"%APPDATA%\\Flowtary\\learned.dat，并在同目录 learned 文件夹镜像一个 "
                                L".lnk 快捷方式（可拷贝、可备份）。同词或前缀词搜索时学习项置顶，exe "
                                L"优先；删除 .lnk 即遗忘对应条目。写盘时机沿用「搜索权重」页设置。",
                                60));
            break;
        case 11:  // 搜索历史
            v.push_back(RowCheck(ID_HIST_ENABLED,
                                 L"记录搜索历史（每轮查询写一条：耗时、各层给了几条、第一行结果）",
                                 g_s.historyEnabled));
            v.push_back(RowChoice(ID_HIST_MAX, L"保留行数：", ft::HistoryMaxOptions(),
                                  g_s.historyMaxLines));
            v.push_back(RowButton(ID_HIST_REFRESH, L"刷新"));
            v.push_back(RowButton(ID_HIST_OPEN, L"打开台账文件"));
            v.push_back(RowButton(ID_HIST_WIPE,
                                  g_wipeArmed == 3 ? L"再次点击确认清空" : L"清空台账…"));
            v.push_back(RowInfo(ID_HIST_INFO, HistoryInfoText()));
            v.push_back(RowMulti(ID_HIST_VIEW, L"", HistoryViewText(), 200, false));
            v.push_back(RowHint(0,
                                L"文件：%APPDATA%\\Flowtary\\search_history.log（UTF-8，一行一条，超出"
                                L"保留行数只留尾部）\nQ=一轮查询（打完字 4 秒后落一条，逐字打字不重复记）："
                                L"first=首屏上屏延迟 total=各层报齐的总耗时 rows=结果条数 WS=Windows "
                                L"Search 返回条数；O=实际打开项。记录只在本机，不参与排序。",
                                64));
            break;
        default:
            break;
    }
    return v;
}

static const WCHAR* kTabNames[] = {
    L"常规", L"网页规则", L"主题", L"一键", L"搜索权重", L"排除路径",
    L"索引目录", L"Shell 与窗口", L"命令", L"截图工具", L"自学习", L"搜索历史",
};
static const int kTabCount = 12;

static void ShowTab(int tab) {
    if (tab < 0 || tab >= kTabCount) tab = 0;
    g_tab = tab;
    g_rows = BuildRows(tab);
    if (g_model) g_model->set_vector(g_rows);
    if (g_win.has_value()) {
        (*g_win)->set_tab(tab);
        (*g_win)->set_tab_title(slint::SharedString(u8(kTabNames[tab]).c_str()));
        (*g_win)->set_can_reset(tab == 1);
        (*g_win)->set_rows(g_model);
    }
    ft::RegSetDword(L"SettingsTab", (DWORD)tab);
}

// 更新某一行的文案（计数 / 按钮状态），不重建整页，避免打断编辑
static void UpdateInfoRow(int id, const std::wstring& text) {
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (g_rows[i].row_id != id) continue;
        g_rows[i].label = slint::SharedString(u8(text).c_str());
        if (g_model) g_model->set_row_data((int)i, g_rows[i]);
        return;
    }
}
static void UpdateButtonRow(int id, const std::wstring& text) {
    UpdateInfoRow(id, text);
}

// ---------------- 保存 / 取消 ----------------
static void QuitApp() {
    g_quit = true;
    if (g_win.has_value()) (*g_win)->hide();
    slint::quit_event_loop();
}
static void DoSave() {
    ft::SetStartupPath(g_mainExe, g_s.startupWanted);
    g_s.Save();
    g_committed = true;
    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgCommit, &hdr, sizeof(hdr));
    QuitApp();
}
static void DoCancel() {
    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgCancel, &hdr, sizeof(hdr));
    QuitApp();
}
static void DoClose() {
    ipc::Header hdr{g_hSelf};
    SendToMain(g_committed ? ipc::MsgClosed : ipc::MsgCancel, &hdr, sizeof(hdr));
    QuitApp();
}

// ---------------- 按钮动作 ----------------
static void DisarmWipe() {
    int gen = ++g_wipeGen;
    slint::Timer::single_shot(std::chrono::milliseconds(3000), [gen]() {
        if (gen != g_wipeGen || g_wipeArmed == 0) return;
        g_wipeArmed = 0;
        if (g_tab == 4) UpdateButtonRow(ID_WIPE_WEIGHTS, L"清空权重数据…");
        if (g_tab == 10) UpdateButtonRow(ID_WIPE_LEARN, L"清空自学习数据…");
        if (g_tab == 11) UpdateButtonRow(ID_HIST_WIPE, L"清空台账…");
    });
}

static void StartDownload(int which) {
    std::wstring dir;
    size_t pos = g_mainExe.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = g_mainExe.substr(0, pos + 1);
    std::wstring dest = dir + (which == 1 ? L"ScreenCapture.exe" : L"ImageReader.exe");
    const wchar_t* url = which == 1
        ? L"https://github.com/xland/ScreenCapture/releases/latest/download/ScreenCapture.exe"
        : L"https://github.com/xland/ImageReader/releases/latest/download/ImageReader.exe";
    UpdateButtonRow(which == 1 ? ID_DL_SC : ID_DL_IR,
                    which == 1 ? L"下载中…" : L"下载中…（约 25MB，请稍候）");
    std::wstring urlCopy(url), tmp = dest + L".tmp";
    std::thread([which, urlCopy, dest, tmp]() {
        HRESULT hr = URLDownloadToFileW(nullptr, urlCopy.c_str(), tmp.c_str(), 0, nullptr);
        bool ok = false;
        if (SUCCEEDED(hr)) {
            DeleteFileW(dest.c_str());
            ok = MoveFileW(tmp.c_str(), dest.c_str()) != FALSE;
        }
        if (!ok) DeleteFileW(tmp.c_str());
        slint::invoke_from_event_loop([which, ok]() {
            UpdateButtonRow(which == 1 ? ID_DL_SC : ID_DL_IR,
                            which == 1 ? L"下载 ScreenCapture.exe（约 1MB）"
                                       : L"下载 ImageReader.exe（约 25MB，文字识别必需）");
            for (size_t i = 0; i < g_rows.size(); ++i) {
                if (g_rows[i].row_id == ID_CAP_STATUS) {
                    g_rows[i].label = slint::SharedString(u8(CaptureStatusText()).c_str());
                    if (g_model) g_model->set_row_data((int)i, g_rows[i]);
                    break;
                }
            }
            if (ok) {
                ipc::Header hdr{g_hSelf};
                SendToMain(ipc::MsgCaptureInstalled, &hdr, sizeof(hdr));
            }
        });
    }).detach();
}

static void OpenHistoryFile() {
    std::wstring path = ft::hist::FilePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        ft::hist::Write(L"# 台账由设置页创建（此后每轮查询一行 Q、每次打开一行 O）",
                        (size_t)g_s.historyMaxLines);
    ShellExecuteW(g_hSelf, L"open", path.c_str(), nullptr, nullptr, SW_SHOW);
}
static void OpenLearnDir() {
    std::wstring dir = ft::LearnedLinkDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(g_hSelf, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
}

static void OnRowClicked(int id) {
    switch (id) {
        case ID_WIPE_WEIGHTS: {
            if (g_wipeArmed != 1) {
                g_wipeArmed = 1;
                UpdateButtonRow(ID_WIPE_WEIGHTS, L"再次点击确认清空");
                DisarmWipe();
                return;
            }
            g_wipeArmed = 0;
            ++g_wipeGen;
            UpdateButtonRow(ID_WIPE_WEIGHTS, L"清空权重数据…");
            UpdateInfoRow(ID_WEIGHT_COUNT, L"当前已记忆 0 条权重数据");
            ipc::Header hdr{g_hSelf};
            SendToMain(ipc::MsgWipeWeights, &hdr, sizeof(hdr));
            break;
        }
        case ID_WIPE_LEARN: {
            if (g_wipeArmed != 2) {
                g_wipeArmed = 2;
                UpdateButtonRow(ID_WIPE_LEARN, L"再次点击确认清空");
                DisarmWipe();
                return;
            }
            g_wipeArmed = 0;
            ++g_wipeGen;
            UpdateButtonRow(ID_WIPE_LEARN, L"清空自学习数据…");
            UpdateInfoRow(ID_LEARN_COUNT, L"当前已学习 0 条快捷方式");
            ipc::Header hdr{g_hSelf};
            SendToMain(ipc::MsgWipeLearned, &hdr, sizeof(hdr));
            break;
        }
        case ID_HIST_WIPE:
            if (g_wipeArmed != 3) {
                g_wipeArmed = 3;
                UpdateButtonRow(ID_HIST_WIPE, L"再次点击确认清空");
                DisarmWipe();
                return;
            }
            g_wipeArmed = 0;
            ++g_wipeGen;
            UpdateButtonRow(ID_HIST_WIPE, L"清空台账…");
            if (!ft::hist::RemoveFile())
                UpdateInfoRow(ID_HIST_INFO, L"清空失败：文件正被编辑器占用");
            UpdateInfoRow(ID_HIST_INFO, HistoryInfoText());
            break;
        case ID_HIST_REFRESH: {
            UpdateInfoRow(ID_HIST_INFO, HistoryInfoText());
            for (size_t i = 0; i < g_rows.size(); ++i) {
                if (g_rows[i].row_id == ID_HIST_VIEW) {
                    g_rows[i].value =
                        slint::SharedString(u8(HistoryViewText()).c_str());
                    if (g_model) g_model->set_row_data((int)i, g_rows[i]);
                    break;
                }
            }
            break;
        }
        case ID_HIST_OPEN: OpenHistoryFile(); break;
        case ID_LEARN_OPEN: OpenLearnDir(); break;
        case ID_DL_SC: StartDownload(1); break;
        case ID_DL_IR: StartDownload(2); break;
        case ID_RESET_EXCLUDE:
            g_s.excludeText = ft::DefaultExcludePathsText();
            UpdateInfoRow(ID_EXCLUDE_COUNT,
                          L"当前已记忆 " +
                              std::to_wstring(ft::ParseExcludePaths(g_s.excludeText).size()) +
                              L" 条排除项");
            for (size_t i = 0; i < g_rows.size(); ++i) {
                if (g_rows[i].row_id == ID_EXCLUDE) {
                    g_rows[i].value =
                        slint::SharedString(u8(g_s.excludeText).c_str());
                    if (g_model) g_model->set_row_data((int)i, g_rows[i]);
                    break;
                }
            }
            break;
        default: break;
    }
}

static void OnRowToggled(int id, bool on) {
    switch (id) {
        case ID_STARTUP: g_s.startupWanted = on; break;
        case ID_FDJ: g_s.fdjEnabled = on; break;
        case ID_BEAUTIFY: g_s.beautify = on; break;
        case ID_ANTIGHOST: g_s.antiGhost = on; break;
        case ID_WEIGHT_ENABLED: g_s.weightEnabled = on; break;
        case ID_SKIPDOT: g_s.skipDotFolders = on; break;
        case ID_SHELLSHOW: g_s.shellShowWindow = on; break;
        case ID_WINGROUP: g_s.winGroupProc = on; break;
        case ID_WINUWP: g_s.winShowUwp = on; break;
        case ID_WINPROC: g_s.winShowProc = on; break;
        case ID_TOP: g_s.topEnabled = on; break;
        case ID_CMD: g_s.cmdEnabled = on; break;
        case ID_WIN: g_s.winEnabled = on; break;
        case ID_CAPTURE: g_s.captureEnabled = on; break;
        case ID_LEARN_ENABLED: g_s.learnEnabled = on; break;
        case ID_HIST_ENABLED: g_s.historyEnabled = on; break;
        default: break;
    }
    // 让界面上的勾选状态跟着模型走（点击回调不会自动改 model）
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (g_rows[i].row_id != id) continue;
        g_rows[i].checked = on;
        if (g_model) g_model->set_row_data((int)i, g_rows[i]);
        break;
    }
    PushPreview();
}

static void OnRowChosen(int id, int idx) {
    auto it = g_choice.find(id);
    if (it == g_choice.end() || idx < 0 || idx >= (int)it->second.size()) return;
    int value = it->second[idx].value;
    switch (id) {
        case ID_UIENGINE: g_s.uiEngine = value; break;
        case ID_HOTKEYMODE: g_s.hotkeyMode = value; break;
        case ID_CENTERWAKE: g_s.centerWake = value; break;
        case ID_DFBACKEND: g_s.dfBackend = value; break;
        case ID_THEME:
            g_s.themeIdx = value;
            ShowTab(2);  // 透明度/圆角滑块绑在当前主题上，换主题要刷新
            PushPreview();
            return;
        case ID_WEIGHT_FLUSH: g_s.weightFlush = value; break;
        case ID_WEIGHT_MAX: g_s.weightMaxEntries = value; break;
        case ID_SHELLTYPE:
            g_s.shellType = value;
            ShowTab(7);  // 自定义 Shell 才会出现的两行
            PushPreview();
            return;
        case ID_SHELLCWD: g_s.shellDefaultCwd = value; break;
        case ID_WINCACHE: g_s.winCacheSec = value; break;
        case ID_LEARN_SCOPE: g_s.learnScope = value; break;
        case ID_LEARN_MAX: g_s.learnMaxEntries = value; break;
        case ID_LEARN_EXPIRE: g_s.learnExpire = value; break;
        case ID_HIST_MAX: g_s.historyMaxLines = value; break;
        default: break;
    }
    // 下拉显示文本跟着更新
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (g_rows[i].row_id != id) continue;
        g_rows[i].option_index = idx;
        if (g_model) g_model->set_row_data((int)i, g_rows[i]);
        break;
    }
    PushPreview();
}

static void OnRowEdited(int id, const std::string& text) {
    switch (id) {
        case ID_RULES: g_s.rulesText = EditorText(text); break;
        case ID_LAUNCH: g_s.launchText = EditorText(text); break;
        case ID_KILL: g_s.killText = EditorText(text); break;
        case ID_EXCLUDE: {
            g_s.excludeText = EditorText(text);
            UpdateInfoRow(ID_EXCLUDE_COUNT,
                          L"当前已记忆 " +
                              std::to_wstring(ft::ParseExcludePaths(g_s.excludeText).size()) +
                              L" 条排除项");
            break;
        }
        case ID_INDEXDIRS: {
            g_s.indexDirsText = EditorText(text);
            UpdateInfoRow(ID_INDEXDIRS_COUNT,
                          L"保留 " +
                              std::to_wstring(ft::ParseIndexDirs(g_s.indexDirsText).size()) +
                              L" 条旧目录配置（不生效）");
            break;
        }
        case ID_SHELLPATH: g_s.shellCustomPath = EditorText(text); break;
        case ID_SHELLARGS: g_s.shellCustomArgs = EditorText(text); break;
        default: return;
    }
    PushPreview();
}

static void OnSliderChanged(int id, int value) {
    if (g_s.themeIdx < 0 || g_s.themeIdx >= (int)g_s.tune.size()) return;
    if (id == ID_ALPHA) g_s.tune[g_s.themeIdx].alpha = value;
    else if (id == ID_RADIUS) g_s.tune[g_s.themeIdx].radius = value;
    else return;
    for (size_t i = 0; i < g_rows.size(); ++i) {
        if (g_rows[i].row_id != id) continue;
        g_rows[i].amount = value;
        if (g_model) g_model->set_row_data((int)i, g_rows[i]);
        break;
    }
    PushPreview();
}

// ---------------- 收主进程消息 ----------------
static WNDPROC g_prevProc = nullptr;
static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)l;
        const DWORD id = cds ? (DWORD)cds->dwData : 0;
        if (cds && ipc::IsOurMsg(id)) {
            if (id == ipc::MsgStats && cds->cbData == sizeof(ipc::Stats)) {
                const ipc::Stats* st = (const ipc::Stats*)cds->lpData;
                unsigned long long weightCount = st->weightCount, learnCount = st->learnCount;
                slint::invoke_from_event_loop([weightCount, learnCount]() {
                    UpdateInfoRow(ID_WEIGHT_COUNT,
                                  L"当前已记忆 " + std::to_wstring(weightCount) + L" 条权重数据");
                    UpdateInfoRow(ID_LEARN_COUNT,
                                  L"当前已学习 " + std::to_wstring(learnCount) + L" 条快捷方式");
                });
            } else if (id == ipc::MsgMainQuitting) {
                slint::invoke_from_event_loop([]() { QuitApp(); });
            }
        }
        return TRUE;
    }
    if (m == WM_CLOSE) {
        DoClose();
        return 0;
    }
    return CallWindowProcW(g_prevProc, h, m, w, l);
}

// ---------------- 启动 ----------------
static void ParseArgs() {
    int wc = 0;
    LPWSTR* wl = CommandLineToArgvW(GetCommandLineW(), &wc);
    if (!wl) return;
    for (int i = 0; i + 1 < wc; ++i) {
        if (wcscmp(wl[i], ipc::kArgIpc) == 0)
            g_hMain = (HWND)(ULONG_PTR)_wtoi64(wl[i + 1]);
        else if (wcscmp(wl[i], ipc::kArgMainExe) == 0)
            g_mainExe = wl[i + 1];
    }
    LocalFree(wl);
}

static void ApplyDarkFrame(HWND hwnd) {
    if (!hwnd) return;
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    DWORD pref = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ParseArgs();
    if (!g_hMain || !IsWindow(g_hMain)) return 1;

    g_s.Load();
    if (g_s.themeIdx < 0 || g_s.themeIdx >= kThemeCount) g_s.themeIdx = 0;
    if ((int)g_s.tune.size() < kThemeCount) g_s.tune.resize(kThemeCount);

    DWORD lastTab = 0;
    if (ft::RegGetDword(L"SettingsTab", lastTab) && lastTab < (DWORD)kTabCount)
        g_tab = (int)lastTab;

    slint::select_backend({ .backend = "winit-software" });
    g_win = SettingsWindow::create();

    std::vector<slint::SharedString> tabNames;
    for (int i = 0; i < kTabCount; ++i) tabNames.push_back(slint::SharedString(u8(kTabNames[i]).c_str()));
    g_tabsModel = std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(tabNames));
    g_model = std::make_shared<slint::VectorModel<FormRow>>();
    (*g_win)->set_tabs(g_tabsModel);

    (*g_win)->on_tab_changed([](int tab) { ShowTab(tab); });
    (*g_win)->on_row_toggled([](int id, bool on) { OnRowToggled(id, on); });
    (*g_win)->on_row_chosen([](int id, int idx) { OnRowChosen(id, idx); });
    (*g_win)->on_row_edited([](int id, const slint::SharedString& text) {
        OnRowEdited(id, std::string(text.data() ? text.data() : "", text.size()));
    });
    (*g_win)->on_row_clicked([](int id) { OnRowClicked(id); });
    (*g_win)->on_slider_changed([](int id, int value) { OnSliderChanged(id, value); });
    (*g_win)->on_reset_clicked([]() {
        g_s.rulesText = ft::DefaultRulesText();
        for (size_t i = 0; i < g_rows.size(); ++i) {
            if (g_rows[i].row_id != ID_RULES) continue;
            g_rows[i].value = slint::SharedString(u8(g_s.rulesText).c_str());
            if (g_model) g_model->set_row_data((int)i, g_rows[i]);
            break;
        }
    });
    (*g_win)->on_save_clicked([]() { DoSave(); });
    (*g_win)->on_cancel_clicked([]() { DoCancel(); });

    (*g_win)->show();
    HWND hwnd = (*g_win)->window().win32_hwnd();
    g_hSelf = hwnd;
    if (hwnd) {
        ApplyDarkFrame(hwnd);
        g_prevProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)SubProc);
        RECT mr{};
        if (GetWindowRect(g_hMain, &mr)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromWindow(g_hMain, MONITOR_DEFAULTTONEAREST), &mi)) {
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                int W = wr.right - wr.left, H = wr.bottom - wr.top;
                int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2;
                int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2;
                SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    ShowTab(g_tab);

    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgHello, &hdr, sizeof(hdr));
    SendToMain(ipc::MsgQueryStats, &hdr, sizeof(hdr));

    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);
    g_win.reset();
    return 0;
}
