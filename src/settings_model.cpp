// =====================================================================
// settings_model.cpp — 见 settings_model.h 顶部说明。
// 实现从 src/main.cpp 原样搬出，行为（含钳制范围、默认值、升级落盘）保持一致。
// =====================================================================
#include "settings_model.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <vector>

namespace ft {

// ---------------- 通用文本工具 ----------------

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (wchar_t)std::towlower(c);
    return r;
}

std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && iswspace(s[b])) ++b;
    while (e > b && iswspace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool EndsWithI(const std::wstring& s, const WCHAR* suffix) {
    size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool MatchWildcardI(const wchar_t* pattern, const wchar_t* text) {
    size_t pn = wcslen(pattern), sn = wcslen(text);
    std::vector<std::vector<bool>> dp(pn + 1, std::vector<bool>(sn + 1, false));
    dp[0][0] = true;
    for (size_t i = 1; i <= pn; ++i) {
        if (pattern[i - 1] == L'*') dp[i][0] = dp[i - 1][0];
        for (size_t j = 1; j <= sn; ++j) {
            wchar_t pc = pattern[i - 1];
            if (pc == L'*')
                dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
            else if (pc == L'?')
                dp[i][j] = dp[i - 1][j - 1];
            else
                dp[i][j] = dp[i - 1][j - 1] && (std::towlower(pc) == std::towlower(text[j - 1]));
        }
    }
    return dp[pn][sn];
}

std::wstring NormalizePathForExclude(const std::wstring& s) {
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

std::wstring NormalizePatternForExclude(const std::wstring& s) {
    return NormalizePathForExclude(s);
}

// ---------------- 注册表原语 ----------------

std::wstring LoadRegText(const WCHAR* name) {
    std::wstring text;
    DWORD cb = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ, nullptr, nullptr, &cb) ==
            ERROR_SUCCESS &&
        cb > sizeof(WCHAR)) {
        std::vector<WCHAR> buf(cb / sizeof(WCHAR));
        if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_SZ, nullptr, buf.data(),
                         &cb) == ERROR_SUCCESS)
            text = buf.data();
    }
    return text;
}

void SaveRegText(const WCHAR* name, const std::wstring& text) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegPath, name, REG_SZ, text.c_str(),
                    (DWORD)((text.size() + 1) * sizeof(WCHAR)));
}

bool RegGetDword(const WCHAR* name, DWORD& out) {
    DWORD cb = sizeof(out);
    return RegGetValueW(HKEY_CURRENT_USER, kRegPath, name, RRF_RT_REG_DWORD, nullptr, &out,
                        &cb) == ERROR_SUCCESS;
}

void RegSetDword(const WCHAR* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegPath, name, REG_DWORD, &v, sizeof(v));
}

// ---------------- 网页规则 ----------------

static const WCHAR* const kDefaultRulesText =
    L"bd https://www.baidu.com/#ie=UTF-8&wd={q}\r\n"
    L"bili https://search.bilibili.com/all?keyword={q}\r\n"
    L"xhs https://www.xiaohongshu.com/search_result?keyword={q}\r\n"
    L"zhihu https://www.zhihu.com/search?q={q}\r\n"
    L"douban https://www.douban.com/search?q={q}\r\n"
    L"gh https://github.com/search?q={q}\r\n"
    L"google https://www.google.com/search?q={q}\r\n"
    L"x https://x.com/search?q={q}";

static const WCHAR* const kLegacyRulesV1 =
    L"bd https://www.baidu.com/#ie=UTF-8&wd={q}\r\n"
    L"bili https://search.bilibili.com/all?keyword={q}\r\n"
    L"xhs https://www.xiaohongshu.com/search_result?keyword={q}\r\n"
    L"zhihu https://www.zhihu.com/search?q={q}\r\n"
    L"douban https://www.douban.com/search?q={q}\r\n"
    L"gh https://github.com/search?q={q}\r\n"
    L"google https://www.google.com/search?q={q}";

std::wstring DefaultRulesText() { return kDefaultRulesText; }
const WCHAR* LegacyRulesV1Text() { return kLegacyRulesV1; }

