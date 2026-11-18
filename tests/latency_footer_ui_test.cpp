// 搜索耗时底栏取证：非秒出（还在等 / 等完但超阈值）才出现底栏并报毫秒；
// pending 实时累计、报齐定格；秒出与空输入保持干净（无底栏、窗口不长高）。
// 驱动生产 Layout()+Paint() 画进 DIB，断言窗口高度增量 + 底栏像素；BMP 落盘供肉眼复核。
#include <windows.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "../src/main.cpp"

// ---- df 后端桩（只画 UI，不查索引） ----
namespace df {
bool WindowsSearchAvailable() { return true; }
std::wstring DiagProbe() { return {}; }
bool FlowtaryIndexAvailable() { return false; }
bool FlowtaryIndexPrewarm() { return false; }
bool WorkshopCatalogAvailable() { return false; }
WorkshopWarmupSummary WorkshopCatalogWarmup(const std::atomic<bool>*) { return {}; }
int WorkshopCatalogQuery(const Query&, std::vector<Hit>& out) { out.clear(); return 0; }
WorkshopScan ScanWorkshopExecutables(const std::wstring&, WorkshopLimits, const std::atomic<bool>*) { WorkshopScan s; s.complete = true; return s; }
std::vector<ProgramEntry> WorkshopProgramEntries(const std::vector<std::wstring>&) { return {}; }
int FlowtaryIndexQuery(const Query&, bool, std::vector<Hit>& out) { out.clear(); return 0; }
int WindowsSearchQuery(const Query&, bool, std::vector<Hit>& out) { out.clear(); return 0; }
}

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct Pixels {
    int w = 0, h = 0;
    std::vector<unsigned char> bgra;
    COLORREF At(int x, int y) const {
        const unsigned char* p = bgra.data() + ((size_t)y * w + x) * 4;
        return RGB(p[2], p[1], p[0]);
    }
    int CountColor(COLORREF c, RECT box) const {
        int n = 0;
        for (int y = box.top; y < box.bottom; ++y)
            for (int x = box.left; x < box.right; ++x)
                if (At(x, y) == c) ++n;
        return n;
    }
};

static Pixels RenderAndGrab(const wchar_t* bmpPath) {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    HGDIOBJ old = SelectObject(dc, bmp);
    Paint(dc);
    Pixels px;
    px.w = W; px.h = H;
    px.bgra.resize((size_t)W * H * 4);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    Check(GetDIBits(screen, bmp, 0, H, px.bgra.data(), &bi, DIB_RGB_COLORS) != 0, "GetDIBits");
    for (size_t i = 3; i < px.bgra.size(); i += 4) px.bgra[i] = 255;
    if (bmpPath) {
        BITMAPFILEHEADER fh{};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + (DWORD)px.bgra.size();
        HANDLE f = CreateFileW(bmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(f != INVALID_HANDLE_VALUE, "open bmp out");
        DWORD wr = 0;
        WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
        WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr);
        WriteFile(f, px.bgra.data(), (DWORD)px.bgra.size(), &wr, nullptr);
        CloseHandle(f);
    }
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return px;
}

static void SeedRows() {
    g.items.clear();
    Row r;
    r.kind = Row::Folder; r.title = L"projects";
    r.sub = L"C:\\Users\\demo\\projects"; r.action = r.sub;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::Folder; r.title = L"workshop";
    r.sub = L"C:\\workshop"; r.action = r.sub;
    g.items.push_back(r);
}

static int ClientH() {
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    return rc.bottom;
}

