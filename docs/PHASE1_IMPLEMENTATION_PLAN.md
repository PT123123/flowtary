# Phase 1 实施计划：Shell 命令 (`>`) + 窗口切换 (`<`)

> 目标：在 Flowtary 中实现 Shell 命令执行和窗口切换功能，包含设置页面完整配置
> 预估工期：5-7 个工作日
> 基于：main.cpp 单文件架构，纯 Win32 + GDI，零依赖
> 文档同步：2026-09-01 已与 src/main.cpp 实际实现核对（含「前台显示输出窗口」默认勾选）

---

## 整体架构影响分析

### 核心数据结构变更
- `Row::Kind` 新增 `Shell`, `Window`
- `Mode` 新增 `Shell`, `Window`
- `App` 结构体新增 12 个字段（配置 + 缓存 + 回退）
- 新增 `WinInfo` 结构体和 `windowCache` 向量

### 现有模块复用点
- `MatchScore` / `IsSubsequence`：窗口标题/进程名模糊匹配直接复用
- `ExecuteRow` / `ShowRowMenu`：扩展 switch 分支即可
- `Refresh` 解析逻辑：在 `WebCmd` 之前插入两个新分支
- 设置页 Tab 机制：完全复用现有 `ShowSettingsTab` / `LayoutSettings` 模式
- 注册表读写：复用 `LoadSettings` / `SaveSettings` 模式
- 自绘控件：复用 `BS_OWNERDRAW` 按钮/复选框/下拉框模式

---

## 详细实施步骤

### Step 1：数据结构与常量定义 (Day 1 上午)

#### 1.1 枚举扩展
**位置**：`Row` 结构体后、`Mode` 枚举后
```cpp
// Row::Kind 新增
enum Kind { File, Folder, Web, Prog, EvFallback, Hint, Group, Shell, Window };

// Mode 新增
enum class Mode { None, Everything, Web, Programs, Shell, Window };
```

#### 1.2 新增控件 ID 常量
**位置**：`IDC_TAB_EXCLUDE` 之后
```cpp
constexpr int IDC_TAB_SHELLWIN   = 3060;   // 新增 Tab：Shell 与窗口

// Shell 设置控件
constexpr int IDC_LBL_SHELLTYPE  = 3061;   // 「Shell 程序」标签
constexpr int IDC_CMB_SHELLTYPE  = 3062;   // Shell 程序下拉（cmd/PowerShell/Git Bash/自定义）
constexpr int IDC_LBL_SHELLPATH  = 3063;   // 「自定义路径」标签
constexpr int IDC_EDT_SHELLPATH  = 3064;   // 自定义 Shell 程序路径编辑框
constexpr int IDC_LBL_SHELLARGS  = 3065;   // 「自定义参数」标签
constexpr int IDC_EDT_SHELLARGS  = 3066;   // 自定义启动参数模板编辑框（{c}=命令）
constexpr int IDC_LBL_SHELLCWD   = 3067;   // 「默认工作目录」标签
constexpr int IDC_CMB_SHELLCWD   = 3068;   // 默认工作目录下拉

// 窗口切换设置控件
constexpr int IDC_CHK_SHOWWIN    = 3069;   // 前台显示输出窗口（复选，默认勾选）
constexpr int IDC_CHK_WINGROUP   = 3070;   // 合并同进程窗口
constexpr int IDC_CHK_WINUWP     = 3071;   // 显示 UWP 应用窗口
constexpr int IDC_CHK_WINPROC    = 3072;   // 副标题显示进程名
constexpr int IDC_LBL_WINCACHE   = 3073;   // 「缓存刷新间隔」标签
constexpr int IDC_CMB_WINCACHE   = 3074;   // 缓存刷新间隔下拉
```

