param(
    [string]$OutputPath = (Join-Path $PSScriptRoot '..\assets\Screen2NVR.ico')
)

Add-Type -AssemblyName System.Drawing

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = [System.Collections.Generic.List[byte[]]]::new()

foreach ($size in $sizes) {
    $bitmap = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $scale = $size / 960.0
    $graphics.ScaleTransform($scale, $scale)
    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(255, 227, 227, 227))

    # Four focus-frame corners from the supplied Material Symbols SVG.
    $graphics.FillPolygon($brush, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(120, 600), [System.Drawing.PointF]::new(200, 600),
        [System.Drawing.PointF]::new(200, 760), [System.Drawing.PointF]::new(360, 760),
        [System.Drawing.PointF]::new(360, 840), [System.Drawing.PointF]::new(200, 840),
        [System.Drawing.PointF]::new(120, 760)))
    $graphics.FillPolygon($brush, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(600, 760), [System.Drawing.PointF]::new(760, 760),
        [System.Drawing.PointF]::new(760, 600), [System.Drawing.PointF]::new(840, 600),
        [System.Drawing.PointF]::new(840, 760), [System.Drawing.PointF]::new(760, 840),
        [System.Drawing.PointF]::new(600, 840)))
    $graphics.FillPolygon($brush, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(120, 360), [System.Drawing.PointF]::new(120, 200),
        [System.Drawing.PointF]::new(200, 120), [System.Drawing.PointF]::new(360, 120),
        [System.Drawing.PointF]::new(360, 200), [System.Drawing.PointF]::new(200, 200),
        [System.Drawing.PointF]::new(200, 360)))
    $graphics.FillPolygon($brush, [System.Drawing.PointF[]]@(
        [System.Drawing.PointF]::new(760, 360), [System.Drawing.PointF]::new(760, 200),
        [System.Drawing.PointF]::new(600, 200), [System.Drawing.PointF]::new(600, 120),
        [System.Drawing.PointF]::new(760, 120), [System.Drawing.PointF]::new(840, 200),
        [System.Drawing.PointF]::new(840, 360)))

    $ring = [System.Drawing.Drawing2D.GraphicsPath]::new([System.Drawing.Drawing2D.FillMode]::Alternate)
    $ring.AddEllipse(280, 280, 400, 400)
    $ring.AddEllipse(360, 360, 240, 240)
    $graphics.FillPath($brush, $ring)

    $stream = [System.IO.MemoryStream]::new()
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
    $images.Add($stream.ToArray())
    $stream.Dispose()
    $ring.Dispose()
    $brush.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$outputDirectory = Split-Path -Parent $OutputPath
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
$file = [System.IO.File]::Open($OutputPath, [System.IO.FileMode]::Create)
$writer = [System.IO.BinaryWriter]::new($file)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$images.Count)
$offset = 6 + 16 * $images.Count
for ($index = 0; $index -lt $images.Count; $index++) {
    $size = $sizes[$index]
    $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
    $writer.Write([byte]$(if ($size -eq 256) { 0 } else { $size }))
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$index].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$index].Length
}
foreach ($image in $images) { $writer.Write($image) }
$writer.Dispose()
$file.Dispose()

Write-Output "Generated $OutputPath"
