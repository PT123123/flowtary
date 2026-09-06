# Flowtary — Windows 极简全局搜索启动器

Win11 原生轻量全局启动工具（类 Listary / FlowLauncher 功能阉割版）。纯 Win32 + GDI 自绘，
零第三方依赖，C++17 单文件实现（`src/main.cpp` 约 1000 行）。

- **唤醒**：全局热键（Alt+Space，被占用时自动降级 Alt+Q / Ctrl+Alt+Space），居中弹出
  黑底白字圆角输入框；再次按下 / 失焦 / `Esc` / 托盘左键 自动隐藏
- **体积目标**：exe ≤ 1MB，稳态内存 ≤ 10MB，唤醒响应毫秒级
- **交互**：`↑`/`↓` 或鼠标悬停选择，回车或单击执行；输入框为自绘实现（支持中文 IME、
  左右/Home/End 编辑、Ctrl+V/C）

## 指令表

| 输入 | 行为 |
| :--- | :--- |
| `f {关键词}` | Everything 只搜本地**文件**（`file:` 修饰符），回车直接打开 |
| `d {关键词}` | Everything 只搜本地**文件夹**（`folder:` 修饰符），回车在资源管理器打开 |
| `bd {q}` | 百度 `https://www.baidu.com/#ie=UTF-8&wd={q}` |
| `bili {q}` | B站 `https://search.bilibili.com/all?keyword={q}` |
| `xhs {q}` | 小红书 `https://www.xiaohongshu.com/search_result?keyword={q}` |
| `zhihu {q}` | 知乎 `https://www.zhihu.com/search?q={q}` |
| `douban {q}` | 豆瓣 `https://www.douban.com/search?q={q}` |
| `google {q}` | Google `https://www.google.com/search?q={q}` |
| `空格 top` | 置顶/取消置顶当前窗口（切换「唤醒前的前台窗口」置顶，在目标窗口位置弹出轻量提示；需在设置「命令」中开启，默认开） |
| `空格 cmd 命令` | 执行 Shell 命令（等价于原 `> 命令`）；需在设置「命令」中开启，默认开 |
| `空格 w 关键词` | 切换/关闭/结束窗口（等价于原 `< 关键词`）；需在设置「命令」中开启，默认开 |
| `空格 ss` | 截图（调用同目录 ScreenCapture.exe）；需在设置「命令」中开启，默认开 |
| `空格 ss pin` | 截图 + 贴图/标注（截图后直接进入钉图编辑窗口） |
| `空格 ss ocr` | 文字识别（框选区域后 OCR 提取文字，需 ImageReader.exe） |
| 其他任意输入 | 程序搜索（见下），回车启动 |
| **一键组关键字** | 整串精确命中「启动组 / 关闭组」关键字时，结果最前面出现 `一键启动：xxx` / `一键关闭：xxx`，回车即执行（详见「一键」Tab） |

> 上表中的网页跳转规则全部可在「托盘右键 → 设置」的规则编辑器中增删改（见下）。

## 托盘与设置

- **托盘图标**：**启动优先创建托盘**——先注册热键并让托盘图标就位，再把较慢的程序扫描放到
  **工作线程**，因此开机后托盘立刻可用且**始终可响应**。左键单击 = 唤出/隐藏输入框。
  - 右键菜单（就绪后）：`设置`、`刷新缓存`（重新扫描已装软件，弹气泡提示已索引数量）、`退出`
  - **启动未完成时**右键照样弹出菜单（有响应），但只显示灰色的 `正在启动中…` 与 `退出`；
    此时托盘提示文案为 `Flowtary — 正在启动中…`，输入框内搜索显示 `正在启动中…`；
    扫描完成后自动切回 `Flowtary — {热键} 唤出`
  - 扫描在工作线程进行，结果经 `WM_APP_PROGRAMS_READY` 交回主线程接管，不与搜索并发读写
  Explorer 重启后自动恢复图标（监听 `TaskbarCreated`）。
- **结果项右键菜单**（选中某行后右键 / 上下文操作）：`打开`、`打开所在文件夹`、`以管理员模式打开`
  （仅文件 / 程序类结果显示，调用 UAC 提权后以管理员身份启动）、`复制路径`（网页结果为 `复制链接`）。