#### 1.3 `App` 结构体新增字段
**位置**：`App` 结构体末尾
```cpp
// Shell 命令配置
int shellType = 0;               // 0=cmd, 1=powershell, 2=git-bash, 3=自定义
std::wstring shellCustomPath;    // 自定义 shell 程序完整路径
std::wstring shellCustomArgs;    // 自定义启动参数模板（{c} 替换为命令）
int shellDefaultCwd = 0;         // 0=用户目录, 1=系统默认(System32), 2=桌面
bool shellShowWindow = true;     // 前台显示输出窗口（否则后台静默执行）默认勾选

// 窗口切换配置
bool winGroupProc = true;        // 合并同进程窗口
bool winShowUwp = true;          // 显示 UWP 应用窗口
bool winShowProc = true;         // 副标题显示进程名
int winCacheSec = 5;             // 窗口枚举缓存刷新间隔（秒）

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

// 窗口枚举缓存
struct WinInfo {
    HWND hwnd;
    std::wstring title;
    std::wstring processName;
    std::wstring className;
    DWORD pid;
    bool isUwp = false;
};
std::vector<WinInfo> windowCache;
DWORD windowCacheTick = 0;
```

#### 1.4 新增消息常量
**位置**：`WM_APP_PROGRAMS_READY` 之后 (约行 385)
```cpp
constexpr int WM_APP_WINDOWS_READY = WM_APP + 4;  // 窗口枚举完成回主线程
constexpr int IDM_WIN_CLOSE = 2021;               // 窗口右键：关闭窗口
constexpr int IDM_WIN_KILL  = 2022;               // 窗口右键：结束进程
```

---

### Step 2：Shell 命令核心逻辑 (Day 1 下午 - Day 2 上午)

#### 2.1 `Refresh()` 解析分支
**位置**：`Refresh()` 函数中，`WebCmd` 分支之前 (约行 1322)
```cpp
// 在 else if (sp != npos && FindWebCmd(tok)) 之前插入：
else if (sp != std::wstring::npos && tok == L">") {
    g.mode = Mode::Shell;
    if (rest.empty()) {
        AddHint(L"输入 Shell 命令，回车执行；Ctrl+Shift+Enter 以管理员运行");
    } else {
        Row r;
        r.kind = Row::Shell;
        r.title = L"> " + rest;
        r.action = rest;
        r.sub = L"Shell 命令";
        g.items.push_back(std::move(r));
    }
}
else if (sp != std::wstring::npos && tok == L"<") {
    g.mode = Mode::Window;
    if (rest.empty()) {
        AddHint(L"输入窗口标题/进程名切换；回车切换，Ctrl+Enter 关闭，Ctrl+Shift+Enter 结束进程");
    } else {
        SearchWindows(rest);
    }
}
```

#### 2.2 `GetCurrentWorkingDir()` 辅助函数
**位置**：`DirOf()` 函数之后 (约行 878)
```cpp
static std::wstring GetCurrentWorkingDir() {
    // 优先级：选中项目录 > %USERPROFILE% > 空(系统默认)
    if (g.sel >= 0 && g.sel < (int)g.items.size()) {
        const auto& r = g.items[g.sel];
        if (r.kind == Row::Folder) return r.action;
        if (r.kind == Row::File || r.kind == Row::Prog) return DirOf(r.action);
    }
    WCHAR buf[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH)) return buf;
    return L"";
}
```

#### 2.3 `ExecuteRow()` Shell 分支
**位置**：`ExecuteRow()` switch 中 (约行 1366)
```cpp
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
```

#### 2.4 热键处理：`Ctrl+Shift+Enter` 设置提权标志
**位置**：主窗口消息循环 `WM_KEYDOWN` 处理 (约行 1700+)
```cpp
// 在 VK_RETURN 处理前检测 Ctrl+Shift：Shell 项以管理员执行，Window 项结束进程
if ((wp == VK_RETURN) && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
    if (g.mode == Mode::Shell && g.sel >= 0 && g.sel < (int)g.items.size() && g.items[g.sel].kind == Row::Shell) {
        ExecuteRow(g.items[g.sel], ExecKind::Admin);  // Shell 分支用 runas + SW_SHOWNORMAL
        return 0;
    }
    if (g.mode == Mode::Window && g.sel >= 0 && g.sel < (int)g.items.size() && g.items[g.sel].kind == Row::Window) {
        ExecuteRow(g.items[g.sel], ExecKind::Kill);   // 窗口项：Ctrl+Shift+Enter = 结束进程
        return 0;
    }
}
```

