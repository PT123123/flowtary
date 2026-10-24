// ============================================================
// Flowtary 设置窗（flowtary-settings.exe）
//   FLTK 1.4 静态链接，独立进程；注册表读写全部走 settings_model.cpp。
//   与主进程的协议见 settings_ipc.h：
//     · 界面改动 → MsgPreview（主进程只改内存，实时预览）
//     · 保存 → 自己写注册表 + MsgCommit（主进程全量重读并重放）
//     · 取消 / X 关闭 → MsgCancel（主进程回退到打开时的快照）
// ============================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wininet.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Radio_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <atomic>
#include <cstdio>
#include <cwchar>
#include <list>
#include <string>
#include <thread>
#include <vector>

#include "settings_ipc.h"
#include "settings_model.h"

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "dwmapi.lib")

// FLTK 静态库自身不带 comctl6 清单，缺了它按钮/输入框会退回 Win95 观感
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ---------------- 逻辑坐标（96dpi 基准；FLTK 1.4 按屏幕缩放自动放大） ----------------
static const int kW = 600, kH = 540;
static const int kSideX = 8, kSideW = 140, kSideRowH = 32, kSideGap = 4;
static const int kDivX = 152;
static const int kContentX = 156;
static const int kContentY = 8;
static const int kPW = kW - kContentX - 16;  // 内容面板宽
static const int kPH = kH - kContentY - 52;  // 内容面板高（底部留给按钮行）
static const int kRowH = 24;
static const int kTabCount = 10;

// ---------------- 全局状态 ----------------
static ft::Settings g_s;          // 界面上的当前值（未保存）
static HWND g_hMain = nullptr;    // 主进程窗口（命令行 --ipc 传入）
static HWND g_hSelf = nullptr;    // 本窗口的 Win32 句柄
static std::wstring g_mainExe;    // 主程序 exe 完整路径（开机自启 / 下载目录都靠它）
static int g_tab = 0;
static bool g_committed = false;  // 本次会话是否已提交（决定关闭走 Commit 还是 Cancel）
static bool g_quit = false;       // 事件循环退出标志

struct SettingsWin;
static SettingsWin* g_win = nullptr;
static Fl_Group* g_panels[kTabCount] = {};
static Fl_Radio_Button* g_side[kTabCount] = {};
static class ThemeList* g_themeList = nullptr;
static Fl_Slider* g_trkAlpha = nullptr;
static Fl_Slider* g_trkRadius = nullptr;
static Fl_Box* g_lblAlpha = nullptr;
static Fl_Box* g_lblRadius = nullptr;
static Fl_Button* g_btnReset = nullptr;
static Fl_Box* g_lblWeightCount = nullptr;
static Fl_Box* g_lblLearnCount = nullptr;
static Fl_Box* g_lblExcludeCount = nullptr;
static Fl_Box* g_lblCaptureStatus = nullptr;
static Fl_Button* g_btnDlSC = nullptr;
static Fl_Button* g_btnDlIR = nullptr;
static Fl_Input* g_edtShellPath = nullptr;
static Fl_Input* g_edtShellArgs = nullptr;
static Fl_Box* g_lblShellPath = nullptr;
static Fl_Box* g_lblShellArgs = nullptr;
static Fl_Multiline_Input* g_edtRules = nullptr;
static Fl_Multiline_Input* g_edtLaunch = nullptr;
static Fl_Multiline_Input* g_edtKill = nullptr;
static Fl_Multiline_Input* g_edtExclude = nullptr;

// Shell 页的「自定义路径 / 启动参数」两行只在自定义 Shell 下出现。FLTK 的 hide() 不回收
// 空间，所以下面这些行各自登记「显示/隐藏」两个 y，切换时整体上下挪，不留空白带。
static const int kShellCustomH = 60;
struct ShellRow {
    Fl_Widget* w;
    int x, yCustom, yPlain;
};
static std::vector<Fl_Widget*> g_shellCustom;  // 仅自定义时出现的行
static std::vector<ShellRow> g_shellRows;      // 其下方的行

