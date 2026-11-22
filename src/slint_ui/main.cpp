// Flowtary Slint 渲染器 —— 只画界面；搜索逻辑全在主进程（flowtary.exe）里。
//
// 分工（Slint 引擎为默认后，这套分工固定不变）：
//   flowtary.exe       = 「大脑」：搜索管线（程序/文件/网页）、自学习、点击权重、
//                        全局热键、托盘、设置 —— 逻辑只有这一份。
//   flowtary-slint.exe = 「皮」：画界面、收键盘、把用户意图回传大脑。
//   两侧通过 src/slint_bridge.h 的共享内存桥通信（请求环 / 状态槽 / 命令环）。
//   这样切到 Slint 引擎后功能与 GDI 版完全一致（同一份搜索逻辑），零功能倒退。
#include "main.h"
#include "private/slint_timer.h"
#include "slint_bridge.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#pragma comment(lib, "dwmapi.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app_icons.h"  // ft::apps::AsyncIcons

#ifndef NOMINMAX
#  define NOMINMAX
#endif

// ---- DWM Acrylic（未公开 API）----
// 只作用在「面板背板」窗口上：整窗亚克力会在 32px 投影边距里留下硬边矩形，
// 所以模糊交给贴在面板正下方的一层独立窗口，主窗继续画投影与内容。
enum ACCENT_STATE { ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 };
struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attrib; void* pvData; SIZE_T cbData; };
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
static void enable_acrylic(HWND hwnd, unsigned tint = 0x8A181218)
{
    if (!hwnd) return;
    auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    ACCENT_POLICY p {};
    p.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    p.AccentFlags = 2;
    p.GradientColor = tint;  // ABGR：跟随主题底色
    WINDOWCOMPOSITIONATTRIBDATA d { 19, &p, sizeof(p) };
    fn(hwnd, &d);
}

// ---- 窗口圆角区域 ----
static void apply_rounded_region(HWND hwnd)
{
    RECT rc {};
    if (!GetClientRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < 16 || h < 16) return;
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    const int r = int(16.0 * dpi / 96.0 + 0.5);
    // CreateRoundRectRgn 的后两个参数是「直径」，不是半径
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2))
        SetWindowRgn(hwnd, rgn, TRUE);  // takes ownership
}

// ---- 工具 ----
static std::wstring wide_from_utf8(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::wstring lower_wide(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// HICON -> Slint RGBA image（32bpp DIB section 绘制，保留 alpha）
static slint::Image icon_to_image(HICON icon)
{
    if (!icon) return {};
    const int N = 32;
    BITMAPINFO bi {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = N;
    bi.bmiHeader.biHeight = N;   // bottom-up
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { ReleaseDC(nullptr, dc); return {}; }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    memset(bits, 0, N * N * 4);
    DrawIconEx(mem, 0, 0, icon, N, N, 0, nullptr, DI_IMAGE | DI_MASK);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, dc);

    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(N, N);
    auto* px = buf.begin();
    auto* src = static_cast<const uint8_t*>(bits);
    for (int y = 0; y < N; ++y) {
        const uint8_t* row = src + (N - 1 - y) * N * 4;  // 翻转成 top-down
        for (int x = 0; x < N; ++x) {
            const uint8_t* b = row + x * 4;
            *px++ = slint::Rgba8Pixel { b[2], b[1], b[0], b[3] };
        }
    }
    DeleteObject(bmp);
    return slint::Image(std::move(buf));
}

// ---- 桥接全局 ----
static slintbridge::Bridge* g_bridge = nullptr;
static HWND g_brain = nullptr;
static HWND g_ipcWnd = nullptr;
static WNDPROC g_origUiWndProc = nullptr;
static LONG g_lastStateSeq = 0;
static LONG g_lastCmdTail = 0;
static bool g_uiActive = false;
static bool g_brainSeen = false;
static bool g_holdHide = false;  // 主进程自己的对话框开着：失焦不隐藏

// 一行结果（来自主进程的状态槽）
struct RowView {
    int kind = 0;
    int hotkey = 0;
    int usage = 0;          // 历史使用次数（跨词点击权重 + 自学习）
    std::string title;      // utf8
    std::string sub;        // utf8
    std::string iconPath;   // utf8（图标来源路径，空 = 用文字徽标）
    std::wstring iconWide;
};

// 窗口四周留给外投影的透明边距（与 main.slint 的 panel 内缩保持一致）
static constexpr int kUiMargin = 32;
static constexpr int kUiWidth = 744;   // 面板 680 + 两侧投影边距

// ---- 磨砂玻璃背板窗口 ----
// 只覆盖面板矩形（不含投影边距），模糊边界就是面板圆角本身；
// 主窗是逐像素透明的，叠在它上面画投影/高光/内容。
static HWND g_glass = nullptr;
static unsigned g_glassTint = 0x8A181218;  // ABGR，随主题底色更新
static int g_uiRadius = 20;                // 面板圆角（逻辑 px），与 Slint 的 th-radius 同步
static bool g_glassArmed = false;          // 面板定位好并开始淡入后才允许背板出现
static void renderer_trace(const char* tag);  // 诊断日志（定义在文件后半段）

// ---- 面板投影 ----
// Slint 的软件渲染器实测不画 drop-shadow，这里自己生成一张模糊圆角阴影图交给 Slint，
// 比叠描边干净，四角也不会出现分层。
static int g_shadowW = 0, g_shadowH = 0, g_shadowR = 0;
static MainWindow* g_uiRaw = nullptr;

static void box_blur_h(std::vector<float>& a, int w, int h, int r)
{
    if (r <= 0) return;
    std::vector<float> tmp(a.size());
    const float inv = 1.f / (2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* src = a.data() + (size_t)y * w;
        float* dst = tmp.data() + (size_t)y * w;
        float sum = 0.f;
        for (int x = -r; x <= r; ++x) sum += src[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            dst[x] = sum * inv;
            sum += src[(std::min)(x + r + 1, w - 1)] - src[(std::max)(x - r, 0)];
        }
    }
    a.swap(tmp);
}

static void box_blur_v(std::vector<float>& a, int w, int h, int r)
{
    if (r <= 0) return;
    std::vector<float> tmp(a.size());
    const float inv = 1.f / (2 * r + 1);
    for (int x = 0; x < w; ++x) {
        float sum = 0.f;
        for (int y = -r; y <= r; ++y) sum += a[(size_t)std::clamp(y, 0, h - 1) * w + x];
        for (int y = 0; y < h; ++y) {
            tmp[(size_t)y * w + x] = sum * inv;
            sum += a[(size_t)(std::min)(y + r + 1, h - 1) * w + x]
                 - a[(size_t)(std::max)(y - r, 0) * w + x];
        }
    }
    a.swap(tmp);
}

// panelW/H、radius 都是物理像素；画布比面板大 2*pad，向下偏移 off
static void regen_panel_shadow(int panelW, int panelH, int radius, UINT dpi)
{
    if (!g_uiRaw || panelW <= 0 || panelH <= 0) return;
    const int pad = MulDiv(24, (int)dpi, 96);
    const int off = MulDiv(8, (int)dpi, 96);
    const int W = panelW + pad * 2, H = panelH + pad * 2;
    if (W == g_shadowW && H == g_shadowH && radius == g_shadowR) return;
    g_shadowW = W; g_shadowH = H; g_shadowR = radius;

    std::vector<float> mask((size_t)W * H, 0.f);
    std::vector<float> sdf((size_t)W * H, 0.f);
    const float cx = W * 0.5f;
    const float cy = H * 0.5f - (float)off;
    const float hw = panelW * 0.5f, hh = panelH * 0.5f;
    const float r = (float)(std::max)(1, radius);
    for (int y = 0; y < H; ++y) {
        const float py = y + 0.5f;
        float* row = mask.data() + (size_t)y * W;
        float* srow = sdf.data() + (size_t)y * W;
        for (int x = 0; x < W; ++x) {
            const float dx = std::fabs((x + 0.5f) - cx) - (hw - r);
            const float dy = std::fabs(py - cy) - (hh - r);
            const float ox = (std::max)(dx, 0.f), oy = (std::max)(dy, 0.f);
            const float dist = (std::min)((std::max)(dx, dy), 0.f) + std::sqrt(ox * ox + oy * oy) - r;
            srow[x] = dist;
            row[x] = std::clamp(0.5f - dist, 0.f, 1.f);
        }
    }
    // 两层合成：贴边的一层收紧（接触阴影），外面一层铺开（环境阴影）
    std::vector<float> wide = mask, tight = mask;
    const int brWide = (std::max)(2, MulDiv(8, (int)dpi, 96));
    const int brTight = (std::max)(1, MulDiv(2, (int)dpi, 96));
    for (int i = 0; i < 3; ++i) {
        box_blur_h(wide, W, H, brWide);
        box_blur_v(wide, W, H, brWide);
    }
    for (int i = 0; i < 2; ++i) {
        box_blur_h(tight, W, H, brTight);
        box_blur_v(tight, W, H, brTight);
    }

    slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(W, H);
    auto* p = buf.begin();
    for (size_t i = 0; i < mask.size(); ++i) {
        const float v = tight[i] * 0.35f + wide[i] * 0.65f;
        // 只保留面板轮廓之外的阴影：否则整块黑底会透进半透明玻璃，面板看起来发死
        const float outside = std::clamp(sdf[i] + 0.5f, 0.f, 1.f);
        const auto a = (uint8_t)std::clamp(v * 400.f * outside + 0.5f, 0.f, 255.f);
        *p++ = slint::Rgba8Pixel { 0, 0, 0, a };
    }
    g_uiRaw->set_shadow_img(slint::Image(std::move(buf)));
}

static LRESULT CALLBACK GlassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;  // 不刷底色，让 DWM 的亚克力透出来
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;  // 点击穿透，不影响主窗整行交互
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_glass()
{
    if (g_glass) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GlassProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FlowtaryGlass";
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return;
    }
    g_glass = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        L"FlowtaryGlass", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_glass) return;
    enable_acrylic(g_glass, g_glassTint);
    // 形状完全交给 SetWindowRgn。DWM 若再按自己的半径圆角、并沿窗口画 1px 边框，
    // 就会在面板内侧多出一圈弧线/白线（左上角最明显），所以这里禁用。
    const DWORD kDwmwaCornerPref = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
    int pref = 1;                        // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(g_glass, kDwmwaCornerPref, &pref, sizeof(pref));
    const DWORD kDwmwaBorderColor = 34;  // DWMWA_BORDER_COLOR
    COLORREF none = 0xFFFFFFFE;          // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g_glass, kDwmwaBorderColor, &none, sizeof(none));
}

