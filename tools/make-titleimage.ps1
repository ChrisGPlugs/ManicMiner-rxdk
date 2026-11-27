# Converts a PNG/JPEG/BMP into an XPR0 texture for imagebld: the XBE's title
# image (dashboard thumbnail, 128x128) or default save image (64x64), e.g.
#   pwsh -File tools/make-titleimage.ps1 -In original/titleimage-source.jpg -Out original/TITLEIMAGE.XBX
#   pwsh -File tools/make-titleimage.ps1 -In original/titleimage-source.jpg -Out original/SAVEIMAGE.XBX -Size 64
#
# The picture is scaled to fit and centred on a black square, then stored as
# one DXT1 level (what XDK titles use; 128x128 is 8 KB of pixels) after a
# 2 KB XPR0 header. -Format X8R8G8B8 writes uncompressed swizzled pixels instead.
param(
    [Parameter(Mandatory)][string]$In,
    [Parameter(Mandatory)][string]$Out,
    [int]$Size = 128,
    [ValidateSet('DXT1', 'X8R8G8B8')][string]$Format = 'DXT1'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$lg = 0; while ((1 -shl $lg) -lt $Size) { $lg++ }
if ((1 -shl $lg) -ne $Size -or $Size -lt 4) { throw "-Size must be a power of two >= 4" }

# Draw the source centred on black.
$src = [System.Drawing.Image]::FromFile((Resolve-Path $In))
$bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Black)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
$scale = [Math]::Min($Size / $src.Width, $Size / $src.Height)
$w = [int][Math]::Round($src.Width * $scale); $h = [int][Math]::Round($src.Height * $scale)
$g.DrawImage($src, [int](($Size - $w) / 2), [int](($Size - $h) / 2), $w, $h)
$g.Dispose(); $src.Dispose()

# Linear BGRA bytes.
$rect = New-Object System.Drawing.Rectangle 0, 0, $Size, $Size
$lock = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, $bmp.PixelFormat)
$px = New-Object byte[] ($Size * $Size * 4)
for ($y = 0; $y -lt $Size; $y++) {
    [System.Runtime.InteropServices.Marshal]::Copy([IntPtr]($lock.Scan0.ToInt64() + $y * $lock.Stride), $px, $y * $Size * 4, $Size * 4)
}
$bmp.UnlockBits($lock); $bmp.Dispose()

function To565([int]$r, [int]$g, [int]$b) { (($r -shr 3) -shl 11) -bor (($g -shr 2) -shl 5) -bor ($b -shr 3) }
function From565([int]$c) {
    $r = ($c -shr 11) -band 31; $g = ($c -shr 5) -band 63; $b = $c -band 31
    @((($r -shl 3) -bor ($r -shr 2)), (($g -shl 2) -bor ($g -shr 4)), (($b -shl 3) -bor ($b -shr 2)))
}