// ---------------- 文本转换（FLTK 只吃 UTF-8） ----------------
static std::string u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
static std::wstring wu8(const char* s) {
    if (!s || !*s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}

// FL_Widget 构造函数只是保存标签指针（不复制），所以静态文本一律进常驻池。
// std::list 的元素地址在插入后不会失效，c_str() 因此可以安全长期持有。
static const char* CL(const WCHAR* text) {
    static std::list<std::string>* pool = new std::list<std::string>();
    return pool->emplace_back(u8(text)).c_str();
}

// 编辑器文本：换行统一成 \n（模型两侧都吃 \r\n 与 \n，FLTK 用 \n）
static std::string u8Lines(const std::wstring& w) {
    std::wstring t;
    t.reserve(w.size());
    for (WCHAR c : w)
        if (c != L'\r') t.push_back(c);
    return u8(t);
}
static std::wstring editorText(Fl_Multiline_Input* in) {
    std::string s = u8Lines(wu8(in->value() ? in->value() : ""));
    return wu8(s.c_str());
}

// ---------------- 前置声明 ----------------
static void ApplyThemeColors();
static void ApplyTitlebarStyle();
static void ApplyGhostStyle();
static void PushPreview();
static void ShowTab(int tab);
static void UpdateCaptureStatus();
static void WeightCountLabel(unsigned long long n);
static void LearnCountLabel(unsigned long long n);
static void UpdateExcludeCount();

// ---------------- 与主进程通信 ----------------
static void SendToMain(DWORD id, const void* data, DWORD cb) {
    if (!g_hMain || !IsWindow(g_hMain)) return;
    COPYDATASTRUCT cds{};
    cds.dwData = id;
    cds.cbData = cb;
    cds.lpData = (PVOID)data;
    DWORD_PTR ok = 0;
    SendMessageTimeoutW(g_hMain, WM_COPYDATA, (WPARAM)g_hSelf, (LPARAM)&cds, SMTO_ABORTIFHUNG,
                        3000, &ok);
}

static ipc::Preview MakePreview() {
    ipc::Preview p{};
    p.hdr.sender = g_hSelf;
    p.themeIdx = g_s.themeIdx;
    p.hotkeyMode = g_s.hotkeyMode;
    p.centerWake = g_s.centerWake ? TRUE : FALSE;
    p.beautify = g_s.beautify ? TRUE : FALSE;
    p.skipDotFolders = g_s.skipDotFolders ? TRUE : FALSE;
    p.fdjEnabled = g_s.fdjEnabled ? TRUE : FALSE;
    p.dfBackend = g_s.dfBackend;
    p.weightEnabled = g_s.weightEnabled ? TRUE : FALSE;
    p.weightFlush = g_s.weightFlush;
    p.weightMaxEntries = g_s.weightMaxEntries;
    p.learnEnabled = g_s.learnEnabled ? TRUE : FALSE;
    p.learnScope = g_s.learnScope;
    p.learnMaxEntries = g_s.learnMaxEntries;
    p.learnExpire = g_s.learnExpire;
    p.topEnabled = g_s.topEnabled ? TRUE : FALSE;
    p.cmdEnabled = g_s.cmdEnabled ? TRUE : FALSE;
    p.winEnabled = g_s.winEnabled ? TRUE : FALSE;
    p.captureEnabled = g_s.captureEnabled ? TRUE : FALSE;
    p.shellType = g_s.shellType;
    p.shellDefaultCwd = g_s.shellDefaultCwd;
    p.shellShowWindow = g_s.shellShowWindow ? TRUE : FALSE;
    p.winGroupProc = g_s.winGroupProc ? TRUE : FALSE;
    p.winShowUwp = g_s.winShowUwp ? TRUE : FALSE;
    p.winShowProc = g_s.winShowProc ? TRUE : FALSE;
    p.winCacheSec = g_s.winCacheSec;
    int n = (int)(g_s.tune.size() < (size_t)ipc::kMaxThemes ? g_s.tune.size()
                                                            : (size_t)ipc::kMaxThemes);
    p.tuneCount = n;
    for (int i = 0; i < n; ++i) {
        p.tune[i].alpha = g_s.tune[i].alpha;
        p.tune[i].radius = g_s.tune[i].radius;
    }
    return p;
}
static void PushPreview() {
    ipc::Preview p = MakePreview();
    SendToMain(ipc::MsgPreview, &p, sizeof(p));
}

// ---------------- 主题着色 ----------------
// FLTK 1.4 的可覆写颜色是「索引槽」：次级文字/分隔线/强调色各占一个自由槽位，
// 换主题时只重写槽位内容，已创建的控件自动跟随，无需遍历改 labelcolor。
constexpr Fl_Color kSub = FL_FREE_COLOR + 8;      // 说明/次要文字
constexpr Fl_Color kDiv = FL_FREE_COLOR + 9;      // 分隔线
constexpr Fl_Color kAccent = FL_FREE_COLOR + 10;  // 强调色

// 主窗渐变端点：Fl_Color 无法反查 RGB，这里另存一份
static int g_bg0[3] = {0, 0, 0}, g_bg1[3] = {0, 0, 0};

static void ApplyThemeColors() {
    if (g_s.themeIdx < 0 || g_s.themeIdx >= kThemeCount) return;
    const Theme& t = kThemes[g_s.themeIdx];
    Fl::set_color(FL_BACKGROUND_COLOR, GetRValue(t.bgCard), GetGValue(t.bgCard), GetBValue(t.bgCard));
    Fl::set_color(FL_BACKGROUND2_COLOR, GetRValue(t.bgInput), GetGValue(t.bgInput),
                  GetBValue(t.bgInput));
    Fl::set_color(FL_FOREGROUND_COLOR, GetRValue(t.textBody), GetGValue(t.textBody),
                  GetBValue(t.textBody));
    Fl::set_color(FL_SELECTION_COLOR, GetRValue(t.selBg), GetGValue(t.selBg), GetBValue(t.selBg));
    Fl::set_color(kSub, GetRValue(t.textSec), GetGValue(t.textSec), GetBValue(t.textSec));
    Fl::set_color(kDiv, GetRValue(t.divider), GetGValue(t.divider), GetBValue(t.divider));
    Fl::set_color(kAccent, GetRValue(t.accent), GetGValue(t.accent), GetBValue(t.accent));
    Fl::background(GetRValue(t.bg), GetGValue(t.bg), GetBValue(t.bg));
    g_bg0[0] = GetRValue(t.bg); g_bg0[1] = GetGValue(t.bg); g_bg0[2] = GetBValue(t.bg);
    g_bg1[0] = GetRValue(t.bg2); g_bg1[1] = GetGValue(t.bg2); g_bg1[2] = GetBValue(t.bg2);
}

// 主窗背景：主题 bg → bg2 垂直渐变（与启动器一致）+ Tab 分隔线
struct SettingsWin : public Fl_Window {
    SettingsWin() : Fl_Window(kW, kH, "") { copy_label(CL(L"Flowtary 设置")); }
    void draw() override {
        int h = this->h();
        int denom = h > 1 ? h - 1 : 1;
        for (int y = 0; y < h; ++y) {
            fl_color(g_bg0[0] + (g_bg1[0] - g_bg0[0]) * y / denom,
                     g_bg0[1] + (g_bg1[1] - g_bg0[1]) * y / denom,
                     g_bg0[2] + (g_bg1[2] - g_bg0[2]) * y / denom);
            fl_rectf(0, y, w(), 1);
        }
        fl_color(kDiv);
        fl_rectf(kDivX, 6, 1, h - 12);
        Fl_Window::draw();
    }
};

// 主题选择器：每行「三色块 + 名称 + 选中对勾」，本程序唯一的自绘控件
class ThemeList : public Fl_Widget {
  public:
    ThemeList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_DOWN_FRAME); }
    int rowH() const { return h() / (kThemeCount > 0 ? kThemeCount : 1); }

  protected:
    void draw() override {
        draw_box();
        int rh = rowH();
        for (int i = 0; i < kThemeCount; ++i) {
            const Theme& t = kThemes[i];
            int ry = y() + i * rh;
            if (i == g_s.themeIdx) fl_rectf(x() + 1, ry + 1, w() - 2, rh - 2, FL_SELECTION_COLOR);
            int sx = x() + 10;
            COLORREF sw[3] = {t.accent, t.bgCard, t.bg};
            for (COLORREF c : sw) {
                fl_rectf(sx, ry + rh / 2 - 7, 14, 14, GetRValue(c), GetGValue(c), GetBValue(c));
                sx += 18;
            }
            // fl_rectf(..., r,g,b) 返回时当前颜色就变成那个 RGB（fl_draw.H 里明写了），
            // 不在这里重设的话，主题名会被画成它自己那一款的背景色——深色款直接看不见。
            fl_color(g_s.beautify ? FL_FOREGROUND_COLOR : kSub);
            fl_font(FL_HELVETICA, 11);
            fl_draw(CL(t.name), sx + 6, ry + rh / 2 + 4);
            if (i == g_s.themeIdx) {
                fl_color(kAccent);
                int cx = x() + w() - 24, cy = ry + rh / 2;
                fl_line(cx, cy, cx + 4, cy + 5);
                fl_line(cx + 4, cy + 5, cx + 12, cy - 6);
            }
        }
    }
    int handle(int e) override {
        if (e == FL_PUSH) {
            int rh = rowH();
            int i = (Fl::event_y() - y()) / (rh > 0 ? rh : 1);
            if (i >= 0 && i < kThemeCount) {
                g_s.themeIdx = i;
                do_callback();
                redraw();
                return 1;
            }
        }
        return Fl_Widget::handle(e);
    }
};