static void set_glass_tint(unsigned tint)
{
    g_glassTint = tint;
    if (g_glass) enable_acrylic(g_glass, tint);
}

static void hide_glass()
{
    if (g_glass) ShowWindow(g_glass, SW_HIDE);
}

// 背板对齐到主窗面板矩形，并保持在主窗正下方（两窗同为置顶时按 z 序紧贴）
static void sync_glass(HWND ui)
{
    ensure_glass();
    if (!g_glass) return;
    if (!ui || !IsWindowVisible(ui)) {
        hide_glass();
        return;
    }
    RECT rc {};
    if (!GetWindowRect(ui, &rc)) return;
    UINT dpi = GetDpiForWindow(ui);
    if (!dpi) dpi = 96;
    const int m = MulDiv(kUiMargin, (int)dpi, 96);
    const int x = rc.left + m, y = rc.top + m;
    const int w = (rc.right - rc.left) - m * 2;
    const int h = (rc.bottom - rc.top) - m * 2;
    if (w < 32 || h < 32) {
        hide_glass();
        return;
    }
    // 背板边缘与面板描边完全重合：不再内缩，避免内侧多出一圈暗边
    const int r = MulDiv(g_uiRadius, (int)dpi, 96);
    regen_panel_shadow(w, h, r, dpi);
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2))
        SetWindowRgn(g_glass, rgn, TRUE);  // takes ownership
    // 没 arm 时只同步几何、保持隐藏：否则唤起瞬间会先看到一块模糊背板从旧位置跳过来
    if (!g_glassArmed) {
        SetWindowPos(g_glass, ui, x, y, w, h, SWP_NOACTIVATE);
        ShowWindow(g_glass, SW_HIDE);
        return;
    }
    SetWindowPos(g_glass, ui, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

struct UiCtx {
    slint::ComponentHandle<MainWindow> ui;
    std::shared_ptr<slint::VectorModel<ResultItem>> model;
    std::shared_ptr<slint::VectorModel<MenuItem>> menuModel;
    std::vector<RowView> rows;
    std::unordered_map<std::wstring, slint::Image> iconCache;   // 小写路径 -> 图标
    std::unordered_map<std::wstring, bool> iconRequested;        // 小写路径 -> 已排队
    bool centerWake = true;   // 屏幕居中唤醒（跟随鼠标为 false）
    int windowHeight = 364;   // 逻辑像素；按结果行数动态伸缩（含投影边距）
    // Slint 自绘右键菜单的状态（不再有原生菜单窗口）
    bool menuOpen = false;
    int menuRow = -1;         // 锚定的结果行
    int menuSel = 0;          // 高亮项
    int menuX = 392, menuY = 70, menuW = 288;
    int menuBaseH = 0;        // 打开菜单前的内容高度
    int menuHeightNow = 0;    // 菜单打开时窗口实际高度
    std::vector<int> menuIds; // 菜单项对应的命令 id
    int aliasPrevH = 0;       // 别名覆盖层打开前的窗口高度（0=未打开）
    UiCtx() : ui(MainWindow::create()) {}
};
static std::shared_ptr<UiCtx> g_ctx;

// ---- 点击窗口外即隐藏 ----
// 只靠 WM_ACTIVATE 失焦不可靠：SetForegroundWindow 被系统拒绝时窗口虽显示但从未激活，
// 之后点击外部不会产生失焦通知。这里挂一个 WH_MOUSE_LL，仅观察按键，不吞事件。
static constexpr UINT kMsgOutsideClick = WM_APP + 0x40;
static HHOOK g_outsideHook = nullptr;

// 判断屏幕坐标是否落在玻璃面板之外（窗口四周的投影边距也算外）
static bool point_outside_panel(HWND h, POINT screenPt)
{
    RECT rc{};
    if (!h || !GetWindowRect(h, &rc)) return false;
    UINT dpi = GetDpiForWindow(h);
    if (!dpi) dpi = 96;
    const int m = MulDiv(kUiMargin, (int)dpi, 96);
    InflateRect(&rc, -m, -m);
    return !PtInRect(&rc, screenPt);
}

static LRESULT CALLBACK OutsideClickProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN || wp == WM_MBUTTONDOWN) &&
        g_ctx && lp) {
        HWND h = g_ctx->ui->window().win32_hwnd();
        // 低级鼠标钩子的 lParam 是 MSLLHOOKSTRUCT*，不是打包坐标；
        // 之前按 LOWORD/HIWORD 解指针值，点哪都被判成“窗口外”，点主窗也会被收掉
        POINT p = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp)->pt;
        if (h && IsWindowVisible(h) && point_outside_panel(h, p))
            PostMessageW(h, kMsgOutsideClick, 0, 0);  // 回到 UI 线程再收尾，避免钩子里重入
    }
    return CallNextHookEx(g_outsideHook, code, wp, lp);
}

static void arm_outside_hook(bool on)
{
    if (on && !g_outsideHook)
        g_outsideHook = SetWindowsHookExW(WH_MOUSE_LL, OutsideClickProc, nullptr, 0);
    else if (!on && g_outsideHook) {
        UnhookWindowsHookEx(g_outsideHook);
        g_outsideHook = nullptr;
    }
}

// 兜底：唤起时若没抢到前台，就设鼠标捕获，窗口外的点击也会回到这里。
static bool g_outsideCapture = false;

static void arm_outside_capture(HWND h, bool on)
{
    if (on && !g_outsideCapture) {
        SetCapture(h);
        g_outsideCapture = (GetCapture() == h);
    } else if (!on && g_outsideCapture) {
        if (GetCapture() == h) ReleaseCapture();
        g_outsideCapture = false;
    }
}

// 顶部 toast（独立无边框工具窗）：点击穿透、屏幕居中、2.2 秒自动消失
// 尺寸与 main.slint 的 ToastWindow 保持一致，用于在 show 之前算好居中位置
static constexpr int kToastW = 360;
static constexpr int kToastH = 56;
static constexpr int kToastRadius = 14;  // 药丸圆角（逻辑像素）

static std::optional<slint::ComponentHandle<ToastWindow>> g_toast;
static WNDPROC g_origToastProc = nullptr;
static int g_toastGen = 0;
static bool g_toastStyled = false;  // 原生窗口已接管样式（首次建窗期间为 false）
static bool g_toastReveal = false;  // 首次建窗完成后需要回来居中显示
static POINT g_menuOpenPt {};       // 右键发生瞬间的光标位置（不是菜单显示时再取）
static int g_menuClickY = -1;       // 右键在窗口逻辑坐标里的 y（用于把菜单第 1 项放到光标下）
static HWND g_toastGlass = nullptr; // toast 背后的亚克力玻璃层（透桌面 + 圆角）
static unsigned g_toastTint = 0x8A221D1C;  // ABGR，跟随菜单主题色