- **设置窗口**（右键托盘 → 设置，黑暗模式自绘，与主界面同风格，**可拖动边框调整大小**，
  最小 `560×430` 逻辑像素）：
  - 左侧 **Tab 栏**：`常规` / `网页规则` / `主题` / `一键` / `搜索权重`。点击切换，关闭窗口后仍记住上次停留的 Tab；
    侧栏顶部有 `Flowtary` 品牌标题，激活页用主题强调色 + 左侧竖条高亮。
  - `常规` 页：
    - `开机自动启动`：勾选后写 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Flowtary`，
      取消则删除该值
    - `结果项快捷键`：下拉选择结果列表中「每行右侧徽章 + 对应 `Alt+` 快捷打开」的方案，**默认 `Alt + 数字`**：
      - `Alt + 数字（1–9,0）`：按结果优先级自上而下分配 `1`/`2`…`0`，`1` = 列表最高优先级项（主键盘或数字小键盘均可）
      - `Alt + 字母（A–J）`：按结果优先级自上而下分配 `A`/`B`…`J`
      - `关闭`：不显示徽章、不绑定快捷打开
      - **最高优先级**：输入框聚焦时，所选方案独占执行；本软件自带的启动热键
        `Alt+Space`/`Alt+Q`/`Ctrl+Alt+Space` 同时被临时屏蔽（不再误隐藏窗口）。
        其它 `Alt+` 组合（如 `Alt+F4`、`Alt+字母`）不拦截，按系统默认行为处理（例如 `Alt+F4` 仍可关闭程序）。
      - 存 `HKCU\Software\Flowtary\HotkeyMode`（`0`=数字 / `1`=字母 / `2`=关闭），旧 `HotkeyLetters` 键兼容读取。
    - `唤醒位置`：屏幕居中 / 跟随鼠标（存 `HKCU\Software\Flowtary\CenterWake`）
    - `文件对话框跳转`：在系统标准文件对话框中直接用 Flowtary 定位/跳转到某个文件夹（类 Listary
      Quick-Switch），**默认勾选**。勾选状态存 `HKCU\Software\Flowtary\FileDlgJump`（`1`=开 / `0`=关，
      缺省为开）；取消则不再劫持文件对话框。**本项已从托盘右键菜单移除，仅在此设置页调整**
    - `命令` 页（新增）：集中管理输入框命令开关，以下三种命令**均需在前面加一个空格键触发**：
      - `top 命令`：勾选后输入 `空格 top` 回车，对「唤醒前的前台窗口」切换置顶/取消置顶（`WS_EX_TOPMOST`），
        结果在目标窗口位置弹出快速消失的轻量提示「已置顶/取消置顶」；**默认勾选**。存 `HKCU\Software\Flowtary\TopCmd`（`1`=开 / `0`=关）
      - `cmd 命令`：勾选后输入 `空格 cmd 命令` 执行 Shell 命令（等价于原 `> 命令`）；**默认勾选**。存 `HKCU\Software\Flowtary\CmdCmd`
      - `w 命令`：勾选后输入 `空格 w 关键词` 切换/关闭/结束窗口（等价于原 `< 关键词`）；**默认勾选**。存 `HKCU\Software\Flowtary\WinCmd`
  - `网页规则` 页：
    - `网页搜索规则`：多行编辑器，保存所有网页跳转规则，每行一条
      `前缀 空格 链接模板`（`{q}` 为关键词占位符，如 `bing https://www.bing.com/search?q={q}`）；
      `#` 开头为注释，重复前缀取第一条；「恢复默认」一键还原内置规则。
      存 `HKCU\Software\Flowtary\WebRules`，保存后立即生效，回车即保存
  - `一键` 页：**关键字 → 批量启动 / 批量结束进程**，两个多行编辑器
    - `启动组（关键字 → 文件）`：目标是**文件全路径**，执行时逐个 `ShellExecute` 打开
      （工作目录取各自所在目录）
    - `关闭组（关键字 → 进程）`：目标是**进程名**（可带或不带 `.exe`），执行时按进程名
      结束进程（等价 `taskkill /F /IM`，用 Toolhelp 快照 + `TerminateProcess` 实现，**不弹控制台窗口**，
      且不会结束 Flowtary 自身）
    - 每行格式 `关键字 + 空格 + 目标;目标;…`（多个目标用英文分号分隔，路径含空格无妨），
      `#` 开头为注释，重复关键字取第一条
    - 例：启动组 `美化 D:\Tools\a.exe;D:\Tools\b.exe`；关闭组 `关闭多余软件 chrome;msedge`
    - 分别存 `HKCU\Software\Flowtary\GroupLaunch` 与 `GroupKill`（`REG_SZ`）
  - `主题` 页：
    - `启用界面美化`：控制「暗色标题栏 + 圆角窗口 + 强制暗色菜单」三项系统级美化，**默认勾选**；
      取消后窗口标题栏与圆角回归系统默认、菜单回归系统主题（内容配色仍按所选主题绘制）。
      切换即时预览，存 `HKCU\Software\Flowtary\Beautify`
    - `毛玻璃背景`：亚克力/模糊背景，依附于 `启用界面美化`（后者关闭时本项置灰不可选），
      **默认勾选**。实现：设置窗（非分层窗口）走文档化 `DWMWA_SYSTEMBACKDROP_TYPE=3`（Acrylic）；
      主窗为分层窗口（需保留半透明主题），改用未公开 `SetWindowCompositionAttribute` 的
      `ACCENT_ENABLE_BLURBEHIND(3)` + `LWA_COLORKEY` 透明键色，让背景完全透明以透出模糊，
      主题色调由 accent 的 `GradientColor` 提供（透明蓝/星空等各显其色）；失效的
      `ACCENT_ENABLE_ACRYLICBLURBEHIND(4)` 已弃用。开启时 `Paint` 不再不透明铺底，模糊正常透出。
      存 `HKCU\Software\Flowtary\Glass`
    - `主题样式`：黑色简洁 / 透明Mac黑暗 / 透明蓝色 / 星空风格 / 日出浅白
      （存 `HKCU\Software\Flowtary\Theme`）；**美化关闭时该项置灰不可选**（原先位于 `常规` 页，已移入本页）
  - `搜索权重` 页：**点击加权排序**的开关与性能参数（见下「点击加权排序」）
    - `启用点击权重记忆`：总开关，**默认勾选**（存 `HKCU\Software\Flowtary\WeightEnabled`）
    - `写入时机`：每次点击立即写入磁盘 / 延迟 3 秒合并写入磁盘（默认）/ 仅程序退出时写入磁盘
      （存 `WeightFlush`）。延迟合并模式下连续点击只触发一次写盘，退出时兜底落盘
    - `条目上限`：1000 / 5000（默认）/ 20000 / 50000 条（存 `WeightMaxEntries`）。
      内存与磁盘均按此裁剪，超限时优先淘汰权重最低的记录
    - `清空权重数据`：两步确认（3 秒内再点一次才执行）清空全部权重并写空数据文件
    - 页面显示当前已记忆条数；数据文件为 `%APPDATA%\Flowtary\weights.dat`
      （UTF-16 文本，每行 `搜索词 \x1f 文件完整路径 \x1f 权重`）