bool RulesTextEqual(const std::wstring& a, const std::wstring& b) {
    auto lines = [](const std::wstring& s) {
        std::vector<std::wstring> v;
        size_t i = 0;
        while (i <= s.size()) {
            size_t j = s.find(L'\n', i);
            if (j == std::wstring::npos) j = s.size();
            std::wstring line = s.substr(i, j - i);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            v.push_back(line);
            if (j == s.size()) break;
            i = j + 1;
        }
        return v;
    };
    return lines(a) == lines(b);
}

std::vector<WebCmd> ParseRules(const std::wstring& text) {
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

std::wstring RulesToText(const std::vector<WebCmd>& rules) {
    std::wstring t;
    for (auto& r : rules) t += r.prefix + L" " + r.urlTemplate + L"\r\n";
    return t;
}

WebRulesLoaded LoadWebRules() {
    WebRulesLoaded out;
    std::wstring text = LoadRegText(L"WebRules");
    // 默认规则集升级：仅当注册表内容仍是某个旧版默认快照（用户从未改过）时整体刷新
    if (!text.empty() && RulesTextEqual(text, kLegacyRulesV1)) {
        text = DefaultRulesText();
        SaveRegText(L"WebRules", text);
        RegSetDword(L"WebRulesVer", (DWORD)kWebRulesVer);
    }
    out.editorText = text.empty() ? std::wstring{} : text;
    out.cmds = ParseRules(text.empty() ? DefaultRulesText() : text);
    return out;
}

void SaveWebRules(const std::wstring& editorText) {
    SaveRegText(L"WebRules", editorText);
    RegSetDword(L"WebRulesVer", (DWORD)kWebRulesVer);  // 标记「已编辑过」，升级时不覆盖用户数据
}

// ---------------- 一键组 ----------------

std::vector<CmdGroup> ParseGroups(const std::wstring& text) {
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

std::wstring GroupsToText(const std::vector<CmdGroup>& groups) {
    std::wstring t;
    for (auto& g : groups) {
        t += g.key + L" ";
        for (size_t i = 0; i < g.targets.size(); ++i) {
            if (i) t += L";";
            t += g.targets[i];
        }
        t += L"\r\n";
    }
    return t;
}

// ---------------- 排除路径 ----------------

std::vector<std::wstring> ParseExcludePaths(const std::wstring& text) {
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

std::wstring ExcludePathsToText(const std::vector<std::wstring>& patterns) {
    std::wstring t;
    t += L"# 排除路径：每行一条，支持通配符 * ?。路径以排除项开头即视为命中（含整棵子树）。\r\n";
    t += L"# 作用于 Everything 搜索（d/f 前缀）与本地程序搜索的结果，网页与一键组不受影响。\r\n";
    for (auto& p : patterns) t += p + L"\r\n";
    return t;
}

std::wstring DefaultExcludePathsText() {
    std::wstring t =
        L"# 排除路径：每行一条，支持通配符 * ?。路径以排除项开头即视为命中（含整棵子树）。\r\n"
        L"# 作用于 Everything 搜索（d/f 前缀）与本地程序搜索的结果，网页与一键组不受影响。\r\n"
        L"# 默认已排除各盘符回收站、系统目录，以及 node_modules/__pycache__ 等开发噪音目录\r\n"
        L"# 和编辑器备份/临时文件（*~、~$、*.swp 等），可自行增删。\r\n"
        L"# 以 . 开头的文件/文件夹（.git/.vscode/.gitignore …）由上方开关统一跳过，无需在此逐条填写。\r\n";
    DWORD bits = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(bits & (1u << i))) continue;
        t.push_back((WCHAR)(L'A' + i));
        t += L":\\$RECYCLE.BIN\r\n";
    }
    t += L"*\\System Volume Information\\*\r\n";
    t += L"*\\RECYCLER\\*\r\n";
    t += L"*\\node_modules\r\n";
    t += L"*\\node_modules\\*\r\n";
    t += L"*\\__pycache__\\*\r\n";
    t += L"*~\r\n";
    t += L"*\\~$*\r\n";
    t += L"*.swp\r\n";
    t += L"*.swo\r\n";
    return t;
}

static const WCHAR* const kLegacyDotDefaults[] = {L"*\\.git", L"*\\.git\\*",
                                                  L"*\\.vscode\\extensions\\*"};

bool IsLegacyDotDefault(const std::wstring& pattern) {
    std::wstring np = NormalizePatternForExclude(pattern);
    for (auto* d : kLegacyDotDefaults)
        if (np == NormalizePatternForExclude(d)) return true;
    return false;
}

std::vector<std::wstring> DefaultExcludePaths() {
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
    out.push_back(L"*\\node_modules");
    out.push_back(L"*\\node_modules\\*");
    out.push_back(L"*\\__pycache__\\*");
    out.push_back(L"*~");
    out.push_back(L"*\\~$*");
    out.push_back(L"*.swp");
    out.push_back(L"*.swo");
    return out;
}

bool HasExcludePattern(const std::vector<std::wstring>& patterns, const std::wstring& p) {
    std::wstring np = NormalizePatternForExclude(p);
    for (auto& e : patterns)
        if (NormalizePatternForExclude(e) == np) return true;
    return false;
}

std::vector<std::wstring> LoadExcludePaths() {
    std::wstring text = LoadRegText(L"ExcludePaths");
    if (text.empty()) {
        // 首次启动：写入默认项文本（便于用户在编辑器看到默认行为），并把默认模式灌入内存
        std::wstring def = DefaultExcludePathsText();
        SaveRegText(L"ExcludePaths", def);
        RegSetDword(L"ExcludeVer", kExcludeDefaultsVer);
        return DefaultExcludePaths();
    }
    auto parsed = ParseExcludePaths(text);
    DWORD ver = 0;
    RegGetDword(L"ExcludeVer", ver);
    if (ver < kExcludeDefaultsVer) {
        // 旧版本升级：剔除已由「不搜索点开头文件夹」开关取代的旧默认项，再并入新增默认项
        std::vector<std::wstring> merged;
        for (auto& p : parsed)
            if (!IsLegacyDotDefault(p)) merged.push_back(p);
        if (merged.empty()) merged = DefaultExcludePaths();
        for (auto& d : DefaultExcludePaths())
            if (!HasExcludePattern(merged, d)) merged.push_back(d);
        SaveRegText(L"ExcludePaths", ExcludePathsToText(merged));
        RegSetDword(L"ExcludeVer", kExcludeDefaultsVer);
        return merged;
    }
    return parsed.empty() ? DefaultExcludePaths() : parsed;
}

// ---------------- weights.dat / learned.dat ----------------

std::wstring FlowtaryDataDir() {
    WCHAR buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH) && buf[0])
        return std::wstring(buf) + L"\\Flowtary";
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring exe(buf);
    size_t slash = exe.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring(L".") : exe.substr(0, slash);
}