---

### Step 3：窗口切换核心逻辑 (Day 2 下午 - Day 3)

#### 3.1 `SearchWindows()` 完整实现
**位置**：`Refresh()` 函数之前新增

```cpp
static void SearchWindows(const std::wstring& query) {
    // 1. 缓存有效性检查
    DWORD now = GetTickCount();
    if (g.windowCache.empty() || (now - g.windowCacheTick) > (DWORD)g.winCacheSec * 1000) {
        // 启动后台线程刷新缓存
        _beginthreadex(nullptr, 0, EnumWindowsThread, nullptr, 0, nullptr);
        AddHint(L"正在枚举窗口…");
        return;  // 等待 WM_APP_WINDOWS_READY 回调再继续
    }
    
    // 2. 模糊匹配
    std::wstring ql = ToLowerW(TrimW(query));
    struct Cand { WinInfo* w; int score; };
    std::vector<Cand> cands;
    
    for (auto& w : g.windowCache) {
        if (!g.winShowUwp && w.isUwp) continue;
        
        int s = 0;
        std::wstring tl = ToLowerW(w.title);
        std::wstring pl = ToLowerW(w.processName);
        std::wstring cl = ToLowerW(w.className);
        
        if (tl == ql || pl == ql) s = 4;
        else if (tl.find(ql) == 0 || pl.find(ql) == 0) s = 3;
        else if (tl.find(ql) != std::wstring::npos || pl.find(ql) != std::wstring::npos) s = 2;
        else if (IsSubsequence(tl, ql) || IsSubsequence(pl, ql)) s = 1;
        
        if (s > 0) cands.push_back({&w, s});
    }
    
    // 3. 排序
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        return _wcsicmp(a.w->title.c_str(), b.w->title.c_str()) < 0;
    });
    
    // 4. 分组或平铺生成结果
    g.items.clear();
    if (g.winGroupProc) {
        // 按 PID 分组
        std::unordered_map<DWORD, std::vector<Cand*>> groups;
        for (auto& c : cands) groups[c.w->pid].push_back(&c);
        
        for (auto& [pid, vec] : groups) {
            if (g.items.size() >= ev::kMaxResults) break;
            WinInfo* first = vec[0]->w;
            Row r;
            r.kind = Row::Window;
            r.title = L"▶ " + first->title + L" (" + std::to_wstring(vec.size()) + L")";
            std::wstring sub = first->processName;
            if (g.winShowProc) sub += L" | " + std::to_wstring(vec.size()) + L" tabs";
            r.sub = sub;
            // action 存第一个窗口的 HWND (作为组代表)
            r.action = std::to_wstring((UINT_PTR)first->hwnd);
            // 额外存储：子窗口列表 (用于展开)
            // 这里简化：用 sub 存储，实际可用额外字段
            g.items.push_back(std::move(r));
            
            // 子窗口作为额外行 (仅当组大小>1且结果未满)
            if (vec.size() > 1) {
                for (size_t i = 1; i < vec.size() && g.items.size() < ev::kMaxResults; ++i) {
                    Row cr;
                    cr.kind = Row::Window;
                    cr.title = L"▷ " + vec[i]->w->title;
                    cr.sub = vec[i]->w->processName;
                    cr.action = std::to_wstring((UINT_PTR)vec[i]->w->hwnd);
                    g.items.push_back(std::move(cr));
                }
            }
        }
    } else {
        // 平铺模式
        for (auto& c : cands) {
            if (g.items.size() >= ev::kMaxResults) break;
            Row r;
            r.kind = Row::Window;
            r.title = c.w->title;
            r.sub = g.winShowProc ? c.w->processName : L"";
            r.action = std::to_wstring((UINT_PTR)c.w->hwnd);
            g.items.push_back(std::move(r));
        }
    }
    
    if (g.items.empty()) AddHint(L"无匹配窗口");
    g.sel = 0;
    LayoutAndRepaint();
}
```

