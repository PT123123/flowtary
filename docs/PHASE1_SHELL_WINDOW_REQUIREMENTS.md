# Phase 1 实施需求文档：Shell 命令 (`>`) + 窗口切换 (`<`)

> 生成日期：2026-09-01  
> 版本：v1.0  
> 状态：已实现（设计文档已与 src/main.cpp 同步，2026-09-01）

---

## 1. 核心功能规格

### 1.1 `>` Shell 命令执行

| 项目 | 规格 |
|------|------|
| **触发前缀** | `>` （单字符，后跟空格） |
| **输入示例** | `> ping localhost`、`> powershell -c "Get-Process"`、`> cmd /c dir`、`> wt -d .` |
| **执行方式** | `ShellExecuteW(..., L"open", cmd, args, cwd, SW_HIDE)` —— **无控制台窗口**，后台静默执行 |
| **管理员提权** | `Ctrl+Shift+Enter` 执行时 → `ShellExecuteW(..., L"runas", ...)` |
| **工作目录 (CWD)** | 优先级：① 当前选中文件夹/程序所在目录 ② `%USERPROFILE%` ③ 系统默认 |
| **结果展示** | 输入框显示 `> 命令` 为单行结果，回车即执行并隐藏窗口；不捕获 stdout/stderr（Phase 1 范围） |
| **不记录权重** | Shell 命令点击不进入点击加权排序（同网页前缀） |

### 1.2 `<` 窗口切换

| 项目 | 规格 |
|------|------|
| **触发前缀** | `<` （单字符，后跟空格） |
| **输入示例** | `< vscode`、`< chrome`、`< explorer`、`< 微信` |
| **枚举范围** | `EnumWindows` → 过滤：`WS_VISIBLE` && !`WS_EX_TOOLWINDOW` && 标题非空 |
| **排除项** | Flowtary 自身窗口（主窗/设置窗）、系统 Shell 窗口 (`Progman`/`WorkerW`)、类名为 `Windows.UI.Core.CoreWindow` (UWP 特殊处理) |
| **分组策略** | 同进程多窗口合并显示：`Chrome (3)` → 展开后显示具体标签页标题 |
| **匹配字段** | 窗口标题、进程名、窗口类名 —— 复用现有 `MatchScore` 逻辑 |
| **结果动作** | 回车：`SetForegroundWindow` 切换；`Ctrl+Enter`：`PostMessage(WM_CLOSE)` 礼貌关闭；`Ctrl+Shift+Enter`：`TerminateProcess` 强制结束 |

---

## 2. 数据结构变更

### 2.1 `Row::Kind` 扩展

```cpp
// 现有
enum Kind { File, Folder, Web, Prog, EvFallback, Hint, Group };

// 新增
enum Kind { File, Folder, Web, Prog, EvFallback, Hint, Group, Shell, Window };
```

### 2.2 `Mode` 扩展

```cpp
enum class Mode { None, Everything, Web, Programs, Shell, Window };
```

### 2.3 新增全局状态

```cpp
struct App {
    // ... 现有字段 ...
    
    // Shell 命令
    std::wstring shellCmd;        // 当前 Shell 命令完整字符串（实际执行用 r.action）
    // 管理员提权通过 ExecuteRow 的 ExecKind::Admin 参数实现，不再用全局布尔
    
    // 窗口切换
    struct WinInfo {
        HWND hwnd;
        std::wstring title;
        std::wstring processName;
        std::wstring className;
        DWORD pid;
        bool isUwp = false;
    };
    std::vector<WinInfo> windowCache;  // 缓存枚举结果，避免频繁 EnumWindows
    DWORD windowCacheTick = 0;         // 缓存时间戳，5 秒过期刷新
};
```

---

## 3. 核心逻辑实现

### 3.1 `Refresh()` 解析分支扩展

```cpp
// 在 Refresh() 中，WebCmd 分支之前插入：
if (sp != std::wstring::npos && tok == L">") {
    g.mode = Mode::Shell;
    if (rest.empty()) {
        AddHint(L"输入 Shell 命令，回车执行；Ctrl+Shift+Enter 以管理员运行");
    } else {
        Row r;
        r.kind = Row::Shell;
        r.title = L"> " + rest;
        r.action = rest;  // 完整命令行
        r.sub = L"Shell 命令";
        g.items.push_back(std::move(r));
    }
} else if (sp != std::wstring::npos && tok == L"<") {
    g.mode = Mode::Window;
    if (rest.empty()) {
        AddHint(L"输入窗口标题/进程名切换；回车切换，Ctrl+Enter 关闭，Ctrl+Shift+Enter 结束进程");
    } else {
        SearchWindows(rest);  // 新函数：枚举+匹配窗口
    }
}
```

### 3.2 `SearchWindows()` 新函数

