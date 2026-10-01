#pragma once
// 主题预设表：主程序与设置 sidecar 共用，避免两份配色漂移。
#include <windows.h>
#include <vector>

struct Theme {
    const WCHAR* name;   // 显示名
    bool dark;           // 菜单 uxtheme 用深色（false=浅色主题菜单）
    BYTE alpha;          // 主窗/设置窗透明度 0-255（255=不透明）
    int fontInput;       // 输入框逻辑字号
    int fontList;        // 列表逻辑字号
    COLORREF bg, bg2;    // 主窗背景（bg2 != bg 时垂直渐变）
    COLORREF text;       // 主文字
    COLORREF sub;        // 副标题（路径）
    COLORREF hintText;   // 占位/提示文字
    COLORREF selBg;      // 选中行背景
    COLORREF divider;    // 分隔线
    COLORREF menuBg;     // 菜单/弹出列表背景
    COLORREF menuHi;     // 菜单/控件选中高亮
    COLORREF editBg;     // 设置窗规则编辑框背景
    COLORREF border;     // GDI 描边回退（同时作为默认描边色）
    bool stars;          // 星空点点缀

    // —— 1. 形状几何（逻辑 px，绘制时经 S() 缩放）——
    int radiusWindow = 10;  // 窗口圆角
    int radiusCard   = 8;   // 卡片/结果面板圆角
    int radiusButton = 6;   // 按钮圆角
    int radiusInput  = 6;   // 输入框圆角
    int radiusSmall  = 3;   // 小控件（徽章/勾选/滑块）圆角
    int borderWidth  = 1;   // 描边粗细：0 或 1
    bool capsuleButtons = false;  // 胶囊按钮：radius = 控件半高

    // —— 2. 颜色系统（扩展）——
    COLORREF accent   = RGB(64, 134, 196);  // 主强调色（按钮/选中/高亮/进度）
    COLORREF accent2  = RGB(94, 162, 224);  // 次强调（hover/focus）
    COLORREF bgCard   = RGB(28, 28, 28);    // 卡片/结果面板背景（> 窗体 bg）
    COLORREF bgInput  = RGB(24, 24, 24);    // 输入框背景（> 卡片 bg）
    COLORREF textTitle = RGB(255, 255, 255); // 标题级文字
    COLORREF textBody  = RGB(235, 235, 235); // 正文（≈原 text）
    COLORREF textSec   = RGB(150, 150, 150); // 次要（≈原 sub）
    COLORREF textDis   = RGB(110, 110, 110); // 禁用/占位（≈原 hintText）
    COLORREF statusOK   = RGB(76, 175, 80);  // 成功绿
    COLORREF statusWarn = RGB(245, 166, 35); // 警告橙
    COLORREF statusErr  = RGB(229, 83, 75);  // 错误红

    // —— 3. 阴影/层级（逻辑 px + alpha 0-255）；固定 3 组可复用参数 ——
    int shWinX = 4, shWinY = 5, shWinBlur = 16, shWinA = 51; COLORREF shWinColor = RGB(0, 0, 0);
    int shCardX = 2, shCardY = 3, shCardBlur = 10, shCardA = 30; COLORREF shCardColor = RGB(0, 0, 0);
    int shHoverX = 5, shHoverY = 8, shHoverBlur = 20, shHoverA = 40; COLORREF shHoverColor = RGB(0, 0, 0);
    int shInset = 6;  // 内阴影深度（输入凹陷/玻璃高光用，统一弱值）

    // —— 7. 动效 ——
    int animMs = 200;    // 过渡时长 150-350，ease-out
    int animCurve = 0;   // 0=ease-out-cubic，1=ease-out-quad

    // —— 9. 图标与装饰 ——
    bool accentStrip = true;   // 顶部强调色条
    bool cornerGlow  = false;  // 低透明度角部辉光
    bool iconFilled  = false;  // 图标风格：false=线性描边 / true=填充
};

