# 生成「旧直画 vs 新超采样」前后对比图
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function Get-CP([int]$cp) {
    if ($cp -lt 0x10000) { return [string][char]$cp }
    $cp -= 0x10000; $hi = 0xD800 + ($cp -shr 10); $lo = 0xDC00 + ($cp -band 0x3FF)
    return [string][char]$hi + [string][char]$lo
}

# 与应用同逻辑渲染：黑圆底+白字形。ss=超采样倍数（1=直画）
function Render-Icon([string]$cp, [int]$px, [int]$ss) {
    $fontPath = "C:\Windows\Fonts\segmdl2.ttf"
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
    $f = New-Object System.Drawing.Font($pfc.Families[0], [float]($work * 0.58), [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = [System.Drawing.StringAlignment]::Center
    $sf.LineAlignment = [System.Drawing.StringAlignment]::Center
    $g.DrawString($cp, $f, [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF(0,0,$work,$work)), $sf)
    $g.Dispose(); $f.Dispose(); $pfc.Dispose(); $b.Dispose()
    if ($ss -eq 1) { return $bmp }
    $out = New-Object System.Drawing.Bitmap($px, $px, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb))
    $g2 = [System.Drawing.Graphics]::FromImage($out)
    $g2.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g2.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
    $g2.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0,0,$px,$px)), 0, 0, $work, $work, [System.Drawing.GraphicsUnit]::Pixel)
    $g2.Dispose(); $bmp.Dispose()
    return $out
}

function Zoom([System.Drawing.Image]$img, [int]$z) {
    $w = [int]$img.Width  * $z
    $h = [int]$img.Height * $z
    $b = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
    $g.Clear([System.Drawing.Color]::FromArgb(255, 60, 60, 66))
    $g.DrawImage($img, 0, 0, $w, $h)
    $g.Dispose(); return $b
}

$cp16 = Get-CP 0xE721
# 旧：16px 直画；新：16px 超采样（与 C++ 同思路）
$old16 = Render-Icon $cp16 16 1
$new16 = Render-Icon $cp16 16 4
Write-Output ("old16 type={0} count={1}" -f $old16.GetType().FullName, @($old16).Count); $old16z = Zoom $old16 8
$new16z = Zoom $new16 8
# 32px：旧直画 vs 实机提取
$old32 = Render-Icon $cp16 32 1
$real32 = [System.Drawing.Image]::FromFile("C:\Users\ted\Desktop\flowtary\tools\icon_new_32.png")
$old32z = Zoom $old32 4
$real32z = Zoom $real32 4

$W = 860; $H = 520
$canvas = New-Object System.Drawing.Bitmap($W, $H)
$gc = [System.Drawing.Graphics]::FromImage($canvas)
$gc.Clear([System.Drawing.Color]::FromArgb(255, 28, 28, 34))
$titleF = New-Object System.Drawing.Font("Microsoft YaHei UI", 15, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$labF   = New-Object System.Drawing.Font("Microsoft YaHei UI", 12, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$gray = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(200, 200, 200, 205))
$white = [System.Drawing.Brushes]::White
$orange = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 170, 60))
$gc.DrawString("Flowtary 托盘图标修复前后对比", $titleF, $white, 24, 16)
$gc.DrawString("方案A：超采样渲染（4x 绘制 → 面积平均缩回）", $labF, $gray, 24, 46)

# 16px 组
$gc.DrawString("托盘 16px（8 倍放大看像素结构）", $labF, $gray, 24, 86)
$gc.DrawString("旧 · 直画", $labF, $orange, 24, 260)
$gc.DrawString("新 · 超采样", $labF, $orange, 470, 260)
$gc.DrawImage($old16z, 24, 112)
$gc.DrawImage($new16z, 470, 112)

# 32px 组
$gc.DrawString("窗口 32px（4 倍放大，右为实机提取）", $labF, $gray, 24, 310)
$gc.DrawString("旧 · 直画", $labF, $orange, 24, 462)
$gc.DrawString("新 · 实机提取", $labF, $orange, 470, 462)
$gc.DrawImage($old32z, 24, 336)
$gc.DrawImage($real32z, 470, 336)

$out = "C:\Users\ted\Desktop\flowtary\tools\icon_before_after.png"
$canvas.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output ("saved: " + $out)
foreach($d in @($canvas,$gc,$old16,$new16,$old16z,$new16z,$old32,$real32,$old32z,$real32z,$titleF,$labF,$gray,$orange)){ try{$d.Dispose()}catch{} }