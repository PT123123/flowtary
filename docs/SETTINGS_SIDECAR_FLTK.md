# 设置窗 FLTK sidecar 化：实现记录

> 交付内容：手自绘的 Win32/GDI 设置窗（窗口类 `FlowtarySettings` + `SettingsProc`）整体下线，
> 由独立进程 `flowtary-settings.exe`（FLTK 1.4.5 静态链接）接替，10 个 Tab 一次迁完。
> 设计依据与逐控件规格见 `docs/SETTINGS_SIDECAR_CONTRACT.md`（下称「契约」）；本文只记**落地成了什么**、
> 与契约的**差异**、以及构建/部署口径。
> 记录日期：2026-10-01。代码基线：`src/main.cpp` 4757 行（迁移前 6933 行）。

---

## 1. 进程与文件划分

| 文件 | 归属 | 职责 |
|------|------|------|
| `src/settings_model.h/.cpp` | 两侧都编译 | 设置项唯一真源：注册表值名/类型/钳制范围、四个多行编辑器语法与序列化、默认文本、下拉候选表、`weights.dat`/`learned.dat` 路径、`Settings` 快照结构 |
| `src/settings_ipc.h` | 两侧都编译 | 消息号、`Preview`/`Stats` POD 布局、命令行参数名、`IsOurMsg` 判定 |
| `src/settings_main.cpp` | 仅 sidecar | FLTK 界面 + 注册表读写 + 截图工具下载 + 本窗暗色标题栏/圆角/双缓冲 |
| `src/main.cpp` | 仅主进程 | 拉起 sidecar、会话快照、实时预览落内存、保存后重放、计数应答、清空数据 |

主进程不再有任何设置界面代码：`IDC_*` 常量、`SettingsProc`、`ShowSettingsTab`、`LayoutSettings`/`CtlGeom`、
`GetFlowtaryDir`/`DownloadFile`/`UpdateCaptureStatus`、窗口类注册、消息循环里的
「回车=保存」与 `IsDialogMessageW` 拦截、`g.*Saved` 回退字段、`g.antiGhost`、
`g.hSettings`、`g.brEditBg`/`g.brSettingsBg` 全部删除。保留的自绘只有主窗与菜单
（`OwnerDrawMenuItem`/`OwnerMeasureMenuItem`/`EnableDarkMenus`）。

## 2. 传输与消息

`WM_COPYDATA` + `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 2~3s)`，双向都超时：
一边卡住不能把另一边冻住。`dwData` 基址 `0x46540000`，与 Everything 的回复号段（`0x7FF0+`）不冲突；
收发两侧都用 `ipc::IsOurMsg` 先过滤，再 `(DWORD)` 收窄后 switch。

| 消息 | 方向 | 载荷 | 语义 |
|------|------|------|------|
| `MsgHello` | sidecar→主 | `Header{sender}` | 报自己的 HWND；主进程回 `MsgStats` |
| `MsgQueryStats` | sidecar→主 | `Header` | 打开时主动要一次权威计数 |
| `MsgPreview` | sidecar→主 | `Preview`（纯标量 + `tune[16]`） | 只改主进程内存，不碰注册表 |
| `MsgCommit` | sidecar→主 | `Header` | sidecar 已写好注册表，主进程全量重读+重放 |
| `MsgCancel` | sidecar→主 | `Header` | 回退到本次会话打开时的快照 |
| `MsgClosed` | sidecar→主 | `Header` | 会话结束（提交过则只收尾） |
| `MsgWipeWeights` / `MsgWipeLearned` | sidecar→主 | `Header` | 清空必须在主进程做（内存 map + 落盘），sidecar 自己删文件会被主进程下次写盘复活 |
| `MsgCaptureInstalled` | sidecar→主 | `Header` | 下载完成后让主进程重跑 `ResolveCaptureExe()` |
| `MsgStats` | 主→sidecar | `Stats{weightCount, learnCount}` | 计数以主进程内存为准 |
| `MsgMainQuitting` | 主→sidecar | `Header` | 主进程退出时通知 sidecar 自行关闭 |

进程句柄兜底：`CreateProcessW` 成功后 `RegisterWaitForSingleObject` 挂在进程句柄上，回调只
`PostMessage(WM_APP_SETTINGS_GONE)` 回主线程；sidecar 被任务管理器杀掉也能回退。

