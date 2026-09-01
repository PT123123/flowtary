# Flowtary 托盘图标候选渲染预览
# 用与应用相同的绘制逻辑（黑圆底 + 白字形）渲染候选，供挑选。
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$mdl2  = "C:\Windows\Fonts\segmdl2.ttf"
$emoji = "C:\Windows\Fonts\seguiemj.ttf"

# 码点字符串辅助
function Get-CP([int]$cp) {
    if ($cp -lt 0x10000) { return [string][char]$cp }
    $cp -= 0x10000
    $hi = 0xD800 + ($cp -shr 10)
    $lo = 0xDC00 + ($cp -band 0x3FF)
    return [string][char]$hi + [string][char]$lo
}

$candidates = @(
    @{ name="Search 放大镜";      cp=(Get-CP 0xE721);  font=$mdl2;  tag="当前" },
    @{ name="Settings 齿轮";      cp=(Get-CP 0xE713);  font=$mdl2;  tag="" },
    @{ name="Edit 铅笔";          cp=(Get-CP 0xE70F);  font=$mdl2;  tag="" },
    @{ name="QuickNote 便签";     cp=(Get-CP 0xE756);  font=$mdl2;  tag="" },
    @{ name="More 三点";          cp=(Get-CP 0xE71D);  font=$mdl2;  tag="" },
    @{ name="FastForward 快进";   cp=(Get-CP 0xE73E);  font=$mdl2;  tag="" },
    @{ name="Lightning 闪电";     cp=(Get-CP 0xE945);  font=$mdl2;  tag="" },
    @{ name="GlobalNav 汉堡";     cp=(Get-CP 0xE700);  font=$mdl2;  tag="" },
    @{ name="Rocket 火箭";        cp=(Get-CP 0xE7C3);  font=$mdl2;  tag="" },
    @{ name="Globe 地球";         cp=(Get-CP 0xE774);  font=$mdl2;  tag="" },
    @{ name="Keyboard 键盘";      cp=(Get-CP 0xE765);  font=$mdl2;  tag="" },
    @{ name="Emoji 左放大镜";     cp=(Get-CP 0x1F50D); font=$emoji; tag="" },
    @{ name="Emoji 右放大镜";     cp=(Get-CP 0x1F50E); font=$emoji; tag="" },
    @{ name="Emoji 闪电";         cp=(Get-CP 0x26A1);  font=$emoji; tag="" },
    @{ name="Emoji 火花";         cp=(Get-CP 0x2728);  font=$emoji; tag="" },
    @{ name="Emoji 魔法棒";       cp=(Get-CP 0x1FA84); font=$emoji; tag="" },
    @{ name="Emoji 海浪";         cp=(Get-CP 0x1F30A); font=$emoji; tag="" },
    @{ name="Emoji 水滴";         cp=(Get-CP 0x1F4A7); font=$emoji; tag="" },
    @{ name="Emoji 火箭";         cp=(Get-CP 0x1F680); font=$emoji; tag="" },
    @{ name="Emoji 钥匙";         cp=(Get-CP 0x1F511); font=$emoji; tag="" }
)

# 渲染单个图标：黑圆底 + 白字形；px=目标像素；ss=超采样倍数(1=直画)
function Render-Icon([string]$cp, [string]$fontPath, [int]$px, [int]$ss) {
    $work = $px * $ss
    $bmp = New-Object System.Drawing.Bitmap($work, $work, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $b = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::Black)
    $g.FillEllipse($b, 0, 0, $work, $work)
    $pfc = New-Object System.Drawing.Text.PrivateFontCollection
    $pfc.AddFontFile($fontPath)
    $fs = [float]($work * 0.58)
    $f = New-Object System.Drawing.Font($pfc.Families[0], $fs, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $rect = New-Object System.Drawing.RectangleF(0, 0, $work, $work)
    $g.DrawString($cp, $f, [System.Drawing.Brushes]::White, $rect, $sf)
    $g.Dispose()
    $f.Dispose(); $pfc.Dispose(); $b.Dispose()
    if ($ss -eq 1) { return $bmp }
    # 高质量缩到目标尺寸
    $out = New-Object System.Drawing.Bitmap($px, $px, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $g2 = [System.Drawing.Graphics]::FromImage($out)
    $g2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g2.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0,0,$px,$px)), 0, 0, $work, $work, [System.Drawing.GraphicsUnit]::Pixel)
    $g2.Dispose()
    $bmp.Dispose()
    return $out
}