int main() {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Flowtary.LatencyTest";
    cls.lpfnWndProc = DefWindowProcW;
    Check(RegisterClassW(&cls) != 0, "window class");
    g.hwnd = CreateWindowW(cls.lpszClassName, L"", WS_POPUP, 0, 0, 600, 200,
                           nullptr, nullptr, cls.hInstance, nullptr);
    Check(g.hwnd != nullptr, "hidden probe window");
    g.historyEnabled = false; g.learnEnabled = false; g.startingUp = false;
    g.text = L"trae";
    g.hoverRow = -1; g.pressRow = -1; g.sel = 0;
    SeedRows();
    g.theme = &kThemes[0];
    ApplyTheme(false);
    Layout();  // 先跑一次让 UpdateScale 定好比例，后面高度断言才用同一 scale

    const Theme& t = *g.theme;
    int inputH = S(kBaseInputH), rowH = S(kBaseRowH), pad = S(kBasePad);
    int rows = (int)g.items.size();
    // 窗口高度公式（与 Layout 一致）：输入栏 + 卡片（间距 + 行 + 底部内边距）
    //                              + 卡片到窗口底的留白 + 可选耗时底栏
    int baseH = S(kBaseInputH + kBaseCardGapTop + rows * kBaseRowH +
                  kBaseCardPadBottom + kBaseWinPadBottom);
    int footH = S(kBaseInputH + kBaseCardGapTop + rows * kBaseRowH +
                  kBaseCardPadBottom + kBaseWinPadBottom + kBaseFooterH);

    // ---- 数据臂：pending 实时累计、报齐定格 ----
    g.mode = Mode::Files; g.dfWsDone = false;
    g.searchRoute = ft::search::Route::WindowsSearch;
    g.qSettledMs = -1;
    g.qT0 = GetTickCount64() - 400;
    int m1 = 0, m2 = 0; bool l1 = false, l2 = false;
    Check(LatencyFooter(m1, l1) && l1 && m1 >= 400, "pending slow reports live elapsed");
    Sleep(60);
    Check(LatencyFooter(m2, l2) && l2 && m2 > m1, "live elapsed counts up while pending");
    FinishRefreshLatency();
    Check(g.qSettledMs == -1, "still pending does not freeze total");
    g.dfWsDone = true;
    FinishRefreshLatency();
    Check(g.qSettledMs >= 400, "settled freezes total ms");

    // ---- 像素臂 A：pending 慢 ⇒ 底栏出现、窗口长高、实时色 + 未回层标签 ----
    g.dfWsDone = false; g.qSettledMs = -1; g.qT0 = GetTickCount64() - 400;
    Layout();
    Check(ClientH() == footH, "pending slow grows window by footer");
    Pixels pa = RenderAndGrab(L"latency_pending.bmp");
    RECT fb{0, ClientH() - S(kBaseFooterH), pa.w, ClientH()};
    Check(pa.CountColor(t.accent2, fb) > 5, "pending footer live ms in accent2");
    Check(pa.CountColor(t.textDis, fb) > 5, "pending footer shows pending layer label");

    // ---- 像素臂 B：pending 但还快（<阈值）⇒ 无底栏 ----
    g.qT0 = GetTickCount64();
    Layout();
    Check(ClientH() == baseH, "pending but fast keeps window clean");
    Pixels pb = RenderAndGrab(L"latency_fast.bmp");
    RECT bb{0, baseH - S(6), pb.w, baseH};
    Check(pb.CountColor(t.accent2, bb) == 0, "fast pending has no footer ink");

    // ---- 像素臂 C：报齐但慢 ⇒ 定格总耗时 + 首屏延迟，非实时色 ----
    g.dfWsDone = true; g.qSettledMs = 300; g.qFirstMs = 80;
    Layout();
    Check(ClientH() == footH, "settled slow keeps footer");
    Pixels pc = RenderAndGrab(L"latency_settled.bmp");
    RECT fc{0, ClientH() - S(kBaseFooterH), pc.w, ClientH()};
    Check(pc.CountColor(t.textSec, fc) > 5, "settled footer total ms in sec color");
    Check(pc.CountColor(t.textDis, fc) > 5, "settled footer shows first-paint ms");
    Check(pc.CountColor(t.accent2, fc) == 0, "settled footer not live colored");

    // ---- 像素臂 D：报齐且快 ⇒ 无底栏 ----
    g.qSettledMs = 5;
    Layout();
    Check(ClientH() == baseH, "settled fast keeps window clean");

    // ---- 对照臂：空输入 ⇒ 无论耗时都不显示 footer（生产 Refresh 会清空结果） ----
    // 空态是紧凑输入框（无引导面板），高度回落到一个输入框，底部仍不应有 footer 文字
    g.qSettledMs = 300; g.text = L""; g.items.clear();
    Layout();
    Check(ClientH() == S(kBaseInputH), "empty input compact, no footer");
    Pixels pe = RenderAndGrab(L"latency_control.bmp");
    // 紧凑空态底部不应有 footer 的 accent2 实时色（footer 未显示）
    RECT bottomStrip{0, ClientH() - S(kBaseFooterH), pc.w, ClientH()};
    if (bottomStrip.top < 0) bottomStrip.top = 0;
    Check(pe.CountColor(t.accent2, bottomStrip) == 0, "empty input has no footer ink");
    (void)pad; (void)inputH; (void)rowH;

    { std::lock_guard<std::mutex> lock(s_wsMu); s_wsQuit = true; } s_wsCv.notify_one();
    DestroyWindow(g.hwnd);
    std::cout << "PASS: latency footer (live pending, settled freeze, fast/empty clean)\n";
}