std::wstring WeightsFilePath() { return FlowtaryDataDir() + L"\\weights.dat"; }
std::wstring LearnedFilePath() { return FlowtaryDataDir() + L"\\learned.dat"; }
std::wstring LearnedLinkDir() { return FlowtaryDataDir() + L"\\learned"; }

// 裸 UTF-16LE 全文件读出；失败返回空
static std::wstring ReadUtf16File(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 2 || sz.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(h);
        return {};
    }
    std::wstring buf((size_t)sz.QuadPart / sizeof(WCHAR), 0);
    DWORD got = 0;
    BOOL ok = ReadFile(h, &buf[0], (DWORD)sz.QuadPart, &got, nullptr);
    CloseHandle(h);
    if (!ok) return {};
    buf.resize(got / sizeof(WCHAR));
    return buf;
}

// 与主进程写出格式一致：行分隔 \r\n，字段分隔 0x1F。
// fields = 字段数；numField（0 基）指定哪个字段必须是正整数，-1 表示不校验。
static size_t CountSepEntries(const std::wstring& text, size_t fields, int numField) {
    size_t count = 0, i = 0, n = text.size();
    const size_t sepCount = fields - 1;
    while (i < n) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = text.substr(i, j - i);
        i = j + 1;
        if (line.empty()) continue;
        std::vector<size_t> seps;
        size_t p = 0;
        while ((p = line.find(kEntrySep, p)) != std::wstring::npos) { seps.push_back(p); ++p; }
        if (seps.size() < sepCount) continue;
        if (numField >= 0) {
            size_t from = (numField == 0) ? 0 : seps[numField - 1] + 1;
            if (_wtoi(line.c_str() + from) <= 0) continue;
        }
        ++count;
    }
    return count;
}