命令行：`flowtary-settings.exe --ipc <主窗HWND> --main-exe <主程序全路径>`。
`--main-exe` 是因为「开机自动启动」写的必须是主程序路径，sidecar 不能用自身模块路径（契约 R15）。

## 3. 预览 / 提交 / 回退（三条路径）

契约 §0 的 G1–G9 副作用矩阵收敛成三条：

1. **预览** `ApplySettingsPreview(Preview)`：把标量灌进 `g`，`tune` 灌进 `kThemes`
   （alpha 钳 0..255、radius 钳 0..14，并推导 `radiusCard=0.8r`、`radiusButton=radiusInput=0.6r`），
   切 `g.theme`，`ApplyTheme()`，美化开关翻转时再 `ApplyBeautify()`。一个字节都不落盘。
2. **提交** `ReplaySettings()`：`LoadSettings()`（含 tune→kThemes）+ `LoadWebRules()` +
   `LoadGroupRules()` + `LoadExcludePaths()`（含归一化镜像重建）+ 刷新 `g.fdjEnabled` +
   作废窗口枚举缓存 + `ApplyTheme()`/`ApplyBeautify()` + 可见时 `LayoutAndRepaint()`，
   最后 `TakeSettingsSnapshot()` 把新状态当新的回退基线。
3. **回退** `RestoreSettingsSnapshot()`：`ApplySettingsToRuntime(sLiveSaved)` + 主题索引 +
   美化 + `RestoreThemeTune()` + `ApplyTheme()`。触发点有三个：`MsgCancel`、`WM_APP_SETTINGS_GONE`
   （进程结束且未提交）、主进程 `WM_DESTROY` 前的清理。

「打开时全量快照 → 保存时全量写回」的语义保持不变（契约 R1）：FLTK 侧只有底部一个「保存」按钮，
`g_s` 是跨 Tab 常驻的同一份 `ft::Settings`，切 Tab 不写盘。

## 4. 与契约的差异（都是有意为之）

| 契约条目 | 现在的做法 |
|----------|------------|
| R4 回车=保存 | 不再在消息循环全局拦截。sidecar 用 `Fl::add_handler(OnEnterKey)`：焦点在四个多行编辑器之一时返回 0（Enter 正常换行），其余情况调 `onSave`。走全局 handler 而不是按钮 shortcut，因为 FLTK 的快捷键派发顺序不保证编辑器先收到 |
| R5 X 关闭不回退 | 改为**回退**。`WM_CLOSE` → 已提交发 `MsgClosed`，未提交发 `MsgCancel`，主进程按快照恢复内存态 |
| R6 `AntiGhost` | 从主进程彻底移除，退化为 sidecar 自己窗口上的 `WS_EX_COMPOSITED` 开关（`ApplyGhostStyle()`），键名与读写仍走 `settings_model` |
| R9 显示值≠存储值 | `ft::LocateOption()`：注册表里的非法档位值（如 3000）作为额外一项插进候选表并定位到它，UI 不说谎 |
| R10 注释/重复行在打开时丢失 | 原样保留（网页规则、一键组仍由解析后重序列化回显） |
| R11 排除路径无法清空 | 原样保留（解析为空则沿用旧值） |
| R12 透明度只对弹窗有效 | 保留滑块与措辞，不做「无效区间」提示 |
| §10 计数来源 | 权重/自学习条数以主进程为准（`MsgStats`），排除项条数由 sidecar 本地解析注册表文本统计 |
| §10 下载归属 | 截图工具下载放在 sidecar（纯 WinINet，不引 urlmon），进度用 `Fl::awake` 回 UI 线程，成功后 `MsgCaptureInstalled` |

## 5. FLTK 1.4.5 落地要点（踩过的坑）

- **颜色只有索引槽**：1.4 没有 `FL_TEXT_COLOR`/`FL_LABEL_COLOR`/`FL_FRAME_COLOR`/`Fl::color()`/
  `Fl::background(i,r,g,b)`。主题派生的次级文字/分隔线/强调色各占一个自由槽
  （`FL_FREE_COLOR+8/+9/+10`），换主题只重写槽内容，已建控件自动跟随，不需要遍历 `labelcolor`。
  主窗渐变端点另存 `int[3]`（`Fl_Color` 无法反查 RGB）。
