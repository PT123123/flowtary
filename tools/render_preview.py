# -*- coding: utf-8 -*-
"""
flowtary 主窗离屏渲染器 —— 严格复刻 src/main.cpp 的 Paint() 绘制公式。
用途：不启动 GUI、不截图，把当前参数下的主界面画成 PNG，用于自查看效果、打磨细节。
参数区 PARAMS / THEME 与 main.cpp 保持同步；改 main.cpp 后同步这里再渲染。
"""
import sys, math
import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---------------- 参数区（与 main.cpp 当前值同步） ----------------
P = dict(
    W=680, SCALE=1.0,
    kBasePad=8, kBaseInputH=62, kBaseRowH=34,
    kBaseCardGapTop=6, kBaseCardPadBottom=5, kBaseWinPadBottom=6,
    kSelPillR=5, kSelInset=2, kContentPad=9,
    radiusWindow=10, radiusCard=8,
    barH=4, barInsetExtra=5, barFade=46,
    iconWellR=8, keyBadgeR=5, keyBadgeW=18, keyBadgeH=18, kBadgeRightPad=4,
    selRight=2,
)

# 主题：'classic'=kThemes[0] 黑色简洁；'obsidian'=kThemes[5]
THEMES = {
 'classic': dict(
    dark=True, bg=(0,0,0), bg2=(0,0,0), text=(255,255,255), sub=(140,140,140),
    hint=(150,150,150), selBg=(51,51,51), divider=(34,34,34), border=(80,80,80),
    accent=(64,134,196), accent2=(94,162,224), bgCard=(28,28,28), bgInput=(24,24,24),
    textTitle=(255,255,255), textBody=(235,235,235), textSec=(150,150,150), textDis=(110,110,110),
    radiusWindow=10, radiusCard=8, accentStrip=True, cornerGlow=False,
    shCardX=2, shCardY=3, shCardA=30, fontInput=18, fontList=15),
 'obsidian': dict(
    dark=True, bg=(0x0D,0x0F,0x14), bg2=(0x0D,0x0F,0x14), text=(255,255,255), sub=(0x88,0x96,0xB0),
    hint=(0x50,0x5A,0x6E), selBg=(0x1A,0x1E,0x27), divider=(0x2C,0x34,0x42), border=(0x3A,0x44,0x55),
    accent=(0x40,0x86,0xC4), accent2=(0x5E,0xA2,0xE0), bgCard=(0x1A,0x1E,0x27), bgInput=(0x12,0x15,0x1C),
    textTitle=(255,255,255), textBody=(255,255,255), textSec=(0x88,0x96,0xB0), textDis=(0x50,0x5A,0x6E),
    radiusWindow=8, radiusCard=6, accentStrip=True, cornerGlow=True,
    shCardX=0, shCardY=0, shCardA=20, fontInput=18, fontList=15),
}
THEME_NAME = sys.argv[1] if len(sys.argv) > 1 else 'obsidian'
T = THEMES[THEME_NAME]
# 主题的可调半径覆盖 P（main.cpp 里 radiusWindow/Card 来自主题，ThemeTune 可能再改）
P['radiusWindow'] = T['radiusWindow']; P['radiusCard'] = T['radiusCard']

S = lambda v: int(v * P['SCALE'] + 0.5)
W = P['W']

# ---------------- 颜色工具（严格复刻 Blend/Darken/Lighten 的整数截断） ----------------
def C(c): return np.asarray(c, dtype=np.float64)
def blend(a, b, t):
    a = np.asarray(a, float); b = np.asarray(b, float)
    r = a + (b - a) * t
    return np.floor(r).astype(np.float64)          # C++ (int) 截断
def darken(c, p):
    c = np.asarray(c, float); p = np.asarray(p, float)
    return np.floor(c * (100 - p) / 100.0)
def lighten(c, p):
    c = np.asarray(c, float); p = np.asarray(p, float)
    return np.minimum(255, np.floor(c * (100 + p) / 100.0))

