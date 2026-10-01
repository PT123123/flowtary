# Flowtary 设置窗口 sidecar 化：数据契约 + 副作用清单

> **状态（2026-10-01）：已落地。** 本文是迁移前的调研契约，其中的行号指向**迁移前的 `src/main.cpp`（7375 行）**，
> 旧的 `SettingsProc`/`IDC_*` 已随迁移删除，读行号时请以此为前提。
> 实际实现、与本文的差异（R4/R5/R6/R9 的处理方式）、FLTK 1.4 的坑与构建/部署口径见
> `docs/SETTINGS_SIDECAR_FLTK.md`。
>
> 目标：把「Flowtary 设置」窗口（窗口类 `FlowtarySettings`，窗口过程 `SettingsProc` at `src/main.cpp:4682`）整体迁出为独立进程 `flowtary-settings.exe`（FLTK UI），主进程只负责拉起它 + 在变更后重放生效。
> 本文是两者之间的**接口契约**草稿：每一条结论都带 `文件:行号` 证据，未读到代码的一律不写。
> 代码基线：`src/main.cpp` 7375 行，`src/filedlg_jump.cpp` 429 行，`src/version.h` = 1.1.14.10。
> 生成日期：2026-10-21，逐行核对。

---

## 0. 迁移风险与未决问题（先读这一节）

按「对 sidecar 模型的杀伤力」排序。