- 可见性只有 `show()/hide()`（无 `visible(bool)`）；`Fl::discard()` 不存在 → `QuitApp()` 藏窗，
  事件循环写成 `while (Fl::wait() > 0 && !g_quit)`；无 `raw_window()` → `fl_xid()`（`<FL/platform.H>`）。
- **`Fl_Window::show(argc, argv)` 会无条件读 `argv[0]` 当窗口类名**（`Fl_arg.cxx` 的
  `fl_filename_name(argv[0])`），即使 `argc==0` 也不跳过。sidecar 没有 argv，早期写成
  `show(0, nullptr)` → 进程起来就在 `show()` 里 0xC0000005 退出，表现为「托盘点设置没反应」。
  现在用无参 `Fl_Window::show()`。
- `Fl_Group::insert` 对「同下标重复添加」是 no-op，所以重复 `add()` 安全。
- **子控件坐标一律相对「窗口」，不是相对所属 `Fl_Group`**：`Fl_Widget::x()` 的注释写的就是
  "the x position relative to the window"，`Fl_Group::draw_child()`/`handle()` 也直接拿 `widget.x()`
  跟窗口坐标比，**组不平移绘图原点**（官方自证：`vendor/fltk-1.4.5/test/coordinates.cxx` 每个框都标了
  "x, y relative to main window"）。1.1.16.10 那版「布局全乱」就是踩在这里——内容面板
  `Fl_Group(156, 8, 428, 480)` 里的子控件按面板内坐标 `x=0` 创建，于是全部画到窗口左缘、压住侧栏。
  现在的口径：**Builder 一律按「面板内坐标」书写，偏移只加在控件工厂里**
  （`AddLabel/AddCheck/AddChoice/AddInput/AddMultiline/AddButton/AddSlider/AddThemeList`
  内部统一 `kContentX + x, kContentY + y`），工厂外不许裸 `new` 内容控件。
- `fl_rectf(x, y, w, h, r, g, b)`（以及 `Fl_Color` 那几个重载）**返回时当前颜色就变成刚画的那个**，
  `FL/fl_draw.H` 里以 `\warning` 明写。主题列表每行先画三个色块再画名字，等于用「该主题自己的 bg」
  画字 → 深色款的名字像素级不可见（实测：8 行里只有 2 个浅色款有 170+ 个亮字像素，其余 0 个）。
  画字前必须重新 `fl_color(...)`。
- 换行说明文字（`FL_ALIGN_WRAP`）不加 `FL_ALIGN_TOP` 时，FLTK 会把整段在盒子里**垂直居中**，
  看起来像飘在半空（权重/自学习页的说明就是这样）。`AddHint()` 已固定加 `FL_ALIGN_TOP`。
- `hide()` 不回收空间：Shell 页「自定义路径/参数」两行隐藏后，下面各行会留出一条 60px 空白带。
  所以那页把下方各行登记成 `ShellRow{w, x, yCustom, yPlain}`，`ApplyShellLayout()` 按 `shellType==3`
  整体上下重排（`Fl_Group::show()` 在 1.4.5 里**不会**把子控件一起 show，所以隐藏态能跨 Tab 保持）。
- 侧栏选中态不用自己画：`Fl_Button::draw()` 在 `value()` 为真时用 `down_box()` 平涂
  `selection_color()`，并按背景自动取对比色。所以 `box(FL_NO_BOX)+down_box(FL_FLAT_BOX)`
  就是「未选中纯文字 / 选中一条高亮带」。
- 标签指针不被复制：所有静态文本经 `CL()` 进 `std::list<std::string>` 常驻池。
- 静态库 /MT（`CMAKE_MSVC_RUNTIME_LIBRARY`，且 `FLTK_MSVC_RUNTIME_DLL=OFF`，否则 options.cmake 会静默改回 /MD）。
- FLTK 不带 comctl6 清单，exe 必须自己 `#pragma comment(linker, "/manifestdependency:...")`，
  否则控件退回 Win95 观感。
- CMake 4.x 需要 `CMAKE_POLICY_VERSION_MINIMUM=3.5` 才能 configure FLTK 源码。

## 6. 构建与部署

