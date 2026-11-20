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
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app_icons.h"  // ft::apps::AsyncIcons

#ifndef NOMINMAX
#  define NOMINMAX
#endif

// ---- DWM Acrylic（未公开 API）----
enum ACCENT_STATE { ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 };
struct ACCENT_POLICY { DWORD AccentState, AccentFlags, GradientColor, AnimationId; };
struct WINDOWCOMPOSITIONATTRIBDATA { DWORD Attrib; void* pvData; SIZE_T cbData; };
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
static void enable_acrylic(HWND hwnd)
{
    auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    ACCENT_POLICY p {};
    p.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    p.AccentFlags = 2;
    p.GradientColor = 0xAA181218;  // ABGR 深蓝灰
    WINDOWCOMPOSITIONATTRIBDATA d { 19, &p, sizeof(p) };
    fn(hwnd, &d);
}
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

// 一行结果（来自主进程的状态槽）
struct RowView {
    int kind = 0;
    int hotkey = 0;
    std::string title;      // utf8
    std::string sub;        // utf8
    std::string iconPath;   // utf8（图标来源路径，空 = 用文字徽标）
    std::wstring iconWide;
};

struct UiCtx {
    slint::ComponentHandle<MainWindow> ui;
    std::shared_ptr<slint::VectorModel<ResultItem>> model;
    std::vector<RowView> rows;
    std::unordered_map<std::wstring, slint::Image> iconCache;   // 小写路径 -> 图标
    std::unordered_map<std::wstring, bool> iconRequested;        // 小写路径 -> 已排队
    bool centerWake = true;   // 屏幕居中唤醒（跟随鼠标为 false）
    int windowHeight = 300;   // 逻辑像素；按结果行数动态伸缩
    UiCtx() : ui(MainWindow::create()) {}
};
static std::shared_ptr<UiCtx> g_ctx;

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
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR want = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME |
                               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)) | WS_POPUP;
    if (want != style) {
        SetWindowLongPtrW(hwnd, GWL_STYLE, want);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

static void position_ui(HWND hwnd, bool center);

// 窗口高度：与 main.slint 的排版常量对应（70 行区起点 + 行高 + 12 底栏间距 + 34 底栏）
static int compute_window_height(const UiCtx& ctx, bool hasQuery)
{
    if (ctx.rows.empty()) return hasQuery ? 230 : 300;
    int content = 0;
    for (auto& r : ctx.rows)
        content += (r.kind == slintbridge::kRowHint ? 34 : 54);
    content += ((int)ctx.rows.size() - 1) * 2;
    return 70 + content + 12 + 34;
}

static void apply_state(UiCtx& ctx)
{
    if (!g_bridge) return;
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
        r.title = take(h.titleLen);
        r.sub = take(h.subLen);
        r.iconPath = take(h.iconLen);
        r.iconWide = wide_from_utf8(r.iconPath);
        rows.push_back(std::move(r));
    }
    ctx.rows = std::move(rows);
    ctx.centerWake = slot.centerWake != 0;
    int h = compute_window_height(ctx, tl > 0);
    if (h != ctx.windowHeight) {
        ctx.windowHeight = h;
        ctx.ui->set_window_height((float)h);
        if (g_uiActive) {
            slint::Timer::single_shot(std::chrono::milliseconds(30), []() {
                if (!g_ctx || !g_uiActive) return;
                HWND hwnd = g_ctx->ui->window().win32_hwnd();
                remove_native_frame(hwnd);
                apply_rounded_region(hwnd);
                position_ui(hwnd, g_ctx->centerWake);
            });
        }
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

static void show_ui()
{
    if (!g_ctx) return;
    auto& ctx = *g_ctx;
    ctx.ui->set_query(slint::SharedString(""));  // 主进程每次唤起都清空输入
    ctx.ui->set_selected(0);
    HWND hwnd = ctx.ui->window().win32_hwnd();
    remove_native_frame(hwnd);
    apply_rounded_region(hwnd);
    position_ui(hwnd, ctx.centerWake);  // 先摆好位置再显示，避免旧位置闪一下
    ctx.ui->window().show();
    if (hwnd) {
        DWORD myTid = GetCurrentThreadId();
        HWND fg = GetForegroundWindow();
        DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        bool attached = fgTid && fgTid != myTid && AttachThreadInput(myTid, fgTid, TRUE);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        if (attached) AttachThreadInput(myTid, fgTid, FALSE);
    }
    g_uiActive = true;
    slint::Timer::single_shot(std::chrono::milliseconds(40), []() {
        if (!g_ctx) return;
        g_ctx->ui->invoke_focus_input();
        HWND h = g_ctx->ui->window().win32_hwnd();
        remove_native_frame(h);
        apply_rounded_region(h);
        position_ui(h, g_ctx->centerWake);
    });
}

static void hide_ui()
{
    if (!g_ctx) return;
    g_uiActive = false;
    g_ctx->ui->window().hide();
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
        case WM_SYSKEYDOWN: {
            WPARAM vk = wp;
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
        case WM_CLOSE:
            send_req(slintbridge::kReqHide);
            hide_ui();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) {
                if (g_uiActive) {
                    g_uiActive = false;
                    send_req(slintbridge::kReqHide);
                }
            } else {
                g_uiActive = true;
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

    auto ctx = std::make_shared<UiCtx>();
    g_ctx = ctx;
    parse_launch_args();
    ctx->model = std::make_shared<slint::VectorModel<ResultItem>>();
    ctx->ui->set_items(ctx->model);

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
    ctx->ui->on_item_activated([](int idx) {
        send_req(slintbridge::kReqActivate, idx);
    });
    ctx->ui->on_escape_pressed([]() {
        send_req(slintbridge::kReqHide);
        hide_ui();
    });

    // 窗口创建后置顶/圆角/Acrylic
    slint::Timer::single_shot(std::chrono::milliseconds(80), [ctx]() {
        HWND hwnd = ctx->ui->window().win32_hwnd();
        if (!hwnd) return;
        remove_native_frame(hwnd);
        enable_acrylic(hwnd);
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

    ctx->ui->window().show();
    HWND h0 = ctx->ui->window().win32_hwnd();
    remove_native_frame(h0);
    enable_acrylic(h0);
    apply_rounded_region(h0);
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
