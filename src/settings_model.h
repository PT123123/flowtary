#pragma once
// =====================================================================
// Flowtary 设置数据模型（主程序与 flowtary-settings.exe 共用）
//
// 这里是「设置项」的唯一真源：注册表值名/类型/钳制范围、四个多行编辑器的
// 语法与序列化、内置默认文本、下拉候选表、weights.dat / learned.dat 路径。
// 两侧都编译 settings_model.cpp，避免解析规则漂移。
//
// 详细的行为依据（含 src/main.cpp 行号证据）见 docs/SETTINGS_SIDECAR_CONTRACT.md。
// =====================================================================

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "themes.h"

namespace ft {

// ---------------- 通用文本工具 ----------------
std::wstring TrimW(const std::wstring& s);
std::wstring ToLowerW(const std::wstring& s);
bool EndsWithI(const std::wstring& s, const WCHAR* suffix);
// 通配符匹配（仅 * 与 ?），大小写不敏感，DP 全串匹配
bool MatchWildcardI(const wchar_t* pattern, const wchar_t* text);

// ---------------- 注册表原语 ----------------
// 根键固定为 HKCU\Software\Flowtary
constexpr const WCHAR* kRegPath = L"Software\\Flowtary";

std::wstring LoadRegText(const WCHAR* name);
void SaveRegText(const WCHAR* name, const std::wstring& text);
bool RegGetDword(const WCHAR* name, DWORD& out);
void RegSetDword(const WCHAR* name, DWORD v);

// ---------------- 数据结构 ----------------
struct WebCmd {
    std::wstring prefix;       // 已小写
    std::wstring urlTemplate;  // 原样（大小写敏感）
};

struct CmdGroup {
    std::wstring key;                       // 关键字（小写存储，比较时忽略大小写）
    std::vector<std::wstring> targets;      // ';' 分隔的目标，已逐段 trim、丢空
};

// ---------------- 网页规则 ----------------
constexpr int kWebRulesVer = 2;
std::wstring DefaultRulesText();
const WCHAR* LegacyRulesV1Text();
bool RulesTextEqual(const std::wstring& a, const std::wstring& b);
std::vector<WebCmd> ParseRules(const std::wstring& text);
std::wstring RulesToText(const std::vector<WebCmd>& rules);
// 读注册表原文（含默认集自动升级落盘）；返回编辑器应显示的文本与解析结果
struct WebRulesLoaded {
    std::wstring editorText;    // 回显用：解析后重序列化（注释/重复行丢失）
    std::vector<WebCmd> cmds;
};
WebRulesLoaded LoadWebRules();
void SaveWebRules(const std::wstring& editorText);   // 原文直写 + WebRulesVer

// ---------------- 一键组 ----------------
std::vector<CmdGroup> ParseGroups(const std::wstring& text);
std::wstring GroupsToText(const std::vector<CmdGroup>& groups);

// ---------------- 排除路径 ----------------
constexpr DWORD kExcludeDefaultsVer = 3;

std::wstring NormalizePathForExclude(const std::wstring& s);
std::wstring NormalizePatternForExclude(const std::wstring& s);
std::vector<std::wstring> ParseExcludePaths(const std::wstring& text);
std::wstring ExcludePathsToText(const std::vector<std::wstring>& patterns);
std::wstring DefaultExcludePathsText();
std::vector<std::wstring> DefaultExcludePaths();
bool IsLegacyDotDefault(const std::wstring& pattern);
bool HasExcludePattern(const std::vector<std::wstring>& patterns, const std::wstring& p);

// 读注册表并做首启落盘 / 旧版默认项迁移；返回「生效的原文条目列表」。
std::vector<std::wstring> LoadExcludePaths();

// ---------------- weights.dat / learned.dat ----------------
// 行内字段分隔符 0x1F；文件为裸 UTF-16LE，全量重写，行尾 \r\n
constexpr WCHAR kEntrySep = L'\x1f';

std::wstring FlowtaryDataDir();   // %APPDATA%\Flowtary
std::wstring WeightsFilePath();
std::wstring LearnedFilePath();
std::wstring LearnedLinkDir();

// 仅用于设置界面的计数显示：按各自行格式解析出条目数（不做内存态合并）。
size_t CountWeightEntries();
size_t CountLearnedEntries();

// ---------------- 开机自启 ----------------
// 注意：必须写入「主程序 exe」路径，sidecar 不能用自己的模块路径。
bool StartupEnabled();
void SetStartupPath(const std::wstring& mainExePath, bool on);

// ---------------- 设置快照（所有可持久化字段） ----------------
struct Settings {
    // 常规
    int centerWake = 1;  // 1=屏幕居中 0=跟随鼠标（下拉候选表用 int 绑定）
    int hotkeyMode = 0;
    // 唤起热键总开关：只读。托盘菜单可实时改它，sidecar 保存时不回写，
    // 否则会把用户在设置窗打开期间的托盘操作覆盖掉。
    bool hotkeyWake = true;
    bool fdjEnabled = true;
    bool startupWanted = false;
    // 主题
    int themeIdx = 0;
    bool beautify = true;
    bool antiGhost = false;
    std::vector<ThemeTune> tune;     // 与 kThemes 下标一一对应
    // 文本类（编辑器原文 + 解析结果）
    std::wstring rulesText;
    std::vector<WebCmd> webCmds;
    std::wstring launchText, killText;
    std::vector<CmdGroup> groupsLaunch, groupsKill;
    std::wstring excludeText;
    std::vector<std::wstring> excludePaths;
    bool skipDotFolders = true;
    int dfBackend = 0;              // d/f 搜索后端：0=新方案（内置索引） 1=Everything 旧链路
    // 搜索权重
    bool weightEnabled = true;
    int weightFlush = 1;
    int weightMaxEntries = 5000;
    // 自学习
    bool learnEnabled = true;
    int learnScope = 1;
    int learnMaxEntries = 500;
    int learnExpire = 0;
    // 命令开关
    bool topEnabled = true, cmdEnabled = true, winEnabled = true, captureEnabled = true;
    // Shell 与窗口
    int shellType = 0;
    std::wstring shellCustomPath, shellCustomArgs;
    int shellDefaultCwd = 0;
    bool shellShowWindow = true;
    bool winGroupProc = true, winShowUwp = true, winShowProc = true;
    int winCacheSec = 5;