size_t CountWeightEntries() {
    // 词 \x1f 路径 \x1f 权重
    return CountSepEntries(ReadUtf16File(WeightsFilePath()), 3, 2);
}

size_t CountLearnedEntries() {
    // 词 \x1f 路径 \x1f 次数 \x1f 最近打开 \x1f lnk 名：次数须 > 0
    std::wstring text = ReadUtf16File(LearnedFilePath());
    size_t count = 0, i = 0, n = text.size();
    while (i < n) {
        size_t j = text.find_first_of(L"\r\n", i);
        if (j == std::wstring::npos) j = n;
        std::wstring line = text.substr(i, j - i);
        i = j + 1;
        if (line.empty()) continue;
        size_t s1 = line.find(kEntrySep);
        size_t s2 = (s1 == std::wstring::npos) ? std::wstring::npos : line.find(kEntrySep, s1 + 1);
        size_t s3 = (s2 == std::wstring::npos) ? std::wstring::npos : line.find(kEntrySep, s2 + 1);
        size_t s4 = (s3 == std::wstring::npos) ? std::wstring::npos : line.find(kEntrySep, s3 + 1);
        if (s1 == std::wstring::npos || s2 == std::wstring::npos || s3 == std::wstring::npos ||
            s4 == std::wstring::npos)
            continue;
        if (line.substr(0, s1).empty() || line.substr(s1 + 1, s2 - s1 - 1).empty()) continue;
        if (_wtoi(line.c_str() + s2 + 1) <= 0) continue;
        ++count;
    }
    return count;
}

// ---------------- 开机自启 ----------------

bool StartupEnabled() {
    WCHAR buf[MAX_PATH]{};
    DWORD cb = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Flowtary",
                        RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS;
}