// toast 的玻璃背板与主窗同一套实现：软件渲染器不会把矩形 alpha 合成到桌面，
// 真正的透明必须靠 DWM 亚克力。Slint 那层只画文字/描边，背景保持透明。
static void ensure_toast_glass()
{
    if (g_toastGlass) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = GlassProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FlowtaryToastGlass";
        registered = RegisterClassExW(&wc) != 0;
        if (!registered) return;
    }
    g_toastGlass = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
        L"FlowtaryToastGlass", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!g_toastGlass) return;
    enable_acrylic(g_toastGlass, g_toastTint);
    const DWORD kCornerPref = 33;
    int pref = 1;  // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(g_toastGlass, kCornerPref, &pref, sizeof(pref));
    const DWORD kBorderColor = 34;
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(g_toastGlass, kBorderColor, &none, sizeof(none));
}

static void toast_glass_hide()
{
    if (g_toastGlass) ShowWindow(g_toastGlass, SW_HIDE);
}

// 玻璃层贴在 toast 正下方：hWndInsertAfter=toast 即放到它后面，z 序不会盖住文字
static void toast_glass_sync(HWND toast)
{
    if (!toast || !IsWindowVisible(toast)) {
        toast_glass_hide();
        return;
    }
    ensure_toast_glass();
    if (!g_toastGlass) return;
    RECT rc {};
    if (!GetWindowRect(toast, &rc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    UINT dpi = GetDpiForWindow(toast);
    if (!dpi) dpi = 96;
    const int r = MulDiv(kToastRadius, (int)dpi, 96);
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2))
        SetWindowRgn(g_toastGlass, rgn, TRUE);  // takes ownership
    enable_acrylic(g_toastGlass, g_toastTint);  // 换主题后重新染色
    SetWindowPos(g_toastGlass, toast, rc.left, rc.top, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// 圆角裁剪：软件渲染没有逐像素透明，靠窗口区域把药丸四角裁掉，避免露出直角底
static void toast_region(HWND hwnd)
{
    RECT rc {};
    if (!GetClientRect(hwnd, &rc)) return;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w < kToastRadius * 2 || h < kToastRadius * 2) return;
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    const int r = MulDiv(kToastRadius, (int)dpi, 96);
    // CreateRoundRectRgn 的后两个参数是「直径」，不是半径
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2))
        SetWindowRgn(hwnd, rgn, TRUE);  // takes ownership
}

// winit 显示/隐藏 toast 时会把窗口样式重写成「带标题栏的普通窗口 + APPWINDOW」，
// 于是任务栏、Alt+Tab 里都会冒出一个窗口。与主窗同一套做法：在 STYLECHANGING
// 里改掉「准备写入的样式」，从源头保证工具窗语义。
static LRESULT CALLBACK ToastWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCACTIVATE:
            return TRUE;
        case WM_NCPAINT:
            return 0;
        case 0x00AE:  // WM_NCUAHDRAWCAPTION（主题标题栏绘制）
        case 0x00AF:  // WM_NCUAHDRAWFRAME（主题边框绘制）
            return 0;
        case WM_NCCALCSIZE:
            if (wp) return 0;
            break;
        case WM_SIZE:
            if (wp != SIZE_MINIMIZED) toast_region(hwnd);
            break;
        case WM_SHOWWINDOW:
            if (!wp) toast_glass_hide();  // 自动消失时把背板一起收掉
            break;
        case WM_WINDOWPOSCHANGED:
            if (IsWindowVisible(hwnd)) toast_glass_sync(hwnd);
            break;
        case WM_STYLECHANGING: {
            auto* ss = reinterpret_cast<STYLESTRUCT*>(lp);
            if (wp == GWL_STYLE) {
                ss->styleNew = (ss->styleNew &
                                ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                                  WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
            } else if (wp == GWL_EXSTYLE) {
                ss->styleNew = (ss->styleNew | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                WS_EX_TRANSPARENT) & ~WS_EX_APPWINDOW;
            }
            break;
        }
    }
    return CallWindowProcW(g_origToastProc, hwnd, msg, wp, lp);
}

// 把 toast 窗口钉成无边框工具窗：不进任务栏/Alt+Tab，不抢焦点，点击穿透
static void toast_style_native(HWND hwnd)
{
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR want = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
    if (want != style) SetWindowLongPtrW(hwnd, GWL_STYLE, want);
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR wex = (ex | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT) &
                   ~WS_EX_APPWINDOW;
    if (wex != ex) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, wex);
    int ncPolicy = 1;  // DWMNCRP_DISABLED
    DwmSetWindowAttribute(hwnd, 2, &ncPolicy, sizeof(ncPolicy));
    const DWORD kDwmwaBorderColor = 34;
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &none, sizeof(none));
}

static ft::apps::AsyncIcons g_icons;

// ---- 行样式（无图标时的文字徽标） ----
static const char* glyph_for(int kind)
{
    switch (kind) {
        case slintbridge::kRowProg: return "EXE";
        case slintbridge::kRowFile: return "FILE";
        case slintbridge::kRowFolder: return "DIR";
        case slintbridge::kRowReveal: return "LOC";
        case slintbridge::kRowWeb: return "WEB";
        case slintbridge::kRowHint: return "i";
        case slintbridge::kRowGroup: return "GRP";
        case slintbridge::kRowShell: return ">_";
        case slintbridge::kRowWindow: return "WIN";
        case slintbridge::kRowTop: return "TOP";
        case slintbridge::kRowCapture: return "CAM";
        default: return "";
    }
}
static slint::Color bg_for(int kind)
{
    static const unsigned rgb[][3] = {
        { 43,140,190 }, { 216,90,48 }, { 83,74,183 }, { 29,158,117 },
        { 186,117,23 }, { 153,53,86 }, { 60,110,150 },
    };
    unsigned c[3] = { 90, 100, 118 };
    switch (kind) {
        case slintbridge::kRowProg: c[0]=rgb[0][0]; c[1]=rgb[0][1]; c[2]=rgb[0][2]; break;
        case slintbridge::kRowWeb: c[0]=rgb[2][0]; c[1]=rgb[2][1]; c[2]=rgb[2][2]; break;
        case slintbridge::kRowGroup: c[0]=rgb[4][0]; c[1]=rgb[4][1]; c[2]=rgb[4][2]; break;
        case slintbridge::kRowWindow: c[0]=rgb[3][0]; c[1]=rgb[3][1]; c[2]=rgb[3][2]; break;
        case slintbridge::kRowTop: c[0]=rgb[1][0]; c[1]=rgb[1][1]; c[2]=rgb[1][2]; break;
        case slintbridge::kRowCapture: c[0]=rgb[5][0]; c[1]=rgb[5][1]; c[2]=rgb[5][2]; break;
        case slintbridge::kRowShell: c[0]=rgb[6][0]; c[1]=rgb[6][1]; c[2]=rgb[6][2]; break;
        default: break;
    }
    return slint::Color::from_rgb_uint8(c[0], c[1], c[2]);
}

// ---- 请求（渲染器 → 大脑） ----
static HWND find_brain()
{
    HWND h = FindWindowW(L"FlowtaryLauncher", L"Flowtary");
    if (h) g_brainSeen = true;
    return h;
}

static void send_req(int op, int arg = 0, const std::string& text = {})
{
    if (!g_bridge) return;
    if (!g_brain || !IsWindow(g_brain)) g_brain = find_brain();
    if (!g_brain) return;
    LONG head = InterlockedCompareExchange(&g_bridge->reqHead, 0, 0);
    LONG tail = InterlockedCompareExchange(&g_bridge->reqTail, 0, 0);
    if (head - tail >= (LONG)slintbridge::kReqRingSize) return;  // 环满：丢弃（极端情况）
    slintbridge::Req& r = g_bridge->req[head % slintbridge::kReqRingSize];
    r.op = op;
    r.arg = arg;
    r.seq = (unsigned)head;
    size_t n = std::min<size_t>(text.size(), (size_t)slintbridge::kTextMax - 1);
    r.textLen = (unsigned)n;
    if (n) memcpy(r.text, text.data(), n);
    r.text[n] = 0;
    InterlockedExchange(&g_bridge->reqHead, head + 1);
    PostMessageW(g_brain, slintbridge::kKickToBrain, 0, 0);
}

