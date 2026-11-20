// Flowtary Slint launcher — Slint 只做 UI 呈现，系统层复用现有 Win32 模块。
// 单实例由 GDI 主程序用互斥体 + 拉起本 exe 保证；本 exe 只负责：
//   1) ft::apps::Collect 拿真实程序目录
//   2) 子序列匹配搜索（与 GDI 版 MatchScore 一致）
//   3) HICON -> RGBA 注入 Slint Image
//   4) 回车/点击 ShellExecute 启动，Esc 隐藏
//   5) DWM Acrylic + SetWindowRgn 圆角
#include "main.h"
#include "private/slint_timer.h"

#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_catalog.h"   // ft::apps::Collect
#include "app_icons.h"     // ft::apps::AsyncIcons

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
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, r * 2, r * 2))
        SetWindowRgn(hwnd, rgn, TRUE);  // takes ownership
}

// ---- 工具 ----
static std::wstring lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static bool is_subseq(const std::wstring& s, const std::wstring& q)
{
    size_t i = 0;
    for (wchar_t c : s) if (i < q.size() && c == q[i]) ++i;
    return i == q.size();
}
static int match_score(const std::wstring& name, const std::wstring& q)
{
    if (q.empty()) return 0;
    if (name == q) return 4;
    if (name.size() >= q.size() && wcsncmp(name.c_str(), q.c_str(), q.size()) == 0) return 3;
    if (name.find(q) != std::wstring::npos) return 2;
    if (is_subseq(name, q)) return 1;
    return 0;
}
static std::string wide_to_utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
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

// ---- 数据 ----
struct Program {
    std::wstring name, path, target;  // target 为最终启动目标（lnk 已解析）
    std::wstring lower;
    std::string glyph;
    slint::Color iconBg;
    slint::Image image;
    bool iconRequested = false;
};

static const char* kGlyphs[] = { "AP", "EX", "PR", "WD", "SY", "AP", "DT" };
static slint::Color glyph_color(size_t i)
{
    static const unsigned rgb[][3] = {
        { 43,140,190 }, { 216,90,48 }, { 83,74,183 }, { 29,158,117 },
        { 186,117,23 }, { 153,53,86 }, { 60,110,150 },
    };
    auto c = rgb[i % 7];
    return slint::Color::from_rgb_uint8(c[0], c[1], c[2]);
}

