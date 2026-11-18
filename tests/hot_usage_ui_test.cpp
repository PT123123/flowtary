// 使用分档醒目样式取证：种入真实权重/自学习数据，驱动生产 Layout()+Paint() 画进 DIB，
// 像素级断言四档（原样/常用/高频/重度）各自的色条宽度、徽章形态、染色强度，
// 以及同档内不同次数的连续渐变；带清空数据的对照臂证明门认的是使用数据而不是常量。
// BMP 落盘供肉眼复核。
#include <windows.h>
#include <atomic>
#include <iostream>
#include <vector>
#include "../src/main.cpp"

// ---- df 后端桩（与 existing_index_ui_test 同款：本测试只画 UI，不查索引） ----
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

static bool Near(COLORREF a, COLORREF b, int tol) {
    return abs(GetRValue(a) - GetRValue(b)) <= tol &&
           abs(GetGValue(a) - GetGValue(b)) <= tol &&
           abs(GetBValue(a) - GetBValue(b)) <= tol;
}
// 与卡片底色的通道距离：衡量染色强度，用于断言热度单调
static int Dist(COLORREF a, COLORREF b) {
    return abs(GetRValue(a) - GetRValue(b)) + abs(GetGValue(a) - GetGValue(b)) +
           abs(GetBValue(a) - GetBValue(b));
}

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
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    Check(GetDIBits(screen, bmp, 0, H, px.bgra.data(), &bi, DIB_RGB_COLORS) != 0, "GetDIBits");
    // 32 位 DIB 的 alpha 未定义（多为 0），转 PNG 会整幅透明；取证图强制不透明
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

static std::vector<Program> s_progs;

// 6 行：高频程序(15)、同目录低频程序(1)、恰入常用档文件夹(3)、零历史文件(0)、
// 同档更高次数文件夹(25)、重度档文件夹(40)
static void SeedRows() {
    s_progs.clear();
    s_progs.resize(2);  // Program 含 unique_ptr，不可复制，只能默认构造
    s_progs[0].name = L"Trae"; s_progs[0].path = L"C:\\Apps\\Trae\\Trae.exe";
    s_progs[0].startMenu = true; s_progs[0].iconTried = true;
    s_progs[1].name = L"Trae CN"; s_progs[1].path = L"C:\\Apps\\Trae\\TraeCN.exe";
    s_progs[1].startMenu = true; s_progs[1].iconTried = true;
    g.items.clear();
    Row r;
    r.kind = Row::Prog; r.prog = &s_progs[0];
    r.title = L"Trae"; r.sub = s_progs[0].path; r.action = s_progs[0].path;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::Prog; r.prog = &s_progs[1];
    r.title = L"Trae CN"; r.sub = s_progs[1].path; r.action = s_progs[1].path;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::Folder; r.title = L"projects";
    r.sub = L"C:\\Users\\demo\\projects"; r.action = r.sub;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::File; r.title = L"notes.txt";
    r.sub = L"C:\\Users\\demo\\notes.txt"; r.action = r.sub;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::Folder; r.title = L"aura-1.2.25";
    r.sub = L"C:\\workshop\\aura-1.2.25"; r.action = r.sub;
    g.items.push_back(r);
    r = Row{};
    r.kind = Row::Folder; r.title = L"workshop";
    r.sub = L"C:\\workshop"; r.action = r.sub;
    g.items.push_back(r);
}

static void SeedUsage() {
    g.weights.clear(); g.weightCount = 0;
    g.learned.clear(); g.learnCount = 0;
    // Trae：两个搜索词的权重 5+7，再叠自学习 3 次 ⇒ 15（高频档）
    // 注意：usage 档位只看累计次数，last 只影响排序加分，这里统一给当前时间。
    g.weights[L"tra"][L"C:\\Apps\\Trae\\Trae.exe"] = WeightVal(5, NowUnix());
    g.weights[L"trae"][L"C:\\Apps\\Trae\\Trae.exe"] = WeightVal(7, NowUnix());
    LearnVal v; v.count = 3; v.last = NowUnix();
    g.learned[L"trae"][L"C:\\Apps\\Trae\\Trae.exe"] = v;
    // Trae CN：同在开始菜单目录，但只点过 1 次 ⇒ 保持原样
    g.weights[L"trae"][L"C:\\Apps\\Trae\\TraeCN.exe"] = WeightVal(1, NowUnix());
    // projects：恰好进入常用档 3
    LearnVal f; f.count = kHotUsageMin; f.last = NowUnix();
    g.learned[L"proj"][L"C:\\Users\\demo\\projects"] = f;
    // aura：与 Trae 同档但次数更高 ⇒ 同档内连续渐变必须可见
    g.weights[L"au"][L"C:\\workshop\\aura-1.2.25"] = WeightVal(25, NowUnix());
    // workshop：重度档 40
    LearnVal w; w.count = 40; w.last = NowUnix();
    g.learned[L"work"][L"C:\\workshop"] = w;
}