# ---------------- 画布（float RGB 0..255） ----------------
H = 0
img = None
def new_canvas(h):
    global img, H
    H = h; img = np.zeros((h, W, 3), np.float64)
    img[:] = C(T['bg2'] if T['bg'] != T['bg2'] else T['bg'])
    if T['bg'] != T['bg2']:
        for y in range(h):
            img[y] = blend(T['bg'], T['bg2'], y / max(1, h - 1))

def _coverage(w, h, rad):
    rad = max(0, min(rad, min(w, h) // 2))
    xs = np.arange(w, dtype=np.float64) + 0.5
    ys = np.arange(h, dtype=np.float64) + 0.5
    hw, vh = w * 0.5, h * 0.5
    innerX = max(0.0, hw - 0.5 - rad); innerY = max(0.0, vh - 0.5 - rad)
    qx = np.maximum(np.abs(xs - hw) - innerX, 0.0)
    qy = np.maximum(np.abs(ys - vh) - innerY, 0.0)
    QX, QY = np.meshgrid(qx, qy)
    dist = np.sqrt(QX * QX + QY * QY)
    return dist, rad, xs, ys

def fill_round_rect(rect, rad, colfn):
    l, t, r, b = [int(v) for v in rect]
    w, h = r - l, b - t
    if w <= 0 or h <= 0 or l < 0 or t < 0 or r > W or b > H: return
    dist, rad, xs, ys = _coverage(w, h, rad)
    cov = np.clip(0.5 - (dist - rad), 0.0, 1.0)
    U = np.broadcast_to((xs / max(1, w - 1))[None, :], (h, w))
    V = np.broadcast_to((ys / max(1, h - 1))[:, None], (h, w))
    color = colfn(U, V)                       # (h,w,3)
    color = np.broadcast_to(color, (h, w, 3))
    a = cov[:, :, None]
    src = img[t:b, l:r]
    src[:] = src * (1 - a) + color * a

def stroke_round_rect(rect, rad, col, width=1):
    l, t, r, b = [int(v) for v in rect]
    w, h = r - l, b - t
    if w <= 2 or h <= 2 or l < 0 or t < 0 or r > W or b > H: return
    if width < 1: width = 1
    dist, rad, xs, ys = _coverage(w, h, rad)
    half = width * 0.5
    cov = np.clip(half + 0.5 - np.abs(dist - rad), 0.0, 1.0)
    a = cov[:, :, None]
    src = img[t:b, l:r]
    src[:] = src * (1 - a) + np.asarray(col, float)[None, None, :] * a

def fill_rect(rect, col, a=1.0):
    l, t, r, b = [int(v) for v in rect]
    l = max(0, l); t = max(0, t); r = min(W, r); b = min(H, b)
    if r <= l or b <= t: return
    img[t:b, l:r] = img[t:b, l:r] * (1 - a) + np.asarray(col, float)[None, None, :] * a

def out_shadow(rect, rad, ox, oy, a, col):
    l, t, r, b = [int(v) for v in rect]
    sr = (l + ox, t + oy, r + ox, b + oy)
    w, h = sr[2] - sr[0], sr[3] - sr[1]
    if w <= 0 or h <= 0 or a <= 0: return
    # 用圆角覆盖率近似 PaintOutShadow 的 inside 测试
    _l, _t, _r, _b = sr
    if _l < 0 or _t < 0 or _r > W or _b > H: return
    dist, rr, xs, ys = _coverage(w, h, rad)
    cov = (dist <= rr).astype(float)
    aa = cov[:, :, None] * (a / 255.0)
    src = img[_t:_b, _l:_r]
    src[:] = src * (1 - aa) + np.asarray(col, float)[None, None, :] * aa

def inset_shadow(rect, rad, base):
    l, t, r, b = [int(v) for v in rect]
    fill_rect((l + rad, t + 1, r - rad, t + 2), lighten(base, 16))
    fill_rect((l + rad, b - 2, r - rad, b - 1), darken(base, 12))

# ---------------- 文字（PIL，anchor=lm 近似 DT_VCENTER） ----------------
FONT_REG = 'C:/Windows/Fonts/msyh.ttc'
FONT_BOLD = 'C:/Windows/Fonts/msyhbd.ttc'
_font_cache = {}
def font(size, bold=False):
    key = (size, bold)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(FONT_BOLD if bold else FONT_REG, size)
    return _font_cache[key]

def text(x, y, s, size, col, bold=False, anchor='lm', ellipsis_room=None):
    if not s: return
    f = font(size, bold)
    draw_s = s
    if ellipsis_room is not None:
        # 简易省略：逐字测量
        if f.getlength(s) > ellipsis_room:
            cut = s
            while cut and f.getlength(cut + '…') > ellipsis_room:
                cut = cut[:-1]
            draw_s = cut + '…'
    layer = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.text((int(x), int(y)), draw_s, font=f, fill=(*map(int, col), 255), anchor=anchor)
    arr = np.asarray(layer, np.float64)
    a = (arr[:, :, 3:4] / 255.0)
    img[:] = img * (1 - a) + arr[:, :, :3] * a

# ---------------- 圆角裁剪（模拟窗口圆角，角外填桌面色，便于看清衔接） ----------------
def round_clip(rad, outside=(38, 42, 50)):
    dist, rr, xs, ys = _coverage(W, H, rad)
    inside = (dist <= rr).astype(float)[:, :, None]   # 硬裁剪，贴近 DWM
    img[:] = img * inside + np.asarray(outside, float)[None, None, :] * (1 - inside)

# ================= 绘制主界面（复刻 Paint） =================
def render(theme_name=THEME_NAME):
    global T
    T = THEMES[theme_name]
    P['radiusWindow'] = T['radiusWindow']; P['radiusCard'] = T['radiusCard']
    pad = S(P['kBasePad']); inputH = S(P['kBaseInputH']); rowH = S(P['kBaseRowH'])
    gapTop = S(P['kBaseCardGapTop'])
    rowsTop = inputH + gapTop
    ROWS = [
        dict(title='微信', sub='C:\\Program Files\\Tencent\\WeChat\\WeChat.exe', hk='1', tier=4, usage=128, icon=(87,158,236)),
        dict(title='Chrome', sub='C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe', hk='2', tier=3, usage=41, icon=(220,120,70)),
        dict(title='Visual Studio Code', sub='C:\\Users\\ted\\AppData\\Local\\Programs\\Microsoft VS Code\\Code.exe', hk='3', tier=2, usage=9, icon=(70,150,220)),
        dict(title='任务管理器', sub='taskmgr.exe', hk='4', tier=0, usage=0, icon=(120,170,200)),
        dict(title='文件资源管理器', sub='explorer.exe', hk='5', tier=0, usage=0, icon=(230,190,110)),
        dict(title='设置', sub='ms-settings:', hk='6', tier=0, usage=0, icon=(150,160,180)),
    ]
    SEL = 2
    n = len(ROWS)
    rowsBottom = rowsTop + n * rowH
    cardR = (S(P['kBasePad']), rowsTop, W - S(P['kBasePad']), rowsBottom + S(P['kBaseCardPadBottom']))
    Hcalc = cardR[3] + S(P['kBaseWinPadBottom'])
    new_canvas(Hcalc)

    # —— 输入栏底色采样（绝对像素，支持标量/数组）——
    bgIn = np.asarray(T['bgInput'], float); ttl = np.asarray(T['textTitle'], float)
    ac2 = np.asarray(T['accent2'], float)
    def input_color_grid(px, py):
        px = np.asarray(px, float); py = np.asarray(py, float)
        u = px / (W - 1) if W > 1 else px * 0
        v = py / (inputH - 1) if inputH > 1 else py * 0
        shp = np.broadcast(u, v).shape
        base = np.broadcast_to(bgIn, (*shp, 3)).copy()
        top = v < 0.14
        if np.any(top):
            k = 1 - v / 0.14
            lit = bgIn + (ttl - bgIn) * ((0.12 if T['dark'] else 0.30) * k)[..., None] if shp else bgIn + (ttl - bgIn) * ((0.12 if T['dark'] else 0.30) * k)
            base = np.where(top[..., None] if shp else top, lit, base)
        bot = v > 0.84
        if np.any(bot):
            kk = (v - 0.84) / 0.16
            amt = np.floor((13 if T['dark'] else 8) * kk + 0.5)
            dk = np.floor(base * (100 - amt[..., None]) / 100.0)
            base = np.where(bot[..., None], dk, base)
        du, dv = (u - 0.5) / 0.66, (v - 0.40) / 0.66
        d2 = du * du + dv * dv
        amb = d2 < 1.0
        if np.any(amb):
            fall = (1 - d2) * (1 - d2)
            aa = (0.16 if T['dark'] else 0.11) * fall
            base = base + (ac2 - base) * (aa[..., None] if shp else aa)
        return np.floor(base)
    # 标量包装（蓝条逐像素循环用）
    def input_color_px(px, py):
        return input_color_grid(np.asarray(px, float), np.asarray(py, float))

    # —— 输入栏玻璃片 ——
    ri = S(P['radiusWindow'])
    def input_grid(U, V):
        return input_color_grid(U * (W - 1), V * (inputH - 1))
    fill_round_rect((0, 0, W, inputH), ri, input_grid)
    inset_shadow((0, 0, W, inputH), ri, T['bgInput'])
    stroke_round_rect((1, 1, W - 1, inputH - 1), ri - 1,
                      blend(T['bgInput'], T['textTitle'], 0.10 if T['dark'] else 0.26), 1)

    # —— 结果卡片 ——
    cardRad = S(P['radiusCard'])
    out_shadow(cardR, cardRad, S(T['shCardX']), S(T['shCardY']), T['shCardA'], (0, 0, 0))
    fill_round_rect(cardR, cardRad, lambda U, V: np.broadcast_to(np.asarray(T['bgCard'], float), (*U.shape, 3)))
    inset_shadow(cardR, cardRad, T['bgCard'])
    stroke_round_rect(cardR, cardRad, blend(T['bgCard'], T['textTitle'], 0.09 if T['dark'] else 0.22), 1)

    # —— 顶部蓝条 ——
    if T['accentStrip']:
        barH = S(P['barH']); inset = S(P['radiusWindow']) + S(P['barInsetExtra'])
        ar = (inset, 0, W - inset, barH); fade = float(S(P['barFade']))
        span = ar[2] - ar[0]
        def bar_col(U, V):
            out = np.zeros((*U.shape, 3))
            for iy in range(U.shape[0]):
                for ix in range(U.shape[1]):
                    u = float(U[iy, ix]); v = float(V[iy, ix])
                    k = u * 2 if u < 0.5 else (1 - u) * 2
                    strip = blend(T['accent2'], T['accent'], k)
                    dL, dR = u * span, span - u * span
                    a = 1.0
                    if dL < fade: a = dL / fade
                    if dR < fade: a = min(a, dR / fade)
                    a = a * a * (3 - 2 * a)
                    px = ar[0] + u * span; py = ar[1] + v * barH
                    back = input_color_px(px, py)
                    out[iy, ix] = back if a <= 0.001 else blend(back, strip, a)
            return out
        fill_round_rect(ar, barH // 2, bar_col)
        glow = (inset, barH, W - inset, barH + S(2))
        def glow_col(U, V):
            out = np.zeros((*U.shape, 3)); gw = glow[2] - glow[0]
            for iy in range(U.shape[0]):
                for ix in range(U.shape[1]):
                    u = float(U[iy, ix]); v = float(V[iy, ix])
                    dL = u * gw; dR = gw - dL; ah = 1.0
                    if dL < fade: ah = dL / fade
                    if dR < fade: ah = min(ah, dR / fade)
                    ah = ah * ah * (3 - 2 * ah)
                    a = 0.22 * (1 - v) * ah
                    px = glow[0] + u * gw; py = glow[1] + v * (glow[3] - glow[1])
                    out[iy, ix] = blend(input_color_px(px, py), T['accent2'], a)
            return out
        fill_round_rect(glow, max(1, (glow[3] - glow[1]) // 2), glow_col)
    if T['cornerGlow']:
        glowR = S(150)
        def glow_grid(U, V):
            dx = U * glowR / inputH
            d = np.sqrt(dx * dx + V * V) / 1.4142
            a = np.clip(1 - d, 0, None) ** 2
            px, py = U * glowR, V * inputH
            base = input_color_grid(px, py)
            return np.floor(base + (np.asarray(T['accent2'], float) - base) * (a * 0.10)[..., None])
        fill_round_rect((0, 0, glowR, inputH), S(P['radiusWindow']), glow_grid)

    # —— 输入行文字（模拟有查询 "code" + 光标）——
    fIn = T['fontInput']
    text(pad, inputH / 2, 'code', fIn, T['textBody'])
    cur_x = pad + font(fIn).getlength('code')
    fill_rect((cur_x, inputH / 2 - S(13), cur_x + S(2), inputH / 2 + S(13)), T['textBody'])

    # —— 结果行 ——
    selR = P['kSelPillR']; selIn = S(P['kSelInset']); contentPad = S(P['kContentPad'])
    kBadgeRight = W - pad - S(P['kBadgeRightPad'])
    selFont = T['fontList']
    for i, row in enumerate(ROWS):
        y = rowsTop + i * rowH
        tier = row['tier']
        heat = {0: 0, 1: 0.15, 2: 0.35, 3: 0.7, 4: 1.0}[tier]
        hot = tier >= 2
        titleBase = (T['accent2'] if T['dark'] else darken(T['accent'], 25)) if tier >= 4 else T['accent']
        titleMix = (0.42 - 0.34 * heat) if T['dark'] else (0.30 + 0.40 * heat)
        hotTitle = blend(titleBase, T['textBody'], titleMix)
        rowBase = blend(T['bgCard'], T['accent'], 0.04 + 0.14 * heat) if hot else np.asarray(T['bgCard'], float)
        cardRow = (S(P['kBasePad']), y, W - S(P['kBasePad']), y + rowH)
        # 高频行染色（卡片内）——这里简化为整行矩形（圆角卡片内的角由卡片覆盖顺序近似）
        if hot and i != SEL:
            fill_rect((cardRow[0] + 1, y, cardRow[2] - 1, y + rowH), rowBase)
        # 选中高亮
        pill = (cardRow[0] + selIn, y, cardRow[2] - selIn, y + rowH)
        if i == SEL:
            def pill_col(U, V):
                out = np.zeros((*U.shape, 3))
                for iy in range(U.shape[0]):
                    for ix in range(U.shape[1]):
                        u = float(U[iy, ix]); v = float(V[iy, ix])
                        base = blend(T['selBg'], T['accent'], 0.34 * (1 - u) ** 2 + 0.12)
                        glow = (1 - v) ** 2
                        base = blend(base, T['textTitle'], (0.10 if T['dark'] else 0.14) * glow)
                        out[iy, ix] = base
                return out
            fill_round_rect(pill, selR, pill_col)
        # 图标托盘
        x = cardRow[0] + contentPad
        iconTop = y + (rowH - S(24)) // 2
        well = (x - S(1), y + S(4), x + S(25), y + S(4) + S(26))
        def well_col(U, V):
            out = np.zeros((*U.shape, 3))
            base = blend(T['bgCard'], T['accent2'], 0.22 if T['dark'] else 0.14)
            for iy in range(U.shape[0]):
                v = float(V[iy, 0])
                if v < 0.22: rowc = blend(base, T['textTitle'], (0.16 if T['dark'] else 0.22) * (1 - v / 0.22))
                elif v > 0.84: rowc = darken(base, int(10 * (v - 0.84) / 0.16 + 0.5))
                else: rowc = base
                out[iy, :, :] = np.asarray(rowc, float)
            return out
        fill_round_rect(well, S(P['iconWellR']), well_col)
        # 图标占位：一个圆角色块 + 简单符号（真实是 DrawIconEx 的系统图标）
        fill_rect((x + S(4), iconTop + S(4), x + S(20), iconTop + S(20)), row['icon'])
        x += S(24) + S(8)
        # 快捷键键帽
        hkReserved = 0
        if row['hk']:
            bw = S(P['keyBadgeW']); bh = S(P['keyBadgeH'])
            br = (kBadgeRight - bw, y + (rowH - bh) // 2, kBadgeRight, y + (rowH - bh) // 2 + bh)
            keyBg = blend(T['selBg'], T['accent'], 0.30) if i == SEL else blend(T['bgCard'], T['accent2'], 0.07)
            fill_round_rect(br, S(P['keyBadgeR']), lambda U, V: np.broadcast_to(np.asarray(keyBg, float), (*U.shape, 3)))
            stroke_round_rect(br, S(P['keyBadgeR']), blend(T['bgCard'], T['textSec'], 0.42 if T['dark'] else 0.50), 1)
            text((br[0] + br[2]) / 2, (br[1] + br[3]) / 2, row['hk'], selFont, T['textTitle'] if i == SEL else T['textSec'], anchor='mm')
            hkReserved = bw + S(6)
        # 档位徽章
        hotReserved = 0
        if hot:
            lab = '重度' if tier >= 4 else ('高频' if tier == 3 else '常用')
            tag = f'{lab} ×{row["usage"]}' if tier >= 4 else lab
            bf = font(11)
            tw = bf.getlength(tag); bw = S(9) + int(tw) + S(9); bh = S(17)
            bx = kBadgeRight - hkReserved - bw
            br = (bx, y + (rowH - bh) // 2, bx + bw, y + (rowH - bh) // 2 + bh)
            badgeBg = T['accent'] if tier >= 4 else blend(T['bgCard'], T['accent'], 0.32 if tier == 3 else 0.20)
            fill_round_rect(br, bh // 2, lambda U, V: np.broadcast_to(np.asarray(badgeBg, float), (*U.shape, 3)))
            text((br[0] + br[2]) / 2, (br[1] + br[3]) / 2, tag, 11, T['bgCard'] if tier >= 4 else hotTitle, anchor='mm')
            hotReserved = bw + S(6)
        # 标题 + 副标题
        trailingRight = W - pad - hkReserved - hotReserved - S(8)
        titleRoom = max(S(40), trailingRight - x - S(8))
        ftitle = font(selFont, hot and bool(T.get('fListHot', True)))
        tw = ftitle.getlength(row['title'])
        tr_end = x + min(int(tw), titleRoom) + S(2)
        text(x, y + rowH / 2, row['title'], selFont,
             T['textDis'] if False else (hotTitle if hot else T['textBody']),
             bold=hot, anchor='lm', ellipsis_room=titleRoom)
        if row['sub']:
            sx = tr_end + S(9); avail = trailingRight - sx
            if avail > S(28):
                text(sx, y + rowH / 2, row['sub'], selFont - 1, T['textSec'], anchor='lm', ellipsis_room=avail)
        # 行间发丝分隔线
        if i + 1 < n and i != SEL and i + 1 != SEL:
            sy = y + rowH - 1
            fill_rect((x, sy, W - pad - S(30), sy + 1), blend(T['bgCard'], T['textSec'], 0.06 if T['dark'] else 0.09))

    round_clip(S(P['radiusWindow']))
    # 转 PIL
    out = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), 'RGB')
    return out

if __name__ == '__main__':
    name = THEME_NAME
    im = render(name)
    full = f'C:/Users/ted/Desktop/flowtary/tools/preview_{name}.png'
    im.save(full)
    # 角部放大图：左上 + 右上 + 输入栏左端 + 卡片左上
    def crop_zoom(box, scale=6):
        c = im.crop(box)
        return c.resize((c.width * scale, c.height * scale), Image.NEAREST)
    c1 = crop_zoom((0, 0, 46, 46), 6)
    c2 = crop_zoom((W - 46, 0, W, 46), 6)
    c3 = crop_zoom((0, 60, 60, 100), 6)
    strip = Image.new('RGB', (c1.width + c2.width + c3.width + 20, max(c1.height, c2.height, c3.height)), (60, 60, 60))
    strip.paste(c1, (0, 0)); strip.paste(c2, (c1.width + 10, 0)); strip.paste(c3, (c1.width + c2.width + 20, 0))
    strip.save(f'C:/Users/ted/Desktop/flowtary/tools/preview_{name}_corners.png')
    print('saved', full)