// ---- 界面更新 ----
static void rebuild_model(UiCtx& ctx)
{
    std::vector<ResultItem> rows;
    rows.reserve(ctx.rows.size());
    for (auto& r : ctx.rows) {
        ResultItem it;
        it.title = slint::SharedString(r.title.c_str());
        it.sub = slint::SharedString(r.sub.c_str());
        if (r.hotkey) {
            char badge[2] = { (char)r.hotkey, 0 };
            it.hotkey = slint::SharedString(badge);
        }
        it.kind = r.kind;
        // 使用次数分档：与 GDI 版一致（0-2 原样，3-9 常用，10-29 高频，>=30 重度），
        // 只对本地程序/文件/文件夹生效，徽章显示「×N」。
        int tier = 0;
        if (r.kind == slintbridge::kRowProg || r.kind == slintbridge::kRowFile ||
            r.kind == slintbridge::kRowFolder) {
            if (r.usage >= 30) tier = 4;
            else if (r.usage >= 10) tier = 3;
            else if (r.usage >= 3) tier = 2;
        }
        it.usage_tier = tier;
        if (tier) {
            std::string badge = "\xC3\x97" + std::to_string(r.usage);  // ×N
            it.usage_badge = slint::SharedString(badge.c_str());
        }
        const char* gl = glyph_for(r.kind);
        it.glyph = slint::SharedString(gl);
        it.icon_bg = bg_for(r.kind);
        auto itc = ctx.iconCache.find(lower_wide(r.iconWide));
        if (itc != ctx.iconCache.end()) it.icon = itc->second;
        rows.push_back(std::move(it));
    }
    ctx.model->set_vector(std::move(rows));
    ctx.ui->set_empty(ctx.rows.empty());
}

// 动态高度下 winit 会重新带上原生标题栏，这里强制回无边框弹出窗
static void remove_native_frame(HWND hwnd)
{
    if (!hwnd) return;
    // 关掉 DWM 的非客户区渲染：即使样式在某一帧里被 winit 改回带标题栏，
    // DWM 也不会把标题栏/边框画出来。这是「激活瞬间闪一下标题栏」的最后一道保险。
    const DWORD kDwmwaNcRenderingPolicy = 2;
    int ncPolicy = 1;  // DWMNCRP_DISABLED
    DwmSetWindowAttribute(hwnd, kDwmwaNcRenderingPolicy, &ncPolicy, sizeof(ncPolicy));
    // Win11 的 1px DWM 边框也去掉，否则会沿窗口/面板顶边画一条白线
    const DWORD kDwmwaBorderColor = 34;
    COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE
    DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &none, sizeof(none));
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR want = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
    if (want != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, want);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// GDI 版启动器是 WS_EX_TOPMOST；winit 创建的窗口默认不是，会被其它窗口盖住
static void apply_topmost(HWND hwnd)
{
    if (!hwnd) return;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// 工具窗：不进任务栏/Alt+Tab（GDI 版一直如此，winit 默认带 WS_EX_APPWINDOW）
static void apply_tool_window(HWND hwnd)
{
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    LONG_PTR want = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
    if (want != ex) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
}

static void position_ui(HWND hwnd, bool center);

// 与 main.slint 结果列表排版对应：y=70 起、行间距 2px、提示行 34px 其余 54px。
// 注意：这里的 70 是「面板内」坐标；面板在窗口里还内缩 kUiMargin，
// 调用方必须先把客户端 y 减掉 kUiMargin 再传进来。
static int row_at_client_y(const UiCtx& ctx, int y)
{
    int top = 70;
    for (int i = 0; i < (int)ctx.rows.size(); ++i) {
        int h = ctx.rows[i].kind == slintbridge::kRowHint ? 34 : 54;
        if (y >= top && y < top + h) return i;
        top += h + 2;
    }
    return -1;
}

// 请求第 idx 行的右键菜单（-1 = 当前选中行）。命令表由主进程写进桥，
// 菜单本身由 Slint 在本窗口内自绘。clickY 是右键发生时的窗口逻辑 y（-1=键盘触发）。
static void request_row_menu(int idx, int clickY = -1)
{
    if (!g_ctx || g_ctx->rows.empty()) return;
    g_menuClickY = clickY;
    GetCursorPos(&g_menuOpenPt);  // 必须在右键这一刻取，等菜单显示再取会晚一拍
    send_req(slintbridge::kReqRowMenu, idx);
}

// 窗口高度：与 main.slint 的排版常量对应（70 行区起点 + 行高 + 12 底栏间距 + 34 底栏 + 上下投影边距）
static int compute_window_height(const UiCtx& ctx, bool hasQuery)
{
    if (ctx.rows.empty()) return hasQuery ? 294 : 364;  // 面板 230 / 300 + 上下投影边距
    int content = 0;
    for (auto& r : ctx.rows)
        content += (r.kind == slintbridge::kRowHint ? 34 : 54);
    content += ((int)ctx.rows.size() - 1) * 2;
    return 70 + content + 12 + 34 + kUiMargin * 2;
}

// 某一行在列表里的顶部坐标（与 main.slint 排版一致）
static int row_top_of(const UiCtx& ctx, int row)
{
    int top = 70;
    for (int i = 0; i < row && i < (int)ctx.rows.size(); ++i)
        top += (ctx.rows[i].kind == slintbridge::kRowHint ? 34 : 54) + 2;
    return top;
}

// Slint 自绘右键菜单：主进程已把命令表写进桥，这里读出来交给界面
static void show_row_menu(UiCtx& ctx)
{
    if (!g_bridge) return;
    LONG published = InterlockedCompareExchange(&g_bridge->menuSeq, 0, 0);
    if (published == 0) return;
    const slintbridge::MenuSlot& m = g_bridge->menu;
    if ((unsigned)published != m.seq) return;  // 读到半截写入，等下一次
    if (m.count <= 0 || ctx.rows.empty()) return;
    const int count = m.count < slintbridge::kMenuMaxItems ? (int)m.count
                                                          : slintbridge::kMenuMaxItems;

    std::vector<MenuItem> items;
    ctx.menuIds.clear();
    items.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto& src = m.items[i];
        MenuItem mi;
        const size_t labelLen = src.labelLen < sizeof(src.label) ? src.labelLen : sizeof(src.label);
        std::string label(src.label, labelLen);
        mi.label = slint::SharedString(label.c_str());
        mi.is_default = (src.flags & 1) != 0;
        items.push_back(std::move(mi));
        ctx.menuIds.push_back(src.id);
    }

    int row = m.row;
    if (row < 0 || row >= (int)ctx.rows.size()) row = 0;
    ctx.menuRow = row;
    ctx.menuSel = 0;
    ctx.menuW = 288;
    ctx.menuBaseH = ctx.menuOpen ? ctx.menuBaseH : ctx.windowHeight;

    const int menuH = count * 32 + 10;  // 与 main.slint 的菜单排版常量一致
    const int panelH = ctx.menuBaseH - kUiMargin * 2;
    // 鼠标右键：把菜单顶边放到点击 y-16，光标正好落在第 1 项中间（不再压到第 2 项）；
    // 键盘（应用键/Shift+F10）触发时没有点击点，退回按行顶定位。
    int y = g_menuClickY >= 0 ? g_menuClickY - 16 : row_top_of(ctx, row) - 4;
    if (y + menuH > panelH - 10) y = panelH - 10 - menuH;
    if (y < 56) y = 56;
    char mdbg[160];
    snprintf(mdbg, sizeof(mdbg), "MenuShow reqRow=%d useRow=%d clickY=%d menuY=%d count=%d",
             m.row, row, g_menuClickY, y + kUiMargin, count);
    renderer_trace(mdbg);
    const int needH = ctx.menuBaseH > y + menuH + 10 + kUiMargin
                          ? ctx.menuBaseH
                          : y + menuH + 10 + kUiMargin;

    ctx.menuX = kUiWidth - kUiMargin - 8 - ctx.menuW;
    ctx.menuY = y + kUiMargin;
    ctx.menuOpen = true;
    ctx.menuHeightNow = needH;
    ctx.menuModel->set_vector(std::move(items));
    ctx.ui->set_menu_x((float)ctx.menuX);
    ctx.ui->set_menu_y((float)ctx.menuY);
    ctx.ui->set_menu_width((float)ctx.menuW);
    ctx.ui->set_menu_selected(0);
    ctx.ui->set_menu_hover_armed(false);  // 等鼠标真的移动才允许悬停改选中
    ctx.ui->set_menu_open(true);
    if (needH != ctx.windowHeight)
        ctx.ui->set_window_height((float)needH);  // 菜单可能比内容高，窗口临时撑大
}

static void close_row_menu(UiCtx& ctx, bool notify)
{
    if (!ctx.menuOpen) return;
    ctx.menuOpen = false;
    ctx.menuIds.clear();
    g_menuClickY = -1;
    ctx.ui->set_menu_open(false);
    ctx.ui->set_menu_hover_armed(false);
    if (ctx.menuHeightNow != ctx.windowHeight)
        ctx.ui->set_window_height((float)ctx.windowHeight);  // 收回临时撑高的部分
    ctx.menuHeightNow = ctx.windowHeight;
    if (notify) send_req(slintbridge::kReqMenuDismiss);
    ctx.ui->invoke_focus_input();
}