if ($Format -eq 'DXT1') {
    # DXT1 blocks in row order (compressed textures are not swizzled).
    # Endpoints: the block's colour bounding box, inset slightly; 4-colour mode.
    $fmtCode = 0x0C
    $data = New-Object byte[] (($Size / 4) * ($Size / 4) * 8)
    $o = 0
    for ($by = 0; $by -lt $Size; $by += 4) {
        for ($bx = 0; $bx -lt $Size; $bx += 4) {
            $blk = for ($i = 0; $i -lt 16; $i++) {
                $s = (($by + ($i -shr 2)) * $Size + $bx + ($i -band 3)) * 4
                , @([int]$px[$s + 2], [int]$px[$s + 1], [int]$px[$s])
            }
            $mn = @(255, 255, 255); $mx = @(0, 0, 0)
            foreach ($p in $blk) { for ($k = 0; $k -lt 3; $k++) { if ($p[$k] -lt $mn[$k]) { $mn[$k] = $p[$k] }; if ($p[$k] -gt $mx[$k]) { $mx[$k] = $p[$k] } } }
            for ($k = 0; $k -lt 3; $k++) { $in = ($mx[$k] - $mn[$k]) -shr 4; $mn[$k] += $in; $mx[$k] -= $in }
            $c0 = To565 $mx[0] $mx[1] $mx[2]; $c1 = To565 $mn[0] $mn[1] $mn[2]
            if ($c0 -lt $c1) { $t = $c0; $c0 = $c1; $c1 = $t }
            $idx = [uint32]0
            if ($c0 -ne $c1) {
                $e0 = From565 $c0; $e1 = From565 $c1
                $pal = @($e0, $e1,
                    @(((2 * $e0[0] + $e1[0]) / 3), ((2 * $e0[1] + $e1[1]) / 3), ((2 * $e0[2] + $e1[2]) / 3)),
                    @((($e0[0] + 2 * $e1[0]) / 3), (($e0[1] + 2 * $e1[1]) / 3), (($e0[2] + 2 * $e1[2]) / 3)))
                for ($i = 0; $i -lt 16; $i++) {
                    $p = $blk[$i]; $best = 0; $bd = [double]::MaxValue
                    for ($j = 0; $j -lt 4; $j++) {
                        $dr = $p[0] - $pal[$j][0]; $dg = $p[1] - $pal[$j][1]; $db = $p[2] - $pal[$j][2]
                        $d = $dr * $dr + $dg * $dg + $db * $db
                        if ($d -lt $bd) { $bd = $d; $best = $j }
                    }
                    $idx = $idx -bor ([uint32]$best -shl (2 * $i))
                }
            }
            $data[$o] = $c0 -band 0xFF; $data[$o + 1] = $c0 -shr 8
            $data[$o + 2] = $c1 -band 0xFF; $data[$o + 3] = $c1 -shr 8
            [BitConverter]::GetBytes([uint32]$idx).CopyTo($data, $o + 4)
            $o += 8
        }
    }
}
else {
    # Swizzled X8R8G8B8: x and y bits interleaved, x in the lower bit.
    $fmtCode = 0x07
    function Spread([int]$v) { $r = 0; for ($i = 0; $i -lt $lg; $i++) { if ($v -band (1 -shl $i)) { $r = $r -bor (1 -shl (2 * $i)) } }; $r }
    $sp = foreach ($v in 0..($Size - 1)) { Spread $v }
    $data = New-Object byte[] ($Size * $Size * 4)
    for ($y = 0; $y -lt $Size; $y++) {
        for ($x = 0; $x -lt $Size; $x++) {
            $s = ($y * $Size + $x) * 4; $d = ($sp[$x] -bor ($sp[$y] -shl 1)) * 4
            $data[$d] = $px[$s]; $data[$d + 1] = $px[$s + 1]; $data[$d + 2] = $px[$s + 2]; $data[$d + 3] = 0xFF
        }
    }
}

# XPR0 header: magic, total size, header size, one D3DTexture resource
# (Common, Data, Lock, Format, Size), an end marker, 0xAD padding to 2 KB.
$headerSize = 0x800
$fmt = 0x1 -bor 0x8 -bor (2 -shl 4) -bor ($fmtCode -shl 8) -bor (1 -shl 16) -bor ($lg -shl 20) -bor ($lg -shl 24)  # DMA A, border colour, 2D, 1 level
$ms = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $ms
$bw.Write([byte[]][char[]]'XPR0')
$bw.Write([uint32]($headerSize + $data.Length)); $bw.Write([uint32]$headerSize)
$bw.Write([uint32]0x00040001); $bw.Write([uint32]0); $bw.Write([uint32]0); $bw.Write([uint32]$fmt); $bw.Write([uint32]0)
$bw.Write([uint32]::MaxValue)
while ($ms.Length -lt $headerSize) { $bw.Write([byte]0xAD) }
$bw.Write($data); $bw.Flush()
[System.IO.File]::WriteAllBytes([System.IO.Path]::GetFullPath($Out, (Get-Location).Path), $ms.ToArray())
"wrote $Out ({0}x{0} {1}, picture {2}x{3}, {4} bytes)" -f $Size, $Format, $w, $h, $ms.Length
