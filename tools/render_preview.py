# -*- coding: utf-8 -*-
"""
flowtary 主窗离屏渲染器 —— 复刻 main.cpp Paint() 的几何/颜色/SDF 抗锯齿公式。
不启动 GUI、不截图，直接生成主界面 PNG，用于自看效果、迭代打磨。
用法：python tools/render_preview.py [obsidian|classic]
改 main.cpp 绘制参数后同步这里的 LAYOUT/THEME 再渲染。
"""
import sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------- 布局参数（逻辑像素，与 main.cpp 同步） ----------------
LAYOUT = dict(
    W=680, SCALE=1.0,
    winPad=14,            # 搜索框/列表距窗口左右上的留白
    winRadius=16,         # 窗口圆角
    winBottomPad=14,      # 列表底部到窗口底
    searchH=52,           # 搜索框高度
    searchRadius=12,      # 搜索框圆角
    gapSearchList=14,     # 搜索框底 → 列表第一项顶
    rowH=54,              # 行高
    selRadius=11,         # 选中胶囊圆角
    selPadX=0,            # 选中胶囊相对列表区的左右内缩
    iconSize=38,          # 图标尺寸
    iconRadius=11,        # 图标圆角(squircle)
    iconGapL=13,          # 图标距胶囊左缘
    iconTextGap=13,       # 图标右缘 → 文字左缘
    titleSize=16, subSize=12,
    badgeW=30, badgeH=19, badgeR=5, badgeGapR=15,
)

THEME_NAME = sys.argv[1] if len(sys.argv) > 1 else 'obsidian'
THEMES = {
 'obsidian': dict(
    dark=True, glass=(13,15,20), glassA=0.72,
    text=(255,255,255), textSec=(136,150,176),
    searchBg=(26,30,39), searchBorder=(58,68,85),
    selFill=(46,124,124), selText=(255,255,255),
    badgeBg=(40,48,60), badgeBorder=(90,102,122), badgeText=(150,162,182),
    accent=(64,134,196), accent2=(94,162,224),
    wall_top=(26,22,52), wall_bot=(10,14,30), wall_glow=[(0.15,0.12,(64,150,150)),(0.85,0.78,(96,64,150))],
    fontList=15),
 'classic': dict(
    dark=True, glass=(10,10,12), glassA=0.76,
    text=(255,255,255), textSec=(150,150,150),
    searchBg=(28,28,30), searchBorder=(70,70,72),
    selFill=(46,124,124), selText=(255,255,255),
    badgeBg=(38,38,40), badgeBorder=(90,90,92), badgeText=(150,150,150),
    accent=(64,134,196), accent2=(94,162,224),
    wall_top=(30,26,34), wall_bot=(8,10,16), wall_glow=[(0.2,0.15,(60,70,130)),(0.8,0.8,(100,50,90))],
    fontList=15),
}
T = THEMES[THEME_NAME]
L = LAYOUT
S = lambda v: int(v * L['SCALE'] + 0.5)
W = L['W']

# ---------------- 颜色工具 ----------------
def blend(a, b, t):
    a = np.asarray(a, float); b = np.asarray(b, float)
    return np.floor(a + (b - a) * t)
def darken(c, p):
    c = np.asarray(c, float); return np.floor(c * (100 - np.asarray(p, float)) / 100.0)
def lighten(c, p):
    c = np.asarray(c, float); return np.minimum(255, np.floor(c * (100 + np.asarray(p, float)) / 100.0))

