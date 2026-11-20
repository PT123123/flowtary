#include "main.h"
#include "private/slint_timer.h"

#ifdef _WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <windows.h>
#    include <cstdio>

// Undocumented DWM composition API — used to enable real Acrylic (see below).
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};
struct ACCENT_POLICY {
    DWORD AccentState;
    DWORD AccentFlags;
    DWORD GradientColor; // ABGR
    DWORD AnimationId;
};
enum WINDOWCOMPOSITIONATTRIB {
    WCA_ACCENT_POLICY = 19,
};
struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    void* pvData;
    DWORD cbData;
};
using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

static void enable_acrylic(HWND hwnd, DWORD abgr)
{
    auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
    if (!fn) return;
    ACCENT_POLICY policy {};
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.AccentFlags = 2; // draw blur behind
    policy.GradientColor = abgr;
    WINDOWCOMPOSITIONATTRIBDATA data { WCA_ACCENT_POLICY, &policy, sizeof(policy) };
    fn(hwnd, &data);
}

static void apply_rounded_region(HWND hwnd)
{
    if (!hwnd) return;
    RECT client {};
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w < 16 || h < 16) return;

    UINT dpi = GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    const int radius = int(16.0 * double(dpi) / 96.0 + 0.5);
    if (HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2)) {
        // SetWindowRgn takes ownership of the region on success.
        SetWindowRgn(hwnd, rgn, TRUE);
    }
}
#endif

int main()
{
    auto ui = MainWindow::create();

#ifdef _WIN32
    // The native HWND isn't created until the event loop has spun up. Schedule the
    // window setup on the event loop so win32_hwnd() is non-null.
    slint::Timer::single_shot(std::chrono::milliseconds(60), [ui]() {
        if (HWND hwnd = ui->window().win32_hwnd()) {
            // Acrylic tint: ABGR 0xAA181218 ~= dark blue-grey, 67% opacity.
            enable_acrylic(hwnd, 0xAA181218);
            apply_rounded_region(hwnd);
            ui->window().request_redraw();
        }
    });
#endif

    ui->run();
    return 0;
}