```cpp
static void SearchWindows(const std::wstring& query) {
    // 1. 缓存检查：5 秒内复用 windowCache
    // 2. EnumWindows 回调收集 WinInfo
    // 3. 按进程分组：同 PID 合并
    // 4. 三字段模糊匹配（标题/进程名/类名），复用 MatchScore
    // 5. 排序：权重 > 精确 > 前缀 > 包含 > 子序列
    // 6. 生成 Row::Window 结果，最多 10 条
}
```

### 3.3 `ExecuteRow()` 扩展

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
case Row::Window: {
    // r.action 存 HWND (转字符串) 或直接用窗口缓存索引
    HWND target = (HWND)_wtoi64(r.action.c_str());
    if (IsWindow(target)) {
        SetForegroundWindow(target);
        return true;
    }
    return false;
}
```

### 3.4 窗口结果右键菜单扩展

```cpp
// ShowRowMenu() 中 Row::Window 额外菜单项：
AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_CLOSE, L"关闭窗口");
AppendMenuW(menu, MF_OWNERDRAW | MF_STRING, IDM_WIN_KILL,  L"结束进程");
// 执行时：IDM_WIN_CLOSE -> PostMessage(WM_CLOSE)；IDM_WIN_KILL -> TerminateProcess
```

---

## 4. 设置页面新增

### 4.1 新增 Tab：「Shell 与窗口」（索引 6，放在「排除路径」之后）

| 控件 | 控件 ID | 类型 | 默认值 / 说明 |
|------|---------|------|---------------|
| **Shell 程序** | `IDC_CMB_SHELLTYPE` (3062) | 自绘下拉 | 0=命令提示符(cmd)、1=PowerShell、2=Git Bash、3=自定义…（默认 0） |
| **自定义 Shell 路径** | `IDC_EDT_SHELLPATH` (3064) | 编辑框 | 仅当选「自定义…」时显示；自定义 shell 程序完整路径 |
| **自定义启动参数** | `IDC_EDT_SHELLARGS` (3066) | 编辑框 | 仅当选「自定义…」时显示；`{c}` 会被替换为命令字符串 |
| **默认工作目录** | `IDC_CMB_SHELLCWD` (3068) | 自绘下拉 | 0=用户目录(%USERPROFILE%)、1=系统默认(System32)、2=桌面目录（默认 0） |
| **前台显示输出窗口** | `IDC_CHK_SHOWWIN` (3069) | 自绘复选 | **默认勾选 (true)**；勾选→`SW_SHOWNORMAL` 前台显示，不勾选→`SW_HIDE` 后台静默执行 |
| **合并同进程窗口** | `IDC_CHK_WINGROUP` (3070) | 自绘复选 | 默认开 (true)；关闭则每个窗口单独列出 |
| **显示 UWP 应用窗口** | `IDC_CHK_WINUWP` (3071) | 自绘复选 | 默认开 (true)；关闭则过滤 `Windows.UI.Core.CoreWindow` 类窗口 |
| **结果副标题显示进程名** | `IDC_CHK_WINPROC` (3072) | 自绘复选 | 默认开 (true) |
| **窗口缓存刷新间隔(秒)** | `IDC_CMB_WINCACHE` (3074) | 自绘下拉 | 默认 5，范围 1-60 |

### 4.2 注册表持久化键名

```
HKCU\Software\Flowtary\ShellType        (DWORD: 0-3, 默认 0)
HKCU\Software\Flowtary\ShellCustomPath  (文本: 自定义 shell 程序路径)
HKCU\Software\Flowtary\ShellCustomArgs  (文本: 自定义启动参数模板，{c}=命令)
HKCU\Software\Flowtary\ShellDefaultCwd  (DWORD: 0-2, 默认 0)
HKCU\Software\Flowtary\ShellShowWindow  (DWORD: 0/1, 默认 1)   ← 前台显示输出窗口（2026-09-01 改为默认 1）
HKCU\Software\Flowtary\WinGroupProc     (DWORD: 0/1, 默认 1)
HKCU\Software\Flowtary\WinShowUwp       (DWORD: 0/1, 默认 1)
HKCU\Software\Flowtary\WinShowProc      (DWORD: 0/1, 默认 1)
HKCU\Software\Flowtary\WinCacheSec      (DWORD: 1-60, 默认 5)
```

### 4.3 `App` 结构体新增字段

```cpp
struct App {
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