- **热键**：依次尝试 `Alt+Space` → `Alt+Q` → `Ctrl+Alt+Space`，全部被占用时弹窗提示；
  实际生效的组合显示在托盘提示文案中（本机 Alt+Space 常被 Flow.Launcher 等占用）。
  当输入框已聚焦时，这些本软件自带的全局 `Alt` 热键会被暂时屏蔽（交给「结果项快捷键」独占），不影响输入；
  其它非本软件的 `Alt+` 组合不受影响。
- **外观**：Win11 DWM 圆角 + 灰色描边（旧系统自动回退 GDI 1px 描边），可由设置 `主题` 页的
  `启用界面美化` 开关整体关闭；同页 `毛玻璃背景` 可叠加亚克力模糊。输入框加高至 56
  逻辑像素。
- **图标零资源**：托盘图标与窗口图标都用**系统自带字体现场绘制**——优先
  `Segoe MDL2 Assets` / `Segoe Fluent Icons` 的放大镜字形（`U+E721`，用 `GetGlyphIndices`
  探测码位是否存在），图标字体缺失时回退 `Segoe UI` 粗体字母「F」。**不引入任何 `.ico` 资源**。
  采用 **4× 超采样**：先在 4 倍尺寸上绘制整枚图标（黑圆底 + 白字形），再面积平均缩回目标尺寸，
  避免小尺寸直画导致纤细字形笔画糊成一团（高 DPI 下更明显）。**不写死像素点**：全部尺寸按统一比例缩放，比例 = max(系统 DPI 缩放,
  屏幕物理高度/1080)，高分辨率小屏上按比例放大不会显得过小；换屏唤醒时自动重算，
  字体随比例重建。