// ---------------- 控件工厂 ----------------
// 注意：FLTK 1.4 的控件坐标一律相对「窗口」而不是相对所属 Fl_Group（见
// vendor/fltk-1.4.5/test/coordinates.cxx 与各控件 draw/handle 的 x() 用法），
// Fl_Group 不会平移子控件。所以下面所有工厂都接收「面板内坐标」，在这里统一
// 加上内容区原点 (kContentX, kContentY)，Builder 里继续按面板内坐标书写。
static Fl_Box* AddLabel(Fl_Group* p, int x, int y, int w, int h, const WCHAR* text,
                        int size = 11, Fl_Color col = FL_FOREGROUND_COLOR) {
    Fl_Box* b = new Fl_Box(kContentX + x, kContentY + y, w, h);
    b->box(FL_NO_BOX);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    b->labelsize(size);
    b->labelcolor(col);
    b->copy_label(CL(text));
    p->add(b);
    return b;
}

static Fl_Box* AddHint(Fl_Group* p, int x, int y, int w, int h, const WCHAR* text) {
    Fl_Box* b = AddLabel(p, x, y, w, h, text, 10, kSub);
    // FL_ALIGN_TOP：说明文字一律贴着自己那块盒子顶端，否则 FLTK 会把换行后的
    // 整段文字垂直居中，看起来像飘在半空（权重/自学习页踩过）。
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
    return b;
}

// 复选框（勾号 + 文字）
struct CheckCtl {
    bool* bind;
    int kind;  // 1=界面美化 2=抗残影
};
static void onCheck(Fl_Widget* w, void* v);
static Fl_Check_Button* AddCheck(Fl_Group* p, int x, int y, int w, const WCHAR* text, bool* bind,
                                 int kind = 0) {
    Fl_Check_Button* b = new Fl_Check_Button(kContentX + x, kContentY + y, w, kRowH);
    b->copy_label(CL(text));
    b->value(*bind ? 1 : 0);
    b->selection_color(kAccent);
    b->labelsize(11);
    b->callback(onCheck, new CheckCtl{bind, kind});
    p->add(b);
    return b;
}

// 下拉框：候选表在打开时定位到「实际值」，档位外的值插入为额外一项（UI 不说谎）
struct ChoiceCtl {
    Fl_Choice* w;
    std::vector<ft::Option> opts;
    int* bind;
    int kind;  // 1=Shell 程序（联动自定义路径/参数）
};
static void onChoice(Fl_Widget* w, void* v);
static ChoiceCtl* AddChoice(Fl_Group* p, int x, int y, int w, std::vector<ft::Option> opts,
                            int* bind, int kind) {
    Fl_Choice* ch = new Fl_Choice(kContentX + x, kContentY + y, w, kRowH);
    ch->labelsize(11);
    ChoiceCtl* ctl = new ChoiceCtl();
    ctl->w = ch;
    ctl->opts = std::move(opts);
    ctl->bind = bind;
    ctl->kind = kind;
    int idx = ft::LocateOption(ctl->opts, *bind, 0, 0x7fffffff);
    if (idx < 0) idx = 0;
    for (const auto& o : ctl->opts) ch->add(CL(o.label.c_str()));
    ch->value(idx);
    ch->callback(onChoice, ctl);
    p->add(ch);
    return ctl;
}

static ChoiceCtl* AddChoiceRow(Fl_Group* p, int y, const WCHAR* label,
                               std::vector<ft::Option> opts, int* bind, int kind = 0) {
    AddLabel(p, 0, y, 160, kRowH, label);
    return AddChoice(p, 166, y, kPW - 166, std::move(opts), bind, kind);
}

static Fl_Multiline_Input* AddMultiline(Fl_Group* p, int y, int w, int h) {
    Fl_Multiline_Input* in = new Fl_Multiline_Input(kContentX, kContentY + y, w, h);
    in->textsize(11);
    p->add(in);
    return in;
}

static Fl_Input* AddInput(Fl_Group* p, int x, int y, int w) {
    Fl_Input* in = new Fl_Input(kContentX + x, kContentY + y, w, kRowH);
    in->textsize(11);
    p->add(in);
    return in;
}

static Fl_Button* AddButton(Fl_Group* p, int x, int y, int w, const WCHAR* text) {
    Fl_Button* b = new Fl_Button(kContentX + x, kContentY + y, w, kRowH);
    b->copy_label(CL(text));
    b->labelsize(11);
    p->add(b);
    return b;
}

static Fl_Slider* AddSlider(Fl_Group* p, int x, int y, int w, int h, double hi) {
    Fl_Slider* s = new Fl_Slider(kContentX + x, kContentY + y, w, h);
    s->type(FL_HORIZONTAL);
    s->bounds(0, hi);
    s->step(1);
    p->add(s);
    return s;
}

static ThemeList* AddThemeList(Fl_Group* p, int y, int h) {
    ThemeList* l = new ThemeList(kContentX, kContentY + y, kPW, h);
    p->add(l);
    return l;
}

// FLTK 1.4 的可见性只有 show()/hide()，没有 Win32 那种传参开关
static void SetVis(Fl_Widget* w, bool on) {
    if (!w) return;
    if (on) w->show();
    else w->hide();
}

// 自定义 Shell 行显隐 + 下方各行重排（坐标是窗口绝对的，所以要自己加偏移）
static void ApplyShellLayout() {
    const bool custom = (g_s.shellType == 3);
    for (Fl_Widget* w : g_shellCustom) SetVis(w, custom);
    for (const ShellRow& r : g_shellRows)
        r.w->position(kContentX + r.x, kContentY + (custom ? r.yCustom : r.yPlain));
}

static void onChoice(Fl_Widget*, void* v) {
    ChoiceCtl* ctl = (ChoiceCtl*)v;
    int i = ctl->w->value();
    if (i < 0 || i >= (int)ctl->opts.size()) return;
    *ctl->bind = ctl->opts[i].value;
    if (ctl->kind == 1) {  // Shell 程序：自定义才出现路径/参数两行
        ApplyShellLayout();
        g_win->redraw();
    }
    PushPreview();
}

