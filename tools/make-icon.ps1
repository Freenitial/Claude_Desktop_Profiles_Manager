<#
.SYNOPSIS
    Regenerates src\app.ico, the Claude Desktop Profiles Manager application icon.

.DESCRIPTION
    Draws the icon with System.Drawing at the sizes Windows asks for at every
    scale from 100 % to 300 % of small icons, taskbar buttons and large icons,
    and 256 for Explorer's largest views (the sizes of the profile icons in
    src\icons.c), and packs the PNG renders into a single .ico. Run it with
    Windows PowerShell 5.1 (System.Drawing ships with the .NET Framework):

        powershell -NoProfile -ExecutionPolicy Bypass -File tools\make-icon.ps1

.PARAMETER OutFile
    The .ico to write (default: src\app.ico). A relative path is relative to
    the current PowerShell location.
#>
[CmdletBinding()]
param(
    [string]$OutFile
)

$ErrorActionPreference = 'Stop'
if (-not $OutFile) {
    $OutFile = Join-Path (Split-Path -Parent $PSScriptRoot) 'src\app.ico'
}
# .NET resolves a relative path against the process's directory, not the PowerShell location.
$OutFile = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
Add-Type -AssemblyName System.Drawing

function New-RoundedRectangle([single]$Left, [single]$Top, [single]$Width, [single]$Height, [single]$Radius) {
    $outline = New-Object System.Drawing.Drawing2D.GraphicsPath
    $diameter = 2 * $Radius
    $outline.AddArc($Left, $Top, $diameter, $diameter, 180, 90)
    $outline.AddArc($Left + $Width - $diameter, $Top, $diameter, $diameter, 270, 90)
    $outline.AddArc($Left + $Width - $diameter, $Top + $Height - $diameter, $diameter, $diameter, 0, 90)
    $outline.AddArc($Left, $Top + $Height - $diameter, $diameter, $diameter, 90, 90)
    $outline.CloseFigure()
    return $outline
}

function Add-Person($Graphics, [single]$CenterX, [single]$Top, [single]$Scale, $Brush) {
    $headSize = 0.30 * $Scale
    $Graphics.FillEllipse($Brush, $CenterX - $headSize / 2, $Top, $headSize, $headSize)
    $bodyWidth = 0.56 * $Scale
    $bodyHeight = 0.30 * $Scale
    $bodyTop = $Top + $headSize + 0.05 * $Scale
    $body = New-RoundedRectangle ($CenterX - $bodyWidth / 2) $bodyTop $bodyWidth $bodyHeight (0.14 * $Scale)
    try {
        $Graphics.FillPath($Brush, $body)
    } finally {
        $body.Dispose()
    }
}

# The icon at Size x Size, as PNG bytes. Every object is disposed, whichever
# constructor throws.
function Get-IconPng([int]$Size) {
    $bitmap = $graphics = $background = $gradient = $backPerson = $frontPerson = $png = $null
    try {
        $bitmap = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $margin = [single]($Size * 0.04)
        $background = New-RoundedRectangle $margin $margin ($Size - 2 * $margin) ($Size - 2 * $margin) ([single]($Size * 0.22))
        $gradient = New-Object System.Drawing.Drawing2D.LinearGradientBrush (
            (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $Size, $Size),
            [System.Drawing.Color]::FromArgb(255, 109, 91, 245), [System.Drawing.Color]::FromArgb(255, 55, 48, 163))
        $backPerson = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(150, 255, 255, 255))
        $frontPerson = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
        $graphics.SmoothingMode = 'AntiAlias'
        $graphics.PixelOffsetMode = 'HighQuality'
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.FillPath($gradient, $background)
        Add-Person $graphics ([single]($Size * 0.63)) ([single]($Size * 0.20)) ([single]($Size * 0.62)) $backPerson
        Add-Person $graphics ([single]($Size * 0.40)) ([single]($Size * 0.31)) ([single]($Size * 0.70)) $frontPerson
        # The drawing is complete in the bitmap once its Graphics is gone.
        $graphics.Dispose()
        $graphics = $null
        $png = New-Object System.IO.MemoryStream
        $bitmap.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
        return , $png.ToArray()
    } finally {
        foreach ($owned in $png, $frontPerson, $backPerson, $gradient, $background, $graphics, $bitmap) {
            if ($null -ne $owned) {
                $owned.Dispose()
            }
        }
    }
}

$sizes = 16, 20, 24, 28, 30, 32, 36, 40, 42, 48, 54, 56, 60, 64, 72, 80, 96, 256
$pngImages = foreach ($size in $sizes) { , (Get-IconPng $size) }

$ico = New-Object System.IO.MemoryStream
$writer = New-Object System.IO.BinaryWriter $ico
try {
    # The ICONDIR header, one ICONDIRENTRY per size, then the PNGs in the same order.
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = $sizes[$i]
        if ($dimension -ge 256) {
            $dimension = 0
        }
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$pngImages[$i].Length); $writer.Write([uint32]$offset)
        $offset += $pngImages[$i].Length
    }
    foreach ($image in $pngImages) { $writer.Write($image) }
    $writer.Flush()
    [IO.File]::WriteAllBytes($OutFile, $ico.ToArray())
} finally {
    $writer.Dispose()   # closes $ico too
}
Write-Host "Wrote $OutFile"