- **中文输入法**：组合串与候选窗锚定在光标处（`CFS_FORCE_POSITION` + `CFS_CANDIDATEPOS`，
  客户区坐标），不再跑到屏幕左上角。

- 网页指令：`{q}` 先做 **UTF-8 percent-encoding** 再替换进模板，`ShellExecute("open")`
  调起默认浏览器。
- 程序搜索范围（启动时一次性枚举，之后纯内存匹配）：
  1. 用户开始菜单 `%APPDATA%\Microsoft\Windows\Start Menu\Programs` 下全部 `.lnk`
  2. 系统开始菜单 `C:\ProgramData\Microsoft\Windows\Start Menu\Programs` 下全部 `.lnk`
  3. `C:\Program Files`、`C:\Program Files (x86)` 一级子目录下的 `.exe`
- 匹配排序：**点击权重 >** 精确 > 前缀 > 包含 > 子序列模糊，同级内开始菜单优先于 Program Files；
  最多显示 10 条。`.lnk` 通过 `IShellLink + IPersistFile` 解析目标路径，以其目录作为
  启动工作目录；图标懒加载缓存。
- **点击加权排序**：点击 / 回车 / `Alt+` 快捷键 / 右键「打开」执行某个本地文件、文件夹或程序条目后，
  针对本次输入框中的**有效搜索词**给该条目 +1 权重；相同有效词再次搜索时该条目排序提前，
  不同搜索词之间权重互相独立。规则：
  - 有效词 = 拆分前置命令关键字后的**实际搜索内容**，做标准化（去首尾空格 + 统一小写）作为记录 key；
    `f`/`d` 前缀只对后面的关键词记录（如 `f 报告` 记 `报告`），无前缀时整个输入即有效词
  - 命中不应保存的命令前缀（如 `gg`、`bd` 等网页指令）时，本次点击**完全不记录、不写入存储**
  - 条目以**文件完整绝对路径**为唯一标识，同名文件不混淆
  - Everything（`f`/`d`）结果返回后同样按权重稳定重排（权重相同保持 Everything 原序）

## 构建与运行

```makefile
make build       # 完整构建：x64（主程序/hook）+ x86（hook/agent），产物汇到 build\
make release     # 版本自增 + 完整构建 + 输出到 dist\（可用 OUT= 覆盖输出目录）
make clean       # 清理 CMake 生成的产物
```
需要 VS 2022 / Build Tools 的 C++ 工作负载（x64 + x86）、CMake、GNU make、ninja（需在 PATH 中）；
底层由 CMake 生成 Ninja 构建文件。

```text
build\flowtary.exe   :: 主程序，启动后无主窗口、常驻托盘，可被全局热键唤出
build\evtest.exe     :: Everything IPC 协议冒烟测试工具（控制台）
```

> **热键占用提示**：若 `Alt + Space` 已被其他程序（如 Flow.Launcher）注册，启动时会弹窗
> 提示一次，并自动改用备用热键 `Alt + Q`。本机实测：稳态工作集 0.3~5MB、私有内存约
> 4MB、exe 186KB；`d/f` 走 Everything IPC 实测可用（`evtest.exe file:hosts` 返回 136 条）。
> 注意：全屏独占游戏（如 VALORANT + Vanguard）前台时，热键可能被反作弊拦截或无法抢占
> 焦点，这是系统限制；切回正常桌面后即可正常使用。

运行前提：使用 `d`/`f` 前缀需要 **Everything 1.4+ 正在后台运行**。若未运行，列表会给出
降级项「在 Everything 中搜索：…」，回车则以 `everything.exe -search "file:xxx"` 方式
打开 Everything 窗口完成搜索（`everything.exe` 依次在 Program Files、`%LOCALAPPDATA%`、
exe 同目录及 PATH 中查找）。

## Everything IPC 协议说明（与原方案的差异更正）

实现严格对照 voidtools 官方 SDK 头文件 `ipc/everything_ipc.h`（Everything 1.4.1）：

1. 目标窗口类名 `EVERYTHING_TASKBAR_NOTIFICATION`，`WM_COPYDATA` 的 `dwData = 2`
   （`EVERYTHING_IPC_COPYDATAQUERYW`）——与原方案一致。