static void pick_row_menu(UiCtx& ctx, int idx)
{
    if (!ctx.menuOpen) return;
    if (idx < 0 || idx >= (int)ctx.menuIds.size()) return;
    const int id = ctx.menuIds[idx];
    close_row_menu(ctx, false);
    send_req(slintbridge::kReqMenuPick, id);
}

// 桥里的 COLORREF+alpha（0xAABBGGRR）→ Slint 颜色
// 纯 COLORREF 字段的高 8 位是 0，这里按不透明处理（否则文字会整块透明）
static slint::Color color_from_wire(unsigned c)
{
    uint8_t a = (uint8_t)(c >> 24);
    if (a == 0) a = 255;
    return slint::Color::from_argb_uint8(a, (uint8_t)(c & 0xFF),
                                         (uint8_t)((c >> 8) & 0xFF), (uint8_t)((c >> 16) & 0xFF));
}

// 把主进程下发的主题调色板绑定到窗口属性（换主题/微调立即生效）
static void apply_theme(UiCtx& ctx)
{
    if (!g_bridge) return;
    LONG seq = InterlockedCompareExchange(&g_bridge->themeSeq, 0, 0);
    static LONG lastThemeSeq = 0;
    if (seq == 0 || seq == lastThemeSeq) return;
    lastThemeSeq = seq;
    const slintbridge::ThemeWire& w = g_bridge->theme;
    ctx.ui->set_th_panel_top(color_from_wire(w.panelTop));
    ctx.ui->set_th_panel_bottom(color_from_wire(w.panelBottom));
    ctx.ui->set_th_glow(color_from_wire(w.glow));
    ctx.ui->set_th_menu(color_from_wire(w.menu));
    ctx.ui->set_th_input(color_from_wire(w.input));
    ctx.ui->set_th_text(color_from_wire(w.text));
    ctx.ui->set_th_sub(color_from_wire(w.sub));
    ctx.ui->set_th_accent(color_from_wire(w.accent));
    ctx.ui->set_th_accent2(color_from_wire(w.accent2));
    ctx.ui->set_th_accent_dark(color_from_wire(w.accentDark));
    ctx.ui->set_th_accent_soft(color_from_wire(w.accentSoft));
    ctx.ui->set_th_accent_line(color_from_wire(w.accentLine));
    ctx.ui->set_th_accent_border(color_from_wire(w.accentBorder));
    ctx.ui->set_th_sel_a(color_from_wire(w.selA));
    ctx.ui->set_th_sel_b(color_from_wire(w.selB));
    ctx.ui->set_th_on_accent(color_from_wire(w.onAccent));
    ctx.ui->set_th_radius((float)w.radius);
    if (w.radius > 0) g_uiRadius = (int)w.radius;
    // 亚克力背板染色跟随主题底色：保留 RGB，压低不透明度让背后的模糊透出来
    set_glass_tint((w.panelTop & 0x00FFFFFFu) | 0x8A000000u);
}

static void apply_state(UiCtx& ctx)
{
    if (!g_bridge) return;
    apply_theme(ctx);
    LONG seq = InterlockedCompareExchange(&g_bridge->stateSeq, 0, 0);
    if (seq == g_lastStateSeq) return;
    g_lastStateSeq = seq;
    const slintbridge::StateSlot& slot = g_bridge->state[seq % slintbridge::kStateSlots];
    uint32_t tl = slot.textLen, rl = slot.rowsLen;
    if (tl > (uint32_t)slintbridge::kTextMax) tl = (uint32_t)slintbridge::kTextMax;
    if (rl > (uint32_t)slintbridge::kRowsMax) rl = (uint32_t)slintbridge::kRowsMax;
    const char* rp = slot.payload + tl;
    uint32_t off = 0;
    std::vector<RowView> rows;
    while (off + sizeof(slintbridge::RowHead) <= rl) {
        slintbridge::RowHead h {};
        memcpy(&h, rp + off, sizeof(h));
        off += (uint32_t)sizeof(h);
        auto take = [&](uint32_t n) -> std::string {
            if (n > rl - off) { off = rl; return {}; }
            std::string s(rp + off, rp + off + n);
            off += n;
            return s;
        };
        RowView r;
        r.kind = h.kind;
        r.hotkey = h.hotkey;
        r.usage = h.usage;
        r.title = take(h.titleLen);
        r.sub = take(h.subLen);
        r.iconPath = take(h.iconLen);
        r.iconWide = wide_from_utf8(r.iconPath);
        rows.push_back(std::move(r));
    }
    ctx.rows = std::move(rows);
    ctx.centerWake = slot.centerWake != 0;
    // 高度变化只改尺寸、不重新居中：窗口顶边固定、向下伸展。
    // 之前每次按键都调 position_ui 按屏幕重新居中，整块面板会上下跳，
    // 看起来就像卡顿；圆角区域交给 WM_SIZE 同步更新。
    int h = compute_window_height(ctx, tl > 0);
    if (ctx.ui->get_alias_open()) {
        // 覆盖层有自己的最小高度，先把新高度记下来，关掉覆盖层时再落地
        ctx.windowHeight = h;
    } else if (h != ctx.windowHeight) {
        ctx.windowHeight = h;
        ctx.ui->set_window_height((float)h);
    }
    int sel = slot.selected;
    if (sel < 0 || sel >= (int)ctx.rows.size()) sel = 0;
    ctx.ui->set_selected(sel);
    rebuild_model(ctx);

    // 新出现的图标路径：排队异步抽取
    for (auto& r : ctx.rows) {
        if (r.iconWide.empty()) continue;
        std::wstring key = lower_wide(r.iconWide);
        if (ctx.iconCache.count(key)) continue;
        if (ctx.iconRequested.count(key)) continue;
        ctx.iconRequested[key] = true;
        g_icons.Request(r.iconWide);
    }
}

// 与 GDI 版一致：居中到鼠标所在显示器的工作区，或跟随鼠标（偏移后夹在工作区内）
static void position_ui(HWND hwnd, bool center)
{
    RECT rc{};
    if (!hwnd || !GetWindowRect(hwnd, &rc)) return;
    const int W = rc.right - rc.left, H = rc.bottom - rc.top;
    POINT cur{};
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    const int dx = MulDiv(16, (int)dpi, 96), dy = MulDiv(12, (int)dpi, 96);
    int x, y;
    if (center) {
        x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2;
        y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2;
    } else {
        x = cur.x + dx;
        y = cur.y + dy;
        if (x + W > mi.rcWork.right) x = mi.rcWork.right - W;
        if (y + H > mi.rcWork.bottom) y = mi.rcWork.bottom - H;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
}

// 抢前台：SetForegroundWindow 可能被系统前台锁拒绝，AttachThreadInput 是兼容做法
static bool activate_ui(HWND hwnd)
{
    if (!hwnd) return false;
    if (GetForegroundWindow() == hwnd) return true;

    // 前台锁只认“最近有输入”：先补一次 Alt 轻敲，把前台授权解锁
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_MENU;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_MENU;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));

    DWORD myTid = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    bool attached = fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    if (GetForegroundWindow() != hwnd) SwitchToThisWindow(hwnd, TRUE);  // 最后的兜底

    const bool ok = GetForegroundWindow() == hwnd;
    char buf[192];
    char cls[64] = {0};
    HWND now = GetForegroundWindow();
    if (now) GetClassNameA(now, cls, 63);
    snprintf(buf, sizeof(buf), "Activate: ok=%d fg=%p [%s]", (int)ok, (void*)now, cls);
    renderer_trace(buf);
    return ok;
}

// 唤起后持续补抢前台：系统前台锁释放有延迟，一次失败不代表永远失败
static void retry_activate(int attempt)
{
    if (!g_ctx) return;
    HWND h = g_ctx->ui->window().win32_hwnd();
    if (!h || !IsWindowVisible(h)) return;
    if (GetForegroundWindow() != h) {
        if (!activate_ui(h)) {
            if (attempt < 5)
                slint::Timer::single_shot(std::chrono::milliseconds(180),
                                          [attempt]() { retry_activate(attempt + 1); });
            return;
        }
        arm_outside_capture(h, false);  // 真拿到了前台，撤掉鼠标捕获兜底
    }
    if (!g_uiActive) {
        g_uiActive = true;
        g_ctx->ui->invoke_focus_input();
    }
}