    void Load();     // 读注册表 + 解析文本
    void Save();  // 全量写回（含 ThemeTune blob），并回灌解析结果
    void ApplyTuneToThemes() const;  // 把 tune 灌进 kThemes（透明度/圆角推导）
};

// ---------------- 下拉候选表（label ↔ 存储值） ----------------
struct Option {
    std::wstring label;
    int value;
};

inline std::vector<Option> HotkeyModeOptions() {
    return {{L"Alt + 数字（1–9,0）", 0}, {L"Alt + 字母（A–J）", 1}, {L"关闭", 2}};
}
inline std::vector<Option> CenterWakeOptions() {
    return {{L"屏幕居中", 1}, {L"跟随鼠标", 0}};
}
inline std::vector<Option> WeightFlushOptions() {
    return {{L"立即写入", 0}, {L"延迟合并写入", 1}, {L"退出时写入", 2}};
}
inline std::vector<Option> WeightMaxOptions() {
    return {{L"1000 条", 1000}, {L"5000 条", 5000}, {L"20000 条", 20000}, {L"50000 条", 50000}};
}
inline std::vector<Option> LearnScopeOptions() {
    return {{L"仅 exe", 0}, {L"exe 与文件夹", 1}, {L"所有文件与文件夹", 2}};
}
inline std::vector<Option> LearnMaxOptions() {
    return {{L"100 条", 100}, {L"200 条", 200}, {L"500 条", 500},
            {L"1000 条", 1000}, {L"2000 条", 2000}, {L"5000 条", 5000}};
}
inline std::vector<Option> LearnExpireOptions() {
    return {{L"永不过期（长期保留）", 0}, {L"90 天未使用即清理", 90},
            {L"180 天未使用即清理", 180}, {L"365 天未使用即清理", 365}};
}
inline std::vector<Option> DfBackendOptions() {
    return {{L"内置搜索（Flowtary Search）", 0}, {L"Everything（旧链路，需 Everything 运行）", 1}};
}
inline std::vector<Option> ShellTypeOptions() {
    return {{L"命令提示符 (cmd)", 0}, {L"PowerShell", 1}, {L"Git Bash", 2}, {L"自定义…", 3}};
}
inline std::vector<Option> ShellCwdOptions() {
    return {{L"用户目录 (%USERPROFILE%)", 0}, {L"系统默认目录 (System32)", 1}, {L"桌面目录", 2}};
}
inline std::vector<Option> WinCacheOptions() {
    return {{L"1 秒", 1}, {L"2 秒", 2}, {L"5 秒", 5},
            {L"10 秒", 10}, {L"30 秒", 30}, {L"60 秒", 60}};
}

// 下拉定位：给定存储值找到最贴近的档位下标；
// 若值不在任何档位上，把它作为额外项插入列表并返回其下标（避免 UI 说谎）。
int LocateOption(std::vector<Option>& opts, int value, int lowerBound, int upperBound);

}  // namespace ft