| # | 风险 | 证据 | 影响 / 待决 |
|---|------|------|-------------|
| R1 | **保存是「全窗口一次性提交」，不是「按 Tab 提交」**。`IDC_BTN_SAVE` 无条件读取全部 6 个编辑框 + 全部控件的内存态并写盘（包括用户从没打开过的 Tab），然后 `DestroyWindow`。 | `src/main.cpp:5804-5948`（尤其 5875-5905、5907-5946）、控件常驻仅 `ShowWindow(SW_HIDE)`（4354-4441） | sidecar 必须保持「打开时全量快照 → 保存时全量写回」的语义。若 FLTK 侧改成 per-tab 保存，会丢掉/覆盖其它 Tab 的值。 |
| R2 | **三个设置项的值只能从主进程内存读到，注册表里没有**：已记忆权重条数 `g.weightCount`（`src/main.cpp:4026`）、已学习条数 `g.learnCount`（4348）、排除项条数 `g.excludePaths.size()`（5079）。它们还带**归一化/去重/升级合并**后的结果，与注册表原文不等价（`LoadExcludePaths` 会剔除旧默认项并补新默认项）。 | 4026 / 4344-4350 / 5079 / 3217-3255 | 协议必须有**请求-应答**（不能只有单向通知）。或者把「计数」语义降级为「sidecar 自己解析注册表文本行数」——但那会与主进程显示不一致。 |
| R3 | **`ClearClickWeights` / `ClearLearned` 必须在主进程执行，且必须持有主进程内存态**：它们清的是 `g.weights`/`g.learned` 内存 map + 写盘（`weightsDirty=true` 后整体重写），sidecar 自己删文件会被主进程下一次写盘复活。清空按钮还是**两步确认**（3 秒定时器）。 | 3486-3491、3876-3891、5401-5416、6038-6068 | 需要独立通知类型；sidecar 不能只删 `%APPDATA%\Flowtary\weights.dat`。 |
| R4 | **「回车 = 保存」是消息循环级别的全局拦截，不区分控件、不看 Shift 状态**。注释声称「规则编辑框多行会吞掉 Return」，但拦截发生在 `DispatchMessage` 之前，代码路径上并不成立。 | `src/main.cpp:7358-7363` | 待与产品确认预期。迁移时若照抄，多行编辑器无法用 Enter 换行；若改掉，是行为变更。建议 FLTK 侧只在非编辑器控件上绑 Enter。 |
| R5 | **用 `X` 关窗口 = 不回退**。`WM_CLOSE` 直接 `DestroyWindow`，`WM_DESTROY` 只清 `g.hSettings` 和两个「武装」标志；实时预览对 `kThemes[]` 的**进程内改动不会被回滚**（只有点「取消」才用 `sTuneSaved` 回退）。`UpdateScale` 也会在比例变化时 `WM_CLOSE` 设置窗（685）。 | 6220-6227、6119-6170、672-686 | sidecar 化后「取消」变成显式动作；X 关闭时主进程内存里的 `kThemes` 是否要回滚，需产品定。当前语义：注册表干净、进程内存脏。 |
| R6 | **`AntiGhost` 与设置窗自身强绑定，主进程完全无关**：它只给设置窗加/去 `WS_EX_COMPOSITED`。迁到 sidecar 后它退化为 sidecar 的内部渲染开关。 | 5961-5972、6148-6155、6259 | 可从主进程通知清单里剔除。但主进程 `g.antiGhost` 仍会被 `LoadSettings` 读（2814），保留字段无害。 |
| R7 | **`fdjEnabled` 不需要重装钩子**：`fdj_init` 只在启动时装一次 WinEvent + CBT 钩子（7337、filedlg_jump.cpp:294-311），开关是**每次调用实时读注册表**（filedlg_jump.cpp:333-343，用于 `fdj_jump_to_folder`:351 / `fdj_sync_from_explorer`:414 / `src/main.cpp:2055`）。 | 同上 | sidecar 直接写 `FileDlgJump` 即生效，主进程无需动作（只有 UI 镜像 `g.fdjEnabled` 需要同步）。用户提示里那句「保存后生效」在别处，不在此。 |
| R8 | **截图工具的 `captureExe` 路径只在启动时解析一次**；下载成功后只改 sidecar/主进程当前内存里的 `g.captureExe`。若下载按钮搬到 sidecar，主进程不会知道新文件已就位。 | 7239-7264（启动解析）、6182-6216（下载后 `g.captureExe = dest`） | 必须新增一条 `CaptureInstalled` 通知让主进程重跑解析，否则 `ss` 命令一直报「未找到 ScreenCapture.exe」。 |
| R9 | **两个上限下拉存在「显示值 ≠ 存储值」的漂移**：`IDC_CMB_MAXENT` 只有 4 档（1000/5000/20000/50000），但 `LoadSettings` 允许 100..200000 的任意值；注册表若是 3000，下拉显示「1000 条」而保存仍写 3000。`WinCacheSec`（1..60 任意）、`LearnMaxEntries`（6 档 vs 10..100000）同问题。 | 5007-5017、6018-6021、5843 / 2864-2870、5185-5195、6101-6104 / 5316-5326、6025-6028、2885-2890 | FLTK 侧要么改成可编辑数字框，要么在载入时把非法值吸附到最近的档，否则 UI 说谎。 |
| R10 | **编辑器的「注释/重复行在打开时就丢了」**：网页规则与启动/关闭组的编辑框初始文本是**重新序列化**的结果（`RulesToText`/`GroupsToText`），`#` 注释、空行、重复关键字行全部丢失；排除路径则保留用户原文（只加固定 2 行头注释）。 | 4905、4946、4960 vs 5069；序列化 2998-3002、3090-3101、3130-3137；解析丢注释 2982、3065、3119 | sidecar 要复刻这个不对称。若想改成「原文回显」，是行为变更，需评审。 |
| R11 | **排除路径无法被清空**：解析后若为空，保存时代码回退到旧的 `g.excludePaths` 并落盘旧值（用户以为清空了，实际没清空）。 | 5902-5905（`if (!parsedExcl.empty()) g.excludePaths = parsedExcl;`） | 保留还是修正？契约里先按原样保留。 |
| R12 | **透明度滑块几乎不影响主窗**：主启动窗不是 `WS_EX_LAYERED`（7331），`theme->alpha` 只被弹窗（Notice / TopToast）使用，且被 `max(alpha, 240)` 抬到最低 240。设置窗自身也不是分层窗。 | 7331、6977、7100、177 | 「透明度」档位在 0..239 区间视觉上完全无效。sidecar 里要不要如实说明/收窄范围，是产品决定。 |
| R13 | **圆角滑块对设置窗无效**：设置窗圆角走 DWM `DWMWCP_ROUND`（固定），`radiusWindow` 只在 DWM 描边不可用时作为主窗 GDI 回退描边；`radiusCard`/`radiusInput` 作用于主窗绘制；`radiusButton`/`radiusSmall` **代码里无人消费**（死字段）。 | 2590-2596、4134、3983-3990、193-197 | sidecar 里「实时预览圆角」只能预览主窗效果；FLTK 重建设置窗时圆角是另一套自绘。 |
| R14 | **主题列表在「美化关闭」时整体禁用**（选中被 `break` 掉、列表置灰、滑块 `EnableWindow(FALSE)`），这是运行时耦合，不是数据约束。 | 6107、5510、4869-4871、5958-5960 | sidecar 若把 beautify 存本地，需要同样 gating，否则用户会在 beautify=off 时改主题却毫无效果。 |
| R15 | **「开机自动启动」读写的是另一个键**（`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值名 `Flowtary`），而且读取只看键是否存在，不看内容；写入内容用的是**主程序自己的 `GetModuleFileNameW`**。 | 2750-2773、4693、5862 | sidecar **不能**用自身 exe 路径；必须由主进程提供路径或 sidecar 从 `RunningPath` 取（`src/main.cpp:7219` 写的是主 exe 全路径）。 |
| R16 | **`WebRulesVer` 是 write-only 字段**：代码只在两处写（`src/main.cpp:3021`、`3031`），从没有任何读取点；真正的「用户是否改过」判定靠注册表文本与 `kLegacyRulesV1` 快照逐行比对。 | 139-147、3017-3023 | 迁移时别误以为可以靠它判断；比对函数 `RulesTextEqual`（152-169）必须一起搬走。 |
| R17 | **`settingsTab`（上次停在哪个 Tab）只在进程内存活**，不写注册表。 | 397（字段注释「关闭后仍记住上次选择」）、4353 | 现在由 sidecar 自己决定持久化位置；主进程不再需要。 |

---

## 1. 注册表值全表

根键：`HKCU\Software\Flowtary`（证据 `src/main.cpp:2729`、`2780` 等）。全部使用 `RegGetValueW` / `RegSetKeyValueW` / `LoadRegText`(3039) / `SaveRegText`(3052)，无 `RegOpenKey` 手写句柄。

### 1.1 「常规」Tab

| 值名 | 类型 | 含义 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `CenterWake` | REG_DWORD | 唤醒位置：1=屏幕居中，0=跟随鼠标 | 1（true） | `main.cpp:2780`（2777 置默认）；用于 `Layout()` `1425` | `main.cpp:5809`（保存按钮） |
| `HotkeyMode` | REG_DWORD | 结果项快捷键方案 0=Alt+数字 / 1=Alt+字母 / 2=关闭；越界钳到 0 | 0 | `2784`，钳制 `2791`；用于 `HotkeyChar` `628-630`、`TryAltHotkey` `6279-6290` | `5813` |
| `HotkeyLetters` | REG_DWORD | **兼容旧键，只读**：1→方案0，0→方案2；新代码从不写 | — | `2787`（仅当 `HotkeyMode` 缺失时作 fallback） | 无 |
| `HotkeyWake` | REG_DWORD | 唤起快捷键总开关。**不在设置窗里**，只由托盘菜单切换 | 1 | `2795`；`WakeHookProc` `2668` | `2729`（`ToggleWakeHotkey`），设置窗不写 |
| `FileDlgJump` | REG_DWORD | 文件对话框原地跳转开关（模块自己的键，语义同上） | 1（缺键即开） | `filedlg_jump.cpp:333`（**每次调用都读**），调用点 `351`/`414`、`main.cpp:2055`；打开设置窗时镜像进 `g.fdjEnabled`（`main.cpp:4703`） | `filedlg_jump.cpp:341`（`fdj_set_enabled`），由 `main.cpp:5858` 在保存时调用 |

### 1.2 「主题」Tab

| 值名 | 类型 | 含义 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `Theme` | REG_DWORD | `kThemes[]` 索引（数组共 **8** 项，`230-305`）；越界回 0 | 0 | `2800`，钳制 `2803` | `5816` |
| `ThemeTune` | REG_BINARY | **每主题的「透明度/窗体圆角」微调快照 blob**。定长 `sizeof(ThemeTune)*8` = 8×(int alpha + int radius) = **64 字节**，与 `kThemes` 下标一一对应。结构定义 `308-311`。加载时**长度不符则整块忽略**（用预设默认），并当场把 alpha 钳 0..255、radius 钳 0..14，然后按 `card=r*0.8 / button=r*0.6 / input=r*0.6` 推导其它圆角。为什么存在：主题本身是编译期常量表，用户的微调必须有一份「按主题索引的外部覆盖层」。 | 无键=不覆盖 | `2832`（探测长度）、`2836`（读内容）、展开 `2838-2846` | `5826`（保存时从 `kThemes[]` **当前值**回灌整个 blob） |
| `Beautify` | REG_DWORD | 界面美化：暗色标题栏 + 圆角 + 强制暗色菜单 | 1 | `2807`；用于 `EnableDarkMenus` `2239`、`ApplyBeautify` `4458` | `5831` |
| `AntiGhost` | REG_DWORD | 抗残影双缓冲（实验性），**只作用于设置窗** `WS_EX_COMPOSITED` | 0 | `2814`；`6259` 建窗时、`5964-5967` 切换时 | `5834` |

### 1.3 「网页规则」Tab

| 值名 | 类型 | 含义 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `WebRules` | REG_SZ | 规则编辑器**原文**（含注释、重复前缀、非法行）；空→用默认集 | `kDefaultRulesText`（`126-134`） | `3009`/`3012`（`LoadWebRules`） | `3028`（`SaveWebRules`）、`3019`（升级刷新） |
| `WebRulesVer` | REG_DWORD | = `kWebRulesVer`（当前 **2**，`139`）。**只写不读**：语义是「这份 WebRules 是程序生成的，不是手改的」；真正的「从未改过」判定是拿 `WebRules` 与 `kLegacyRulesV1`(`140-147`) 做行级比对 `3017` | 2 | **无**（见 R16） | `3021`（升级后）、`3031`（用户保存后） |

### 1.4 「一键」Tab

| 值名 | 类型 | 含义 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `GroupLaunch` | REG_SZ | 启动组编辑器原文（目标=文件路径） | 空（无内置默认） | `3104` | `5886` |
| `GroupKill` | REG_SZ | 关闭组编辑器原文（目标=进程名） | 空 | `3105` | `5894` |

### 1.5 「排除路径」Tab

| 值名 | 类型 | 含义 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `ExcludePaths` | REG_SZ | 排除路径编辑器原文 | 首次启动写入 `DefaultExcludePathsText()`（`3149-3173`） | `3218` | `3222`（首启）、`3247`（升级合并）、`5905`（保存） |
| `ExcludeVer` | REG_DWORD | **默认项集版本号**，当前 `kExcludeDefaultsVer = 3`（`3143`）。存在理由：默认排除集在升级中变过三次（v1 系统目录 → v2 加开发噪音 → v3 撤销硬编码 `.git/.vscode` 改由开关管辖），需要一次性「剔除某几条旧默认 + 并入新默认」的迁移，而不是每次都覆盖用户。读到时 `ver < 3` 才走合并分支。 | 3（写入即标记） | `3233`（缺失按 0） | `3226`（首启）、`3249`（升级后） |
| `SkipDotFolders` | REG_DWORD | 不搜索名以 `.` 开头的文件与文件夹 | 1 | `2822`；用于 `IsPathExcluded` `1028` → `PathHasDotFolder` `1004` | `5860` |

### 1.6 「Shell 与窗口」Tab

| 值名 | 类型 | 含义 / 取值 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `ShellType` | REG_DWORD | 0=cmd / 1=PowerShell / 2=Git Bash / 3=自定义；越界→0 | 0 | `2929-2933`；`BuildShellCommand` `1722-1742` | `5925` |
| `ShellCustomPath` | REG_SZ | 自定义 shell 全路径；**非自定义时被清空**（`5921`） | 空 | `2934`；`1736` | `5927` |
| `ShellCustomArgs` | REG_SZ | 自定义启动参数模板，占位符 `{c}` = 命令 | 空 | `2935`；`1737-1741` | `5928` |
| `ShellDefaultCwd` | REG_DWORD | 0=`%USERPROFILE%` / 1=System32 / 2=桌面；越界→0 | 0 | `2937-2941`；`GetShellCwd` `1679-1691` | `5930` |
| `ShellShowWindow` | REG_DWORD | 前台显示输出窗口（否则 `SW_HIDE` 静默） | 1（`g` 初始化 `512`；`2943` 无 else 分支，缺键保持 true） | `2943`；`BuildShellCommand` `1720` | `5933` |
| `WinGroupProc` | REG_DWORD | 合并同进程窗口 | 1 | `2947`；`SearchWindows` `1811` | `5936` |
| `WinShowUwp` | REG_DWORD | 显示 UWP 应用窗口 | 1 | `2953`；`1795` | `5939` |
| `WinShowProc` | REG_DWORD | 副标题显示进程名 | 1 | `2959`；`1825`/`1834`/`1844` | `5942` |
| `WinCacheSec` | REG_DWORD | 窗口枚举缓存刷新间隔（秒），钳 1..60，越界→5 | 5 | `2965-2969`；`SearchWindows` `1784` | `5945` |

### 1.7 「命令」Tab（四个开关，全是 REG_DWORD，默认全 1）

| 值名 | 含义 | 读 | 写 |
|------|------|----|----|
| `TopCmd` | `空格+top` 置顶/取消置顶前台窗口 | `2900`；`Refresh` `1896` | `5864` |
| `CmdCmd` | `空格+cmd` 执行 Shell 命令（原 `> 命令`） | `2907`；`1913` | `5867` |
| `WinCmd` | `空格+w` 切换窗口（原 `< 关键词`） | `2914`；`1926` | `5870` |
| `CaptureCmd` | `空格+ss` 截图 | `2921`；`1934` | `5873` |

### 1.8 「搜索权重」Tab

| 值名 | 类型 | 含义 / 取值 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `WeightEnabled` | REG_DWORD | 点击权重记忆总开关 | 1 | `2853`；`1354`/`1379`/`1602`/`3386` | `5837` |
| `WeightFlush` | REG_DWORD | 写盘时机 0=每次立即 / 1=延迟 3s 合并 / 2=仅退出；越界→1 | 1 | `2860-2863`；`3406-3411`（`RecordClickWeight`）、`3631-3636`（**自学习共用同一策略** `ScheduleLearnedSave`） | `5840` |
| `WeightMaxEntries` | REG_DWORD | 条目上限（内存+磁盘同时裁剪）；<100→5000，>200000→200000 | 5000 | `2866-2870`；`3396`/`3426`/`3466` | `5843` |

### 1.9 「自学习」Tab

| 值名 | 类型 | 含义 / 取值 | 默认 | 读 | 写 |
|------|------|------|------|----|----|
| `LearnEnabled` | REG_DWORD | 自学习总开关 | 1 | `2874`；`3644`/`3895` | `5847` |
| `LearnScope` | REG_DWORD | 记录范围 0=仅 exe / 1=exe+文件夹 / 2=所有文件+文件夹；越界→1 | 1 | `2881-2884`；`3668-3669` | `5850` |
| `LearnMaxEntries` | REG_DWORD | 条目上限，<10 或 >100000 → 500 | 500 | `2887-2890`；`3675`/`3705`/`3755` | `5853` |
| `LearnExpireDays` | REG_DWORD | 过期清理：0=永不过期，否则 N 天未用即遗忘；<0→0 | 0 | `2893-2896`；`3857-3871` | `5856` |

### 1.10 「截图工具」Tab + 非设置类

| 值名 | 类型 | 含义 | 读 | 写 |
|------|------|------|----|----|
| `CaptureExe` | REG_SZ | ScreenCapture.exe 的**自定义备选路径**（同目录优先）。**代码里从未写过它**（UI 也没有输入框）→ 留给手工配置 | `main.cpp:7252`/`7255`（仅当同目录没找到时） | 无 |
| `RunningPath` | REG_SZ | 当前运行的主 exe 全路径（单实例/新版本接管判定） | `7209` | `7219` |
| `RunningStamp` | REG_QWORD | 主 exe 的 last-write-time | `7212` | `7221` |
| `HKCU\...\Run\Flowtary` | REG_SZ | 开机自启，值 = `"<主 exe 路径>"`（带引号，无参数） | `2759` | `2768` / 删除 `2771` |

---

## 2. 非注册表持久化

### 2.1 点击权重 `weights.dat`

| 项 | 内容 | 证据 |
|----|------|------|
| 路径 | `%APPDATA%\Flowtary\weights.dat`；`APPDATA` 取不到时**回退到 exe 同目录** | `main.cpp:3298-3306` |
| 编码 | 裸 UTF-16LE（`WCHAR` 数组直写，**无 BOM、无长度头**）；写用 `CREATE_ALWAYS` 全量重写 | `3438-3443` |
| 行格式 | `搜索词 \x1F 完整路径 \x1F 权重`，行分隔 `\r\n`；分隔符常量 `kWeightSep = L'\x1f'` | `3296`、`3429-3435` |
| 排序 | 写出前按权重降序，截断到 `max(1, weightMaxEntries)` | `3420-3427` |
| 读 | `LoadClickWeights()` `3447`；文件 <2 字节或 >64MB 直接放弃；行内两个 `\x1F` 缺一即跳过；`w<=0` 跳过 | `3451-3482` |
| 写 | `SaveClickWeightsNow()` `3415`；失败（`CreateFile` 拿不到句柄）时把 `weightsDirty` 置回 true 下次再试 | `3440` |
| 清空 | `ClearClickWeights()` `3486`：清内存 + `weightsDirty=true` + 立即写**空文件**（不是删文件） | `3485-3491` |
| 生效语义 | 记录点 `RecordClickWeight` `3385`（只记 File/Folder/Prog 行）；有效词由 `WeightTermFromInput` `3337` 拆：`f`/`d` 只取前缀后的词、命中网页前缀整条不记、输入含 `\` 或 `%` 不记。另有**前缀聚合推荐** `CollectPrefixWeights` `1352` | — |

### 2.2 自学习 `learned.dat` + `learned\*.lnk`

| 项 | 内容 | 证据 |
|----|------|------|
| dat 路径 | `%APPDATA%\Flowtary\learned.dat`（同回退规则） | `3524-3532` |
| lnk 目录 | `%APPDATA%\Flowtary\learned` | `3514-3522` |
| dat 编码 | 裸 UTF-16LE，全量重写 | `3728-3735` |
| 行格式 | `搜索词 \x1F 完整路径 \x1F 次数 \x1F 最近打开Unix秒 \x1F lnk文件名`，行尾 `\r\n`（第 5 字段可为空=尚未建 lnk） | `3719-3727`；结构 `LearnVal` `362-366` |
| lnk 命名 | `SanitizeTermForFile(term)`：`<>:"/\|?*` 与控制字符→`_`，截断到 100 字符且不切断代理对，去尾部空格/点，空则用 `learned`；同词多目标追加 `" (2)"`… | `3535-3549`、`3572-3587` |
| lnk 内容 | `IShellLinkW`：SetPath(目标) + SetWorkingDirectory(目标所在目录) + SetDescription(L"Flowtary 自学习：" + 词) | `3551-3569` |
| 权威数据 | `learned.dat` 是权威；`.lnk` 目录可人工查看/增删 | 注释 `3498-3499` |
| **双向同步** | 启动时 `LoadLearned()` `3739` 做一次：① 目录里被删掉的 `.lnk` → 对应条目遗忘（`3804-3815`）② 用户手动放进来的 `.lnk` → 以 `count=1`、`last=文件写入时间` 吸收为学习项，词 = 文件名去 `.lnk`、去 `" (N)"` 序号、标准化小写（`3816-3852`）③ 按 `LearnExpireDays` 过期清理并删 lnk（`3856-3871`） | — |
| 淘汰 | 超上限时按「次数最少、其次最久未用」淘汰，连带删 `.lnk`（`PruneOneLearned` `3607`；写出前二次裁剪 `3705-3717`） | — |
| 清空 | `ClearLearned()` `3876`：`FindFirstFile(dir\*.lnk)` 逐个 `DeleteFileW`（**只删文件，不删目录**），再清内存 + 立即写空文件 | `3875-3891` |
| 「打开快捷方式目录」按钮 | 先 `CreateDirectoryW` 再 `ShellExecuteW("open", dir)` | `6033-6037` |
| 不学习自己 | 路径位于 `learned\` 目录内的条目不记；被排除路径命中的不记 | `3661-3664` |

---

## 3. 四个多行文本编辑器的精确语法

通用：行分隔按 `\r` 或 `\n` 任一字符切（`find_first_of(L"\r\n")`），因此 `\r\n`/`\n`/单独 `\r` 混用都能解析；行内容用 `TrimW`（`iswspace` 两端裁）。解析失败的行**静默丢弃，不报错、不弹窗**。

### 3.1 网页规则 `IDC_EDT_RULES`（Tab 1）

解析函数 `ParseRules` `src/main.cpp:2974-2995`；序列化 `RulesToText` `2998-3002`。

| 规则 | 细节 | 行号 |
|------|------|------|
| 字段数 | 2：`前缀` + `URL 模板` | 120-124 |
| 分隔符 | 第一个空格**或制表符**（`find_first_of(L" \t")`）；之后整段（可含空格）都是模板 | 2983、2987 |
| 注释 | 行首（trim 后）为 `#` → 忽略 | 2982 |
| 空行 | 忽略 | 2982 |
| 非法行 | 无空格分隔符 → 跳过；trim 后前缀或模板为空 → 跳过 | 2984、2988 |
| 重复前缀 | **第一条生效**，后续同前缀行丢弃 | 2989-2992 |
| 大小写 | 前缀 **`ToLowerW` 后存储**；匹配时输入 token 也小写（`Refresh` `1891`/`FindWebCmd` `1623-1626`）→ **前缀大小写不敏感**。URL 模板**原样保留**（大小写敏感） | 2986、1625 |
| 占位符 | `{q}` = 关键词，**UTF-8 百分号编码后**替换，只替换**第一个** `{q}` | `BuildUrl` `1150-1156`、`UrlEncodeUtf8` `1066` |
| 回显 | 编辑框初始文本 = `RulesToText(g.webCmds)` = **已解析结果的重序列化** → 注释/空行/重复行丢失（R10） | `4905` |
| 落盘 | `SaveWebRules` 写的是**编辑器原文**（不是重序列化），即非法行/注释也原样留在注册表 | `3027-3034` |