#### 3.2 窗口枚举回调线程
**位置**：`ScanProgramsThread` 之后 (约行 1011)
```cpp
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* cache = (std::vector<WinInfo>*)lParam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    
    LONG exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return TRUE;
    
    WCHAR title[512]{};
    if (GetWindowTextW(hwnd, title, 512) == 0) return TRUE;  // 无标题跳过
    
    // 排除 Flowtary 自身窗口
    if (hwnd == g.hwnd || hwnd == g.hSettings) return TRUE;
    
    // 排除系统 Shell 窗口
    WCHAR cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0) return TRUE;
    
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    
    // 进程名
    std::wstring procName;
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        WCHAR buf[MAX_PATH]{};
        if (GetModuleFileNameExW(hProc, nullptr, buf, MAX_PATH)) {
            procName = StripExt(wcsrchr(buf, L'\\') ? wcsrchr(buf, L'\\') + 1 : buf);
        }
        CloseHandle(hProc);
    }
    if (procName.empty()) procName = L"Unknown";
    
    // UWP 检测
    bool isUwp = (wcscmp(cls, L"Windows.UI.Core.CoreWindow") == 0);
    
    cache->push_back({hwnd, title, procName, cls, pid, isUwp});
    return TRUE;
}

static unsigned __stdcall EnumWindowsThread(void*) {
    auto* cache = new (std::nothrow) std::vector<WinInfo>();
    if (cache) {
        EnumWindows(EnumWindowsProc, (LPARAM)cache);
        if (g.hwnd) {
            PostMessageW(g.hwnd, WM_APP_WINDOWS_READY, 0, (LPARAM)cache);
        } else {
            delete cache;
        }
    }
    return 0;
}
```

#### 3.3 `WM_APP_WINDOWS_READY` 处理
**位置**：主窗口消息循环 `WM_APP_PROGRAMS_READY` 处理之后
```cpp
case WM_APP_WINDOWS_READY: {
    auto* cache = (std::vector<WinInfo>*)lp;
    if (cache) {
        g.windowCache = std::move(*cache);
        g.windowCacheTick = GetTickCount();
        delete cache;
        // 缓存就绪，重新执行搜索 (当前查询词在 g.text 中)
        if (g.mode == Mode::Window && !g.text.empty()) {
            size_t sp = g.text.find(L' ');
            std::wstring rest = sp == std::wstring::npos ? L"" : TrimW(g.text.substr(sp + 1));
            if (!rest.empty()) SearchWindows(rest);
        }
    }
    return 0;
}
```

#### 3.4 `ExecuteRow()` Window 分支
**位置**：`ExecuteRow()` switch 中
```cpp
case Row::Window: {
    HWND target = (HWND)_wtoi64(r.action.c_str());
    if (!IsWindow(target)) return false;
    
    // 判断是组头行还是子行：组头行标题以 "▶" 开头
    bool isGroup = !r.title.empty() && r.title[0] == L'▶';
    
    if (isGroup) {
        // 组头行：切换到该组最近活跃窗口 (第一个)
        SetForegroundWindow(target);
    } else {
        // 子行：直接切换
        SetForegroundWindow(target);
    }
    return true;
}
```

#### 3.5 窗口项热键处理
**位置**：主窗口 `WM_KEYDOWN` 处理中 (延续 Step 2.4)
```cpp
// Ctrl+Enter = 关闭窗口 (WM_CLOSE)
if ((wp == VK_RETURN) && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000)) {
    if (g.mode == Mode::Window && g.sel >= 0 && g.sel < (int)g.items.size() && g.items[g.sel].kind == Row::Window) {
        HWND target = (HWND)_wtoi64(g.items[g.sel].action.c_str());
        if (IsWindow(target)) PostMessageW(target, WM_CLOSE, 0, 0);
        // 不隐藏 Flowtary，允许连续关闭多个窗口
        return 0;
    }
}

// Ctrl+Shift+Enter = 结束进程 (TerminateProcess)
if ((wp == VK_RETURN) && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
    if (g.mode == Mode::Window && g.sel >= 0 && g.sel < (int)g.items.size() && g.items[g.sel].kind == Row::Window) {
        HWND target = (HWND)_wtoi64(g.items[g.sel].action.c_str());
        if (IsWindow(target)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(target, &pid);
            if (pid != GetCurrentProcessId()) {  // 保护自身
                HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (hProc) {
                    TerminateProcess(hProc, 1);
                    CloseHandle(hProc);
                }
            }
        }
        return 0;
    }
}
```