void SetStartupPath(const std::wstring& mainExePath, bool on) {
    const WCHAR* run = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (on) {
        std::wstring v = L"\"" + mainExePath + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, run, L"Flowtary", REG_SZ, v.c_str(),
                        (DWORD)((v.size() + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, run, L"Flowtary");
    }
}

// ---------------- Settings::Load / Save ----------------

void Settings::Load() {
    DWORD v = 0;

    centerWake = 1;
    if (RegGetDword(L"CenterWake", v)) centerWake = v ? 1 : 0;

    hotkeyMode = 0;
    if (RegGetDword(L"HotkeyMode", v)) {
        hotkeyMode = (int)v;
    } else if (RegGetDword(L"HotkeyLetters", v)) {
        hotkeyMode = v ? 0 : 2;  // 兼容旧键：1=数字(0)，0=关闭(2)
    }
    if (hotkeyMode < 0 || hotkeyMode > 2) hotkeyMode = 0;

    v = 1;
    hotkeyWake = RegGetDword(L"HotkeyWake", v) ? v != 0 : true;

    v = 1;
    fdjEnabled = RegGetDword(L"FileDlgJump", v) ? v != 0 : true;

    themeIdx = 0;
    if (RegGetDword(L"Theme", v)) themeIdx = (int)v;
    if (themeIdx < 0 || themeIdx >= kThemeCount) themeIdx = 0;

    v = 1;
    beautify = RegGetDword(L"Beautify", v) ? v != 0 : true;
    v = 0;
    antiGhost = RegGetDword(L"AntiGhost", v) ? v != 0 : false;
    v = 1;
    skipDotFolders = RegGetDword(L"SkipDotFolders", v) ? v != 0 : true;
    v = 0;
    if (RegGetDword(L"DfBackend", v)) dfBackend = (v == 1) ? 1 : 0;

    // 主题微调 blob：长度与主题数量不符时整块忽略（用 kThemes 预设默认）
    tune.assign(kThemeCount, ThemeTune());
    {
        DWORD cbT = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, L"ThemeTune", RRF_RT_REG_BINARY, nullptr,
                         nullptr, &cbT) == ERROR_SUCCESS && cbT == sizeof(ThemeTune) * kThemeCount) {
            if (RegGetValueW(HKEY_CURRENT_USER, kRegPath, L"ThemeTune", RRF_RT_REG_BINARY, nullptr,
                             tune.data(), &cbT) == ERROR_SUCCESS) {
                for (auto& t : tune) {
                    t.alpha = (std::max)(0, (std::min)(255, t.alpha));
                    t.radius = (std::max)(0, (std::min)(14, t.radius));
                }
            }
        }
    }

    auto web = LoadWebRules();
    webCmds = std::move(web.cmds);
    // 编辑器回显 = 解析结果重序列化（注释/空行/重复行丢失，与原设置窗一致）
    rulesText = RulesToText(webCmds);

    launchText = LoadRegText(L"GroupLaunch");
    killText = LoadRegText(L"GroupKill");
    groupsLaunch = ParseGroups(launchText);
    groupsKill = ParseGroups(killText);
    launchText = GroupsToText(groupsLaunch);
    killText = GroupsToText(groupsKill);

    excludePaths = LoadExcludePaths();
    excludeText = ExcludePathsToText(excludePaths);

    v = 1;
    weightEnabled = RegGetDword(L"WeightEnabled", v) ? v != 0 : true;
    v = 1;
    if (RegGetDword(L"WeightFlush", v)) weightFlush = (int)v;
    if (weightFlush < 0 || weightFlush > 2) weightFlush = 1;
    v = 5000;
    if (RegGetDword(L"WeightMaxEntries", v)) weightMaxEntries = (int)v;
    if (weightMaxEntries < 100) weightMaxEntries = 5000;
    if (weightMaxEntries > 200000) weightMaxEntries = 200000;

    v = 1;
    learnEnabled = RegGetDword(L"LearnEnabled", v) ? v != 0 : true;
    v = 1;
    if (RegGetDword(L"LearnScope", v)) learnScope = (int)v;
    if (learnScope < 0 || learnScope > 2) learnScope = 1;
    v = 500;
    if (RegGetDword(L"LearnMaxEntries", v)) learnMaxEntries = (int)v;
    if (learnMaxEntries < 10 || learnMaxEntries > 100000) learnMaxEntries = 500;
    v = 0;
    if (RegGetDword(L"LearnExpireDays", v)) learnExpire = (int)v;
    if (learnExpire < 0) learnExpire = 0;

    v = 1;
    topEnabled = RegGetDword(L"TopCmd", v) ? v != 0 : true;
    v = 1;
    cmdEnabled = RegGetDword(L"CmdCmd", v) ? v != 0 : true;
    v = 1;
    winEnabled = RegGetDword(L"WinCmd", v) ? v != 0 : true;
    v = 1;
    captureEnabled = RegGetDword(L"CaptureCmd", v) ? v != 0 : true;

    shellType = 0;
    if (RegGetDword(L"ShellType", v)) {
        shellType = (int)v;
        if (shellType < 0 || shellType > 3) shellType = 0;
    }
    shellCustomPath = LoadRegText(L"ShellCustomPath");
    shellCustomArgs = LoadRegText(L"ShellCustomArgs");
    shellDefaultCwd = 0;
    if (RegGetDword(L"ShellDefaultCwd", v)) {
        shellDefaultCwd = (int)v;
        if (shellDefaultCwd < 0 || shellDefaultCwd > 2) shellDefaultCwd = 0;
    }
    v = 1;
    shellShowWindow = RegGetDword(L"ShellShowWindow", v) ? v != 0 : true;
    v = 1;
    winGroupProc = RegGetDword(L"WinGroupProc", v) ? v != 0 : true;
    v = 1;
    winShowUwp = RegGetDword(L"WinShowUwp", v) ? v != 0 : true;
    v = 1;
    winShowProc = RegGetDword(L"WinShowProc", v) ? v != 0 : true;
    v = 5;
    if (RegGetDword(L"WinCacheSec", v)) {
        winCacheSec = (int)v;
        if (winCacheSec < 1 || winCacheSec > 60) winCacheSec = 5;
    }

    startupWanted = StartupEnabled();
}