### 3.2 启动组 / 关闭组 `IDC_EDT_LAUNCH` / `IDC_EDT_KILL`（Tab 3）

解析 `ParseGroups` `3057-3088`；序列化 `GroupsToText` `3090-3101`。两个编辑器语法**完全相同**，只有语义不同。

| 规则 | 细节 | 行号 |
|------|------|------|
| 字段数 | 2 段：`关键字` + `目标列表`；目标列表内用 `;` 分隔多个目标 | 3037 注释、3066、3074 |
| 关键字分隔符 | 第一个空格**或制表符** | 3066 |
| 注释 / 空行 | 行首 `#` 忽略、空行忽略 | 3065 |
| 非法行 | 无空白分隔符（缺目标）→ 跳过；关键字 trim 后为空 → 跳过；目标全为空 → 跳过 | 3067、3070、3081 |
| 目标项 | 每个 `;` 分段独立 trim，空段丢弃（所以 `a;;b` 等价 `a;b`，末尾 `;` 无害） | 3073-3080 |
| 重复关键字 | **第一条生效** | 3082-3085 |
| 大小写 | 关键字 `ToLowerW` 存储；匹配用 `ToLowerW(TrimW(整条输入))`（`Refresh` `2009`、`FindGroup` `3257-3261`）→ **不敏感** | 3069 |
| 语义差异 | 启动组目标 = **文件全路径**（`ShellExecute` 启动）；关闭组目标 = **进程名**，`.exe` 可省（`KillProcessesByName` 自动补 `.exe` 并小写比较，且永不结束自身） | 3038 注释、`3264-3286` |
| 内置默认 | **无**（空即空，注册表缺键就是空表） | `3103-3106` |
| 回显 | `GroupsToText(g.groupsLaunch/Kill)` = 重序列化，注释丢失（`4946`/`4960`） | — |
| 落盘 | 编辑器原文直写（`5886`/`5894`），随后 `ParseGroups` 重建内存 | — |

