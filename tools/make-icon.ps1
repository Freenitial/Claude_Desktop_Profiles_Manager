<#
.SYNOPSIS
    Regenerates src\app.ico, the Claude Desktop Profiles Manager application icon.

.DESCRIPTION
    Draws the icon with System.Drawing at every size Windows asks for and packs
    the PNG renders into a single .ico. Run it with Windows PowerShell 5.1
    (System.Drawing ships with the .NET Framework):

        powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1
#>
[CmdletBinding()]
param(
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
if (-not $OutFile) {
    $OutFile = Join-Path (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) 'src\app.ico'
}
Add-Type -AssemblyName System.Drawing

function New-RoundedRect([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90)
    $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure()
    return $p
}

function Add-Person($g, [single]$cx, [single]$top, [single]$scale, $brush) {
    $head = 0.30 * $scale
    $g.FillEllipse($brush, $cx - $head / 2, $top, $head, $head)
    $bodyW = 0.56 * $scale
    $bodyH = 0.30 * $scale
    $bodyTop = $top + $head + 0.05 * $scale
    $body = New-RoundedRect ($cx - $bodyW / 2) $bodyTop $bodyW $bodyH (0.14 * $scale)
    $g.FillPath($brush, $body)
    $body.Dispose()
}

function Render([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.PixelOffsetMode = 'HighQuality'
    $g.Clear([System.Drawing.Color]::Transparent)

    $m = [single]($s * 0.04)
    $bg = New-RoundedRect $m $m ($s - 2 * $m) ($s - 2 * $m) ([single]($s * 0.22))
    $grad = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
        (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $s, $s),
        [System.Drawing.Color]::FromArgb(255, 109, 91, 245), [System.Drawing.Color]::FromArgb(255, 55, 48, 163))
    $g.FillPath($grad, $bg)

    $back = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 255, 255, 255))
    $front = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    Add-Person $g ([single]($s * 0.63)) ([single]($s * 0.20)) ([single]($s * 0.62)) $back
    Add-Person $g ([single]($s * 0.40)) ([single]($s * 0.31)) ([single]($s * 0.70)) $front

    $g.Dispose(); $grad.Dispose(); $back.Dispose(); $front.Dispose(); $bg.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return , $ms.ToArray()
}

$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$images = foreach ($s in $sizes) { , (Render $s) }

$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $dim = if ($sizes[$i] -ge 256) { 0 } else { $sizes[$i] }
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$images[$i].Length); $w.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($img in $images) { $w.Write($img) }
$w.Flush()
[IO.File]::WriteAllBytes($OutFile, $out.ToArray())
Write-Host "Wrote $OutFile"