int main()
{
    auto ui = MainWindow::create();

    // 程序目录：后台构建，完成后灌入 model
    auto programs = std::make_shared<std::vector<Program>>();
    auto icons = std::make_shared<ft::apps::AsyncIcons>();
    auto model = std::make_shared<slint::VectorModel<ResultItem>>();
    ui->set_items(model);
    std::atomic<bool> ready { false };

    std::thread([programs, &ready]() {
        auto cat = ft::apps::Collect();
        std::vector<Program> out;
        out.reserve(cat.entries.size());
        for (auto& e : cat.entries) {
            Program p;
            p.name = std::move(e.name);
            p.path = std::move(e.path);
            p.target = e.target.empty() ? p.path : e.target;
            p.lower = lower(p.name);
            size_t i = out.size();
            p.glyph = kGlyphs[i % (sizeof(kGlyphs) / sizeof(kGlyphs[0]))];
            p.iconBg = glyph_color(i);
            out.push_back(std::move(p));
        }
        std::sort(out.begin(), out.end(), [](const Program& a, const Program& b) {
            return a.name < b.name;
        });
        *programs = std::move(out);
        ready.store(true, std::memory_order_release);
    }).detach();

    // 当前展示的项（programs 的下标）
    auto shown = std::make_shared<std::vector<size_t>>();

    auto rebuild = [&](const std::string& queryUtf8) {
        if (!ready.load(std::memory_order_acquire)) return;
        std::wstring q;
        if (!queryUtf8.empty()) {
            int n = MultiByteToWideChar(CP_UTF8, 0, queryUtf8.c_str(), (int)queryUtf8.size(), nullptr, 0);
            q.resize(n);
            MultiByteToWideChar(CP_UTF8, 0, queryUtf8.c_str(), (int)queryUtf8.size(), q.data(), n);
            q = lower(q);
        }
        struct Hit { size_t idx; int score; };
        std::vector<Hit> hits;
        for (size_t i = 0; i < programs->size() && hits.size() < 40; ++i) {
            int sc = q.empty() ? 1 : match_score((*programs)[i].lower, q);
            if (sc > 0) hits.push_back({ i, sc });
        }
        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            return a.score > b.score;  // 同分保持原名序（programs 已按名字排序）
        });
        std::vector<ResultItem> rows;
        shown->clear();
        for (auto& h : hits) {
            auto& p = (*programs)[h.idx];
            ResultItem it;
            auto titleUtf8 = wide_to_utf8(p.name);
            auto subUtf8 = wide_to_utf8(p.path);
            auto hotkey = std::to_string(rows.size() + 1);
            it.title = slint::SharedString(titleUtf8.c_str());
            it.sub = slint::SharedString(subUtf8.c_str());
            it.hotkey = slint::SharedString(hotkey.c_str());
            it.glyph = slint::SharedString(p.glyph.c_str());
            it.icon_bg = p.iconBg;
            it.icon = p.image;
            rows.push_back(std::move(it));
            shown->push_back(h.idx);
        }
        model->set_vector(std::move(rows));
        ui->set_empty(shown->empty());
        ui->set_selected(0);
    };

    ui->on_query_changed([rebuild](const slint::SharedString& text) {
        rebuild(text.data());
    });
    ui->on_move_selection([ui](int delta) {
        int n = (int)ui->get_items()->row_count();
        if (n <= 0) return;
        int cur = ui->get_selected();
        int next = cur + delta;
        if (next < 0) next = n - 1;
        if (next >= n) next = 0;
        ui->set_selected(next);
    });
    MainWindow* rawUi = &*ui;
    ui->on_item_activated([rawUi, shown, programs](int idx) {
        if (idx < 0 || idx >= (int)shown->size()) return;
        const auto& p = (*programs)[(*shown)[idx]];
        std::wstring dirW;
        size_t slash = p.path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dirW = p.path.substr(0, slash);
        ShellExecuteW(nullptr, L"open", p.target.c_str(), nullptr,
                      dirW.empty() ? nullptr : dirW.c_str(), SW_SHOWNORMAL);
        rawUi->hide();
    });
    ui->on_escape_pressed([rawUi]() { rawUi->hide(); });

    // 窗口创建后置顶/圆角/Acrylic + 首屏灌数据
    slint::Timer::single_shot(std::chrono::milliseconds(80), [ui, rebuild, icons, programs, model]() {
        if (HWND hwnd = ui->window().win32_hwnd()) {
            enable_acrylic(hwnd);
            apply_rounded_region(hwnd);
        }
        rebuild("");
    });

    // 图标异步填充：定时把 AsyncIcons 的结果塞进对应 model 行
    auto tick = std::make_shared<slint::Timer>();
    tick->start(slint::TimerMode::Repeated, std::chrono::milliseconds(120), [icons, programs, shown, model]() {
        // 请求当前展示项的图标
        for (size_t row = 0; row < shown->size() && row < model->row_count(); ++row) {
            auto& p = (*programs)[(*shown)[row]];
            if (!p.iconRequested) {
                p.iconRequested = true;
                icons->Request(p.path);
            }
        }
        auto results = icons->Drain();
        if (results.empty()) return;
        for (auto& r : results) {
            slint::Image img = icon_to_image(r.icon.get());
            for (size_t i = 0; i < programs->size(); ++i) {
                auto& p = (*programs)[i];
                if (p.path == r.path) { p.image = img; break; }
            }
        }
        // 刷新已展示行
        for (size_t row = 0; row < shown->size() && row < model->row_count(); ++row) {
            auto& p = (*programs)[(*shown)[row]];
            if (auto item = model->row_data(row)) {
                item->icon = p.image;
                model->set_row_data(row, *item);
            }
        }
    });

    ui->window().show();
    ui->run();
    tick->stop();
    icons->Stop();
    return 0;
}