img = None; H = 0
def _coverage(w, h, rad):
    rad = max(0, min(rad, min(w, h) // 2))
    xs = np.arange(w, dtype=np.float64) + 0.5
    ys = np.arange(h, dtype=np.float64) + 0.5
    hw, vh = w * 0.5, h * 0.5
    innerX = max(0.0, hw - 0.5 - rad); innerY = max(0.0, vh - 0.5 - rad)
    qx = np.maximum(np.abs(xs - hw) - innerX, 0.0)
    qy = np.maximum(np.abs(ys - vh) - innerY, 0.0)
    QX, QY = np.meshgrid(qx, qy)
    return np.sqrt(QX * QX + QY * QY), rad

def fill_round_rect(rect, rad, colfn):
    l, t, r, b = [int(v) for v in rect]
    w, h = r - l, b - t
    if w <= 0 or h <= 0 or l < 0 or t < 0 or r > W or b > H: return
    dist, rad = _coverage(w, h, rad)
    cov = np.clip(0.5 - (dist - rad), 0.0, 1.0)
    xs = np.arange(w, dtype=np.float64); ys = np.arange(h, dtype=np.float64)
    U = np.broadcast_to((xs / max(1, w - 1))[None, :], (h, w))
    V = np.broadcast_to((ys / max(1, h - 1))[:, None], (h, w))
    color = np.broadcast_to(np.asarray(colfn(U, V), float), (h, w, 3))
    a = cov[:, :, None]
    src = img[t:b, l:r]
    src[:] = src * (1 - a) + color * a

def stroke_round_rect(rect, rad, col, width=1):
    l, t, r, b = [int(v) for v in rect]
    w, h = r - l, b - t
    if w <= 2 or h <= 2 or l < 0 or t < 0 or r > W or b > H: return
    if width < 1: width = 1
    dist, rad = _coverage(w, h, rad)
    cov = np.clip(width * 0.5 + 0.5 - np.abs(dist - rad), 0.0, 1.0)
    a = cov[:, :, None]
    src = img[t:b, l:r]
    src[:] = src * (1 - a) + np.asarray(col, float)[None, None, :] * a

def fill_rect(rect, col):
    l, t, r, b = [int(v) for v in rect]
    l = max(0, l); t = max(0, t); r = min(W, r); b = min(H, b)
    if r <= l or b <= t: return
    img[t:b, l:r] = np.asarray(col, float)[None, None, :]

# ---------------- 文字 ----------------
FONT_REG = 'C:/Windows/Fonts/msyh.ttc'
FONT_BOLD = 'C:/Windows/Fonts/msyhbd.ttc'
_fc = {}
def font(size, bold=False):
    k = (size, bold)
    if k not in _fc: _fc[k] = ImageFont.truetype(FONT_BOLD if bold else FONT_REG, size)
    return _fc[k]
def text(x, y, s, size, col, bold=False, anchor='lm', room=None):
    if not s: return
    f = font(size, bold); ds = s
    if room is not None and f.getlength(s) > room:
        cut = s
        while cut and f.getlength(cut + '…') > room: cut = cut[:-1]
        ds = cut + '…'
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((int(x), int(y)), ds, font=f, fill=(*map(int, col), 255), anchor=anchor)
    arr = np.asarray(layer, float); a = arr[:, :, 3:4] / 255.0
    img[:] = img * (1 - a) + arr[:, :, :3] * a

# ---------------- 壁纸 + 毛玻璃 ----------------
def make_wallpaper():
    yy = np.linspace(0, 1, H)[:, None, None]; xx = np.linspace(0, 1, W)[None, :, None]
    base = np.asarray(T['wall_top'], float)[None, None, :] * (1 - yy) + np.asarray(T['wall_bot'], float)[None, None, :] * yy
    base = np.broadcast_to(base, (H, W, 3)).copy()
    for cx, cy, col in T['wall_glow']:
        d2 = ((xx - cx) / 0.55) ** 2 + ((yy - cy) / 0.55) ** 2
        a = np.clip(1 - d2, 0, None) ** 2 * 0.55
        base = base + (np.asarray(col, float) - base) * a
    return np.clip(base, 0, 255)

def apply_glass(wall):
    # 窗口圆角外的壁纸压暗（失焦桌面），窗口内叠半透明深色玻璃
    dist, rad = _coverage(W, H, S(L['winRadius']))
    inside = (dist <= rad)[:, :, None]
    outside = np.clip(wall * 0.42, 0, 255)
    glass = wall * (1 - T['glassA']) + np.asarray(T['glass'], float) * T['glassA']
    img[:] = np.where(inside, glass, outside)

# ================= 绘制主界面 =================
ROWS = [
    dict(title='Safari', sub='/Application/Safari.app', short='Safari', hk='1', tier=4, usage=128, icon=(58,152,232), glyph='compass'),
    dict(title='System Settings', sub='/Application/System Settings.app', short='System Settings', hk='2', tier=3, usage=41, icon=(150,158,170), glyph='gear'),
    dict(title='Jahangir Sadri', sub='View contact in Alfred', short='Jahangir Sadri', hk='3', tier=2, usage=9, icon=(210,180,150), glyph='person'),
    dict(title='Spell Word...', sub='', short='Spell Word...', hk='4', tier=1, usage=3, icon=(235,235,235), glyph='Aa'),
    dict(title='Bitwig Studio', sub='/Application/Bitwig Studio.app', short='Bitwig Studio', hk='5', tier=0, usage=0, icon=(240,120,40), glyph='dots'),
    dict(title='Visual Studio Code', sub='/Application/Visual Studio Code.app', short='Visual Studio Code', hk='6', tier=0, usage=0, icon=(70,150,220), glyph='code'),
]
SEL = 0

def draw_icon(x, y, size, row):
    rad = S(L['iconRadius'])
    # squircle 底
    def ic(U, V):
        base = np.asarray(row['icon'], float)
        # 顶部略亮、底部略暗的立体感
        vv = V
        base = base + (np.asarray(T['text'], float) - base) * (np.clip(0.16 - vv * 0.2, 0, None))[..., None]
        base = base * (1 - np.clip((vv - 0.72) / 0.28, 0, None) * 0.18)[..., None]
        return base
    fill_round_rect((x, y, x + size, y + size), rad, ic)
    stroke_round_rect((x, y, x + size, y + size), rad, np.asarray(row['icon'], float) * 0.6, 1)
    # 简易字形
    cx, cy = x + size / 2, y + size / 2
    g = row['glyph']
    if g == 'compass':
        for a in range(0, 360, 30):
            r1, r2 = size * 0.22, size * 0.30
            rad_ = math.radians(a)
            fill_rect((cx + r1 * math.cos(rad_) - 1, cy + r1 * math.sin(rad_) - 1,
                       cx + r2 * math.cos(rad_) + 1, cy + r2 * math.sin(rad_) + 1), (255, 255, 255))
        fill_rect((cx - 2, cy - size * 0.18, cx + 2, cy + size * 0.18), (240, 90, 80))
    elif g == 'gear':
        stroke_round_rect((cx - size*0.16, cy - size*0.16, cx + size*0.16, cy + size*0.16), size*0.06, (110,110,115), 2)
    elif g == 'person':
        fill_rect((cx - size*0.10, cy - size*0.16, cx + size*0.10, cy + size*0.04), (120,100,85))
        fill_rect((cx - size*0.18, cy + size*0.06, cx + size*0.18, cy + size*0.20), (120,100,85))
    elif g == 'Aa':
        text(cx, cy, 'Aa', 15, (30,30,30), bold=True, anchor='mm')
    elif g == 'dots':
        for gy in range(3):
            for gx in range(3):
                fill_rect((cx - 9 + gx*7, cy - 9 + gy*7, cx - 5 + gx*7, cy - 5 + gy*7), (255,255,255))
    elif g == 'code':
        text(cx, cy, '</>', 11, (255,255,255), bold=True, anchor='mm')

def render():
    global img, H
    pad = S(L['winPad']); searchH = S(L['searchH']); rowH = S(L['rowH'])
    n = len(ROWS)
    rowsTop = pad + searchH + S(L['gapSearchList'])
    H = rowsTop + n * rowH + S(L['winBottomPad'])
    img = np.zeros((H, W, 3), float)
    wall = make_wallpaper()
    apply_glass(wall)

    # —— 独立搜索框 ——
    sr = (pad, pad, W - pad, pad + searchH)
    fill_round_rect(sr, S(L['searchRadius']),
                    lambda U, V: np.broadcast_to(np.asarray(T['searchBg'], float), (*U.shape, 3)))
    stroke_round_rect(sr, S(L['searchRadius']), T['searchBorder'], 1)
    # 放大镜图标
    gx, gy, gs = pad + S(16), pad + (searchH - S(18)) // 2, S(18)
    ox, oy = gx + gs / 2, gy + gs / 2
    rr = gs * 0.3
    for a in range(0, 360, 20):
        ra = math.radians(a)
        px, py = ox + rr * math.cos(ra), oy + rr * math.sin(ra)
        fill_rect((px - 1, py - 1, px + 1, py + 1), T['textSec'])
    fill_rect((ox + rr*0.7, oy + rr*0.7, ox + rr*1.25, oy + rr*1.25), T['textSec'])
    tx = gx + gs + S(12)
    text(tx, pad + searchH / 2, 'Safari', L['titleSize'], T['text'], anchor='lm')
    # 光标
    tw = font(L['titleSize']).getlength('Safari')
    fill_rect((tx + tw + 1, pad + searchH/2 - S(11), tx + tw + 1 + S(2), pad + searchH/2 + S(11)), T['text'])
    # 搜索框右侧小图标
    rx = W - pad - S(26)
    fill_round_rect((rx, pad + (searchH - S(22))//2, rx + S(22), pad + (searchH - S(22))//2 + S(22)), S(7),
                    lambda U, V: np.broadcast_to(np.asarray((235,235,235), float), (*U.shape, 3)))

    # —— 结果行（直接铺在玻璃上，无卡片）——
    listL = pad + S(L['selPadX']); listR = W - pad - S(L['selPadX'])
    iconX = listL + S(L['iconGapL'])
    textX = iconX + S(L['iconSize']) + S(L['iconTextGap'])
    badgeRight = listR - S(L['badgeGapR'])
    for i, row in enumerate(ROWS):
        y = rowsTop + i * rowH
        if i == SEL:
            pill = (listL, y, listR, y + rowH)
            fill_round_rect(pill, S(L['selRadius']),
                            lambda U, V: np.broadcast_to(np.asarray(T['selFill'], float), (*U.shape, 3)))
        iconY = y + (rowH - S(L['iconSize'])) // 2
        draw_icon(iconX, iconY, S(L['iconSize']), row)
        # 标题 / 副标题 上下双行
        titleCol = T['selText'] if i == SEL else T['text']
        subCol = blend(T['selText'], T['selFill'], 0.35) if i == SEL else T['textSec']
        badgeRoom = S(L['badgeW']) + S(L['badgeGapR']) + S(10)
        text(textX, y + rowH / 2 - S(9), row['title'], L['titleSize'], titleCol, bold=True,
             anchor='lm', room=badgeRight - textX - S(L['badgeW']) - S(10))
        if row['sub']:
            text(textX, y + rowH / 2 + S(9), row['sub'], L['subSize'], subCol, anchor='lm',
                 room=badgeRight - textX - S(L['badgeW']) - S(10))
        # 快捷键键帽：几何 ⌘ 符号 + 数字（不用字体字符，避免 Windows 缺字变方框）
        bw, bh = S(L['badgeW']), S(L['badgeH'])
        bxx = badgeRight - bw; byy = y + (rowH - bh) // 2
        fill_round_rect((bxx, byy, bxx + bw, byy + bh), S(L['badgeR']),
                        lambda U, V: np.broadcast_to(np.asarray(T['badgeBg'], float), (*U.shape, 3)))
        stroke_round_rect((bxx, byy, bxx + bw, byy + bh), S(L['badgeR']), T['badgeBorder'], 1)
        # ⌘ 四爪符号（约 9x9）
        ccx, ccy = bxx + S(9), byy + bh / 2
        bc = T['badgeText']
        fill_rect((ccx - S(4), ccy - S(1), ccx + S(4), ccy + S(1)), bc)   # 横
        fill_rect((ccx - S(1), ccy - S(4), ccx + S(1), ccy + S(4)), bc)   # 竖
        for dx, dy in [(-4,-4),(4,-4),(-4,4),(4,4)]:
            fill_rect((ccx + S(dx) - S(1), ccy + S(dy) - S(1),
                       ccx + S(dx) + S(1), ccy + S(dy) + S(1)), bc)
        text(bxx + bw - S(8), byy + bh / 2, row['hk'], L['subSize'], T['badgeText'], anchor='mm')

    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    return out

if __name__ == '__main__':
    im = render()
    base = f'C:/Users/ted/Desktop/flowtary/tools/preview_{THEME_NAME}'
    im.save(base + '.png')
    def z(box, s=6):
        c = im.crop(box); return c.resize((c.width*s, c.height*s), Image.NEAREST)
    z((0, 0, 46, 46), 6).save(base + '_tl.png')
    print('saved', base + '.png', im.size)