### 3.3 排除路径 `IDC_EDT_EXCLUDE`（Tab 5）

解析 `ParseExcludePaths` `3111-3128`；序列化 `ExcludePathsToText` `3130-3137`；匹配 `IsPathExcluded` `1026-1049`。

| 规则 | 细节 | 行号 |
|------|------|------|
| 字段数 | **1**：每行一条模式 | 3109 注释 |
| 注释 / 空行 | 行首 `#` 忽略、空行忽略（trim 后判断） | 3119 |
| 通配符 | 仅 `*` 与 `?`；`MatchWildcardI` 是 DP 全串匹配（`*` 含空串，`?` 恰好一个字符） | 3109、`953-972` |
| 无通配符时 | 按**目录边界前缀**匹配：路径以前缀开头且「等长 或 前缀后紧跟 `\`」才命中 → 填一个目录即排除整棵子树；`C:\a` 不会误排 `C:\ab` | 1037-1045 |
| 大小写 | **不敏感**：模式与路径都归一化（`/`→`\`、去尾部 `\` 但保留盘符 `C:\`、`towlower`）后比较 | `975-990`、`1024-1034` |
| 归一化镜像 | `g.excludePaths` 存原文（供回显），`g.excludePathsNorm` 存归一化镜像（供匹配），**下标一一对应**；任何改写原文处都必须调 `RebuildExcludeNorm()` `994` | 465-471、992-998 |
| 重复 | 按**归一化形式**去重，保留第一次出现的原文 | 3120-3125 |
| 回显 | 初始文本 = `ExcludePathsToText(g.excludePaths)`：固定加 **2 行头注释**（语法说明 + 作用范围），然后每条原文 + `\r\n` | `3130-3137`、`5069` |
| 落盘 | 保存时写 `ExcludePathsToText(g.excludePaths)`，即**永远是 2 行头注释 + 生效条目**；用户手打的其它注释行会丢 | `5905` |
| 空集兜底 | 解析结果为空 → 保留旧内存值（清空不生效，R11）`5902-5903`；加载时解析为空 → 用内置默认 `3253` | — |
| 作用范围 | 仅 Everything（`f`/`d`）结果 `1579` 与本地程序结果 `1396`；网页/一键组不过滤（`1579` 之前 `Refresh` 的 Web/Group 分支不查） | 465-467 注释、`1396`、`1579` |

---

## 4. 「恢复默认」的默认值来源

三个「恢复默认」按钮对应三套内置常量；**排除路径和网页规则各只有一个按钮，一键 Tab 没有恢复默认**。

### 4.1 网页规则：`IDC_BTN_RESET`（仅 Tab 1 可见，`4369`）

处理：`SetWindowTextW(IDC_EDT_RULES, DefaultRulesText())`，**不落盘**（保存时才写）— `src/main.cpp:6171-6172`。
`DefaultRulesText()` `149` → `kDefaultRulesText` `126-134`，原文（8 条，最后一行无尾 `\r\n`）：

```
bd https://www.baidu.com/#ie=UTF-8&wd={q}
bili https://search.bilibili.com/all?keyword={q}
xhs https://www.xiaohongshu.com/search_result?keyword={q}
zhihu https://www.zhihu.com/search?q={q}
douban https://www.douban.com/search?q={q}
gh https://github.com/search?q={q}
google https://www.google.com/search?q={q}
x https://x.com/search?q={q}
```

另有历史快照 `kLegacyRulesV1`（`140-147`，去掉最后一行 `x`，共 7 条）用于「用户从未改过则整体刷新」的自动升级 `3017-3023`。

### 4.2 排除路径：`IDC_BTN_EXCLUDE_DEFAULT`

处理：`6173-6181` — 编辑器写入 `DefaultExcludePathsText()`，计数标签写入 `DefaultExcludePaths().size()`，**不落盘**。两个函数必须保持一致（注释里明说按归一化比对）。

`DefaultExcludePathsText()` `3149-3173`（5 行头注释 + 动态展开）：

```
# 排除路径：每行一条，支持通配符 * ?。路径以排除项开头即视为命中（含整棵子树）。
# 作用于 Everything 搜索（d/f 前缀）与本地程序搜索的结果，网页与一键组不受影响。
# 默认已排除各盘符回收站、系统目录，以及 node_modules/__pycache__ 等开发噪音目录
# 和编辑器备份/临时文件（*~、~$、*.swp 等），可自行增删。
# 以 . 开头的文件/文件夹（.git/.vscode/.gitignore …）由上方开关统一跳过，无需在此逐条填写。
```
随后：对 `GetLogicalDrives()` 的**每个现存盘符**生成一行 `<X>:\$RECYCLE.BIN`（`3156-3162`，sidecar 必须同样动态展开，不能硬编码 `C:`），再加固定 9 条：

```
*\System Volume Information\*
*\RECYCLER\*
*\node_modules
*\node_modules\*
*\__pycache__\*
*~
*\~$*
*.swp
*.swo
```

`DefaultExcludePaths()` `3187-3207` 是同上的**纯条目版本**（无注释行），用于计数与升级合并。

被 v3 撤销的旧默认（升级时按归一化精确剔除，只剔这 3 条，用户自加的其它点开头条目一律保留）：`kLegacyDotDefaults` `3177-3178` = `*\.git`、`*\.git\*`、`*\.vscode\extensions\*`；剔除函数 `IsLegacyDotDefault` `3179-3184`，迁移主体 `LoadExcludePaths` `3236-3251`。

### 4.3 主题 / ThemeTune

「恢复默认」= 无按钮；语义由 `ThemeTune` 缺键 + 长度不符即整块忽略来承担（`2832-2848`），预设值写死在 `kThemes[]`（`alpha` 第 3 个字段、`radiusWindow` 默认 10，`193-197`、`308-311`）。

---

## 5. 保存 / 取消 / 实时预览的现有语义

### 5.1 快照字段（`xxxSaved`）

设置窗打开时抓快照，「取消」时逐项回退。分两处抓：

- `OpenSettings()` `6238-6248`：`themeSaved`、`beautifySaved`、`antiGhostSaved`、整表 `sTuneSaved`（每个主题的 alpha/radius）。
- `WM_CREATE` `4693-4709`：`startupSaved`、`hotkeyModeSaved`、`weightEnabledSaved`、`weightFlushSaved`、`weightMaxSaved`、`learnEnabledSaved`、`learnScopeSaved`、`learnMaxSaved`、`learnExpireSaved`、`fdjEnabled`←**从注册表重读**（`4703`，注意这里会覆盖主进程运行态）、`fdjEnabledSaved`、`skipDotFoldersSaved`、`topEnabledSaved`、`cmdEnabledSaved`、`winEnabledSaved`、`captureEnabledSaved`。
- Shell 组 `5198-5207`：`shellTypeSaved`、`shellCustomPathSaved`、`shellCustomArgsSaved`、`shellDefaultCwdSaved`、`shellShowWindowSaved`、`winGroupProcSaved`、`winShowUwpSaved`、`winShowProcSaved`、`winCacheSecSaved`。

| 快照字段 | 保护什么（即取消能撤销的副作用） |
|----------|-----------------------------------|
| `startupSaved` | 「开机自动启动」勾选（`g.startupWanted`）；只有保存才 `SetStartup`（`5862`），因此不会误写 Run 键 |
| `themeSaved` + `sTuneSaved` | 主题索引 + **全部 8 个主题的 alpha/radius**（`kThemes[]` 是被实时预览直接改写的编译期数组，回退是**整表恢复**，`6121-6130`） |
| `beautifySaved` | 标题栏/圆角/暗色菜单开关（回退时再调一次 `ApplyBeautify`，`6131`） |
| `antiGhostSaved` | 设置窗 `WS_EX_COMPOSITED` 位（回退时重设扩展风格 + `SWP_FRAMECHANGED`，`6148-6155`） |
| `skipDotFoldersSaved` | 点开头过滤开关（影响 `IsPathExcluded` 全局行为） |
| `fdjEnabledSaved` | 文件对话框跳转镜像值 |
| `topEnabledSaved` / `cmdEnabledSaved` / `winEnabledSaved` | `命令` Tab 三个开关（`6167-6169`；`topEnabled` 在 `6147` 被回退了一次，重复但无害） |
| `weightEnabledSaved`/`weightFlushSaved`/`weightMaxSaved` | 搜索权重三参数（`6137-6139`） |
| `learnEnabledSaved`/`learnScopeSaved`/`learnMaxSaved`/`learnExpireSaved` | 自学习四参数（`6141-6144`） |
| `sTuneSaved` | 见「themeSaved」行；单独列出因为它是**数组**，且 `kThemes` 项数变化即失效（循环上限 `i < sTuneSaved.size()`，`6122`） |

**没有快照保护的**：四个编辑器文本（`IDC_EDT_RULES`/`LAUNCH`/`KILL`/`EXCLUDE`）——取消即丢弃，保存即生效，没有回退动作；`settingsTab`；「清空权重/清空学习」是**点了就立刻落盘**的即时动作，取消按钮无法撤销（`6038-6068`）。

### 5.2 保存（`IDC_BTN_SAVE`，`5806-5948`）

顺序：写 `CenterWake`/`HotkeyMode`/`Theme`/`ThemeTune`(整 blob 从当前 `kThemes` 回灌) → `Beautify`/`AntiGhost` → 权重 3 项 → 自学习 4 项 → `fdj_set_enabled` → `SkipDotFolders` → `SetStartup` → 4 个命令开关 → 读 4 个编辑器并 `SaveWebRules`/`SaveRegText(GroupLaunch/GroupKill/ExcludePaths)` → Shell 5+ 项 → `if (IsWindowVisible(g.hwnd)) LayoutAndRepaint()` → `DestroyWindow(h)`。

要点：
- **不做** 校验、**不给** 错误提示、**不重启**任何钩子/索引；重生效全靠「控件点击时已直接改内存」+ 后续按需重绘。
- 主题/美化的效果在点下的瞬间就已应用（见 5.3），保存只负责持久化。
- `ThemeTune` 是从 `kThemes[]` **当前内存值**反向序列化（`5819-5828`），所以「未点保存但预览过」的改动若随后按保存会被一并写死。

### 5.3 实时预览调用链

| 交互 | 链路 |
|------|------|
| 透明度/圆角滑块拖动 | trackbar 发 `WM_HSCROLL`（不进 `WM_COMMAND`）→ `SettingsProc` `5505`；`g.beautify` 为 false 时**直接 break 不预览**（`5510`）→ `TBM_GETPOS` `5511` → alpha：`g.theme->alpha = v`（`5512`）；radius：`g.theme->radiusWindow=v` 并推导 `radiusCard=r*0.8`、`radiusButton=r*0.6`、`radiusInput=r*0.6`（`5513-5518`）→ **`ApplyTheme()`**（`5519` → `697`：重设 `fInput/fList`、销毁并重建全部 13 个刷子 + 描边笔、`EnableDarkMenus()`、`ApplyDarkTitlebar(设置窗)`、星空主题重跑 `GenerateStars()`、`EnumChildWindows(RefreshChildFont)` + `RefreshComposition` 设置窗、`Layout()`+`RepaintNow()` 主窗）→ `UpdateTuneLabel`（`5520` → `4629-4637` 把标签写成「透明度：N」/「圆角半径：N」） |
| 主题列表选择 | `LBN_SELCHANGE` `6105`；beautify off 时 `break`（`6107`）→ 改 `g.themeIdx`/`g.theme` → `ApplyTheme()`（`6114`）→ `SyncTuneSliders(h)`（`6117` → `4638`：把两个滑块位置/标签同步到新主题的 alpha/radius）→ `InvalidateRect` 列表 |
| 美化开关点击 | `5952-5956`：`g.beautify` 取反 → `ApplyBeautify()`（`4456`：`ApplyDarkTitlebar` `4448` + `ApplyRoundCorners` `2590` 或反向撤销 DWM 属性 `4462-4467` → `EnableDarkMenus()` `4469`）→ 重绘列表框、`EnableWindow` 两个滑块 |
| 抗残影点击 | `5961-5972`：取反 → 改设置窗 `GWL_EXSTYLE` 的 `WS_EX_COMPOSITED` → `SetWindowPos(SWP_FRAMECHANGED…)` → `RefreshComposition(h)` `689` |
| 快捷键方案切换 | `5973-5978`：改 `g.hotkeyMode` → 主窗可见时 `RepaintNow()`（结果行右侧徽章字母/数字即时变化，`4094`） |
| 其余复选框/下拉 | **无预览**：只改 `g.*` 内存字段 + `InvalidateRect` 自绘控件（`5979-6032`、`6085-6104`），保存才落盘 |
| 回车 | 消息循环全局拦截 → `SendMessage(WM_COMMAND, IDC_BTN_SAVE)`（`7358-7363`，见 R4） |

---

## 6. ★ 保存后主进程需要重放生效的设置（分组 + 函数 + 行号）

现状：设置窗和主进程**同进程**，所以「重放」大多被点击事件顺带完成了。拆成 sidecar 后，下面每一组都需要一条显式通知。

### G1 主题 / 外观 —— 必须重建 DWM 属性、字体、刷子（**必须通知**）

| 项 | 主进程要跑的代码 |
|----|------------------|
| `Theme`、`ThemeTune`（alpha/radius）、`Beautify` | ① 重新载入主题索引与 Tune blob：`LoadSettings` 的 `2798-2849` 段 ② **`ApplyTheme()`** `697`（重建 `fInput/fList`、`brDivider/brMenuBg/brEditBg/brSettingsBg/brCardBg/brInputBg/brAccent/brAccent2/brTextDis/brStatus{OK,Warn,Err}`、`penBorder`；`GenerateStars` `742`；主窗 `Layout()` `1412` + `RepaintNow()` `1441`） ③ **`EnableDarkMenus()`** `2230`（uxtheme 135/136 强制菜单深浅，进程级） ④ `ApplyDarkTitlebar(g.hwnd)` 语义：主窗无标题栏，仅设置窗用（`729`）；若 sidecar 自己有标题栏，则 sidecar 自理 |
| `AntiGhost` | **不需要通知主进程**：只影响设置窗扩展风格（`5964-5970`、`6259`）。见 R6 |
| alpha 消费点 | 只有弹窗（Notice `6977`、TopToast `7100`，且 `max(alpha,240)`）→ 下次弹窗自然生效，无需重放 |

### G2 排除路径 / 结果过滤 —— 必须重建归一化镜像（**必须通知**）

| 项 | 主进程要跑 |
|----|-----------|
| `ExcludePaths` | `ParseExcludePaths` `3111` 重解析 → `RebuildExcludeNorm()` `994`。匹配点在查询期（`1396` 程序、`1579` Everything），**程序索引不需要重建**（`BuildProgramsInto` `1254` 不查排除） |
| `SkipDotFolders` | 写 `g.skipDotFolders`；`IsPathExcluded` `1028` 立即读到，无需其它动作 |
| 若主窗当前可见且有结果 | 应触发一次 `Refresh()` `1852` 重算（当前同进程版本没做，靠下次输入生效） |

### G3 命令文本类（网页规则 / 一键组）—— 必须重建解析表（**必须通知**）

| 项 | 主进程要跑 |
|----|-----------|
| `WebRules` | `LoadWebRules()` `3006`（含默认集升级判定 `3017-3023`）→ `g.webCmds`；消费点 `FindWebCmd` `1623`、`Refresh` `1992` |
| `GroupLaunch` / `GroupKill` | `LoadGroupRules()` `3103` → `g.groupsLaunch/g.groupsKill`；消费点 `FindGroup` `3257`、`Refresh` `2026-2027` |
| 注意 | 这两个是**派生缓存**，注册表存原文。sidecar 只写注册表即可，解析交给主进程（避免两套解析实现漂移） |

### G4 快捷键 / 钩子 —— 设置窗实际**不产生**需要重注册的热键（重要澄清）

| 项 | 事实 |
|----|------|
| `HotkeyWake` | **不在设置窗**，只由托盘菜单 `ToggleWakeHotkey` `2712` 改：`UnregisterHotKey` `2715` / `InstallWakeHook` `2690` + `RegisterWakeHotkey` `2648` / `RemoveWakeHook` `2696` / `UpdateHotkeyRetryTimer` `2704` / `TrayUpdateTip` `2627`。sidecar 若要暴露这个开关，必须走通知（主进程动作不可省） |
| `HotkeyMode` | 无需重注册。`g.hotkeyMode` 是每次按键现读（`TryAltHotkey` `6279-6290`、`WM_SYSKEYDOWN` `6674`）；但**结果徽章要重绘** → `RepaintNow()` `1441`（同进程版本正是在 `5977` 做的） |
| `CenterWake` | 无需任何重放：`Layout()` `1425` 每次唤醒现读 |
| `Ctrl+Alt+G`（fdj 同步） | 固定注册（`7348`），不可配 |

### G5 文件对话框跳转（`fdjEnabled`）—— **无需重装钩子**

- 钩子安装是一次性的：`fdj_init(g.hwnd)` `main.cpp:7337` → `SetWinEventHook` ×2 + `SweepOldHookCopies` + `InstallHook64`（`filedlg_jump.cpp:253`，加载 `%LOCALAPPDATA%\Flowtary\hooks\filedlg_hook64_<版本>.dll` 版本化副本，`VersionedHookCopy` `183`）+ `StartAgent32`（`272`，启动 `filedlg_agent32.exe "<版本化 hook32 路径>"` 装 32 位钩子）。
- 开关是**每次调用现读注册表**：`fdj_enabled()` `filedlg_jump.cpp:333`，用于 `351`/`414`，以及 `main.cpp:2055`。
- 因此 sidecar 直接 `fdj_set_enabled()`（`339`，就是 `RegSetKeyValueW`）即可即时生效。主进程侧**唯一**要同步的是 UI 镜像 `g.fdjEnabled`（`main.cpp:409`）——它不参与逻辑判定。
- 卸载只在退出：`fdj_uninit()` `7369`（含 `WM_DESTROY` 路径）。
- 32 位链路：`src/agent32.cpp`（76 行）+ `src/hookdlg.cpp`（238 行）注入 DLL；`hookdlg.cpp` 只被子类化对话框所在进程加载，与设置窗无交互 → **sidecar 不需要链接任何钩子代码**。

### G6 索引 / 缓存类

| 项 | 需要的主进程动作 |
|----|------------------|
| `WinCacheSec` | 逻辑上是**自动过期**：`SearchWindows` `1784` 比较 `now - windowCacheTick >= winCacheSec*1000`。改小后最坏要等旧间隔才见效 → 建议通知里让主进程 `g.windowCache.clear()` 或重置 `windowCacheTick = 0`（字段 `529-530`）。**不**需要重建程序索引 |
| `WinGroupProc`/`WinShowUwp`/`WinShowProc` | 现读，无需重放（`1795`/`1811`/`1825`）。缓存里存的是原始 `WinInfo`，不受这些开关影响 |
| 程序索引 | 与设置无关，只由托盘「刷新缓存」触发：`IDM_REFRESH` `6546-6548` → `BuildPrograms()` `1286`；启动扫描 `ScanProgramsThread` `1294` → `WM_APP_PROGRAMS_READY` `6497` |

### G7 只影响下一次搜索/执行，无需任何重放（**通知可省略，或合并成一条泛化的 `ReloadSettings`**）

| 项 | 现读点 |
|----|--------|
| `WeightEnabled` | `RecordClickWeight` `3386`、`CollectPrefixWeights` `1354`、`GetClickWeight` `1379`、`1602` |
| `WeightMaxEntries` | `3396`/`3426`/`3466`（裁剪/上限） |
| `WeightFlush` | `3406-3411`、`ScheduleLearnedSave` `3631-3636` |
| `LearnEnabled`/`LearnScope` | `RecordLearned` `3644`/`3668-3669`、`PrependLearnedRows` `3895` |
| `LearnMaxEntries`/`LearnExpireDays` | 上限在写入/写出时生效（`3675`/`3705`）；**过期只在启动 `LoadLearned` 时清理一次**（`3857`）→ 改小过期天数不会立刻遗忘，属可接受降级，但需记录 |
| `TopCmd`/`CmdCmd`/`WinCmd`/`CaptureCmd` | `Refresh` `1896`/`1913`/`1926`/`1934` |
| `ShellType`/`ShellCustomPath`/`ShellCustomArgs`/`ShellDefaultCwd`/`ShellShowWindow` | `BuildShellCommand` `1714-1743`、`GetShellCwd` `1679` |
| `CenterWake` | `Layout` `1425` |
| 开机自启 | `SetStartup` 由谁写都行；主进程仅在打开设置窗时读一次（`4693`） |

### G8 要求重启主进程才生效的

严格意义上 **只有一项**：

1. **`CaptureExe` / ScreenCapture.exe 的安装位置**：启动时一次性解析（`main.cpp:7239-7264`：同目录优先 → 注册表 `CaptureExe` 兜底），运行期唯一的更新点是下载按钮成功回调里 `g.captureExe = dest`（`6193`）。sidecar 接管下载后主进程拿不到 → 要么新增 `CaptureInstalled` 通知（让主进程重跑 7239-7264 那段），要么要求重启。**建议前者**，改动小。

其余都可通过重放生效。**另有两处「重启才好」的既存行为**（不算阻塞，但要知道）：
- `ThemeTune` 长度与 `kThemes` 项数绑定（`2834`）：主程序加主题后旧 blob 自动作废，sidecar 无法「部分迁移」。
- 全局钩子/agent32 只在 `fdj_init`/`fdj_uninit` 生命周期里存在（`7337`/`7369`），没有「重装」入口。

### G9 建议的最小通知集（由上面各组推导）

| 通知 | 触发项 | 主进程动作 |
|------|--------|-----------|
| `AppearanceChanged` | Theme, ThemeTune, Beautify | 重载主题段 → `ApplyTheme()` → `EnableDarkMenus()` → `Layout`+`RepaintNow` |
| `FiltersChanged` | ExcludePaths, SkipDotFolders | 重解析 → `RebuildExcludeNorm()` →（可选 `Refresh()`） |
| `CommandTextChanged` | WebRules, GroupLaunch, GroupKill | `LoadWebRules()` + `LoadGroupRules()` |
| `WindowCacheChanged` | WinCacheSec | 清 `windowCache`/重置 tick |
| `BehaviorFlagsChanged` | 其余全部现读项 + HotkeyMode | `LoadSettings()` 一次即可（HotkeyMode 额外 `RepaintNow()`） |
| `HotkeyWakeChanged` | （仅托盘菜单，若未来进设置窗） | `ToggleWakeHotkey` 等价逻辑 |
| `ClearWeights` / `ClearLearned` | 清空按钮 | 在主进程执行 `ClearClickWeights()` `3486` / `ClearLearned()` `3876`（**请求-应答**回新计数） |
| `CaptureInstalled` | 下载成功 | 重跑 `7239-7264` 的解析 |
| `FdjToggled` | FileDlgJump | 无动作（可选：同步 `g.fdjEnabled` 镜像） |
| 反向请求 `QueryStats` | sidecar 打开时 | 回 `weightCount`/`learnCount`/`excludePaths.size()`/`capture 状态`（R2） |

---

## 7. 开机自启的具体写法

```
键：HKCU\Software\Microsoft\Windows\CurrentVersion\Run
值名：Flowtary
类型：REG_SZ
内容："<主 exe 完整路径>"            // 外层带英文双引号，无任何命令行参数
```

- 写：`SetStartup(true)` `src/main.cpp:2764-2770`，路径来自 `GetExePath()` = `GetModuleFileNameW`（`2750-2754`）→ **进程自己的 exe**。
- 删：`SetStartup(false)` → `RegDeleteKeyValueW`（`2771`）。
- 读（勾选态）：`GetStartupEnabled()` `2756-2762` = **只要 `RegGetValueW` 返回 `ERROR_SUCCESS` 就算开**，不校验内容、不校验路径是否指向当前 exe。
- 触发时机：仅保存按钮（`5862`），取消按钮不回滚 Run 键（因为勾选只改内存）。
- **sidecar 注意**：sidecar 的 `GetModuleFileNameW` 是 `flowtary-settings.exe`。路径必须由主进程传入（命令行参数 / 环境变量 / 或读 `RunningPath` `7219`）。

---

## 8. 截图工具 Tab：两个下载按钮

| 按钮 | 源 URL | 落地路径 | 证据 |
|------|--------|----------|------|
| `IDC_BTN_DL_SCREENCAPTURE`（3091） | `https://github.com/xland/ScreenCapture/releases/latest/download/ScreenCapture.exe` | `GetFlowtaryDir() + "ScreenCapture.exe"` | `6182-6199` |
| `IDC_BTN_DL_IMAGEREADER`（3092） | `https://github.com/xland/ImageReader/releases/latest/download/ImageReader.exe` | `GetFlowtaryDir() + "ImageReader.exe"` | `6200-6216` |