static void show_ui()
{
    if (!g_ctx) return;
    auto& ctx = *g_ctx;
    HWND hwnd = ctx.ui->window().win32_hwnd();
    const bool wasVisible = hwnd && IsWindowVisible(hwnd);
    ctx.ui->set_query(slint::SharedString(""));  // 主进程每次唤起都清空输入
    ctx.ui->set_selected(0);
    ctx.ui->set_alias_open(false);
    if (!wasVisible) {
        ctx.ui->set_revealed(false);  // 重新播放淡入
        g_glassArmed = false;         // 淡入前不亮背板，位置定了再说
    }
    remove_native_frame(hwnd);
    apply_rounded_region(hwnd);
    position_ui(hwnd, ctx.centerWake);  // 先摆好位置再显示，避免旧位置闪一下
    ctx.ui->window().show();
    remove_native_frame(hwnd);  // show 之后 winit 会重写样式，立刻再压一遍
    apply_topmost(hwnd);
    apply_tool_window(hwnd);  // winit 可能在 show 时又把 APPWINDOW 写回来
    sync_glass(hwnd);         // 磨砂背板跟着面板走（在投影边距内不铺模糊）
    g_uiActive = activate_ui(hwnd);
    // 只有真的拿到前台才算「活跃」：SetForegroundWindow 被系统拒绝时窗口虽显示，
    // 键盘仍在前一个窗口，不能把它当成已激活（否则后续失焦逻辑会错乱）。
    arm_outside_hook(true);  // 点击窗口外直接收掉
    if (!g_uiActive) arm_outside_capture(hwnd, true);  // 没抢到前台时用捕获兜底
    slint::Timer::single_shot(std::chrono::milliseconds(40), []() {
        if (!g_ctx) return;
        g_ctx->ui->invoke_focus_input();
        HWND h = g_ctx->ui->window().win32_hwnd();
        remove_native_frame(h);
        apply_rounded_region(h);
        position_ui(h, g_ctx->centerWake);
        // 位置确定后再显形：面板淡入与磨砂背板同一帧出现，不会先跳到别处再回中间
        g_ctx->ui->set_revealed(true);
        g_glassArmed = true;
        sync_glass(h);
    });
    // 设置窗刚关掉 / 别的进程还占着前台时，前台锁可能晚一拍才释放：多补几次
    slint::Timer::single_shot(std::chrono::milliseconds(140), []() { retry_activate(0); });
}

static void hide_ui()
{
    if (!g_ctx) return;
    arm_outside_hook(false);
    arm_outside_capture(g_ctx->ui->window().win32_hwnd(), false);
    g_uiActive = false;
    if (g_ctx->menuOpen) close_row_menu(*g_ctx, false);  // 隐藏时顺手收掉菜单
    if (g_ctx->ui->get_alias_open() || g_ctx->aliasPrevH) {
        g_ctx->ui->set_alias_open(false);
        if (g_ctx->aliasPrevH) {
            g_ctx->ui->set_window_height((float)g_ctx->windowHeight);
            g_ctx->aliasPrevH = 0;
        }
    }
    g_glassArmed = false;
    g_ctx->ui->window().hide();
    hide_glass();
}

// 读取桥里的单段文本槽（别名预填 / toast 文案）
static std::string read_text_slot(unsigned seq, const slintbridge::TextSlot& slot)
{
    if (!seq) return {};
    unsigned n = slot.len;
    if (n > (unsigned)slintbridge::kTextSlotMax) n = (unsigned)slintbridge::kTextSlotMax;
    return std::string(slot.text, slot.text + n);
}

// 别名输入覆盖层：主进程已经把预填名写进桥
static void show_alias_prompt(UiCtx& ctx)
{
    if (!g_bridge) return;
    unsigned seq = (unsigned)InterlockedCompareExchange(&g_bridge->aliasSeq, 0, 0);
    std::string name = read_text_slot(seq, g_bridge->alias);
    if (ctx.menuOpen) close_row_menu(ctx, false);
    ctx.aliasPrevH = ctx.windowHeight;
    if (ctx.windowHeight < 364) ctx.ui->set_window_height(364);  // 覆盖层放得下
    ctx.ui->set_alias_text(slint::SharedString(name.c_str()));
    ctx.ui->set_alias_open(true);
}