- **前置（一次性）**：`tools\build-fltk.bat` 把 FLTK 预编译到 `vendor/fltk-install`。
  未就绪时 CMake 打印 `FLTK 未就绪…` 并**跳过** `flowtary-settings` 目标，主程序不受影响。
- `make build` / `just build` 会连带构建该目标；CMakeLists 里 `flowtary-settings` 仅在
  `TARGET fltk::fltk` 存在时创建。
- 产物清单已加入 `tools/release_copy.ps1` 与 `tools/deploy_workshop.ps1`；
  两处都是「存在才拷，缺失则 `Write-Warning`」——少了 sidecar，托盘「设置」只能弹一句
  「未找到 flowtary-settings.exe」。
- `.gitignore` 新增 `vendor/fltk-1.4.5/`、`vendor/fltk-build/`、`vendor/fltk-install/`、
  `vendor/fltk-src.tar.gz`、`vendor/slint-cpp-1.18.1/`、`vendor/UltraFastFileSearch/`、
  `build-fltk.log`；并补好了原先被截断的 `phaseX_build.log` 行。
- 主程序体积：646,144 → 550,400 字节（-94KB）；sidecar 930,816 字节（1.1.17.10），静态链接，无额外运行库。

## 7. 验证状态与待办

已验证：两个目标全量编译链接通过，`make build`（x64+x86）跑通，sidecar 内嵌 comctl6 清单确认存在，
主程序不再引用任何已删除的设置窗符号。
1.1.15.10 部署后托盘「设置」点了没反应，用「起进程 → `EnumWindows` 数该 PID 的顶层窗 → 读退出码」
定位到 sidecar 在 `Fl_Window::show(0, nullptr)` 里 0xC0000005 退出（全程没有可见窗口），
按 §5 改成无参 `show()`；1.1.16.10 复测：200 ms 内出现 1 个可见顶层窗、进程稳定存活。

**未验证：界面实际观感与交互。** 本仓库约定不用截屏/模拟点击来点桌面 UI，需要人工启动
`C:\workshop\Flowtary-<版本>\flowtary-<版本>.exe` → 托盘「设置」逐项走一遍：10 个 Tab 的实时预览（主题/透明度/圆角/美化）、
保存后重放、取消与 X 关闭的回退、杀 sidecar 进程后的回退、清空权重/自学习的两步确认、
截图工具下载进度与 `ss` 命令。

### 7.1 布局是怎么在没有人工点测的情况下验的（1.1.17.10）

约定禁的是「点桌面 UI / 截桌面」，不是「问系统要事实」。这一版补了一个**只画自己窗口**的离线自渲染：
临时给 sidecar 加 `--dump-ui <目录>`，把窗挪到屏幕外 `(0,4000)`，逐 Tab `ShowTab → make_current() →
draw() → Fl::flush()`，再 `GetDC` + top-down 24bpp `CreateDIBSection` + `BitBlt(SRCCOPY)` + 原地
BGR→RGB，最后 `fl_write_png()`；同时把控件树几何（窗口子控件 + 每面板子控件的 `x/y/w/h/visible`）
写成 `tree.txt`。`PrintWindow`/`PW_RENDERFULLCONTENT` 对 FLTK 窗只给全黑图，别用。
拿到 13 张图（10 个 Tab + Shell 自定义态 + 浅色主题两张）后逐眼看，可疑处再用 PowerShell
`System.Drawing` 数像素坐实（例如主题名那 8 行的亮字像素计数）——**不靠肉眼定性**。
验完把 `--dump-ui`/`DumpTabPng`/`DumpUi`/`g_dumpDir` 与 `Fl_PNG_Image.H` 一并删除，不进交付物；
要再验就按本节重搭（约 90 行）。

已验证（自渲染口径）：10 个 Tab 内容全部落在 x=156 右侧、无重叠无越界；侧栏选中带、底部按钮行、
两个多行编辑器页、Shell 页自定义/非自定义两种排布、深色与浅色（Ice-Quartz）主题的对比度均正常。

已知遗留：
- 主窗对「透明度」几乎无感（契约 R12），预览时看不出差别属预期。
- 美化开关在主进程侧只影响弹出菜单深浅；设置窗自身的标题栏/圆角由 sidecar 施加。
- `dist\` 里若还是旧版单 exe，升级安装需带上 `flowtary-settings.exe`。