---

### Step 4：右键菜单扩展 (Day 3 下午)

#### 4.1 `ShowRowMenu()` Window 分支
**位置**：`ShowRowMenu()` 函数中 (约行 1477)
```cpp
if (r.kind == Row::Window) {
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_OPEN, (LPCWSTR)L"切换到此窗口");
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_CLOSE, (LPCWSTR)L"关闭窗口");
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_KILL, (LPCWSTR)L"结束进程");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_COPYPATH, (LPCWSTR)L"复制标题");
    // 新增：复制进程名
    AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, 2023, (LPCWSTR)L"复制进程名");
}

// 菜单命令处理 (switch cmd 中新增)：
case IDM_WIN_CLOSE: {
    HWND target = (HWND)_wtoi64(r.action.c_str());
    if (IsWindow(target)) PostMessageW(target, WM_CLOSE, 0, 0);
    break;
}
case IDM_WIN_KILL: {
    HWND target = (HWND)_wtoi64(r.action.c_str());
    if (IsWindow(target)) {
        DWORD pid = 0; GetWindowThreadProcessId(target, &pid);
        if (pid != GetCurrentProcessId()) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    }
    break;
}
case 2023: {  // 复制进程名
    CopyTextToClipboard(r.sub);  // sub 存的是进程名
    break;
}
```

---

### Step 5：设置页面 Tab 6 完整实现 (Day 4 - Day 5)

#### 5.1 `ShowSettingsTab()` 新增 case 6
**位置**：`ShowSettingsTab()` switch 中 (约行 3818)
```cpp
case 6: {  // Shell 与窗口
    // Shell 程序（cmd / PowerShell / Git Bash / 自定义）
    c = CreateWindowExW(0, L"STATIC", L"Shell 程序：",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(18), S(110), S(28), h,
        (HMENU)(INT_PTR)IDC_LBL_SHELLTYPE, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
    c = CreateWindowExW(0, L"BUTTON", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin + S(110), S(18), contentW - S(110), S(28), h,
        (HMENU)(INT_PTR)IDC_CMB_SHELLTYPE, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 自定义 Shell 路径（仅当选「自定义…」时显示）
    c = CreateWindowExW(0, L"STATIC", L"自定义 Shell 路径：",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(54), S(150), S(24), h,
        (HMENU)(INT_PTR)IDC_LBL_SHELLPATH, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
    c = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        margin, S(80), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_EDT_SHELLPATH, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

    // 自定义启动参数（{c} 会被替换为命令）
    c = CreateWindowExW(0, L"STATIC", L"自定义启动参数：",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(112), S(150), S(24), h,
        (HMENU)(INT_PTR)IDC_LBL_SHELLARGS, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
    c = CreateWindowExW(0, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        margin, S(138), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_EDT_SHELLARGS, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fList, TRUE);
    SetWindowTheme(c, L"DarkMode_Explorer", nullptr);

    // 默认工作目录
    c = CreateWindowExW(0, L"STATIC", L"默认工作目录：",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(176), S(140), S(28), h,
        (HMENU)(INT_PTR)IDC_LBL_SHELLCWD, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
    c = CreateWindowExW(0, L"BUTTON", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin + S(140), S(176), contentW - S(140), S(28), h,
        (HMENU)(INT_PTR)IDC_CMB_SHELLCWD, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 前台显示输出窗口（默认勾选）
    c = CreateWindowExW(0, L"BUTTON", L"前台显示输出窗口（否则后台静默执行）",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin, S(216), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_CHK_SHOWWIN, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 合并同进程窗口
    c = CreateWindowExW(0, L"BUTTON", L"合并同进程窗口（多窗口进程显示为分组）",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin, S(250), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_CHK_WINGROUP, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 显示 UWP 应用窗口
    c = CreateWindowExW(0, L"BUTTON", L"显示 UWP 应用窗口",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin, S(278), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_CHK_WINUWP, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 结果副标题显示进程名
    c = CreateWindowExW(0, L"BUTTON", L"结果副标题显示进程名",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin, S(306), contentW, S(24), h,
        (HMENU)(INT_PTR)IDC_CHK_WINPROC, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    // 窗口缓存刷新间隔
    c = CreateWindowExW(0, L"STATIC", L"窗口缓存刷新间隔(秒)：",
        WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE, margin, S(340), S(160), S(28), h,
        (HMENU)(INT_PTR)IDC_LBL_WINCACHE, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);
    c = CreateWindowExW(0, L"BUTTON", nullptr,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        margin + S(160), S(340), contentW - S(160), S(28), h,
        (HMENU)(INT_PTR)IDC_CMB_WINCACHE, g.inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.fInput, TRUE);

    break;
}
```

