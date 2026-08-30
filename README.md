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
| `d {关键词}` | Everything 只搜本地**文件**（`file:` 修饰符），回车直接打开 |
| `f {关键词}` | Everything 只搜本地**文件夹**（`folder:` 修饰符），回车在资源管理器打开 |
| `bd {q}` | 百度 `https://www.baidu.com/#ie=UTF-8&wd={q}` |
| `bili {q}` | B站 `https://search.bilibili.com/all?keyword={q}` |
| `xhs {q}` | 小红书 `https://www.xiaohongshu.com/search_result?keyword={q}` |
| `zhihu {q}` | 知乎 `https://www.zhihu.com/search?q={q}` |
| `douban {q}` | 豆瓣 `https://www.douban.com/search?q={q}` |
| `google {q}` | Google `https://www.google.com/search?q={q}` |
| 其他任意输入 | 程序搜索（见下），回车启动 |

> 上表中的网页跳转规则全部可在「托盘右键 → 设置」的规则编辑器中增删改（见下）。

## 托盘与设置

- **托盘图标**：启动后常驻任务栏托盘（黑底白点圆形图标）。左键单击 = 唤出/隐藏输入框；
  右键菜单：`设置`、`退出`。Explorer 重启后自动恢复图标（监听 `TaskbarCreated`）。
- **设置窗口**（右键托盘 → 设置，黑暗模式自绘，与主界面同风格）：
  - 左侧 **Tab 栏**：`常规` / `网页规则`。点击切换，`网页搜索规则`（原直接平铺在主页）已收入
    `网页规则` 分页，主页只保留常用项；关闭窗口后仍记住上次停留的 Tab。
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
    - `主题样式`：黑色简洁 / 透明Mac黑暗 / 透明蓝色 / 星空风格 / 日出浅白
      （存 `HKCU\Software\Flowtary\Theme`）
  - `网页规则` 页：
    - `网页搜索规则`：多行编辑器，保存所有网页跳转规则，每行一条
      `前缀 空格 链接模板`（`{q}` 为关键词占位符，如 `bing https://www.bing.com/search?q={q}`）；
      `#` 开头为注释，重复前缀取第一条；「恢复默认」一键还原内置规则。
      存 `HKCU\Software\Flowtary\WebRules`，保存后立即生效，回车即保存
- **热键**：依次尝试 `Alt+Space` → `Alt+Q` → `Ctrl+Alt+Space`，全部被占用时弹窗提示；
  实际生效的组合显示在托盘提示文案中（本机 Alt+Space 常被 Flow.Launcher 等占用）。
  当输入框已聚焦时，这些本软件自带的全局 `Alt` 热键会被暂时屏蔽（交给「结果项快捷键」独占），不影响输入；
  其它非本软件的 `Alt+` 组合不受影响。
- **外观**：Win11 DWM 圆角 + 灰色描边（旧系统自动回退 GDI 1px 描边）；输入框加高至 56
  逻辑像素。**不写死像素点**：全部尺寸按统一比例缩放，比例 = max(系统 DPI 缩放,
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
- 匹配排序：精确 > 前缀 > 包含 > 子序列模糊，同级内开始菜单优先于 Program Files；
  最多显示 10 条。`.lnk` 通过 `IShellLink + IPersistFile` 解析目标路径，以其目录作为
  启动工作目录；图标懒加载缓存。

## 构建与运行

```bat
build.bat        :: 需要安装 VS 2022 / Build Tools 的 C++ 工作负载（x64）
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
src/main.cpp      全部实现（Everything IPC / 网页指令 / 程序枚举匹配 / 自绘 UI / 热键）
tools/evtest.cpp  Everything IPC 协议测试工具
build.bat         MSVC 一键构建（vswhere 定位 VS，cl /O2 /MT Release）
```

## 已知限制

- 全屏独占程序中热键仍可响应，但焦点抢占可能被系统限制
- 程序索引在启动时构建，安装/卸载新程序需重启 flowtary 生效
- UWP 应用不在搜索范围（按方案仅索引 .lnk 与一级 .exe）
