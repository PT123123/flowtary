// filedlg_jump.h
//
// Flowtary —— Windows 文件对话框“文件夹原地跳转”增强模块（类 Listary Quick-Switch）
//
// 设计要点
// --------
// 1) 检测（宿主进程内）：用 SetWinEventHook 监听 EVENT_OBJECT_CREATE / DESTROY /
//    SYSTEM_FOREGROUND。这些回调由系统跨进程投递到宿主，因此 32/64 位对话框都能看到。
//    判定“系统标准文件对话框”的依据：窗口类为 #32770，且包含 shell 视图子窗口
//    （旧版 SHELLDLL_DefView，或 Vista+ 的 NamespaceTreeControl / DirectUIHWND 等），
//    据此自动跳过纯自绘的第三方对话框（Electron / Qt / Java 等没有 shell 视图）。
//
// 2) 跳转（必须在对话框所属进程内完成）：COM 接口指针是进程私有的，无法在宿主进程里
//    直接持有目标对话框的 IFileDialog。因此用全局 CBT 钩子 DLL（按位数分别提供 32/64
//    位）注入到对话框所属进程，在进程内子类化对话框，宿主通过 WM_COPYDATA 把目标路径
//    发送到对话框窗口，由 DLL 的子类过程在“对话框自己的进程”里调用原生 COM 接口
//    （IFileDialog::SetFolder / IShellBrowser::BrowseObject）完成原地跳转。
//    全程不模拟键盘、不粘贴剪贴板，仅调用系统对话框公开接口。
//
// 3) 32 位进程内跳转需要 32 位 DLL；64 位宿主无法加载 32 位 DLL，故由配套的 32 位
//    小助手 agent32.exe 负责安装 32 位钩子。程序退出时统一卸载钩子。
//
// 本文件同时被宿主（main 链接）与注入 DLL 包含，只放声明与跨模块共享常量，无链接依赖。

#pragma once
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

// 注册表中的功能开关
static const WCHAR* FDJ_REG_KEY = L"Software\\Flowtary";
static const WCHAR* FDJ_REG_VAL = L"FileDlgJump";   // REG_DWORD：1=开，0=关

// WM_COPYDATA 的 dwData：宿主 -> 对话框所属进程 的“导航请求”
// lpData 为以 NUL 结尾的目标文件夹绝对路径（系统会跨进程拷贝，DLL 端可直接读）
#define FDJ_CD_NAVIGATE 0x464A0001u   // 'FJ' + 1

// 宿主与 DLL 之间约定的注册消息名（用于 DLL 把导航结果回传给宿主做日志）
static const WCHAR* FDJ_MSG_NAVRESULT_NAME = L"Flowtary.FDJ.NavResult";

// 32 位钩子安装助手（agent32.exe）的窗口类名，宿主据此查找并通知退出
static const WCHAR* FDJ_AGENT32_CLASS = L"FlowtaryFDJAgent32";

// ---------------- 宿主侧 API（由 src/filedlg_jump.cpp 实现）----------------

// 初始化：安装窗口事件钩子；加载并安装 32/64 位 CBT 注入 DLL（64 位由本进程直接安装，
// 32 位通过启动 agent32.exe 安装）。hostWnd 为 Flowtary 主窗口，用于接收导航结果回传。
// 返回是否初始化成功（失败不弹窗，静默返回 false）。
BOOL  fdj_init(HWND hostWnd);

// 卸载：移除窗口事件钩子，卸载 CBT 钩子，结束 32 位助手进程。务必在程序退出前调用。
void  fdj_uninit(void);

// 功能开关（读写注册表 HKCU\Software\Flowtary\FileDlgJump，默认开）。
BOOL  fdj_enabled(void);
void  fdj_set_enabled(BOOL on);

// 当前是否存在已识别且仍打开的系统标准文件对话框（不要求它此刻处于前台——
// 因为这是在 Flowtary 自身抢走焦点之后才查询的）。
BOOL  fdj_dialog_open(void);

// 把“当前焦点对话框”原地跳转到 path（不关闭、不模拟输入）。
// 无焦点对话框 / 功能关闭时静默返回 FALSE。
BOOL  fdj_jump_to_folder(const WCHAR* path);

// 辅助热键：把前台资源管理器当前目录同步到焦点对话框。
BOOL  fdj_sync_from_explorer(void);

#ifdef __cplusplus
}  // extern "C"
#endif