#### 5.2 Tab 切换逻辑扩展
**位置**：`ShowSettingsTab()` 的 Tab 索引映射 (约行 3814-3822)
```cpp
// 新增 case IDC_TAB_SHELLWIN -> 索引 6
: (id == IDC_TAB_SHELLWIN) ? 6
```

#### 5.3 Tab 标签绘制扩展
**位置**：`WM_DRAWITEM` 中 Tab 绘制部分 (约行 3596-3601)
```cpp
const WCHAR* lbl = ...
    : (id == IDC_TAB_WEIGHT)  ? L"搜索权重"
    : (id == IDC_TAB_EXCLUDE) ? L"排除路径"
                              : L"Shell 与窗口";
```

#### 5.4 设置页初始化/保存/回退逻辑
**位置**：`SettingsProc` `WM_CREATE` 中 (约行 2982) 和 `IDC_BTN_SAVE` / `IDC_BTN_CANCEL` 处理

```cpp
// WM_CREATE 中初始化 saved 备份
g.shellTypeSaved = g.shellType;
g.shellCustomPathSaved = g.shellCustomPath;
g.shellCustomArgsSaved = g.shellCustomArgs;
g.shellDefaultCwdSaved = g.shellDefaultCwd;
g.shellShowWindowSaved = g.shellShowWindow;
g.winGroupProcSaved = g.winGroupProc;
g.winShowUwpSaved = g.winShowUwp;
g.winShowProcSaved = g.winShowProc;
g.winCacheSecSaved = g.winCacheSec;

// IDC_BTN_SAVE 中持久化
RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellType", REG_DWORD, &g.shellType, sizeof(DWORD));
SaveRegText(L"ShellCustomPath", g.shellCustomPath);
SaveRegText(L"ShellCustomArgs", g.shellCustomArgs);
RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellDefaultCwd", REG_DWORD, &g.shellDefaultCwd, sizeof(DWORD));
DWORD v = g.shellShowWindow ? 1 : 0;
RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellShowWindow", REG_DWORD, &v, sizeof(v));  // 默认 1
v = g.winGroupProc ? 1 : 0; RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinGroupProc", REG_DWORD, &v, sizeof(v));
v = g.winShowUwp ? 1 : 0;   RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowUwp", REG_DWORD, &v, sizeof(v));
v = g.winShowProc ? 1 : 0;  RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinShowProc", REG_DWORD, &v, sizeof(v));
v = (DWORD)g.winCacheSec;   RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinCacheSec", REG_DWORD, &v, sizeof(v));

// IDC_BTN_CANCEL 中回退
g.shellType = g.shellTypeSaved;
g.shellCustomPath = g.shellCustomPathSaved;
g.shellCustomArgs = g.shellCustomArgsSaved;
g.shellDefaultCwd = g.shellDefaultCwdSaved;
g.shellShowWindow = g.shellShowWindowSaved;
g.winGroupProc = g.winGroupProcSaved;
g.winShowUwp = g.winShowUwpSaved;
g.winShowProc = g.winShowProcSaved;
g.winCacheSec = g.winCacheSecSaved;
```