inline Theme kThemes[] = {
    {L"黑色简洁", true, 255, 16, 13,
     RGB(0, 0, 0), RGB(0, 0, 0), RGB(255, 255, 255), RGB(140, 140, 140),
     RGB(150, 150, 150), RGB(51, 51, 51), RGB(34, 34, 34), RGB(28, 28, 28),
     RGB(51, 51, 51), RGB(24, 24, 24), RGB(80, 80, 80), false},
    {L"透明Mac黑暗", true, 224, 16, 13,
     RGB(16, 16, 16), RGB(12, 12, 12), RGB(255, 255, 255), RGB(153, 153, 153),
     RGB(135, 135, 135), RGB(42, 42, 42), RGB(33, 33, 33), RGB(28, 28, 28),
     RGB(51, 51, 51), RGB(18, 18, 18), RGB(85, 85, 85), false},
    {L"透明蓝色", true, 224, 16, 13,
     RGB(26, 52, 96), RGB(12, 23, 48), RGB(234, 243, 255), RGB(127, 163, 201),
     RGB(110, 143, 186), RGB(31, 58, 92), RGB(40, 72, 110), RGB(20, 39, 63),
     RGB(31, 58, 92), RGB(15, 30, 54), RGB(63, 107, 150), false},
    {L"星空风格", true, 250, 17, 14,
     RGB(12, 16, 48), RGB(4, 5, 15), RGB(232, 236, 255), RGB(138, 146, 200),
     RGB(112, 119, 173), RGB(30, 36, 72), RGB(40, 48, 92), RGB(13, 18, 38),
     RGB(30, 36, 72), RGB(9, 13, 32), RGB(74, 86, 160), true},
    {L"日出浅白", false, 255, 16, 13,
     RGB(244, 244, 244), RGB(244, 244, 244), RGB(26, 26, 26), RGB(110, 110, 110),
     RGB(155, 160, 166), RGB(223, 223, 223), RGB(208, 208, 208), RGB(255, 255, 255),
     RGB(228, 228, 228), RGB(243, 243, 243), RGB(196, 196, 196), false,
     10, 8, 6, 6, 3, 1, false,
     RGB(0x3B,0x7E,0xA8), RGB(0x63,0xA8,0xD4),
     RGB(255,255,255), RGB(252,252,252),
     RGB(26,26,26), RGB(26,26,26), RGB(110,110,110), RGB(155,160,166),
     RGB(0x2E,0x9E,0x5B), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     4, 5, 16, 51, RGB(0x82,0x9C,0xB3),
     5, 8, 20, 40, RGB(0x82,0x9C,0xB3),
     2, 3, 10, 30, RGB(0x82,0x9C,0xB3),
     6, 220, 0, true, false, false},

    // 6. Ice-Quartz（浅冷色，真实 Mica / Win10 回退哑光）
    {L"Ice-Quartz", false, 255, 16, 13,
     RGB(0xEE,0xF3,0xF7), RGB(0xEE,0xF3,0xF7), RGB(0x1A,0x27,0x33), RGB(0x64,0x74,0x84),
     RGB(0x9A,0xA8,0xB6), RGB(0xDD,0xE6,0xEE), RGB(0xC8,0xD6,0xE2), RGB(0xFF,0xFF,0xFF),
     RGB(0xE7,0xEE,0xF3), RGB(0xF4,0xF8,0xFB), RGB(0xB9,0xC9,0xD8), false,
     10, 8, 6, 6, 3, 1, false,
     RGB(0x3B,0x7E,0xA8), RGB(0x63,0xA8,0xD4),
     RGB(0xFF,0xFF,0xFF), RGB(0xF4,0xF8,0xFB),
     RGB(0x1A,0x27,0x33), RGB(0x1A,0x27,0x33), RGB(0x64,0x74,0x84), RGB(0x9A,0xA8,0xB6),
     RGB(0x2E,0x9E,0x5B), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     4, 5, 16, 51, RGB(0x82,0x9C,0xB3),
     5, 8, 20, 40, RGB(0x82,0x9C,0xB3),
     2, 3, 10, 30, RGB(0x82,0x9C,0xB3),
     6, 220, 0, true, false, false},

    // 7. Obsidian（极深暗黑工具风，真实 Mica + 微弱蓝辉光）
    {L"Obsidian", true, 255, 16, 13,
     RGB(0x0D,0x0F,0x14), RGB(0x0D,0x0F,0x14), RGB(0xFF,0xFF,0xFF), RGB(0x88,0x96,0xB0),
     RGB(0x50,0x5A,0x6E), RGB(0x1A,0x1E,0x27), RGB(0x2C,0x34,0x42), RGB(0x1A,0x1E,0x27),
     RGB(0x10,0x13,0x18), RGB(0x12,0x15,0x1C), RGB(0x3A,0x44,0x55), false,
     8, 6, 5, 5, 3, 1, false,
     RGB(0x40,0x86,0xC4), RGB(0x5E,0xA2,0xE0),
     RGB(0x1A,0x1E,0x27), RGB(0x12,0x15,0x1C),
     RGB(0xFF,0xFF,0xFF), RGB(0xFF,0xFF,0xFF), RGB(0x88,0x96,0xB0), RGB(0x50,0x5A,0x6E),
     RGB(0x3E,0xC0,0x7A), RGB(0xE6,0xA6,0x23), RGB(0xE5,0x53,0x4B),
     2, 3, 12, 89, RGB(0,0,0),
     0, 0, 8, 20, RGB(0,0,0),
     1, 2, 6, 22, RGB(0,0,0),
     6, 200, 0, true, true, false},

    // 8. Honey-Amber（暖暗色，真实 Mica / Win10 回退哑光）
    {L"Honey-Amber", true, 255, 16, 13,
     RGB(0x1C,0x19,0x17), RGB(0x1C,0x19,0x17), RGB(0xF8,0xE9,0xD7), RGB(0xB8,0xA4,0x8C),
     RGB(0x7E,0x6E,0x5A), RGB(0x2D,0x28,0x23), RGB(0x3E,0x36,0x2E), RGB(0x2D,0x28,0x23),
     RGB(0x18,0x15,0x12), RGB(0x22,0x1E,0x19), RGB(0x4A,0x40,0x34), false,
     7, 8, 7, 7, 3, 1, false,
     RGB(0xD4,0x80,0x38), RGB(0xE8,0xA2,0x5C),
     RGB(0x2D,0x28,0x23), RGB(0x22,0x1E,0x19),
     RGB(0xF8,0xE9,0xD7), RGB(0xF8,0xE9,0xD7), RGB(0xB8,0xA4,0x8C), RGB(0x7E,0x6E,0x5A),
     RGB(0x6F,0xCF,0x8A), RGB(0xE8,0xA2,0x5C), RGB(0xE5,0x6B,0x4B),
     3, 4, 14, 66, RGB(0x0A,0x08,0x06),
     1, 2, 7, 24, RGB(0x0A,0x08,0x06),
     1, 2, 7, 24, RGB(0x0A,0x08,0x06),
     6, 200, 0, true, false, false},
};

// 主题微调快照（透明度/圆角半径），随主题索引一一对应，持久化到注册表 ThemeTune blob
struct ThemeTune {
    int alpha = 255;     // 透明度 0-255（影响弹窗等离屏层）
    int radius = 10;     // 窗体圆角（卡片/按钮/输入按比例推导）
};

inline const int kThemeCount = (int)(sizeof(kThemes) / sizeof(kThemes[0]));
