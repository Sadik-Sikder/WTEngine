param([string]$outDir, [switch]$previewOnly)
Add-Type -AssemblyName System.Drawing

# Draws the icon at `size` pixels. Small sizes drop the orbit ring and use
# a thicker W so they stay legible.
function Draw-Icon([int]$size) {
  $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'
  $g.PixelOffsetMode = 'HighQuality'
  $g.InterpolationMode = 'HighQualityBicubic'
  $g.Clear([System.Drawing.Color]::Transparent)

  $s = [double]$size
  $m = [Math]::Max(0.5, $s * 0.03)          # margin
  $w = $s - 2 * $m
  $r = $w * 0.23                             # corner radius
  $path = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = 2 * $r
  $path.AddArc($m, $m, $d, $d, 180, 90)
  $path.AddArc($m + $w - $d, $m, $d, $d, 270, 90)
  $path.AddArc($m + $w - $d, $m + $w - $d, $d, $d, 0, 90)
  $path.AddArc($m, $m + $w - $d, $d, $d, 90, 90)
  $path.CloseFigure()

  # Background: indigo (top left) to cyan (bottom right).
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF $m, $m), (New-Object System.Drawing.PointF ($m + $w), ($m + $w)), ([System.Drawing.Color]::FromArgb(255, 79, 70, 229)), ([System.Drawing.Color]::FromArgb(255, 6, 182, 212))
  $g.FillPath($bg, $path)

  # A soft highlight across the top half.
  $hl = New-Object System.Drawing.Drawing2D.LinearGradientBrush (New-Object System.Drawing.PointF 0, $m), (New-Object System.Drawing.PointF 0, ($m + $w * 0.55)), ([System.Drawing.Color]::FromArgb(60, 255, 255, 255)), ([System.Drawing.Color]::FromArgb(0, 255, 255, 255))
  $hl.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipXY # no seam where the gradient ends
  $g.SetClip($path)
  $g.FillRectangle($hl, [float]$m, [float]$m, [float]$w, [float]($w * 0.55))

  $cx = $s / 2; $cy = $s / 2
  # Orbit ring (the "web"): a tilted ellipse behind the W, large sizes only.
  if ($size -ge 48) {
    $ring = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(90, 255, 255, 255)), ([float]($s * 0.035))
    $state = $g.Save()
    $g.TranslateTransform([float]$cx, [float]$cy)
    $g.RotateTransform(-22)
    $g.DrawEllipse($ring, [float](-$s * 0.40), [float](-$s * 0.17), [float]($s * 0.80), [float]($s * 0.34))
    $g.Restore($state)
    # A small dot on the ring, like a satellite.
    $dot = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 255, 255, 255))
    $dr = $s * 0.045
    $a = [Math]::PI * (-22 + 205) / 180
    $ex = $s * 0.40 * [Math]::Cos([Math]::PI * 205 / 180); $ey = $s * 0.17 * [Math]::Sin([Math]::PI * 205 / 180)
    $rot = [Math]::PI * -22 / 180
    $px = $cx + $ex * [Math]::Cos($rot) - $ey * [Math]::Sin($rot)
    $py = $cy + $ex * [Math]::Sin($rot) + $ey * [Math]::Cos($rot)
    $g.FillEllipse($dot, [float]($px - $dr), [float]($py - $dr), [float](2 * $dr), [float](2 * $dr))
  }

  # The W: one thick polyline with round joins, with a faint shadow.
  $stroke = if ($size -le 24) { $s * 0.15 } elseif ($size -le 48) { $s * 0.125 } else { $s * 0.11 }
  $top = $s * 0.30; $bottom = $s * 0.71; $mid = $s * 0.47
  $xs = @(0.22, 0.355, 0.50, 0.645, 0.78)
  $pts = @(
    (New-Object System.Drawing.PointF ([float]($s * $xs[0])), ([float]$top)),
    (New-Object System.Drawing.PointF ([float]($s * $xs[1])), ([float]$bottom)),
    (New-Object System.Drawing.PointF ([float]($s * $xs[2])), ([float]$mid)),
    (New-Object System.Drawing.PointF ([float]($s * $xs[3])), ([float]$bottom)),
    (New-Object System.Drawing.PointF ([float]($s * $xs[4])), ([float]$top))
  )
  if ($size -ge 32) {
    $shadow = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(55, 20, 20, 80)), ([float]$stroke)
    $shadow.LineJoin = 'Round'; $shadow.StartCap = 'Round'; $shadow.EndCap = 'Round'
    $off = [float]($s * 0.018)
    $spts = $pts | ForEach-Object { New-Object System.Drawing.PointF ($_.X + $off), ($_.Y + $off) }
    $g.DrawLines($shadow, [System.Drawing.PointF[]]$spts)
  }
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([float]$stroke)
  $pen.LineJoin = 'Round'; $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
  $g.DrawLines($pen, [System.Drawing.PointF[]]$pts)

  $g.ResetClip()
  $g.Dispose()
  return $bmp
}

function PngBytes($bmp) {
  $ms = New-Object System.IO.MemoryStream
  $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
  return ,$ms.ToArray()
}

if ($previewOnly) {
  # A strip of every size on white and on dark, scaled up to judge them.
  $sizes = 16, 24, 32, 48, 64, 128, 256
  [int]$sheetW = 20; foreach ($z in $sizes) { $sheetW += $z + 20 }
  $sheet = New-Object System.Drawing.Bitmap $sheetW, 600
  $gs = [System.Drawing.Graphics]::FromImage($sheet)
  $gs.Clear([System.Drawing.Color]::White)
  $gs.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 32, 32, 36))), 0, 300, $sheetW, 300)
  $x = 20
  foreach ($sz in $sizes) {
    $b = Draw-Icon $sz
    $gs.DrawImage($b, $x, 20, $sz, $sz)
    $gs.DrawImage($b, $x, 320, $sz, $sz)
    $x += $sz + 20
    $b.Dispose()
  }
  $sheet.Save("$outDir\preview.png")
  $big = Draw-Icon 512
  $big.Save("$outDir\preview512.png")
  return
}

# The .ico: one PNG-compressed image per size (Windows Vista and later).
$sizes = 16, 20, 24, 32, 40, 48, 64, 96, 128, 256
$images = foreach ($sz in $sizes) { $b = Draw-Icon $sz; ,(PngBytes $b); $b.Dispose() }
$fs = [System.IO.File]::Create("$outDir\WTEngine.ico")
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $sz = $sizes[$i]; $data = $images[$i]
  $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
  $bw.Write([byte]($(if ($sz -ge 256) { 0 } else { $sz })))
  $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([uint16]1); $bw.Write([uint16]32)
  $bw.Write([uint32]$data.Length); $bw.Write([uint32]$offset)
  $offset += $data.Length
}
foreach ($data in $images) { $bw.Write([byte[]]$data) }
$bw.Close()
"wrote $outDir\WTEngine.ico ($((Get-Item "$outDir\WTEngine.ico").Length) bytes)"