#### 5.5 下拉框菜单处理 (IDC_CMB_SHELLCWD)
**位置**：`SettingsProc` `WM_COMMAND` 中
```cpp
case IDC_CMB_SHELLCWD: {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g.shellDefaultCwd==0?MF_CHECKED:0), 1, L"用户目录 (%USERPROFILE%)");
    AppendMenuW(menu, MF_STRING | (g.shellDefaultCwd==1?MF_CHECKED:0), 2, L"系统默认目录 (System32)");
    AppendMenuW(menu, MF_STRING | (g.shellDefaultCwd==2?MF_CHECKED:0), 3, L"桌面目录");
    RECT rc; GetWindowRect(GetDlgItem(h, IDC_CMB_SHELLCWD), &rc);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON, rc.left, rc.bottom, 0, h, nullptr);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= 3) g.shellDefaultCwd = cmd - 1;
    // 刷新按钮文本（见 ShellCwdName）
    const WCHAR* texts[] = {L"用户目录 (%USERPROFILE%)", L"系统默认目录 (System32)", L"桌面目录"};
    SetWindowTextW(GetDlgItem(h, IDC_CMB_SHELLCWD), texts[g.shellDefaultCwd]);
    break;
}
```

#### 5.6 复选框状态同步
**位置**：`SettingsProc` `WM_COMMAND` 中 (BN_CLICKED)
```cpp
case IDC_CHK_WINUWP:   g.winShowUwp = !g.winShowUwp; break;
case IDC_CHK_WINGROUP: g.winGroupProc = !g.winGroupProc; break;
case IDC_CHK_WINPROC:  g.winShowProc = !g.winShowProc; break;
case IDC_CHK_SHOWWIN:  g.shellShowWindow = !g.shellShowWindow; break;
```

#### 5.7 缓存间隔读取 (保存时)
```cpp
// IDC_CMB_WINCACHE 下拉选定 1-60 秒；保存时 clamp 到 [1,60]
if (g.winCacheSec < 1) g.winCacheSec = 1;
if (g.winCacheSec > 60) g.winCacheSec = 60;
```

#### 5.8 `LoadSettings()` 读取注册表
**位置**：`LoadSettings()` 函数末尾 (约行 2007)
```cpp
// ShellType / ShellCustomPath / ShellCustomArgs 用 LoadRegText 读取
cbv = sizeof(vv);
if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellDefaultCwd",
                 RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS) {
    g.shellDefaultCwd = (int)vv;
    if (g.shellDefaultCwd < 0 || g.shellDefaultCwd > 2) g.shellDefaultCwd = 0;
}
cbv = sizeof(vv);
if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"ShellShowWindow",
                 RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
    g.shellShowWindow = vv != 0;   // 键不存在时保持结构体默认 true（前台显示）
cbv = sizeof(vv);
if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Flowtary", L"WinGroupProc",
                 RRF_RT_REG_DWORD, nullptr, &vv, &cbv) == ERROR_SUCCESS)
    g.winGroupProc = vv != 0;
else
    g.winGroupProc = true;
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
    if (vv >= 1 && vv <= 60) g.winCacheSec = (int)vv; else g.winCacheSec = 5;
}
```

#### 5.9 布局规则注册
**位置**：`ApplyLayoutRule()` switch 中 (约行 2808)
```cpp
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
    cg.stretchW = true;
    break;
```

---

### Step 6：集成测试与边界修复 (Day 6 - Day 7)