- `GetFlowtaryDir()` `4255-4262` = `GetModuleFileNameW` 取到最后一个 `\` 为止（**含尾分隔符**）→ 又是「主进程 exe 目录」耦合（同 R15）。
- **下载是同步阻塞 UI 线程的**：`DownloadFile` `4285` 先 `URLDownloadToFileW`（`4287`），失败再走 WinINET `InternetOpenUrlW` + 8KB 循环读 `.tmp` 再 `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`（`4293-4339`）。期间只把按钮文字改成「下载中…/下载中…（约 25MB，请稍候）」并 `EnableWindow(FALSE)`（`6185-6186`、`6202-6204`）——**没有任何进度回调**（`URLDownloadToFileW` 的 `IBindStatusCallback` 传 `nullptr`，`4287`）。
- **无校验**：不比对大小/哈希/版本；成功判据只是 `hr==S_OK` 或写入 `.tmp` 后 rename 成功。
- 失败提示：`MessageBoxW` ×5 分支（内存不足 / 无法建立网络连接 / 无法访问下载链接 / 无法创建临时文件 / 写入出错 / 无法替换目标文件，`4290-4337`）。成功提示：`MessageBoxW` 「下载成功」×2（`6194`、`6211`）。
- 状态检测 `UpdateCaptureStatus(HWND hSettings)` `4265-4282`：`GetFileAttributesW` 两个文件是否存在 → 4 种文案（均就绪 / SC 就绪 IR 缺（文字识别不可用）/ IR 就绪 SC 缺 / 均未下载）。调用点：切到该 Tab（`4428`）、`WM_CREATE` 末尾（`5382`）、每个下载按钮结束后（`6199`/`6216`）。
- 相关但**不在本 Tab** 的：`ss` 命令开关在「命令」Tab（`IDC_CHK_CAPTURE` `5235-5240`）；执行在 `ExecuteRow` 的 `Row::Capture` 分支 `2153-2161`，命令行固定 `enter=<cap|pin|ocr> tray=false auto-quit=true`。
- 建议（含依据）：**下载逻辑放 sidecar**，因为 ① 它是纯 UI + 网络，与主进程运行时状态无关；② 同步阻塞会把卡顿留在 sidecar 自己的窗口上，可以按 FLTK 惯例改成后台线程 + 进度条，不必动主进程消息循环。但必须配套两件事：① sidecar 需要**主 exe 目录**（不能用自己的），② 下载成功后要发 `CaptureInstalled` 让主进程重跑 `7239-7264`（否则 `ss` 仍提示未找到，见 R8）。状态检测 `UpdateCaptureStatus` 同理可搬到 sidecar（只需 exe 目录）。

---

## 9. 控件清单（FLTK 重建规格书）

窗口：类 `FlowtarySettings`（`7328`），标题 **「Flowtary 设置」**（`6260`），默认尺寸 `S(600)×S(540)`（`6251`），最小跟踪 `S(560)×S(430)`（`5389-5390`），风格 `WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN|WS_THICKFRAME`（`6261-6262`，即可拖拽改尺寸 + 原生标题栏）。左侧 Tab 栏宽 `S(140)` + 1px 分隔（`5429-5438`），内容区左边距 `S(156)`（`4686`）。所有坐标经 `S()` = 逻辑像素 × 全局比例（`388`）。

`IDC_*` 常量总表在 `4144-4239`。可见性分组由 `ShowSettingsTab` `4352-4445` 决定。

### Tab 0 「常规」（`IDC_TAB_GENERAL` 3014）

| ID | Win32 类 / 风格 | 标签文字（原文） | 绑定 | 候选项 |
|----|-----------------|------------------|------|--------|
| `IDC_CHK_START` 3001 | BUTTON, BS_OWNERDRAW | `开机自动启动` | `g.startupWanted`→Run 键 | 勾选/不勾 |
| `IDC_LBL_HOTKEY` 3010 | STATIC | `结果项快捷键：` | — | — |
| `IDC_CMB_HOTKEY` 3013 | COMBOBOX CBS_DROPDOWNLIST\|OWNERDRAWFIXED | — | `HotkeyMode` | `Alt + 数字（1–9,0）`→0；`Alt + 字母（A–J）`→1；`关闭`→2（`4783`） |
| `IDC_LBL_WAKE` 3002 | STATIC | `唤醒位置：` | — | — |
| `IDC_CMB_WAKE` 3003 | COMBOBOX | — | `CenterWake` | `屏幕居中`→1；`跟随鼠标`→0（`4803`、`6087`） |
| `IDC_CHK_FILEDLGJUMP` 3034 | BUTTON OWNERDRAW | `文件对话框跳转（在文件对话框中直接定位文件夹）` | `FileDlgJump` | 勾选/不勾 |

### Tab 1 「网页规则」（`IDC_TAB_WEB` 3015）

| ID | 类 | 文字 | 绑定 |
|----|----|------|------|
| `IDC_LBL_RULES` 3006 | STATIC | `网页搜索规则：` | — |
| `IDC_EDT_RULES` 3007 | EDIT ES_MULTILINE\|ES_AUTOVSCROLL\|WS_VSCROLL，字体 `fList`，`DarkMode_Explorer` | 初值 = `RulesToText(g.webCmds)`（`4905`） | `WebRules` |
| `IDC_LBL_RULEHINT` 3009 | STATIC | `每行一条：前缀 + 空格 + 链接模板（{q} 为关键词，回车跳转）` | — |
| `IDC_BTN_RESET` 3008 | BUTTON OWNERDRAW | `恢复默认` | 写编辑器文本（`6172`） |

### Tab 2 「主题」（`IDC_TAB_THEME` 3016）

| ID | 类 | 文字 | 绑定 |
|----|----|------|------|
| `IDC_CHK_BEAUTIFY` 3017 | BUTTON OWNERDRAW | `启用界面美化（暗色标题栏 / 圆角窗口 / 暗色菜单）` | `Beautify` |
| `IDC_CHK_GHOST` 3053 | BUTTON OWNERDRAW | `启用抗残影双缓冲（实验性）` | `AntiGhost` |
| `IDC_LBL_GHOSTHINT` 3054 | STATIC | `实验性：开启整窗双缓冲消除残影；个别控件可能偶发闪烁` | — |
| `IDC_LBL_THEME` 3011 | STATIC | `主题：` | — |
| `IDC_LST_THEME` 3040 | LISTBOX LBS_OWNERDRAWFIXED\|HASSTRINGS\|NOTIFY，行高 `S(34)` | 自绘：左三色块(accent/bgCard/bg) + 名称 + 当前项对勾（`5570-5619`） | `Theme`。候选（8 项，`230-305`）：`黑色简洁`0、`透明Mac黑暗`1、`透明蓝色`2、`星空风格`3、`日出浅白`4、`Ice-Quartz`5、`Obsidian`6、`Honey-Amber`7 |
| `IDC_LBL_TUNE` 3041 | STATIC | `微调（实时预览，保存后生效）` | — |
| `IDC_LBL_ALPHA` 3043 | STATIC | `透明度：N`（`UpdateTuneLabel` `4629`） | — |
| `IDC_TRK_ALPHA` 3042 | TRACKBAR_CLASS TBS_AUTOTICKS\|TBS_BOTH | 范围 `0..255`（`4853`） | `ThemeTune[Theme].alpha` |
| `IDC_LBL_RADIUS` 3047 | STATIC | `圆角半径：N` | — |
| `IDC_TRK_RADIUS` 3046 | TRACKBAR_CLASS | 范围 `0..14`（`4864`） | `ThemeTune[Theme].radius` |
| — | 注：`IDC_CMB_THEME` 3012 在 `5680`/`5694`/`5699` 被引用，但**从未创建控件** → 死代码，迁移可删 | | |

### Tab 3 「一键」（`IDC_TAB_GROUP` 3019）

| ID | 类 | 文字 | 绑定 |
|----|----|------|------|
| `IDC_LBL_LAUNCH` 3020 | STATIC | `启动组（关键字 → 文件）：` | — |
| `IDC_EDT_LAUNCH` 3021 | EDIT MULTILINE，初值 `GroupsToText(g.groupsLaunch)`（`4946`） | | `GroupLaunch` |
| `IDC_LBL_KILL` 3022 | STATIC | `关闭组（关键字 → 进程）：` | — |
| `IDC_EDT_KILL` 3023 | EDIT MULTILINE，初值 `GroupsToText(g.groupsKill)`（`4960`） | | `GroupKill` |
| `IDC_LBL_GROUPHINT` 3024 | STATIC（2 段拼接） | `每行：关键字 + 空格 + 目标（多个用 ; 分隔）；# 开头为注释。启动组填文件全路径，关闭组填进程名（可省 .exe）` | — |