void Settings::Save() {
    Settings& self = *this;

    RegSetDword(L"CenterWake", centerWake ? 1 : 0);
    RegSetDword(L"HotkeyMode", (DWORD)hotkeyMode);
    RegSetDword(L"FileDlgJump", fdjEnabled ? 1 : 0);
    RegSetDword(L"Theme", (DWORD)themeIdx);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegPath, L"ThemeTune", REG_BINARY, tune.data(),
                    (DWORD)(tune.size() * sizeof(ThemeTune)));
    RegSetDword(L"Beautify", beautify ? 1 : 0);
    RegSetDword(L"AntiGhost", antiGhost ? 1 : 0);

    RegSetDword(L"WeightEnabled", weightEnabled ? 1 : 0);
    RegSetDword(L"WeightFlush", (DWORD)weightFlush);
    RegSetDword(L"WeightMaxEntries", (DWORD)weightMaxEntries);

    RegSetDword(L"LearnEnabled", learnEnabled ? 1 : 0);
    RegSetDword(L"LearnScope", (DWORD)learnScope);
    RegSetDword(L"LearnMaxEntries", (DWORD)learnMaxEntries);
    RegSetDword(L"LearnExpireDays", (DWORD)learnExpire);

    RegSetDword(L"SkipDotFolders", skipDotFolders ? 1 : 0);
    RegSetDword(L"DfBackend", (DWORD)dfBackend);
    RegSetDword(L"TopCmd", topEnabled ? 1 : 0);
    RegSetDword(L"CmdCmd", cmdEnabled ? 1 : 0);
    RegSetDword(L"WinCmd", winEnabled ? 1 : 0);
    RegSetDword(L"CaptureCmd", captureEnabled ? 1 : 0);

    // 文本类：编辑器原文直写；解析结果同步回灌，供调用方立即取用
    SaveWebRules(rulesText);
    self.webCmds = ParseRules(rulesText);
    SaveRegText(L"GroupLaunch", launchText);
    self.groupsLaunch = ParseGroups(launchText);
    SaveRegText(L"GroupKill", killText);
    self.groupsKill = ParseGroups(killText);

    auto parsedExcl = ParseExcludePaths(excludeText);
    if (!parsedExcl.empty()) self.excludePaths = parsedExcl;  // 清空不生效：保留旧值（原行为）
    SaveRegText(L"ExcludePaths", ExcludePathsToText(excludePaths));

    if (shellType == 3) {
        self.shellCustomPath = shellCustomPath;
        self.shellCustomArgs = shellCustomArgs;
    } else {
        self.shellCustomPath.clear();
        self.shellCustomArgs.clear();
    }
    RegSetDword(L"ShellType", (DWORD)shellType);
    SaveRegText(L"ShellCustomPath", shellCustomPath);
    SaveRegText(L"ShellCustomArgs", shellCustomArgs);
    RegSetDword(L"ShellDefaultCwd", (DWORD)shellDefaultCwd);
    RegSetDword(L"ShellShowWindow", shellShowWindow ? 1 : 0);
    RegSetDword(L"WinGroupProc", winGroupProc ? 1 : 0);
    RegSetDword(L"WinShowUwp", winShowUwp ? 1 : 0);
    RegSetDword(L"WinShowProc", winShowProc ? 1 : 0);
    RegSetDword(L"WinCacheSec", (DWORD)winCacheSec);
}

void Settings::ApplyTuneToThemes() const {
    int n = (std::min)(kThemeCount, (int)tune.size());
    for (int i = 0; i < n; ++i) {
        kThemes[i].alpha = (BYTE)tune[i].alpha;
        kThemes[i].radiusWindow = tune[i].radius;
        kThemes[i].radiusCard = (int)(tune[i].radius * 0.8);
        kThemes[i].radiusButton = (int)(tune[i].radius * 0.6);
        kThemes[i].radiusInput = (int)(tune[i].radius * 0.6);
    }
}

// ---------------- 下拉定位 ----------------

int LocateOption(std::vector<Option>& opts, int value, int lowerBound, int upperBound) {
    for (size_t i = 0; i < opts.size(); ++i)
        if (opts[i].value == value) return (int)i;
    if (value < lowerBound || value > upperBound) return -1;  // 非法值：交给钳制逻辑
    // 合法但不在档位上（老版本/手工改过）：插入真实值，保证 UI 说的是实话
    std::wstring label = std::to_wstring(value);
    size_t pos = opts.size();
    for (size_t i = 0; i < opts.size(); ++i) {
        if (opts[i].value > value) { pos = i; break; }
    }
    opts.insert(opts.begin() + pos, Option{label.c_str(), value});
    return (int)pos;
}

}  // namespace ft