static void onCheck(Fl_Widget*, void* v) {
    CheckCtl* ctl = (CheckCtl*)v;
    *ctl->bind = !*ctl->bind;
    if (ctl->kind == 1) {  // 界面美化：主题列表与滑块随之置灰，本窗标题栏即时切换
        if (g_trkAlpha) {
            if (g_s.beautify) g_trkAlpha->activate();
            else g_trkAlpha->deactivate();
        }
        if (g_trkRadius) {
            if (g_s.beautify) g_trkRadius->activate();
            else g_trkRadius->deactivate();
        }
        ApplyTitlebarStyle();
        if (g_themeList) g_themeList->redraw();
    }
    if (ctl->kind == 2) ApplyGhostStyle();  // 抗残影只作用于本窗口
    PushPreview();
}

// ---------------- 本窗口的外壳风格（暗色标题栏 / 圆角 / 整窗双缓冲） ----------------
static void ApplyTitlebarStyle() {
    if (!g_hSelf) return;
    const Theme& t = kThemes[g_s.themeIdx];
    if (g_s.beautify) {
        BOOL dark = t.dark ? TRUE : FALSE;
        if (FAILED(DwmSetWindowAttribute(g_hSelf, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark,
                                         sizeof(dark))))
            DwmSetWindowAttribute(g_hSelf, 19, &dark, sizeof(dark));
        DWORD pref = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(g_hSelf, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    } else {
        BOOL dark = FALSE;
        DwmSetWindowAttribute(g_hSelf, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(g_hSelf, 19, &dark, sizeof(dark));
        DWORD pref = 0;  // 回归系统默认
        DwmSetWindowAttribute(g_hSelf, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }
}
static void ApplyGhostStyle() {
    if (!g_hSelf) return;
    LONG_PTR ex = GetWindowLongPtrW(g_hSelf, GWL_EXSTYLE);
    if (g_s.antiGhost) ex |= WS_EX_COMPOSITED;
    else ex &= ~WS_EX_COMPOSITED;
    SetWindowLongPtrW(g_hSelf, GWL_EXSTYLE, ex);
    SetWindowPos(g_hSelf, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------- 主题微调滑块（实时预览） ----------------
static std::wstring TuneLabel(const WCHAR* name, int v) {
    WCHAR buf[64];
    swprintf(buf, 64, L"%s：%d", name, v);
    return buf;
}
static void RefreshTuneLabels() {
    if (g_s.themeIdx < 0 || g_s.themeIdx >= (int)g_s.tune.size()) return;
    const ThemeTune& tt = g_s.tune[g_s.themeIdx];
    g_trkAlpha->value(tt.alpha);
    g_trkRadius->value(tt.radius);
    g_lblAlpha->copy_label(u8(TuneLabel(L"透明度", tt.alpha)).c_str());
    g_lblRadius->copy_label(u8(TuneLabel(L"圆角半径", tt.radius)).c_str());
}
static void onSlider(Fl_Widget* w, void* v) {
    if (g_s.themeIdx < 0 || g_s.themeIdx >= (int)g_s.tune.size()) return;
    int pos = (int)(((Fl_Slider*)w)->value() + 0.5);
    if ((intptr_t)v == 0) {
        g_s.tune[g_s.themeIdx].alpha = pos;
        g_lblAlpha->copy_label(u8(TuneLabel(L"透明度", pos)).c_str());
    } else {
        g_s.tune[g_s.themeIdx].radius = pos;
        g_lblRadius->copy_label(u8(TuneLabel(L"圆角半径", pos)).c_str());
    }
    PushPreview();
}

// ---------------- 编辑器 ----------------
static void FillEditor(Fl_Multiline_Input* in, const std::wstring& text) {
    std::string s = u8Lines(text);
    in->value(s.c_str());
}
static void SyncEditorsToSettings() {
    g_s.rulesText = editorText(g_edtRules);
    g_s.launchText = editorText(g_edtLaunch);
    g_s.killText = editorText(g_edtKill);
    g_s.excludeText = editorText(g_edtExclude);
    g_s.shellCustomPath = wu8(g_edtShellPath ? g_edtShellPath->value() : "");
    g_s.shellCustomArgs = wu8(g_edtShellArgs ? g_edtShellArgs->value() : "");
}
static void UpdateExcludeCount() {
    auto parsed = ft::ParseExcludePaths(g_s.excludeText);
    size_t n = parsed.empty() ? g_s.excludePaths.size() : parsed.size();
    g_lblExcludeCount->copy_label(u8(L"当前已记忆 " + std::to_wstring(n) + L" 条排除项").c_str());
}
static void WeightCountLabel(unsigned long long n) {
    g_lblWeightCount->copy_label(
        u8(L"当前已记忆 " + std::to_wstring(n) + L" 条权重数据").c_str());
}
static void LearnCountLabel(unsigned long long n) {
    g_lblLearnCount->copy_label(u8(L"当前已学习 " + std::to_wstring(n) + L" 条快捷方式").c_str());
}

// ---------------- 两步确认清空（权重 / 自学习） ----------------
struct WipeCtl {
    Fl_Button* btn;
    std::string idle;   // 常态文案（UTF-8）
    int kind;
    bool armed;
};
static void DisarmWipe(void* v) {
    WipeCtl* wc = (WipeCtl*)v;
    wc->armed = false;
    wc->btn->copy_label(wc->idle.c_str());
}
static void onWipe(Fl_Widget* w, void* v) {
    WipeCtl* wc = (WipeCtl*)v;
    if (!wc->armed) {
        wc->armed = true;
        wc->btn->copy_label(CL(L"再次点击确认清空"));
        Fl::add_timeout(3.0, DisarmWipe, wc);  // add_timeout 就是一次性；3 秒不确认自动解除
        return;
    }
    Fl::remove_timeout(DisarmWipe, wc);
    wc->armed = false;
    wc->btn->copy_label(wc->idle.c_str());
    ipc::Header hdr{g_hSelf};
    SendToMain(wc->kind == 1 ? ipc::MsgWipeWeights : ipc::MsgWipeLearned, &hdr, sizeof(hdr));
    // 计数由主进程回 MsgStats 覆盖；这里先归零，避免等待期间显示旧值
    if (wc->kind == 1) WeightCountLabel(0);
    else LearnCountLabel(0);
    (void)w;
}

// ---------------- 截图工具：状态 + 下载 ----------------
static std::wstring FlowtaryDir() {
    size_t pos = g_mainExe.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"" : g_mainExe.substr(0, pos + 1);
}
static void UpdateCaptureStatus() {
    if (!g_lblCaptureStatus) return;
    std::wstring dir = FlowtaryDir();
    bool sc = GetFileAttributesW((dir + L"ScreenCapture.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    bool ir = GetFileAttributesW((dir + L"ImageReader.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
    std::wstring text;
    if (sc && ir) text = L"ScreenCapture.exe 和 ImageReader.exe 均已就绪";
    else if (sc) text = L"ScreenCapture.exe 已就绪，ImageReader.exe 未下载（文字识别不可用）";
    else if (ir) text = L"ImageReader.exe 已就绪，ScreenCapture.exe 未下载";
    else text = L"ScreenCapture.exe 和 ImageReader.exe 均未下载";
    g_lblCaptureStatus->copy_label(u8(text).c_str());
}

struct DlJob {
    std::wstring url;
    std::wstring dest;
    Fl_Button* btn;
    std::wstring labelIdle;
    std::wstring labelBusy;
    std::atomic<long long> done;
    std::atomic<bool> running;
    std::atomic<bool> ok;
    DlJob() : btn(nullptr), done(0), running(false), ok(false) {}
};
static DlJob g_dl[2];

static void DlProgress(void* v) {
    DlJob* j = (DlJob*)v;
    WCHAR buf[64];
    swprintf(buf, 64, L"下载中… %lld KB", j->done.load() / 1024);
    j->btn->copy_label(u8(buf).c_str());
}
static void DlFinished(void* v) {
    DlJob* j = (DlJob*)v;
    j->running = false;
    j->btn->activate();
    j->btn->copy_label(u8(j->labelIdle).c_str());
    UpdateCaptureStatus();
    if (j->ok) {  // 主进程重解析 ScreenCapture.exe（否则 ss 仍提示未找到）
        ipc::Header hdr{g_hSelf};
        SendToMain(ipc::MsgCaptureInstalled, &hdr, sizeof(hdr));
    }
}
// 纯 WinINet 下载（不引 urlmon）：先写 .tmp，成功后原子替换
static bool DownloadTo(DlJob* j) {
    bool ok = false;
    std::wstring tmp = j->dest + L".tmp";
    HINTERNET sess = InternetOpenW(L"FlowtarySettings", INTERNET_OPEN_TYPE_PRECONFIG, nullptr,
                                   nullptr, 0);
    if (!sess) return false;
    // WinINet 默认跟随重定向，无需额外 flag
    HINTERNET conn = InternetOpenUrlW(sess, j->url.c_str(), nullptr, 0,
                                      INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (conn) {
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char buf[8192];
            DWORD got = 0;
            long long total = 0;
            ok = true;
            while (InternetReadFile(conn, buf, sizeof(buf), &got) && got > 0) {
                DWORD written = 0;
                if (!WriteFile(f, buf, got, &written, nullptr) || written != got) {
                    ok = false;
                    break;
                }
                total += got;
                j->done = total;
                Fl::awake(DlProgress, j);
            }
            CloseHandle(f);
            if (ok) {
                ok = MoveFileExW(tmp.c_str(), j->dest.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
                if (!ok) DeleteFileW(tmp.c_str());
            }
        }
        InternetCloseHandle(conn);
    }
    InternetCloseHandle(sess);
    return ok;
}
static void DlWorker(void* v) {
    DlJob* j = (DlJob*)v;
    j->ok = DownloadTo(j);
    Fl::awake(DlFinished, j);
}
static void onDownload(Fl_Widget*, void* v) {
    DlJob* j = (DlJob*)v;
    if (j->running) return;
    j->running = true;
    j->done = 0;
    j->btn->deactivate();
    j->btn->copy_label(u8(j->labelBusy).c_str());
    std::thread(DlWorker, j).detach();
}

// ---------------- Tab ----------------
static void ShowTab(int tab) {
    g_tab = tab;
    for (int i = 0; i < kTabCount; ++i) {
        if (g_panels[i]) {
            if (i == tab) g_panels[i]->show();
            else g_panels[i]->hide();
        }
        if (g_side[i]) g_side[i]->value(i == tab ? 1 : 0);
    }
    SetVis(g_btnReset, tab == 1);
    if (tab == 8) UpdateCaptureStatus();
    if (tab == 2) RefreshTuneLabels();
    ft::RegSetDword(L"SettingsTab", (DWORD)tab);
    g_win->redraw();
}
static void onTab(Fl_Widget*, void* v) { ShowTab((int)(intptr_t)v); }

// ---------------- 保存 / 取消 / 关闭 ----------------
// FLTK 1.4 没有 Fl::discard()：藏掉窗口即可让等待循环退出。
static void QuitApp() {
    g_quit = true;
    if (g_win) g_win->hide();
}

static void onSave(Fl_Widget*, void*) {
    SyncEditorsToSettings();
    ft::SetStartupPath(g_mainExe, g_s.startupWanted);  // 必须写主程序路径，不能用 sidecar 自己的
    g_s.Save();
    g_committed = true;
    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgCommit, &hdr, sizeof(hdr));
    QuitApp();
}
static void onCancel(Fl_Widget*, void*) {
    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgCancel, &hdr, sizeof(hdr));  // 主进程回退到打开时的快照
    QuitApp();
}
// X 关闭：提交过就只收尾；没提交过等价「取消」
static void onClose() {
    ipc::Header hdr{g_hSelf};
    SendToMain(g_committed ? ipc::MsgClosed : ipc::MsgCancel, &hdr, sizeof(hdr));
    QuitApp();
}

// ---------------- 收主进程消息 ----------------
// Enter = 保存；焦点在多行编辑器时留给换行（契约 §9）。
// 走全局 handler 而不是按钮 shortcut，因为 FLTK 的快捷键派发顺序不保证编辑器先收到。
static int OnEnterKey(int ev) {
    if (ev != FL_KEYDOWN) return 0;
    if (Fl::event_key() != FL_Enter) return 0;
    Fl_Widget* f = Fl::focus();
    if (f == (Fl_Widget*)g_edtRules || f == (Fl_Widget*)g_edtLaunch ||
        f == (Fl_Widget*)g_edtKill || f == (Fl_Widget*)g_edtExclude)
        return 0;
    onSave(nullptr, nullptr);
    return 1;
}

static WNDPROC g_prevProc = nullptr;
static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)l;
        const DWORD id = cds ? (DWORD)cds->dwData : 0;
        if (cds && ipc::IsOurMsg(id)) {
            if (id == ipc::MsgStats && cds->cbData == sizeof(ipc::Stats)) {
                const ipc::Stats* st = (const ipc::Stats*)cds->lpData;
                WeightCountLabel((unsigned long long)st->weightCount);
                LearnCountLabel((unsigned long long)st->learnCount);
            } else if (id == ipc::MsgMainQuitting) {
                QuitApp();  // 主进程要走：本窗不再有价值，直接收摊
            }
        }
        return TRUE;
    }
    if (m == WM_CLOSE) {
        onClose();
        return 0;
    }
    return CallWindowProcW(g_prevProc, h, m, w, l);
}

// ---------------- 界面构建 ----------------
static const WCHAR* kTabNames[kTabCount] = {
    L"常规", L"网页规则", L"主题", L"一键", L"搜索权重", L"排除路径",
    L"Shell 与窗口", L"命令", L"截图工具", L"自学习",
};

// 面板创建后会成为「当前组」，后续控件自动挂到它上面；建完必须 end()。
static Fl_Group* NewPanel(int tab) {
    Fl_Group* p = new Fl_Group(kContentX, kContentY, kPW, kPH);
    p->box(FL_NO_BOX);
    g_panels[tab] = p;
    return p;
}

static void BuildSidebar() {
    for (int i = 0; i < kTabCount; ++i) {
        Fl_Radio_Button* b =
            new Fl_Radio_Button(kSideX, kContentY + i * (kSideRowH + kSideGap), kSideW, kSideRowH);
        b->copy_label(CL(kTabNames[i]));
        // Fl_Button::draw()：value() 为真时用 down_box() 平涂 selection_color()，
        // 为假时用 box()=FL_NO_BOX 什么都不画 —— 正好是「选中项一条高亮带」的侧栏观感。
        b->box(FL_NO_BOX);
        b->down_box(FL_FLAT_BOX);
        b->selection_color(FL_SELECTION_COLOR);
        b->labelsize(11);
        b->callback(onTab, (void*)(intptr_t)i);
        g_win->add(b);
        g_side[i] = b;
    }
}

static void BuildTabGeneral() {
    Fl_Group* p = NewPanel(0);
    AddCheck(p, 0, 8, kPW, L"开机自动启动", &g_s.startupWanted);
    AddChoiceRow(p, 44, L"结果项快捷键：", ft::HotkeyModeOptions(), &g_s.hotkeyMode);
    AddChoiceRow(p, 78, L"唤醒位置：", ft::CenterWakeOptions(), &g_s.centerWake);
    AddCheck(p, 0, 116, kPW, L"文件对话框跳转（在文件对话框中直接定位文件夹）", &g_s.fdjEnabled);
    AddChoiceRow(p, 150, L"d/f 搜索后端：", ft::DfBackendOptions(), &g_s.dfBackend);
    AddHint(p, 0, 184, kPW, 46,
            L"d/f 搜索后端：新方案=内置索引+fd（不依赖 Everything，后台自动建索引）；"
            L"Everything=旧链路（需 Everything 正在运行）。\n"
            L"唤起快捷键（Alt+Space）在托盘菜单里开关；结果项快捷键改动立即生效。");
    p->end();
}

static void BuildTabWeb() {
    Fl_Group* p = NewPanel(1);
    AddLabel(p, 0, 4, 200, 20, L"网页搜索规则：");
    g_edtRules = AddMultiline(p, 28, kPW, kPH - 78);
    FillEditor(g_edtRules, g_s.rulesText);
    AddHint(p, 0, kPH - 44, kPW, 44, L"每行一条：前缀 + 空格 + 链接模板（{q} 为关键词，回车跳转）");
    p->end();
}

static void OnThemePicked(Fl_Widget*) {
    ApplyThemeColors();
    RefreshTuneLabels();
    ApplyTitlebarStyle();
    PushPreview();
    g_win->redraw();
}
static void BuildTabTheme() {
    Fl_Group* p = NewPanel(2);
    AddCheck(p, 0, 4, kPW, L"启用界面美化（暗色标题栏 / 圆角窗口 / 暗色菜单）", &g_s.beautify, 1);
    AddCheck(p, 0, 32, kPW, L"启用抗残影双缓冲（实验性）", &g_s.antiGhost, 2);
    AddHint(p, 0, 58, kPW, 18, L"实验性：开启整窗双缓冲消除残影；个别控件可能偶发闪烁");
    AddLabel(p, 0, 82, 100, 20, L"主题：");
    g_themeList = AddThemeList(p, 104, kThemeCount * 30);
    g_themeList->callback(OnThemePicked);
    int y = 104 + kThemeCount * 30 + 10;
    AddLabel(p, 0, y, kPW, 18, L"微调（实时预览，保存后生效）", 10, kSub);
    y += 24;
    g_lblAlpha = AddLabel(p, 0, y, 112, 20, L"透明度：0");
    g_trkAlpha = AddSlider(p, 118, y, kPW - 118, 20, 255);
    g_trkAlpha->callback(onSlider, (void*)(intptr_t)0);
    y += 28;
    g_lblRadius = AddLabel(p, 0, y, 112, 20, L"圆角半径：0");
    g_trkRadius = AddSlider(p, 118, y, kPW - 118, 20, 14);
    g_trkRadius->callback(onSlider, (void*)(intptr_t)1);
    RefreshTuneLabels();
    if (!g_s.beautify) {
        g_trkAlpha->deactivate();
        g_trkRadius->deactivate();
    }
    p->end();
}

static void BuildTabGroup() {
    Fl_Group* p = NewPanel(3);
    int half = (kPH - 100) / 2;
    AddLabel(p, 0, 4, 260, 20, L"启动组（关键字 → 文件）：");
    g_edtLaunch = AddMultiline(p, 28, kPW, half);
    FillEditor(g_edtLaunch, g_s.launchText);
    AddLabel(p, 0, 34 + half, 260, 20, L"关闭组（关键字 → 进程）：");
    g_edtKill = AddMultiline(p, 58 + half, kPW, half);
    FillEditor(g_edtKill, g_s.killText);
    AddHint(p, 0, kPH - 36, kPW, 36,
            L"每行：关键字 + 空格 + 目标（多个用 ; 分隔）；# 开头为注释。启动组填文件全路径，"
            L"关闭组填进程名（可省 .exe）");
    p->end();
}

static void BuildTabWeight() {
    Fl_Group* p = NewPanel(4);
    AddCheck(p, 0, 4, kPW, L"启用点击权重记忆（相同搜索词下点过的条目排序提前）",
             &g_s.weightEnabled);
    AddChoiceRow(p, 36, L"写入时机：", ft::WeightFlushOptions(), &g_s.weightFlush);
    AddChoiceRow(p, 70, L"条目上限：", ft::WeightMaxOptions(), &g_s.weightMaxEntries);
    Fl_Button* wipe = AddButton(p, 0, 106, 180, L"清空权重数据…");
    WipeCtl* wc = new WipeCtl();
    wc->btn = wipe;
    wc->idle = u8(L"清空权重数据…");
    wc->kind = 1;
    wc->armed = false;
    wipe->callback(onWipe, wc);
    g_lblWeightCount = AddLabel(p, 192, 106, kPW - 192, kRowH, L"当前已记忆 0 条权重数据", 10);
    AddHint(p, 0, 142, kPW, kPH - 142,
            L"权重按「搜索词 → 文件完整路径」记录，不同搜索词互相独立；f/d 前缀只对后面的关键词"
            L"记录，网页前缀（如 gg、bd）的点击不记录。数据文件：%APPDATA%\\Flowtary\\weights.dat");
    p->end();
}

static void RestoreExcludeDefault(Fl_Widget*) {
    FillEditor(g_edtExclude, ft::DefaultExcludePathsText());
    g_s.excludeText = ft::DefaultExcludePathsText();
    UpdateExcludeCount();
}
static void BuildTabExclude() {
    Fl_Group* p = NewPanel(5);
    AddCheck(p, 0, 4, kPW, L"不搜索 . 开头的文件与文件夹（.git / .gitignore …）",
             &g_s.skipDotFolders);
    g_edtExclude = AddMultiline(p, 32, kPW, kPH - 112);
    FillEditor(g_edtExclude, g_s.excludeText);
    Fl_Button* dft = AddButton(p, 0, kPH - 72, 110, L"恢复默认");
    dft->callback(RestoreExcludeDefault);
    g_lblExcludeCount = AddLabel(p, 122, kPH - 72, kPW - 122, kRowH, L"当前已记忆 0 条排除项", 10);
    AddHint(p, 0, kPH - 42, kPW, 42,
            L"每行一条路径，大小写不敏感，支持通配符 * ?。以排除项开头的文件/文件夹/程序会被过滤。"
            L"网页与一键组不受影响，. 开头的文件/文件夹由上方开关统一跳过。保存后生效。");
    p->end();
}

static void BuildTabShell() {
    Fl_Group* p = NewPanel(6);
    AddChoiceRow(p, 4, L"Shell 程序：", ft::ShellTypeOptions(), &g_s.shellType, 1);
    // 只有「自定义 Shell」才出现的三行
    g_lblShellPath = AddLabel(p, 0, 36, 160, 20, L"自定义 Shell 路径：");
    g_edtShellPath = AddInput(p, 166, 36, kPW - 166);
    g_edtShellPath->value(u8(g_s.shellCustomPath).c_str());
    g_lblShellArgs = AddLabel(p, 0, 66, 160, 20, L"自定义启动参数：");
    g_edtShellArgs = AddInput(p, 166, 66, kPW - 166);
    g_edtShellArgs->value(u8(g_s.shellCustomArgs).c_str());
    Fl_Box* cHint = AddLabel(p, 166, 96, kPW - 166, 16, L"提示：{c} = 命令", 10, kSub);
    g_shellCustom = {g_lblShellPath, g_edtShellPath, g_lblShellArgs, g_edtShellArgs, cHint};
    // 其下方的各行：自定义块隐藏时整体上移 kShellCustomH
    auto below = [&](Fl_Widget* w, int x, int y) {
        g_shellRows.push_back(ShellRow{w, x, y, y - kShellCustomH});
    };
    int y = 142;
    below(AddLabel(p, 0, y, 160, 20, L"默认工作目录："), 0, y);
    below(AddChoice(p, 166, y, kPW - 166, ft::ShellCwdOptions(), &g_s.shellDefaultCwd, 0)->w, 166,
          y);
    y += 34;
    below(AddCheck(p, 0, y, kPW, L"前台显示输出窗口（否则后台静默执行）", &g_s.shellShowWindow), 0,
          y);
    y += 30;
    below(AddCheck(p, 0, y, kPW, L"合并同进程窗口（多窗口进程显示为分组）", &g_s.winGroupProc), 0,
          y);
    y += 30;
    below(AddCheck(p, 0, y, kPW, L"显示 UWP 应用窗口", &g_s.winShowUwp), 0, y);
    y += 30;
    below(AddCheck(p, 0, y, kPW, L"结果副标题显示进程名", &g_s.winShowProc), 0, y);
    y += 34;
    below(AddLabel(p, 0, y, 160, 20, L"窗口缓存刷新间隔(秒)："), 0, y);
    below(AddChoice(p, 166, y, kPW - 166, ft::WinCacheOptions(), &g_s.winCacheSec, 0)->w, 166, y);
    ApplyShellLayout();
    p->end();
}

static void BuildTabCmd() {
    Fl_Group* p = NewPanel(7);
    AddCheck(p, 0, 4, kPW, L"top 命令（空格+top 回车，置顶/取消置顶当前窗口）", &g_s.topEnabled);
    AddCheck(p, 0, 34, kPW, L"cmd 命令（空格+cmd 执行 Shell 命令，原 “> 命令”）", &g_s.cmdEnabled);
    AddCheck(p, 0, 64, kPW, L"w 命令（空格+w 切换窗口，原 “< 关键词”）", &g_s.winEnabled);
    AddCheck(p, 0, 94, kPW, L"ss 命令（空格+ss 截图，空格+ss pin 贴图）", &g_s.captureEnabled);
    AddHint(p, 0, 128, kPW, 34, L"关闭后对应前缀不再被识别，搜索时按普通关键词处理。保存后生效。");
    p->end();
}

static void BuildTabCapture() {
    Fl_Group* p = NewPanel(8);
    g_lblCaptureStatus = AddLabel(p, 0, 4, kPW, 22, L"", 11);
    g_btnDlSC = AddButton(p, 0, 34, 300, L"下载 ScreenCapture.exe（约 1MB）");
    g_btnDlIR = AddButton(p, 0, 66, 300, L"下载 ImageReader.exe（约 25MB，文字识别必需）");
    g_dl[0].url = L"https://github.com/xland/ScreenCapture/releases/latest/download/ScreenCapture.exe";
    g_dl[0].dest = FlowtaryDir() + L"ScreenCapture.exe";
    g_dl[0].btn = g_btnDlSC;
    g_dl[0].labelIdle = L"下载 ScreenCapture.exe（约 1MB）";
    g_dl[0].labelBusy = L"下载中…";
    g_dl[1].url = L"https://github.com/xland/ImageReader/releases/latest/download/ImageReader.exe";
    g_dl[1].dest = FlowtaryDir() + L"ImageReader.exe";
    g_dl[1].btn = g_btnDlIR;
    g_dl[1].labelIdle = L"下载 ImageReader.exe（约 25MB，文字识别必需）";
    g_dl[1].labelBusy = L"下载中…（约 25MB，请稍候）";
    g_btnDlSC->callback(onDownload, &g_dl[0]);
    g_btnDlIR->callback(onDownload, &g_dl[1]);
    AddHint(p, 0, 104, kPW, 34,
            L"下载后将自动保存到 Flowtary.exe 同目录。OCR 功能（空格+ss ocr）需要 ImageReader.exe "
            L"才可使用。");
    AddHint(p, 0, 144, kPW, 40,
            L"提示：文字识别结果会自动复制到剪贴板。支持的命令：空格+ss（截图）/ 空格+ss pin（贴图）"
            L"/ 空格+ss ocr（文字识别）");
    p->end();
}

static void OpenLearnDir(Fl_Widget*) {
    std::wstring dir = ft::LearnedLinkDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(g_hSelf, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW);
}
static void BuildTabLearn() {
    Fl_Group* p = NewPanel(9);
    AddCheck(p, 0, 4, kPW, L"启用自学习（f/d 搜索打开过的条目自动记忆，下次同词搜索置顶）",
             &g_s.learnEnabled);
    AddChoiceRow(p, 36, L"记录范围：", ft::LearnScopeOptions(), &g_s.learnScope);
    AddChoiceRow(p, 70, L"条目上限：", ft::LearnMaxOptions(), &g_s.learnMaxEntries);
    AddChoiceRow(p, 104, L"过期清理：", ft::LearnExpireOptions(), &g_s.learnExpire);
    Fl_Button* open = AddButton(p, 0, 140, 160, L"打开快捷方式目录");
    open->callback(OpenLearnDir);
    Fl_Button* wipe = AddButton(p, 172, 140, 180, L"清空自学习数据…");
    WipeCtl* wc = new WipeCtl();
    wc->btn = wipe;
    wc->idle = u8(L"清空自学习数据…");
    wc->kind = 2;
    wc->armed = false;
    wipe->callback(onWipe, wc);
    g_lblLearnCount = AddLabel(p, 0, 170, kPW, 20, L"当前已学习 0 条快捷方式", 10);
    AddHint(p, 0, 196, kPW, kPH - 196,
            L"仅在 f / d 前缀搜索生效：打开成功后按「搜索词 → 路径」记入 "
            L"%APPDATA%\\Flowtary\\learned.dat，并在同目录 learned 文件夹镜像一个 .lnk 快捷方式"
            L"（可拷贝、可备份）。同词或前缀词搜索时学习项置顶，exe 优先；删除 .lnk 即遗忘对应条目。"
            L"写盘时机沿用「搜索权重」页设置。");
    p->end();
}

static void RestoreRulesDefault(Fl_Widget*) {
    FillEditor(g_edtRules, ft::DefaultRulesText());
    g_s.rulesText = ft::DefaultRulesText();
}
static void BuildBottomRow() {
    int y = kH - 44;
    g_btnReset = new Fl_Button(kContentX, y, 90, kRowH);
    g_btnReset->copy_label(CL(L"恢复默认"));
    g_btnReset->callback(RestoreRulesDefault);
    g_btnReset->hide();
    Fl_Button* save = new Fl_Button(kContentX + 100, y, 90, kRowH);
    save->copy_label(CL(L"保存"));
    save->callback(onSave);
    Fl_Button* cancel = new Fl_Button(kContentX + 200, y, 90, kRowH);
    cancel->copy_label(CL(L"取消"));
    cancel->callback(onCancel);
    g_win->end();
}

// ---------------- 命令行 ----------------
static void ParseArgs() {
    int wc = 0;
    LPWSTR* wl = CommandLineToArgvW(GetCommandLineW(), &wc);
    if (!wl) return;
    for (int i = 0; i + 1 < wc; ++i) {
        if (wcscmp(wl[i], ipc::kArgIpc) == 0)
            g_hMain = (HWND)(ULONG_PTR)_wtoi64(wl[i + 1]);
        else if (wcscmp(wl[i], ipc::kArgMainExe) == 0)
            g_mainExe = wl[i + 1];
    }
    LocalFree(wl);
}

static int AppMain() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ParseArgs();
    if (!g_hMain || !IsWindow(g_hMain)) return 1;  // 没有主进程就无从同步，直接退出

    g_s.Load();
    if (g_s.themeIdx < 0 || g_s.themeIdx >= kThemeCount) g_s.themeIdx = 0;
    if ((int)g_s.tune.size() < kThemeCount) g_s.tune.resize(kThemeCount);
    DWORD lastTab = 0;
    if (ft::RegGetDword(L"SettingsTab", lastTab) && lastTab < kTabCount) g_tab = (int)lastTab;

    Fl::set_font(FL_HELVETICA, "Microsoft YaHei UI");
    ApplyThemeColors();

    g_win = new SettingsWin();
    BuildSidebar();
    BuildTabGeneral();
    BuildTabWeb();
    BuildTabTheme();
    BuildTabGroup();
    BuildTabWeight();
    BuildTabExclude();
    BuildTabShell();
    BuildTabCmd();
    BuildTabCapture();
    BuildTabLearn();
    BuildBottomRow();

    // 居中到主窗所在显示器的工作区：入参是物理像素，FLTK 返回的是逻辑坐标
    RECT mr{};
    if (GetWindowRect(g_hMain, &mr)) {
        int X, Y, W, H;
        Fl::screen_work_area(X, Y, W, H, (mr.left + mr.right) / 2, (mr.top + mr.bottom) / 2);
        g_win->position(X + (W - kW) / 2, Y + (H - kH) / 2);
    }
    // 不能用 show(0, nullptr)：Fl_Window::show(argc,argv) 会无条件把 argv[0] 当类名
    // （Fl_arg.cxx 里的 fl_filename_name(argv[0])），空 argv 直接访问违例。
    g_win->show();
    g_hSelf = (HWND)fl_xid(g_win);
    if (g_hSelf) {
        g_prevProc = (WNDPROC)SetWindowLongPtrW(g_hSelf, GWLP_WNDPROC, (LONG_PTR)SubProc);
        ApplyTitlebarStyle();
        ApplyGhostStyle();
    }

    UpdateExcludeCount();
    ShowTab(g_tab);

    ipc::Header hdr{g_hSelf};
    SendToMain(ipc::MsgHello, &hdr, sizeof(hdr));
    SendToMain(ipc::MsgQueryStats, &hdr, sizeof(hdr));

    Fl::add_handler(OnEnterKey);
    while (Fl::wait() > 0 && !g_quit) {
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return AppMain(); }