### Tab 4 「搜索权重」（`IDC_TAB_WEIGHT` 3025）

| ID | 类 | 文字 | 绑定 | 候选 |
|----|----|------|------|------|
| `IDC_CHK_WEIGHTON` 3026 | BUTTON OWNERDRAW | `启用点击权重记忆（相同搜索词下点过的条目排序提前）` | `WeightEnabled` | |
| `IDC_LBL_FLUSH` 3027 | STATIC | `写入时机：` | | |
| `IDC_CMB_FLUSH` 3028 | COMBOBOX | | `WeightFlush` | `立即写入`0 / `延迟合并写入`1 / `退出时写入`2（`4990`） |
| `IDC_LBL_MAXENT` 3029 | STATIC | `条目上限：` | | |
| `IDC_CMB_MAXENT` 3030 | COMBOBOX | | `WeightMaxEntries` | `1000 条` / `5000 条` / `20000 条` / `50000 条`（`5008-5012`） |
| `IDC_BTN_WIPE` 3031 | BUTTON OWNERDRAW | `清空权重数据…` ↔ 武装态 `再次点击确认清空`（`6058`/`6063`） | 动作 | 两步确认，3 s 定时器 9001（`6059`、`5403-5408`） |
| `IDC_LBL_WCOUNT` 3032 | STATIC | `当前已记忆 {N} 条权重数据`（`4026`） | **运行时** | |
| `IDC_LBL_WEIHINT` 3033 | STATIC | `权重按「搜索词 → 文件完整路径」记录，不同搜索词互相独立；f/d 前缀只对后面的关键词记录，网页前缀（如 gg、bd）的点击不记录。数据文件：%APPDATA%\Flowtary\weights.dat` | — | |