2. **原方案中的 `dwData=5`（取结果数）、`10/11`（按索引取名/路径）命令并不存在**。
   官方协议是：`EVERYTHING_IPC_QUERYW` 结构体中自带 `reply_hwnd` 与
   `reply_copydata_message`（由调用方指定），Everything 搜索完成后**异步**把打包好的
   `EVERYTHING_IPC_LISTW` 结果列表通过一条 `WM_COPYDATA` 推回该窗口。本实现即采用该
   异步回复模型（回复 `dwData = 0x46540000 | 查询序号`，防止串话）。
3. `EVERYTHING_IPC_LISTW`/`EVERYTHING_IPC_ITEMW` 均为 `#pragma pack(1)`，
   `filename_offset/path_offset` 相对列表结构起始；`flags & 0x1` 表示文件夹。
4. `file:` / `folder:` 是 Everything 搜索语法中的修饰符，**仅作用于紧随其后的一个词**，
   故多词关键词逐词添加前缀（`d foo bar` → `file:foo file:bar`），引号短语整体传递。
5. 协议验证工具：`build\evtest.exe [查询串]` 可直接打印 IPC 返回结果。

## 目录结构

```
src/main.cpp         全部实现（Everything IPC / 网页指令 / 程序枚举匹配 / 拼音搜索 / 自绘 UI / 热键）
tools/evtest.cpp     Everything IPC 协议测试工具
vendor/ScreenCapture/  截图工具源码（git submodule，xland/ScreenCapture 2.4.25），构建产物 ScreenCapture.exe
vendor/Ling/           GUI 框架源码（git submodule，xland/Ling），OCR 链依赖
vendor/TinyOCR/        OCR 推理引擎源码（git submodule，xland/TinyOCR），基于 onnxruntime
vendor/ImageReader/    OCR 独立进程源码（git submodule，xland/ImageReader 1.0.2），构建产物 ImageReader.exe
vendor/CMakeLists.txt  第三方源码构建定义（不改动 submodule 内部，所有 target 在此声明）
cmake/msvc-include.cmake  MSVC include 路径自动检测（解决 ninja 生成器不传递 SDK 头文件路径的问题）
cmake/wrapper/Util.h   Ling::Util 头文件包装（替换 MSVC 不完全支持的 C++20 template lambda 宏）
CMakeLists.txt    CMake 构建定义（x64 主程序 + x86 hook/agent 双架构，MSVC /O2 /MT Release）
Makefile          Make 驱动入口（make build / make release / make clean）
```

### 截图工具（ScreenCapture）

`ScreenCapture.exe` 由 `vendor/ScreenCapture` 源码编译而来（C++20 + Direct2D/D3D11/DComposition），
与主程序同目录输出，Flowtary 通过 `ShellExecute` 以独立进程调用。

按 Flowtary 的实际需要，构建时**只保留截图 / 贴图 / OCR 三个模式**，
上游的录屏（MP4 + GIF）与长截图模块不参与编译：
`WinVideo.cpp` / `WinLong.cpp` / `ToolVideo.cpp` / `ToolLong.cpp` / `Win/cgif` 被排除，
`App.cpp` 与 `Tray.cpp` 会先生成剔除了这些模块引用的副本再编译——submodule 本身零改动，
裁剪规则集中在 `vendor/CMakeLists.txt`。

连带好处：录屏模块依赖的 ATL（`atlbase.h`）与 MediaFoundation 都不再需要，
本机未安装 ATL 组件也能正常构建。

### OCR 链（ImageReader）

`ImageReader.exe` 由 `vendor/ImageReader` 源码编译而来（C++20 + Ling + TinyOCR + onnxruntime），
与主程序同目录输出，Flowtary 通过 `ShellExecute` 以独立进程调用（`ss ocr` 命令）。

OCR 链的依赖关系：`ImageReader` → `TinyOCR` + `Ling` → `yoga` + `Clipper2`，
第三方依赖通过 vcpkg 的 `x64-windows-static` 三元组提供（opencv、onnxruntime）。

## 已知限制

- 全屏独占程序中热键仍可响应，但焦点抢占可能被系统限制
- 程序索引在启动时构建，安装/卸载新程序需重启 flowtary 生效
- UWP 应用不在搜索范围（按方案仅索引 .lnk 与一级 .exe）