#### 6.1 测试清单
- [ ] `> ping localhost` 正常执行，无控制台窗口
- [ ] `> powershell -c "ls"` + `Ctrl+Shift+Enter` 触发 UAC 提权
- [ ] Shell 命令在三种工作目录下均正常
- [ ] `< chrome` 列出 Chrome 窗口，分组/平铺模式切换正常
- [ ] 窗口组行回车切换到最近活跃窗口
- [ ] 子行 `Ctrl+Enter` 发送 `WM_CLOSE` 正常关闭
- [ ] 子行 `Ctrl+Shift+Enter` 调用 `TerminateProcess` 结束进程
- [ ] 右键菜单：切换/关闭/结束进程/复制标题/复制进程名 均正常
- [ ] 设置页 Tab 6 显示正常，所有控件交互正常
- [ ] 保存/取消 按钮正确持久化/回退所有 6 项配置
- [ ] 修改配置后即时生效 (无需重启)
- [ ] 窗口枚举缓存 5 秒 TTL 工作正常
- [ ] 排除 Flowtary 自身窗口、系统 Shell 窗口
- [ ] UWP 过滤开关生效
- [ ] 高 DPI / 多显示器下布局正常

#### 6.2 边界情况处理
- **EnumWindows 失败**：`AddHint(L"窗口枚举失败")`，不崩溃
- **目标窗口已销毁**：`IsWindow` 检查，优雅降级
- **提权被用户拒绝**：`ShellExecute` 返回 <=32，静默失败
- **结束关键进程 (explorer.exe)**：允许但需谨慎，当前策略只保护自身 PID
- **缓存线程与主线程竞争**：`windowCache` 仅在主线程 `WM_APP_WINDOWS_READY` 中替换，线程安全

---

## 代码变更影响范围总结

| 模块 | 变更类型 | 预估行数 | 风险等级 |
|------|---------|---------|---------|
| 数据结构定义 | 新增字段/枚举 | ~40 | 低 |
| `Refresh()` | 新增解析分支 | ~40 | 低 |
| `SearchWindows()` | 新函数 | ~120 | 中 |
| `EnumWindowsThread` | 新线程函数 | ~60 | 中 |
| `ExecuteRow()` | 扩展 switch | ~30 | 低 |
| 主窗口热键处理 | 扩展 `WM_KEYDOWN` | ~40 | 中 |
| `ShowRowMenu()` | 扩展菜单项 | ~30 | 低 |
| `SettingsProc` | 新增 Tab 6 完整逻辑 | ~150 | 中 |
| `LoadSettings()` | 读取 6 个注册表键 | ~30 | 低 |
| `ApplyLayoutRule` | 新增布局规则 | ~15 | 低 |
| 常量定义 | 新增 ID/消息 | ~15 | 无 |

**总计新增/修改：约 570 行**

---

## 编译依赖检查

需确保以下头文件已包含 (main.cpp 顶部已有)：
- `<tlhelp32.h>` - `CreateToolhelp32Snapshot` / `Process32First/Next` (已有)
- `<psapi.h>` - `GetModuleFileNameExW` (需确认 `#pragma comment(lib, "psapi.lib")`)
- `<shellapi.h>` - `ShellExecuteW` (已有)

若缺少 `psapi.lib`，在顶部添加：
```cpp
#pragma comment(lib, "psapi.lib")
```

---

## 验收演示脚本

实施完成后，可按以下顺序演示：

1. **Shell 命令基础**
   - `Alt+Space` 唤醒 → `> ping 127.0.0.1` → 回车 → 观察无控制台窗口、Flowtary 隐藏

2. **Shell 提权**
   - `> powershell -c "Get-Process -Name svchost"` → `Ctrl+Shift+Enter` → 观察 UAC 弹窗 → 确认后执行

3. **工作目录切换**
   - 设置页 → Shell 与窗口 → 默认工作目录选 `%USERPROFILE%` → 保存
   - `> echo %CD%` → 观察输出在用户目录

4. **窗口切换基础**
   - 打开多个记事本/Chrome/资源管理器窗口
   - `< 记事本` → 回车 → 切换到记事本
   - `< chrome` → 观察分组显示 `▶ Chrome (3)`

5. **窗口操作热键**
   - 选中子行 → `Ctrl+Enter` → 窗口关闭
   - 选中子行 → `Ctrl+Shift+Enter` → 进程结束 (谨慎测试)

6. **设置页完整流程**
   - 打开设置 → 切到「Shell 与窗口」Tab
   - 修改所有 6 项配置 → 保存 → 重启 Flowtary → 验证配置保留
   - 修改配置 → 取消 → 验证回退