### Tab 5 「排除路径」（`IDC_TAB_EXCLUDE` 3048）

| ID | 类 | 文字 | 绑定 |
|----|----|------|------|
| `IDC_CHK_DOTFOLDER` 3096 | BUTTON OWNERDRAW | `不搜索 . 开头的文件与文件夹（.git / .gitignore …）` | `SkipDotFolders` |
| `IDC_LBL_EXCLUDEHINT` 3051 | STATIC | `每行一条路径，大小写不敏感，支持通配符 * ?。以排除项开头的文件/文件夹/程序会被过滤。网页与一键组不受影响，. 开头的文件/文件夹（.git / .gitignore 等）由上方开关统一跳过。保存后生效。` | — |
| `IDC_EDT_EXCLUDE` 3049 | EDIT MULTILINE，初值 `ExcludePathsToText(g.excludePaths)`（`5069`） | | `ExcludePaths` |
| `IDC_BTN_EXCLUDE_DEFAULT` 3050 | BUTTON OWNERDRAW | `恢复默认` | 写编辑器 + 计数（`6176-6181`） |
| `IDC_LBL_EXCLUDECOUNT` 3052 | STATIC | `当前已记忆 {N} 条排除项` | **运行时**（`g.excludePaths.size()`） |

### Tab 6 「Shell 与窗口」（`IDC_TAB_SHELL` 3060）