// 目标显示器的缩放：toast 要跨显示器居中，必须按「鼠标所在显示器」的 DPI 算尺寸/位置，
// 否则 200% 屏上会被 winit 的 DPI 变更放大一倍，看起来又偏又大。
static UINT monitor_dpi(HMONITOR mon)
{
    static auto getDpi = reinterpret_cast<HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*)>(
        GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"));
    UINT x = 96, y = 96;
    if (getDpi && SUCCEEDED(getDpi(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && x) return x;
    return 96;
}

// 屏幕居中显示：先算好位置和尺寸再 show，避免窗口先出现在默认位置再跳过来
static void toast_present()
{
    if (!g_toast.has_value()) return;
    auto& win = (*g_toast)->window();
    POINT cur {};
    GetCursorPos(&cur);
    MONITORINFO mi {};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(mon, &mi)) {
        const UINT dpi = monitor_dpi(mon);
        // 只定位、不强行改尺寸：窗口逻辑尺寸本来就是 360x56，winit 会按目标显示器
        // 的 DPI 自己缩放；手动设物理尺寸会在跨 DPI 移动时被再放大一次。
        const int W = MulDiv(kToastW, (int)dpi, 96), H = MulDiv(kToastH, (int)dpi, 96);
        slint::PhysicalPosition pos;
        pos.x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2;
        pos.y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2;
        win.set_position(pos);
    }
    win.show();
    HWND h = win.win32_hwnd();
    if (h) {
        toast_style_native(h);
        toast_region(h);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        toast_glass_sync(h);
    }
    const int gen = ++g_toastGen;
    slint::Timer::single_shot(std::chrono::milliseconds(2200), [gen]() {
        if (gen != g_toastGen || !g_toast.has_value()) return;
        (*g_toast)->window().hide();
    });
}

// 首次显示时 winit 的 HWND 是异步建出来的：等它就绪后接管窗口过程与样式。
// 这期间窗口藏在屏幕外，不会出现「带标题栏的普通窗口」在角落闪一帧。
static void toast_bind_native(int attempt)
{
    if (!g_toast.has_value()) return;
    HWND h = (*g_toast)->window().win32_hwnd();
    if (!h || !IsWindow(h)) {
        if (attempt < 60) {
            slint::Timer::single_shot(std::chrono::milliseconds(8),
                                      [attempt]() { toast_bind_native(attempt + 1); });
            return;
        }
    } else {
        if (!g_origToastProc)
            g_origToastProc =
                (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ToastWndProc);
        toast_style_native(h);
        toast_region(h);
    }
    g_toastStyled = true;
    if (g_toastReveal) {
        g_toastReveal = false;
        toast_present();
    }
}

// 顶部 toast：独立工具窗，定位到鼠标所在显示器工作区中央（与原 GDI 版一致）
static void show_toast(const std::string& text)
{
    if (!g_toast.has_value()) {
        g_toast = ToastWindow::create();
    }
    (*g_toast)->set_message(slint::SharedString(text.c_str()));
    if (g_bridge && g_bridge->themeSeq) {
        (*g_toast)->set_th_menu(color_from_wire(g_bridge->theme.menu));
        (*g_toast)->set_th_text(color_from_wire(g_bridge->theme.text));
        // COLORREF 与 DWM 渐变色同为 ABGR 字节序，只换掉高 8 位透明度
        g_toastTint = (g_bridge->theme.menu & 0x00FFFFFFu) | 0x8A000000u;
    }
    if (g_toastStyled) {
        toast_present();
        return;
    }
    if (!g_toastReveal) {
        // 第一次：先在屏幕外把原生窗口建出来（不可见），样式接管后再回来居中显示
        auto& win = (*g_toast)->window();
        slint::PhysicalPosition pos;
        pos.x = -20000;
        pos.y = -20000;
        win.set_position(pos);
        win.show();
        g_toastReveal = true;
        toast_bind_native(0);
    }
}

// 首帧位置由主进程通过命令行传入：常驻渲染器被复用时则用状态槽里的最新值
static void parse_launch_args()
{
    if (!g_ctx) return;
    const WCHAR* cmd = GetCommandLineW();
    if (wcsstr(cmd, L"--mouse")) g_ctx->centerWake = false;
    else if (wcsstr(cmd, L"--center")) g_ctx->centerWake = true;
}

static void on_bridge_update()
{
    if (!g_ctx) return;
    if (g_bridge) {
        LONG head = InterlockedCompareExchange(&g_bridge->cmdHead, 0, 0);
        LONG tail = g_lastCmdTail;
        const int kCmdCap = (int)(sizeof(g_bridge->cmd) / sizeof(g_bridge->cmd[0]));
        while (tail < head) {
            int cmd = g_bridge->cmd[tail % kCmdCap];
            ++tail;
            switch (cmd) {
                case slintbridge::kCmdShow: show_ui(); break;
                case slintbridge::kCmdHide: hide_ui(); break;
                case slintbridge::kCmdQuit: slint::quit_event_loop(); break;
                case slintbridge::kCmdMenuShow: show_row_menu(*g_ctx); break;
                case slintbridge::kCmdHoldShow: g_holdHide = true; break;
                case slintbridge::kCmdReleaseShow: g_holdHide = false; break;
                case slintbridge::kCmdAliasPrompt: show_alias_prompt(*g_ctx); break;
                case slintbridge::kCmdToast: {
                    unsigned seq = (unsigned)InterlockedCompareExchange(&g_bridge->toastSeq, 0, 0);
                    show_toast(read_text_slot(seq, g_bridge->toast));
                    break;
                }
                default: break;
            }
        }
        g_lastCmdTail = tail;
        InterlockedExchange(&g_bridge->cmdTail, tail);
    }
    apply_state(*g_ctx);
}

// ---- IPC 窗口（只收「有更新了」的通知） ----
static LRESULT CALLBACK IpcWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == slintbridge::kKickToRender) {
        slint::invoke_from_event_loop([]() { on_bridge_update(); });
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- Slint 界面窗口的过程（拦 Alt+数字 / 关闭 / 失焦） ----
static LRESULT CALLBACK UiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_NCACTIVATE:
            // 自己从不画非客户区：激活/失焦都直接返回 TRUE，
            // 不让 DefWindowProc 去重绘标题栏（否则激活瞬间会闪一帧）。
            return TRUE;
        case WM_NCPAINT:
            return 0;  // 非客户区不画
        case 0x00AE:  // WM_NCUAHDRAWCAPTION（主题标题栏绘制）
        case 0x00AF:  // WM_NCUAHDRAWFRAME（主题边框绘制）
            return 0;
        case WM_STYLECHANGING:
            // winit 在显示/隐藏/改尺寸时会重新写窗口样式，把 WS_CAPTION 加回来，
            // 用户就会看到原生标题栏闪一下。这里直接改掉「准备写入的样式」，
            // 从源头保证窗口始终是无边框弹出窗。
            if (wp == GWL_STYLE) {
                auto* ss = reinterpret_cast<STYLESTRUCT*>(lp);
                ss->styleNew = (ss->styleNew &
                                ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                                  WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
            } else if (wp == GWL_EXSTYLE) {
                // winit 会在启动后把 WS_EX_APPWINDOW 写回来（窗口进任务栏/Alt+Tab），
                // 这里始终强制工具窗语义，与 GDI 版保持一致。
                auto* ss = reinterpret_cast<STYLESTRUCT*>(lp);
                ss->styleNew = (ss->styleNew | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
            }
            return 0;
        case WM_NCCALCSIZE:
            // 最后一道保险：客户区铺满整窗。即使某个时刻样式里混进了
            // WS_CAPTION，也永远不会画出标题栏/边框。
            if (wp) return 0;
            break;
        case WM_SHOWWINDOW:
            // 每次重新显示都压一遍无边框样式：窗口隐藏期间 winit 可能写过样式
            if (wp) {
                remove_native_frame(hwnd);
                sync_glass(hwnd);
            } else {
                hide_glass();
            }
            break;
        case WM_WINDOWPOSCHANGED:
            // 移动/改高（结果增减、菜单撑高、别名覆盖层）时背板同步贴合面板
            if (IsWindowVisible(hwnd)) sync_glass(hwnd);
            break;
        case WM_SIZE:
            // 动态高度：尺寸一落地就同步更新圆角区域，避免旧区域的残帧
            if (wp != SIZE_MINIMIZED) apply_rounded_region(hwnd);
            break;
        case WM_MOUSEMOVE:
            // 菜单弹出时光标常压在第 2 项上：位移超过 4px 才认定是用户主动悬停
            if (g_ctx && g_ctx->menuOpen) {
                POINT cur {};
                GetCursorPos(&cur);
                const int dx = cur.x - g_menuOpenPt.x, dy = cur.y - g_menuOpenPt.y;
                if (dx * dx + dy * dy > 16) g_ctx->ui->set_menu_hover_armed(true);
            }
            break;
        case WM_KEYDOWN:
            if (g_ctx && g_ctx->menuOpen) {
                // 菜单打开时自己接管上下/回车/Esc（不让 TextInput 吃掉）
                int n = (int)g_ctx->menuIds.size();
                if (wp == VK_UP || wp == VK_DOWN) {
                    if (n > 0) {
                        int s = (g_ctx->menuSel + (wp == VK_UP ? -1 : 1) + n) % n;
                        g_ctx->menuSel = s;
                        g_ctx->ui->set_menu_selected(s);
                    }
                    return 0;
                }
                if (wp == VK_RETURN) { pick_row_menu(*g_ctx, g_ctx->menuSel); return 0; }
                if (wp == VK_ESCAPE) { close_row_menu(*g_ctx, true); return 0; }
            }
            // 应用键：打开当前结果的右键菜单（与 GDI 版一致）
            if (wp == VK_APPS) {
                request_row_menu(-1);
                return 0;
            }
            break;
        case WM_CHAR:
        case WM_SYSCHAR:
            // 菜单打开时不要再用按键改动搜索词（菜单是模态的）
            if (g_ctx && g_ctx->menuOpen) return 0;
            break;
        case WM_RBUTTONUP: {
            if (g_ctx && g_ctx->menuOpen) { close_row_menu(*g_ctx, true); return 0; }
            int y = (int)(short)HIWORD(lp);
            UINT dpi = GetDpiForWindow(hwnd);
            if (!dpi) dpi = 96;
            const int sceneY = MulDiv(y, 96, (int)dpi);  // 客户端像素 → 窗口逻辑坐标
            // 结果列表的 70px 起点是「面板内」坐标，面板四周还有 kUiMargin 投影边距，
            // 不减掉它就会把第 1 行判成第 2 行（32px ≈ 半行）。
            int idx = g_ctx ? row_at_client_y(*g_ctx, sceneY - kUiMargin) : -1;
            char dbg[160];
            snprintf(dbg, sizeof(dbg), "RBtnUp clientY=%d sceneY=%d panelY=%d idx=%d rows=%zu",
                     y, sceneY, sceneY - kUiMargin, idx, g_ctx ? g_ctx->rows.size() : 0);
            renderer_trace(dbg);
            if (idx >= 0) request_row_menu(idx, sceneY);
            return 0;  // 输入框区域不弹系统编辑菜单
        }
        case WM_SYSKEYDOWN: {
            WPARAM vk = wp;
            if (g_ctx && g_ctx->menuOpen) return 0;  // 菜单打开时不响应 Alt 组合
            // Shift+F10：打开当前结果的右键菜单（与 GDI 版一致）
            if (vk == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                request_row_menu(-1);
                return 0;
            }
            bool digit = (vk >= '0' && vk <= '9') || (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9);
            bool letter = (vk >= 'A' && vk <= 'Z') || (vk >= 'a' && vk <= 'z');
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            // 只接管「本软件自己的 Alt+数字/字母」，其它 Alt 组合（Alt+F4 等）交回系统
            if ((digit || letter) && !ctrl) {
                send_req(slintbridge::kReqAltKey, (int)vk);
                return 0;
            }
            break;
        }
        case kMsgOutsideClick:
            // 点击了启动器窗口以外的区域：直接隐藏（别名覆盖层等主进程对话框期间除外）
            if (IsWindowVisible(hwnd) && !g_holdHide) {
                send_req(slintbridge::kReqHide);
                hide_ui();
            }
            return 0;
        case WM_CLOSE:
            send_req(slintbridge::kReqHide);
            hide_ui();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            // 兜底捕获生效时，窗口外的点击也会送到这里
            POINT p{ (LONG)(short)LOWORD(lp), (LONG)(short)HIWORD(lp) };
            ClientToScreen(hwnd, &p);
            if (point_outside_panel(hwnd, p)) {
                if (!g_holdHide) {
                    send_req(slintbridge::kReqHide);
                    hide_ui();
                }
                return 0;
            }
            break;
        }
        case WM_SETFOCUS:
            // 系统焦点真正落到窗口后，再补一次输入框聚焦：
            // 避免 Slint 的内部焦点在激活事件之前被清掉，出现“窗口在前台但打不了字”
            if (g_ctx) {
                slint::Timer::single_shot(std::chrono::milliseconds(0), []() {
                    if (g_ctx) g_ctx->ui->invoke_focus_input();
                });
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (g_uiActive && !g_holdHide) {
                    g_uiActive = false;
                    send_req(slintbridge::kReqHide);
                }
            } else {
                g_uiActive = true;
                arm_outside_capture(hwnd, false);  // 已激活就不再需要捕获
                // 点回来时再压一遍：个别路径下 winit 会把带标题栏的样式写回去，
                // 这里无条件恢复无边框 + 关掉 DWM 非客户区渲染。
                remove_native_frame(hwnd);
                apply_rounded_region(hwnd);
            }
            break;
    }
    return CallWindowProcW(g_origUiWndProc, hwnd, msg, wp, lp);
}