# ---- 布局 ----
$cols = 5
$cellW = 172; $cellH = 132
$pad = 18
$headerH = 96
$rows = [math]::Ceiling($candidates.Count / $cols)
$W = $pad*2 + $cellW*$cols
$H = $headerH + $rows*$cellH + $pad

$canvas = New-Object System.Drawing.Bitmap($W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
$gc = [System.Drawing.Graphics]::FromImage($canvas)
$gc.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$gc.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit
$gc.Clear([System.Drawing.Color]::FromArgb(255, 30, 30, 34))

$titleFont = New-Object System.Drawing.Font("Microsoft YaHei UI", 15, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$subFont  = New-Object System.Drawing.Font("Microsoft YaHei UI", 11, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$cellFont = New-Object System.Drawing.Font("Microsoft YaHei UI", 10.5, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$tagFont  = New-Object System.Drawing.Font("Microsoft YaHei UI", 9, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)

$white = [System.Drawing.Brushes]::White
$gray  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(180, 200, 200, 205))
$orange = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 170, 60))

# 标题
$gc.DrawString("Flowtary 托盘图标候选 · 黑圆底 + 白字形（与应用同渲染逻辑）", $titleFont, $white, ($pad+2), 14)
$gc.DrawString("每格上排 = 16px 超采样修复效果（4x 渲染后高质量缩到 16px）  下排 = 64px 原图，可看字形细节", $subFont, $gray, ($pad+2), 46)
# 当前 vs 修复对比小条
$gc.DrawString("当前 U+E721 直画 16px  vs  超采样修复", $subFont, $gray, ($pad+2), 68)

$curDirect = Render-Icon (Get-CP 0xE721) $mdl2 16 1
$curFixed  = Render-Icon (Get-CP 0xE721) $mdl2 16 4
$d0x = $pad+2; $d0y = 84
# 放大到 80px 看像素结构
$zoom = 80
$z1 = New-Object System.Drawing.Bitmap($zoom,$zoom)
$gz = [System.Drawing.Graphics]::FromImage($z1)
$gz.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$gz.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$gz.DrawImage($curDirect, 0,0,$zoom,$zoom); $gz.Dispose()
$z2 = New-Object System.Drawing.Bitmap($zoom,$zoom)
$gz = [System.Drawing.Graphics]::FromImage($z2)
$gz.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$gz.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$gz.DrawImage($curFixed, 0,0,$zoom,$zoom); $gz.Dispose()
$gc.DrawImage($z1, $d0x, $d0y)
$gc.DrawImage($z2, $d0x+$zoom+22, $d0y)
$gc.DrawString("直画（现状：笔画粗、糊）", $cellFont, $gray, $d0x, $d0y+$zoom+6)
$gc.DrawString("超采样（笔画更细更锐）", $cellFont, $orange, $d0x+$zoom+22, $d0y+$zoom+6)

# 候选网格
$startY = $headerH
for ($i=0; $i -lt $candidates.Count; $i++) {
    $c = $candidates[$i]
    $col = $i % $cols
    $row = [math]::Floor($i / $cols)
    $x = $pad + $col*$cellW
    $y = $startY + $row*$cellH + 8
    # 16px 固定效果 + 64px 原图
    $i16 = Render-Icon $c.cp $c.font 16 4
    $i64 = Render-Icon $c.cp $c.font 64 1
    $cx16 = $x + ($cellW-16)/2
    $gc.DrawImage($i16, $cx16, $y+2)
    $cx64 = $x + ($cellW-64)/2
    $gc.DrawImage($i64, $cx64, $y+26)
    $gc.DrawString($c.name, $cellFont, $white, $x+6, $y+94)
    if ($c.tag) {
        $gc.DrawString("★" + $c.tag, $tagFont, $orange, $x+$cellW-52, $y+94)
    }
    $i16.Dispose(); $i64.Dispose()
}

# 保存
$out = "C:\Users\ted\Desktop\flowtary\tools\tray_icon_candidates.png"
$canvas.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("saved: " + $out)
foreach($d in @($canvas,$gc,$curDirect,$curFixed,$z1,$z2,$titleFont,$subFont,$cellFont,$tagFont,$orange,$gray)){ try{$d.Dispose()}catch{} }