int main() {
    WNDCLASSW cls{};
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"Flowtary.HotUsageTest";
    cls.lpfnWndProc = DefWindowProcW;
    Check(RegisterClassW(&cls) != 0, "window class");
    g.hwnd = CreateWindowW(cls.lpszClassName, L"", WS_POPUP, 0, 0, 600, 200,
                           nullptr, nullptr, cls.hInstance, nullptr);
    Check(g.hwnd != nullptr, "hidden probe window");
    g.historyEnabled = false; g.learnEnabled = false; g.startingUp = false;
    g.text = L"trae";
    g.hoverRow = -1; g.pressRow = -1; g.sel = 0;

    SeedRows();
    SeedUsage();
    g.theme = &kThemes[0];  // 黑色简洁
    ApplyTheme(false);
    Layout();  // 生产路径：回填 usage + 定窗口尺寸

    // ---- 数据臂：跨词求和 + 自学习叠加 + 分档 ----
    Check(g.items[0].usage == 15, "usage sums weights across terms plus learned count");
    Check(g.items[1].usage == 1, "rarely-used item keeps low usage");
    Check(g.items[2].usage == kHotUsageMin, "learned-only item enters first hot tier");
    Check(g.items[3].usage == 0, "never-used item has zero usage");
    Check(g.items[4].usage == 25 && g.items[5].usage == 40, "higher tiers seeded");
    Check(UsageTier(0) == 0 && UsageTier(2) == 0, "tier 0 covers never/rarely used");
    Check(UsageTier(3) == 2 && UsageTier(9) == 2, "tier 2 spans 3-9");
    Check(UsageTier(10) == 3 && UsageTier(29) == 3, "tier 3 spans 10-29");
    Check(UsageTier(30) == 4 && UsageTier(500) == 4, "tier 4 spans 30+");
    Check(UsageHeat(25) > UsageHeat(15) && UsageHeat(15) > UsageHeat(3),
          "heat monotonic within and across tiers");

    // ---- 字体臂：常用加粗字体存在且更宽 ----
    Check(g.fListHot != nullptr, "hot bold font built");
    TEXTMETRICW tm{};
    HDC dc = GetDC(g.hwnd);
    SelectObject(dc, g.fListHot);
    GetTextMetricsW(dc, &tm);
    Check(tm.tmWeight >= FW_BOLD, "hot font is bold");
    SIZE a{}, b{};
    GetTextExtentPoint32W(dc, L"Trae", 4, &a);
    SelectObject(dc, g.fList);
    GetTextExtentPoint32W(dc, L"Trae", 4, &b);
    Check(a.cx > b.cx, "bold title wider than regular");
    ReleaseDC(g.hwnd, dc);

    // ---- 像素臂（暗色主题）----
    Pixels px = RenderAndGrab(L"hot_usage_dark.bmp");
    const Theme& t = *g.theme;
    int inputH = S(kBaseInputH), rowH = S(kBaseRowH), pad = S(kBasePad);
    auto rowY = [&](int i) { return inputH + i * rowH; };
    auto titleOfTh = [&](const Theme& th, int i) {
        int tier = UsageTier(g.items[i].usage);
        float heat = UsageHeat(g.items[i].usage);
        COLORREF base = tier >= 4 ? (th.dark ? th.accent2 : Darken(th.accent, 25)) : th.accent;
        float mix = th.dark ? (0.42f - 0.34f * heat) : (0.30f + 0.40f * heat);
        return Blend(base, th.textBody, mix);
    };
    auto titleOf = [&](int i) { return titleOfTh(t, i); };
    auto tintOf = [&](int i) {
        return Blend(t.bgCard, t.accent, 0.04f + 0.14f * UsageHeat(g.items[i].usage));
    };
    COLORREF badge2 = Blend(t.bgCard, t.accent, 0.20f);
    COLORREF badge3 = Blend(t.bgCard, t.accent, 0.32f);
    COLORREF barMid = Blend(t.accent2, t.accent, 0.5f);
    int barX = S(2);
    auto midY = [&](int i) { return rowY(i) + rowH / 2; };
    auto rightBox = [&](int i) { return RECT{px.w / 2, rowY(i), px.w, rowY(i) + rowH}; };
    auto titleBox = [&](int i) { return RECT{pad + S(24) + S(8), rowY(i), px.w / 2, rowY(i) + rowH}; };

    // 色条宽度随档位：常用 3px / 高频 4px / 重度 5px（逻辑）
    Check(Near(px.At(barX + S(1), midY(2)), barMid, 12), "tier2 bar present");
    Check(px.At(barX + S(3) + S(1), midY(2)) == t.bg, "tier2 bar narrower than tier3");
    Check(Near(px.At(barX + S(3), midY(4)), barMid, 12), "tier3 bar reaches 4px");
    Check(px.At(barX + S(4), midY(4)) == t.bg, "tier3 bar narrower than tier4");
    Check(Near(px.At(barX + S(4), midY(5)), barMid, 12), "tier4 bar reaches 5px");
    Check(Near(px.At(barX + S(1), midY(0)), barMid, 12), "selected hot row keeps bar");
    // 重度档色条外罩软辉光；高频档没有
    int bar4R = barX + S(1) + S(4);
    // 重度档色条外罩软辉光（预乘 alpha 后 = 底色叠 accent2 低 alpha）；高频档没有
    Check(Near(px.At(bar4R + S(1), midY(5)), Blend(t.bg, t.accent2, 46 / 255.f), 3),
          "tier4 bar glow halo");
    Check(px.At(barX + S(1) + S(3) + S(1), midY(4)) == t.bg, "tier3 bar has no glow");
    // 原样行：无色条、无染色
    Check(px.At(barX + S(1), midY(1)) == t.bg, "rare row has no accent bar");
    Check(px.At(barX + S(1), midY(3)) == t.bg, "never-used row has no accent bar");
    Check(px.At(barX + S(1), rowY(1) + S(2)) == t.bg, "rare row keeps original background");

    // 连续热度：同档 15 与 25 染色不同，且强度随次数单调；染色只落在卡片内
    Check(tintOf(0) != tintOf(4), "same tier different counts tint differently");
    int inCardX = pad + S(2);
    Check(px.At(inCardX, rowY(4) + S(2)) == tintOf(4), "tier3 higher count tint rendered");
    Check(px.At(inCardX, rowY(2) + S(2)) == tintOf(2), "tier2 tint rendered");
    // 选中行玻璃胶囊与卡片对齐（iOS 内嵌列表选中态，高亮不再满宽铺出卡片）：
    // 胶囊内采样玻璃色（左浓右淡 + 顶缘 specular）；卡外边距保持窗体底色
    {
        int gw = px.w - 2 * pad;
        int sx = pad + S(24) + S(8) + S(2);  // 文字起点右侧，远离胶囊圆角区
        float ug = (float)(sx - pad) / (float)(gw - 1);
        COLORREF glassBase = Blend(t.selBg, t.accent, 0.28f + 0.20f * (1.0f - ug));
        float vg = 2.0f / (float)(rowH - 1);
        COLORREF glassTop = Blend(glassBase, t.textTitle,
                                  (t.dark ? 0.14f : 0.20f) * (1.0f - vg / 0.22f));
        Check(Near(px.At(sx, rowY(0) + S(2)), glassTop, 2),
              "selected glass pill aligned inside card");
        Check(px.At(S(8), midY(0)) == t.bg, "selection stays inside card (no full-width band)");
    }
    Check(px.At(barX + S(1), rowY(2) + S(2)) == t.bg, "tint stays inside card bounds");
    Check(Dist(tintOf(4), t.bgCard) > Dist(tintOf(0), t.bgCard) &&
          Dist(tintOf(0), t.bgCard) > Dist(tintOf(2), t.bgCard),
          "tint strength monotonic 3 < 15 < 25");
    Check(Dist(tintOf(5), t.bgCard) > Dist(tintOf(4), t.bgCard), "tier4 tint strongest");

    // 标题：每档字芯色存在且互不相同；原样行只有正文色
    for (int i : {0, 2, 4, 5})
        Check(px.CountColor(titleOf(i), titleBox(i)) > 10, "hot title drawn in tier color");
    Check(titleOf(0) != titleOf(4) && titleOf(4) != titleOf(5), "title color varies by heat/tier");
    Check(px.CountColor(titleOf(0), titleBox(1)) == 0, "rare title not accent colored");
    Check(px.CountColor(t.textBody, titleBox(1)) > 10, "rare title keeps body color");
    Check(px.CountColor(t.textBody, titleBox(3)) > 10, "never-used title keeps body color");

    // 徽章形态：常用淡底 / 高频浓底 / 重度实底反白；原样行无任何徽章
    Check(px.CountColor(badge2, rightBox(2)) > 30, "tier2 light badge");
    Check(px.CountColor(badge3, rightBox(0)) > 30, "tier3 stronger badge");
    Check(px.CountColor(badge3, rightBox(4)) > 30, "tier3 badge on second row");
    Check(px.CountColor(t.accent, rightBox(5)) > 30, "tier4 solid accent badge");
    Check(px.CountColor(t.bgCard, rightBox(5)) > 5, "tier4 badge inverted text");
    Check(px.CountColor(t.accent, rightBox(2)) > 3, "tier2 badge tier dot");
    Check(px.CountColor(t.accent, rightBox(4)) > 3, "tier3 badge tier dot");
    // 重度档标题后软辉光（字上方采样 = 染色底叠 accent2 低 alpha）
    Check(Near(px.At(inCardX + S(24) + S(8) + S(2), rowY(5) + S(2)),
               Blend(tintOf(5), t.accent2, 26 / 255.f), 2), "tier4 title halo");
    for (int i : {1, 3}) {
        Check(px.CountColor(badge2, rightBox(i)) == 0, "plain row no tier2 badge");
        Check(px.CountColor(badge3, rightBox(i)) == 0, "plain row no tier3 badge");
        Check(px.CountColor(t.accent, rightBox(i)) == 0, "plain row no tier4 badge");
    }

    // ---- 浅色主题臂 ----
    g.theme = &kThemes[4];  // 日出浅白
    ApplyTheme(false);
    Layout();
    Pixels lp = RenderAndGrab(L"hot_usage_light.bmp");
    const Theme& lt = *g.theme;
    COLORREF lBadge2 = Blend(lt.bgCard, lt.accent, 0.20f);
    COLORREF lBarMid = Blend(lt.accent2, lt.accent, 0.5f);
    Check(Near(lp.At(barX + S(1), midY(2)), lBarMid, 12), "light theme tier2 bar");
    Check(Near(lp.At(barX + S(4), midY(5)), lBarMid, 12), "light theme tier4 bar");
    Check(lp.CountColor(lBadge2, rightBox(2)) > 30, "light theme tier2 badge");
    Check(lp.CountColor(lt.accent, rightBox(5)) > 30, "light theme tier4 solid badge");
    Check(lp.At(barX + S(1), midY(1)) == lt.bg, "light theme rare row untouched");
    Check(lp.At(barX + S(1), rowY(2) + S(2)) == lt.bg, "light theme tint stays inside card");
    for (int i : {0, 2, 4, 5})
        Check(lp.CountColor(titleOfTh(lt, i), titleBox(i)) > 10, "light theme hot title in tier ink");
    Check(Dist(titleOfTh(lt, 5), lt.bgCard) > Dist(titleOfTh(lt, 2), lt.bgCard),
          "light theme heavy title has stronger contrast than light tier");

    // ---- 对照臂：清空全部使用数据 ⇒ 所有醒目样式必须消失 ----
    g.theme = &kThemes[0];
    ApplyTheme(false);
    g.weights.clear(); g.learned.clear();
    Layout();
    for (auto& r : g.items) Check(r.usage == 0, "control arm usage cleared");
    Pixels cp = RenderAndGrab(L"hot_usage_control.bmp");
    {
        int gw = px.w - 2 * pad;
        int sx = pad + S(24) + S(8) + S(2);
        float uc = (float)(sx - pad) / (float)(gw - 1);
        COLORREF glassMid = Blend(t.selBg, t.accent, 0.28f + 0.20f * (1.0f - uc));
        Check(Near(cp.At(sx, midY(0)), glassMid, 2),
              "control: selected row plain glass inside card");
        Check(cp.At(barX + S(1), midY(0)) == t.bg, "control: selection does not spill outside card");
    }
    Check(cp.At(barX + S(1), midY(5)) == t.bg, "control: no bar without usage data");
    Check(cp.At(pad + S(2), rowY(4) + S(2)) == t.bgCard, "control: no tint without usage data");
    Check(cp.At(pad + S(2) + S(24) + S(8) + S(2), rowY(5) + S(2)) == t.bgCard, "control: no halo without data");
    Check(cp.CountColor(titleOf(0), titleBox(0)) == 0, "control: no accent title without data");
    Check(cp.CountColor(badge2, rightBox(2)) == 0, "control: no tier2 badge without data");
    Check(cp.CountColor(badge3, rightBox(0)) == 0, "control: no tier3 badge without data");
    Check(cp.CountColor(t.accent, rightBox(5)) == 0, "control: no tier4 badge without data");
    Check(cp.CountColor(t.textBody, titleBox(0)) > 10, "control: title back to body color");

    { std::lock_guard<std::mutex> lock(s_wsMu); s_wsQuit = true; } s_wsCv.notify_one();
    DestroyWindow(g.hwnd);
    std::cout << "PASS: usage tiers (data, bar widths, continuous heat, badge forms, dark/light, control)\n";
}