// ---- 共享内存 ----
static void renderer_trace(const char* tag)
{
    char path[MAX_PATH];
    if (!GetEnvironmentVariableA("APPDATA", path, MAX_PATH)) return;
    lstrcatA(path, "\\Flowtary\\trace.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[160];
    int n = wsprintfA(buf, "%s (renderer pid=%lu)\r\n", tag, GetCurrentProcessId());
    DWORD written = 0;
    WriteFile(f, buf, (DWORD)n, &written, nullptr);
    CloseHandle(f);
}

static bool bridge_open()
{
    std::wstring path = slintbridge::BridgeFilePath();
    if (path.empty()) { renderer_trace("SlintBridge: no bridge path"); return false; }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        renderer_trace("SlintBridge: open bridge file failed");
        return false;
    }
    LARGE_INTEGER sz{};
    sz.QuadPart = (LONGLONG)sizeof(slintbridge::Bridge);
    SetFilePointerEx(f, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(f);
    HANDLE h = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0,
                                  (DWORD)sizeof(slintbridge::Bridge), nullptr);
    if (!h) { CloseHandle(f); renderer_trace("SlintBridge: create mapping failed"); return false; }
    g_bridge = (slintbridge::Bridge*)MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0,
                                                   sizeof(slintbridge::Bridge));
    CloseHandle(h);
    CloseHandle(f);
    renderer_trace(g_bridge ? "SlintBridge: renderer mapped" : "SlintBridge: renderer map failed");
    return g_bridge != nullptr;
}

int main()
{
    // 单实例：渲染器只能有一个（否则会多出多个窗口抢输入焦点）
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FlowtarySlintRenderer");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    // 启动器是 2D 小面板，软件光栅化足够；GPU 后端（Skia/D3D）会常驻
    // 近 200MB 私有内存，换成 CPU 渲染后只有 ~12MB。必须在创建组件前选。
    slint::select_backend({ .backend = "winit-software" });

    auto ctx = std::make_shared<UiCtx>();
    g_ctx = ctx;
    g_uiRaw = ctx->ui.operator->();
    parse_launch_args();
    ctx->model = std::make_shared<slint::VectorModel<ResultItem>>();
    ctx->ui->set_items(ctx->model);
    ctx->menuModel = std::make_shared<slint::VectorModel<MenuItem>>();
    ctx->ui->set_menu_items(ctx->menuModel);

    // IPC 窗口：主进程靠类名找到我们
    WNDCLASSEXW wc {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = IpcWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = slintbridge::kIpcClass;
    RegisterClassExW(&wc);
    g_ipcWnd = CreateWindowExW(WS_EX_TOOLWINDOW, slintbridge::kIpcClass, L"flowtary-slint-ipc",
                               WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);

    bridge_open();
    g_brain = find_brain();
    if (g_bridge) {
        // 跳过上一轮残留的命令；新显示由下面的 kReqReady 触发
        g_lastCmdTail = InterlockedCompareExchange(&g_bridge->cmdHead, 0, 0);
        InterlockedExchange(&g_bridge->cmdTail, g_lastCmdTail);
    }

    // 输入 → 大脑
    ctx->ui->on_query_changed([](const slint::SharedString& text) {
        send_req(slintbridge::kReqText, 0, std::string(text.data() ? text.data() : "", text.size()));
    });
    ctx->ui->on_move_selection([](int delta) {
        send_req(slintbridge::kReqNav, delta);
    });
    // 回车：带上 Ctrl/Shift 语义（Ctrl=关闭、Ctrl+Shift=提权，与 GDI 版一致）；
    // 鼠标点击走 item-clicked，永远是普通执行。
    ctx->ui->on_item_activated([](int idx) {
        int flags = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) flags |= slintbridge::kActCtrl;
        if (GetKeyState(VK_SHIFT) & 0x8000) flags |= slintbridge::kActShift;
        int row = idx < 0 ? 0 : (idx & slintbridge::kActRowMask);
        send_req(slintbridge::kReqActivate, row | flags);
    });
    ctx->ui->on_item_clicked([](int idx) {
        int row = idx < 0 ? 0 : (idx & slintbridge::kActRowMask);
        send_req(slintbridge::kReqActivate, row);
    });
    ctx->ui->on_escape_pressed([]() {
        send_req(slintbridge::kReqHide);
        hide_ui();
    });
    ctx->ui->on_clear_query([]() {
        if (!g_ctx) return;
        g_ctx->ui->set_query(slint::SharedString(""));
        send_req(slintbridge::kReqText, 0, std::string());
        g_ctx->ui->invoke_focus_input();
    });
    // Slint 自绘右键菜单
    ctx->ui->on_menu_picked([](int idx) {
        if (g_ctx) pick_row_menu(*g_ctx, idx);
    });
    ctx->ui->on_menu_dismissed([]() {
        if (g_ctx) close_row_menu(*g_ctx, true);
    });
    // 添加别名覆盖层：确认把文本回传大脑，取消直接关掉
    ctx->ui->on_alias_committed([](const slint::SharedString& text) {
        if (g_ctx) {
            g_ctx->ui->set_alias_open(false);
            if (g_ctx->aliasPrevH) {
                g_ctx->ui->set_window_height((float)g_ctx->windowHeight);
                g_ctx->aliasPrevH = 0;
            }
        }
        send_req(slintbridge::kReqAliasCommit, 0,
                 std::string(text.data() ? text.data() : "", text.size()));
        if (g_ctx) g_ctx->ui->invoke_focus_input();
    });
    ctx->ui->on_alias_cancelled([]() {
        if (g_ctx) {
            g_ctx->ui->set_alias_open(false);
            if (g_ctx->aliasPrevH) {
                g_ctx->ui->set_window_height((float)g_ctx->windowHeight);
                g_ctx->aliasPrevH = 0;
            }
        }
        send_req(slintbridge::kReqAliasCancel);
        if (g_ctx) g_ctx->ui->invoke_focus_input();
    });

    // 窗口创建后置顶/圆角
    slint::Timer::single_shot(std::chrono::milliseconds(80), [ctx]() {
        HWND hwnd = ctx->ui->window().win32_hwnd();
        if (!hwnd) return;
        remove_native_frame(hwnd);
        apply_rounded_region(hwnd);
        // 接管窗口过程：Alt+数字 / 关闭 / 失焦
        if (!g_origUiWndProc)
            g_origUiWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)UiWndProc);
    });

    // 图标异步填充：定时把 AsyncIcons 的结果塞进缓存并刷新行
    auto tick = std::make_shared<slint::Timer>();
    tick->start(slint::TimerMode::Repeated, std::chrono::milliseconds(100), [ctx]() {
        auto results = g_icons.Drain();
        bool changed = false;
        for (auto& r : results) {
            slint::Image img = icon_to_image(r.icon.get());
            ctx->iconCache[lower_wide(r.path)] = std::move(img);
            changed = true;
        }
        if (changed) rebuild_model(*ctx);
    });

    // 心跳：主进程没了就自行退出（避免残留一个空壳窗口）
    auto watch = std::make_shared<slint::Timer>();
    watch->start(slint::TimerMode::Repeated, std::chrono::milliseconds(2000), []() {
        if (!g_brainSeen) return;
        if (!g_brain || !IsWindow(g_brain)) {
            slint::quit_event_loop();
        }
    });

    // 首帧先整块透明：等主进程的 Show 指令到位、位置算好后再淡入，
    // 避免窗口先出现在默认位置再跳回屏幕中央
    ctx->ui->set_revealed(false);
    ctx->ui->window().show();
    HWND h0 = ctx->ui->window().win32_hwnd();
    remove_native_frame(h0);
    apply_rounded_region(h0);
    apply_topmost(h0);
    apply_tool_window(h0);
    sync_glass(h0);  // 未 arm：只同步背板几何，不显示
    if (!g_origUiWndProc)
        g_origUiWndProc = (WNDPROC)SetWindowLongPtrW(h0, GWLP_WNDPROC, (LONG_PTR)UiWndProc);
    position_ui(h0, ctx->centerWake);
    g_uiActive = true;
    send_req(slintbridge::kReqReady);
    // 渲染器是常驻「皮」：隐藏窗口后事件循环不能退出，否则下次唤起又要重开进程
    slint::run_event_loop(slint::EventLoopMode::RunUntilQuit);

    tick->stop();
    watch->stop();
    g_icons.Stop();
    if (g_ipcWnd) DestroyWindow(g_ipcWnd);
    if (mutex) CloseHandle(mutex);
    return 0;
}