    // 取消时回退用的 saved 版本
    int shellTypeSaved = 0;
    std::wstring shellCustomPathSaved;
    std::wstring shellCustomArgsSaved;
    int shellDefaultCwdSaved = 0;
    bool shellShowWindowSaved = true;
    bool winGroupProcSaved = true;
    bool winShowUwpSaved = true;
    bool winShowProcSaved = true;
    int winCacheSecSaved = 5;
};
```

---

## 5. 交互细节

### 5.1 热键行为表

| 场景 | 热键 | 行为 |
|------|------|------|
| Shell 命令行选中 | `Enter` | 普通权限执行，隐藏窗口 |
| Shell 命令行选中 | `Ctrl+Shift+Enter` | 管理员权限执行 (`runas`)，隐藏窗口 |
| 窗口项选中 | `Enter` | `SetForegroundWindow` 切换，隐藏窗口 |
| 窗口项选中 | `Ctrl+Enter` | `PostMessage(WM_CLOSE)` 礼貌关闭，保持窗口显示 |
| 窗口项选中 | `Ctrl+Shift+Enter` | `TerminateProcess` 强制结束，保持窗口显示 |
| 窗口项右键 | 菜单 | 「切换到此窗口」「关闭窗口」「结束进程」「复制标题」「复制进程名」 |

### 5.2 结果行显示格式

```
Shell:  > ping localhost          [副标题: Shell 命令]
Window: ▶ Chrome (3)              [副标题: chrome.exe | 标签页: GitHub, Gmail, 设置]
         ▷ GitHub - Chrome        [展开后单独行]
         ▷ Gmail - Chrome
```

- 分组头行：`kind=Window, title="▶ Chrome (3)", sub="chrome.exe | 3 tabs"`
- 子窗口行：`kind=Window, title="▷ GitHub - Chrome", sub="chrome.exe"`

### 5.3 缓存刷新策略

- `SearchWindows()` 首先检查 `GetTickCount() - g.windowCacheTick < g.winCacheSec * 1000`
- 过期或首次调用 → 后台线程 `EnumWindows` 刷新 `windowCache` → `PostMessage(WM_APP_WINDOWS_READY)` 回主线程
- 防止频繁输入导致 UI 卡顿

---

## 6. 代码修改清单

| 文件/区域 | 修改内容 | 预估行数 |
|----------|---------|---------|
| `Row::Kind` / `Mode` | 新增 `Shell`, `Window` 枚举值 | 5 |
| `App` 结构体 | 新增 Shell/窗口配置字段 + saved 回退字段 | 20 |
| `Refresh()` | 解析 `>` `<` 前缀，分派到新模式 | 40 |
| `SearchWindows()` | 新函数：枚举/缓存/匹配/分组窗口 | 120 |
| `ExecuteRow()` | 处理 `Row::Shell` / `Row::Window` | 50 |
| `ShowRowMenu()` | 窗口项右键菜单扩展 | 30 |
| `LoadSettings()` | 读取 6 个新注册表键 | 30 |
| `SettingsProc` / `ShowSettingsTab()` | 新增 Tab 6：创建控件、布局、保存/取消逻辑 | 150 |
| `LayoutSettings()` | 新控件几何规则注册 | 20 |
| 资源 ID (`IDC_*`) | 新增 15 个控件 ID 常量 | 15 |

**总计：约 480 行新增/修改代码**

---

## 7. 验收标准

| 测试用例 | 预期结果 |
|---------|---------|
| `> ping localhost` 回车 | 后台执行 ping，无控制台窗口弹出，Flowtary 隐藏 |
| `> powershell -c "ls"` `Ctrl+Shift+Enter` | UAC 提权弹窗，确认后以管理员执行 |
| `< chrome` 输入 | 列出所有 Chrome 窗口（合并显示），标题模糊匹配 |
| 窗口组行回车 | 切换到该组最近活跃的窗口 |
| 窗口子行 `Ctrl+Enter` | 该窗口收到 `WM_CLOSE` 正常关闭 |
| 窗口子行 `Ctrl+Shift+Enter` | 该进程被 `TerminateProcess` 结束 |
| 设置页「Shell 与窗口」Tab | 所有控件正常显示、交互、保存/取消生效 |
| 修改「默认工作目录」为 `%USERPROFILE%` | 后续 Shell 命令在用户目录下执行 |
| 关闭「合并同进程窗口」 | `< chrome` 直接列出每个标签页为独立行 |

---

## 8. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| `EnumWindows` 卡死/慢 | UI 阻塞 | 工作线程异步枚举 + 缓存 + 5 秒 TTL |
| UWP 应用无法正常切换/关闭 | 功能缺失 | 单独处理 `CoreWindow`，`SetForegroundWindow` 通常有效；关闭用 `PostMessage(WM_CLOSE)` 尝试 |
| `ShellExecute` `runas` 在非交互会话失败 | 提权无效 | 仅在交互桌面会话下提供 `Ctrl+Shift+Enter`，否则菜单项置灰 |
| 窗口标题包含敏感信息 | 隐私泄露 | 设置页提供「隐藏窗口标题」选项（Phase 2 做） |

---

## 9. 实施顺序建议

1. **Step 1**：数据结构 + `Refresh()` 解析分支 + `Row::Shell` 执行
2. **Step 2**：`SearchWindows()` 枚举/缓存/匹配/分组 + `Row::Window` 执行
3. **Step 3**：右键菜单扩展 + 热键行为 (`Ctrl+Enter`/`Ctrl+Shift+Enter`)
4. **Step 4**：设置页 Tab 6 完整实现（控件创建、布局、持久化、回退）
5. **Step 5**：集成测试 + 边界情况修复
