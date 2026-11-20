// 使用分档醒目样式取证：种入真实权重/自学习数据，驱动生产 Layout()+Paint() 画进 DIB，
// 像素级断言各档的染色强度、徽章形态、选中高亮与卡片/图标的关系、同档内不同次数的
// 连续渐变；带清空数据的对照臂证明门认的是使用数据而不是常量。
// 另加排序臂：热度档优先（重度/高频/常用必须排在零使用行之前），与行上徽章同口径。
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
    bi.bmiHeader.biSize = sizeof(bi);
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
    MarkUsageDirty();
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
    g.weightEnabled = true;
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
    int gapTop = S(kBaseCardGapTop);
    auto rowY = [&](int i) { return inputH + gapTop + i * rowH; };
    auto midY = [&](int i) { return rowY(i) + rowH / 2; };
    // 行内容左内边距：图标托盘必须整块落在选中高亮的左圆角之内
    int contentLeft = pad + S(9);
    int textX = contentLeft + S(24) + S(8);
    int pillInset = S(2);
    int pillW = (px.w - 2 * pad) - 2 * pillInset;
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
    auto rightBox = [&](int i) { return RECT{px.w / 2, rowY(i), px.w, rowY(i) + rowH}; };
    auto titleBox = [&](int i) { return RECT{textX, rowY(i), px.w / 2, rowY(i) + rowH}; };
    // 选中玻璃高亮的颜色（与 Paint 里的公式一致）
    auto glassAt = [&](float u, float v) {
        COLORREF base = Blend(t.selBg, t.accent, 0.34f * (1.0f - u) * (1.0f - u) + 0.12f);
        float glow = (1.0f - v); glow *= glow;
        return Blend(base, t.textTitle, (t.dark ? 0.10f : 0.14f) * glow);
    };

    // 档位色条已删除：卡片左缘内侧（图标托盘之前）必须是该行底色，
    // 不能再出现一枚画在窗体背景上的孤立小色块。
    {
        COLORREF want = (g.items[4].usage >= kHotUsageMin) ? tintOf(4) : t.bgCard;
        Check(Near(px.At(pad + S(1), rowY(4) + S(2)), want, 6),
              "no stray tier bar inside card left edge");
        Check(Dist(px.At(1, midY(0)), t.bgCard) > 20,
              "card does not bleed into the window gutter");
    }

    // 连续热度：同档 15 与 25 染色不同，且强度随次数单调；染色只落在卡片内
    Check(tintOf(0) != tintOf(4), "same tier different counts tint differently");
    int inCardX = pad + S(2);
    Check(px.At(inCardX, rowY(4) + S(2)) == tintOf(4), "tier3 higher count tint rendered");
    Check(px.At(inCardX, rowY(2) + S(2)) == tintOf(2), "tier2 tint rendered");
    // 选中行玻璃高亮：卡片内圆角块，左右内缩 S(2)，渐变平滑无台阶。
    // 顶部 2px 处采样 = 玻璃基色（左浓右淡二次衰减）+ 顶部柔光
    {
        int sx = textX + S(2);  // 文字起点右侧，远离高亮圆角区
        float ug = (float)(sx - (pad + pillInset)) / (float)(pillW - 1);
        float vg = 2.0f / (float)(rowH - 1);
        COLORREF glassTop = glassAt(ug, vg);
        Check(Near(px.At(sx, rowY(0) + S(2)), glassTop, 2),
              "selected glass pill aligned inside card");
        // 图标托盘不许压在高亮左圆角上：圆角内侧第一列必须是玻璃色
        float uc = 1.0f / (float)(pillW - 1);
        Check(Near(px.At(pad + pillInset + S(1), midY(0)), glassAt(uc, 0.5f), 3),
              "icon tray does not break selection pill corner");
        // 内缩验证：卡左缘与高亮左缘之间是卡片底色（带内阴影，用容差而非精确色值）
        Check(Dist(px.At(pad + S(2), rowY(0) + S(2)), t.bgCard) < 30,
              "selection inset from card left edge");
    }
    Check(Dist(tintOf(4), t.bgCard) > Dist(tintOf(0), t.bgCard) &&
          Dist(tintOf(0), t.bgCard) > Dist(tintOf(2), t.bgCard),
          "tint strength monotonic 3 < 15 < 25");
    Check(Dist(tintOf(5), t.bgCard) > Dist(tintOf(4), t.bgCard), "tier4 tint strongest");

    // 末行完整：最后一行底边不得越过卡片底边，卡片底边不得贴死窗口底边
    {
        int rowsBottom = rowY((int)g.items.size() - 1) + rowH;
        int cardBottom = rowsBottom + S(kBaseCardPadBottom);
        Check(cardBottom <= px.h - S(kBaseWinPadBottom) + 1, "card bottom leaves window padding");
        // 窗口最底一排是背景（不是卡片被切断的残片）
        Check(Dist(px.At(px.w / 2, px.h - 1), t.bgCard) > 20,
              "window bottom edge is not a cut-off card");
    }

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
    // 徽章已平面化：胶囊左端的孤立小圆点必须消失（重度实底行除外）
    Check(px.CountColor(t.accent, rightBox(2)) == 0, "no tier dot on light badge");
    Check(px.CountColor(t.accent, rightBox(4)) == 0, "no tier dot on strong badge");
    // 重度档标题后软辉光（字上方采样 = 染色底叠 accent2 低 alpha）
    Check(Near(px.At(textX + S(2), rowY(5) + S(2)),
               Blend(tintOf(5), t.accent2, 26 / 255.f), 2), "tier4 title halo");
    for (int i : {1, 3}) {
        Check(px.CountColor(badge2, rightBox(i)) == 0, "plain row no tier2 badge");
        Check(px.CountColor(badge3, rightBox(i)) == 0, "plain row no tier3 badge");
        Check(px.CountColor(t.accent, rightBox(i)) == 0, "plain row no tier4 badge");
    }

    // ---- 对照臂：清空全部使用数据 ⇒ 所有醒目样式必须消失 ----
    g.theme = &kThemes[0];
    ApplyTheme(false);
    g.weights.clear(); g.learned.clear();
    MarkUsageDirty();
    Layout();
    for (auto& r : g.items) Check(r.usage == 0, "control arm usage cleared");
    Pixels cp = RenderAndGrab(L"hot_usage_control.bmp");
    {
        int sx = textX + S(2);
        float uc = (float)(sx - (pad + pillInset)) / (float)(pillW - 1);
        COLORREF glassMid = glassAt(uc, 0.5f);
        Check(Near(cp.At(sx, midY(0)), glassMid, 2),
              "control: selected row plain glass inside card");
        Check(Dist(cp.At(1, midY(0)), t.bgCard) > 20,
              "control: selection does not spill outside card");
    }
    Check(Dist(cp.At(pad + S(2), rowY(5) + S(2)), t.bgCard) < 30,
          "control: no bar or tint without usage data");
    Check(cp.At(pad + S(2) + S(9) + S(24) + S(8) + S(2), rowY(5) + S(2)) == t.bgCard,
          "control: no halo without data");
    Check(cp.CountColor(titleOf(0), titleBox(0)) == 0, "control: no accent title without data");
    Check(cp.CountColor(badge2, rightBox(2)) == 0, "control: no tier2 badge without data");
    Check(cp.CountColor(badge3, rightBox(0)) == 0, "control: no tier3 badge without data");
    Check(cp.CountColor(t.accent, rightBox(5)) == 0, "control: no tier4 badge without data");
    Check(cp.CountColor(t.textBody, titleBox(0)) > 10, "control: title back to body color");

    // ---- 排序臂：热度档优先，重度必须排最前（与徽章同口径）----
    // 放在最后：本臂会重建 g.items（只剩 3 行），前面按 6 行索引的断言必须先跑完。
    {
        g.programs.clear();
        g.programs.resize(3);  // Program 含 unique_ptr，只能默认构造
        g.programs[0].name = L"Trae";      g.programs[0].path = L"C:\\Apps\\Trae\\Trae.exe";
        g.programs[0].startMenu = true;    g.programs[0].iconTried = true;
        g.programs[1].name = L"Trae CN";   g.programs[1].path = L"C:\\Apps\\Trae\\TraeCN.exe";
        g.programs[1].startMenu = true;    g.programs[1].iconTried = true;
        g.programs[2].name = L"Trae IDE";  g.programs[2].path = L"C:\\Apps\\Trae\\TraeIDE.exe";
        g.programs[2].startMenu = true;    g.programs[2].iconTried = true;
        // Trae IDE：只在别的词下被点过 40 次（重度档），当前词「trae」下 0 次
        g.weights[L"ide"][L"C:\\Apps\\Trae\\TraeIDE.exe"] = WeightVal(40, NowUnix());
        MarkUsageDirty();
        g.items.clear();
        SearchPrograms(L"trae");
        Check(g.items.size() == 3, "all three candidates listed");
        Check(g.items[0].title == L"Trae IDE", "heavy-usage item sorts to the top");
        Check(g.items[1].title == L"Trae", "hot item follows heavy one");
        Check(g.items[2].title == L"Trae CN", "never-hot item sinks to the bottom");
        Check(UsageTier(GlobalUsageCount(L"C:\\Apps\\Trae\\TraeIDE.exe")) == 4,
              "heavy item is tier 4 in the global ledger");
    }

    // ---- 重排臂：热度档统一重排（自学习行压住重度行的线上真实场景）----
    // 回归点：旧实现把「排序后的 slots」当写回目标，等于恒等置换 → 重排从未生效，
    // 结果是「重度 ×81」被只用 1 次的自学习行压在下面。
    {
        g.weights.clear(); g.learned.clear();
        g.weightCount = 0; g.learnCount = 0;
        // 自学习行：只用 1 次（tier 0），但 PrependLearnedRows 总是把它插在最前
        g.weights[L"trae"][L"C:\\Users\\demo\\Apps\\Trae CN\\Trae CN.exe"] = WeightVal(1, NowUnix());
        // 重度行：81 次（tier 4），线上真实数据显示为「重度 ×81」
        g.weights[L"tcode"][L"C:\\Users\\demo\\Roaming\\TraeCode CN.lnk"] = WeightVal(81, NowUnix());
        // 常用行：4 次（tier 2）
        g.weights[L"trae"][L"C:\\Users\\demo\\Roaming\\TRAE CN.lnk"] = WeightVal(4, NowUnix());
        MarkUsageDirty();

        g.items.clear();
        Row grp; grp.kind = Row::Group; grp.title = L"一键启动：trae"; grp.action = L"trae";
        g.items.push_back(grp);
        Row learned; learned.kind = Row::File; learned.title = L"Trae CN.exe";
        learned.sub = learned.action = L"C:\\Users\\demo\\Apps\\Trae CN\\Trae CN.exe";
        g.items.push_back(learned);
        Row heavy; heavy.kind = Row::Prog; heavy.title = L"TraeCode CN";
        heavy.sub = heavy.action = L"C:\\Users\\demo\\Roaming\\TraeCode CN.lnk";
        g.items.push_back(heavy);
        Row mid; mid.kind = Row::Prog; mid.title = L"TRAE CN";
        mid.sub = mid.action = L"C:\\Users\\demo\\Roaming\\TRAE CN.lnk";
        g.items.push_back(mid);
        Row zero; zero.kind = Row::Prog; zero.title = L"Control Panel";
        zero.sub = zero.action = L"C:\\Users\\demo\\Roaming\\Control Panel.lnk";
        g.items.push_back(zero);

        RerankByTier();
        Check(g.items.size() == 5, "rerank keeps every row");
        Check(g.items[0].kind == Row::Group && g.items[0].title == L"一键启动：trae",
              "group row keeps its position");
        Check(g.items[1].title == L"TraeCode CN", "heavy-usage row moves above the learned row");
        Check(g.items[2].title == L"TRAE CN", "common-usage row follows the heavy one");
        Check(g.items[3].title == L"Trae CN.exe", "learned row sinks below hot rows");
        Check(g.items[4].title == L"Control Panel", "zero-usage row stays last");
    }

    { std::lock_guard<std::mutex> lock(s_wsMu); s_wsQuit = true; s_wsCv.notify_one(); }
    DestroyWindow(g.hwnd);
    std::cout << "PASS: usage tiers (data, tint, badge forms, hot-sorted, control)\n";
}