| ID | 类 | 文字 | 绑定 | 候选 |
|----|----|------|------|------|
| `IDC_LBL_SHELLTYPE` 3061 | STATIC | `Shell 程序：` | | |
| `IDC_CMB_SHELLTYPE` 3062 | COMBOBOX | | `ShellType` | `命令提示符 (cmd)`0 / `PowerShell`1 / `Git Bash`2 / `自定义…`3（`5103`） |
| `IDC_LBL_SHELLPATH` 3063 | STATIC | `自定义 Shell 路径：` | 仅 shellType==3 显示（`5209-5214`、`6092-6096`） | |
| `IDC_EDT_SHELLPATH` 3064 | EDIT ES_AUTOHSCROLL | | `ShellCustomPath` | |
| `IDC_LBL_SHELLARGS` 3065 | STATIC | `自定义启动参数：` | 同上条件显示 | |
| `IDC_EDT_SHELLARGS` 3066 | EDIT ES_AUTOHSCROLL | 提示语义：`{c}` = 命令 | `ShellCustomArgs` | |
| `IDC_LBL_SHELLCWD` 3067 | STATIC | `默认工作目录：` | | |
| `IDC_CMB_SHELLCWD` 3068 | COMBOBOX | | `ShellDefaultCwd` | `用户目录 (%USERPROFILE%)`0 / `系统默认目录 (System32)`1 / `桌面目录`2（`5144`） |
| `IDC_CHK_SHOWWIN` 3069 | BUTTON OWNERDRAW | `前台显示输出窗口（否则后台静默执行）` | `ShellShowWindow` | |
| `IDC_CHK_WINGROUP` 3070 | BUTTON OWNERDRAW | `合并同进程窗口（多窗口进程显示为分组）` | `WinGroupProc` | |
| `IDC_CHK_WINUWP` 3071 | BUTTON OWNERDRAW | `显示 UWP 应用窗口` | `WinShowUwp` | |
| `IDC_CHK_WINPROC` 3072 | BUTTON OWNERDRAW | `结果副标题显示进程名` | `WinShowProc` | |
| `IDC_LBL_WINCACHE` 3073 | STATIC | `窗口缓存刷新间隔(秒)：` | | |
| `IDC_CMB_WINCACHE` 3074 | COMBOBOX | | `WinCacheSec` | `1 秒`/`2 秒`/`5 秒`/`10 秒`/`30 秒`/`60 秒` → 1/2/5/10/30/60（`5186-5188`） |

### Tab 7 「命令」（`IDC_TAB_CMD` 3080）

| ID | 文字 | 绑定 |
|----|------|------|
| `IDC_CHK_TOP` 3035 | `top 命令（空格+top 回车，置顶/取消置顶当前窗口）` | `TopCmd` |
| `IDC_CHK_CMD` 3081 | `cmd 命令（空格+cmd 执行 Shell 命令，原 “> 命令”）` | `CmdCmd` |
| `IDC_CHK_WIN` 3082 | `w 命令（空格+w 切换窗口，原 “< 关键词”）` | `WinCmd` |
| `IDC_CHK_CAPTURE` 3083 | `ss 命令（空格+ss 截图，空格+ss pin 贴图）` | `CaptureCmd` |

### Tab 8 「截图工具」（`IDC_TAB_CAPTURE` 3090）

| ID | 类 | 文字 | 绑定 |
|----|----|------|------|
| `IDC_LBL_CAPTURE_STATUS` 3093 | STATIC（初始空串，`4265` 填） | 4 种状态文案（见 §8） | 运行时（文件存在性） |
| `IDC_BTN_DL_SCREENCAPTURE` 3091 | BUTTON OWNERDRAW | `下载 ScreenCapture.exe（约 1MB）` ↔ `下载中…` | 动作（§8） |
| `IDC_BTN_DL_IMAGEREADER` 3092 | BUTTON OWNERDRAW | `下载 ImageReader.exe（约 25MB，文字识别必需）` ↔ `下载中…（约 25MB，请稍候）` | 动作（§8） |
| `IDC_LBL_CAPTURE_HINT` 3094 | STATIC SS_LEFT | `下载后将自动保存到 Flowtary.exe 同目录。OCR 功能（空格+ss ocr）需要 ImageReader.exe 才可使用。` | — |
| `IDC_LBL_CAPTURE_NOTE` 3095 | STATIC SS_LEFT | `提示：文字识别结果会自动复制到剪贴板。支持的命令：空格+ss（截图）/ 空格+ss pin（贴图）/ 空格+ss ocr（文字识别）` | — |

### Tab 9 「自学习」（`IDC_TAB_LEARN` 3100）

| ID | 类 | 文字 | 绑定 | 候选 |
|----|----|------|------|------|
| `IDC_CHK_LEARNON` 3101 | BUTTON OWNERDRAW | `启用自学习（f/d 搜索打开过的条目自动记忆，下次同词搜索置顶）` | `LearnEnabled` | |
| `IDC_LBL_LEARNSCOPE` 3102 | STATIC | `记录范围：` | | |
| `IDC_CMB_LEARNSCOPE` 3103 | COMBOBOX | | `LearnScope` | `仅 exe`0 / `exe 与文件夹`1 / `所有文件与文件夹`2（`LearnScopeText` `3315`） |
| `IDC_LBL_LEARNMAX` 3104 | STATIC | `条目上限：` | | |
| `IDC_CMB_LEARNMAX` 3105 | COMBOBOX | | `LearnMaxEntries` | `100 条`/`200 条`/`500 条`/`1000 条`/`2000 条`/`5000 条`（`5317`），默认选中第 3 档（`sel=2`，`5319`） |
| `IDC_LBL_LEARNEXPIRE` 3106 | STATIC | `过期清理：` | | |
| `IDC_CMB_LEARNEXPIRE` 3107 | COMBOBOX | | `LearnExpireDays` | `永不过期（长期保留）`0 / `90 天未使用即清理`90 / `180 天未使用即清理`180 / `365 天未使用即清理`365（`LearnExpireText` `3320`、`5340`） |
| `IDC_BTN_LEARNOPEN` 3108 | BUTTON OWNERDRAW | `打开快捷方式目录` | 动作：`CreateDirectoryW` + `ShellExecuteW`（`6033-6037`） | |
| `IDC_BTN_LEARNWIPE` 3109 | BUTTON OWNERDRAW | `清空自学习数据…` ↔ `再次点击确认清空` | 动作：两步确认 + `ClearLearned()`（`6038-6052`） | |
| `IDC_LBL_LCOUNT` 3110 | STATIC | `当前已学习 {N} 条快捷方式` | **运行时** `g.learnCount`（`3444-3450`） | |
| `IDC_LBL_LEARNHINT` 3111 | STATIC | `仅在 f / d 前缀搜索生效：打开成功后按「搜索词 → 路径」记入 %APPDATA%\Flowtary\learned.dat，并在同目录 learned 文件夹镜像一个 .lnk 快捷方式（可拷贝、可备份）。同词或前缀词搜索时学习项置顶，exe 优先；删除 .lnk 即遗忘对应条目。写盘时机沿用「搜索权重」页设置。` | — | |
| — | 注：UI 文案说「仅在 f / d 前缀搜索生效」，但代码里普通程序搜索也会学习（`RecordLearned` `3648-3653`、`git 88bd718`） | | |

### 全局底部按钮行（所有 Tab 常驻，贴底居中）

| ID | 文字 | 说明 |
|----|------|------|
| `IDC_BTN_SAVE` 3004 | `保存` | 强调色底（`5779`）；整窗口全量提交（`5806`） |
| `IDC_BTN_CANCEL` 3005 | `取消` | 逐项回退（`6119-6170`） |
| `IDC_BTN_RESET` 3008 | `恢复默认` | 与上面两个同排，但**只在 Tab 1 可见**（`4369`） |

布局由 `RecordSettingsLayout` `4605` + `ApplyLayoutRule` `4505` + `LayoutSettings` `4649` 实现（记录初始几何 + stretchW/stretchH/anchorBottom/centerRow/growSlot 规则）。FLTK 侧对应关系：两个一键编辑器**对半分纵向余量**（`4511-4523`），`IDC_EDT_RULES`/`IDC_EDT_EXCLUDE` 独占全部余量（`4507-4510`、`4544-4547`），底部三按钮整行居中（`4592-4598`）。

---

## 10. 迁移前必须拍板的清单

1. R4：设置窗内 Enter 是否继续等于「保存」（当前实现会连多行编辑器一起截走）。
2. R5：用 X 关闭窗口时，主进程要不要回滚实时预览对 `kThemes[]` 的内存改动。
3. R2/R9：计数显示与「档位外数值」的取数来源——sidecar 自查注册表，还是向主进程请求。
4. R15：`SetStartup` / `GetFlowtaryDir` / `RunningPath` 三处「主 exe 路径」的传递方式。
5. R10：编辑器「注释被吞」要不要顺手修正（行为变更）。
6. R11：排除路径清空不生效的兜底要不要保留。
7. R12/R13：透明度、圆角滑标的真实作用范围是否要在 UI 上如实标注。
8. §8：下载按钮归属（建议 sidecar）+ 配套的 `CaptureInstalled` 通知。
