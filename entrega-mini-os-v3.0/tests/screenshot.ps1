param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Title = "MiniOS v3.0 - pruebas"
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class CapturaWin {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out RECT pvAttribute, int cbAttribute);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
"@

[CapturaWin]::SetProcessDPIAware() | Out-Null

function Get-Rect([IntPtr]$h) {
    $r = New-Object -TypeName "CapturaWin+RECT"
    $dwm = [CapturaWin]::DwmGetWindowAttribute($h, 9, [ref]$r, 16)
    if ($dwm -eq 0) {
        return @($r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top))
    }
    if ($h -ne [IntPtr]::Zero -and [CapturaWin]::GetWindowRect($h, [ref]$r)) {
        return @($r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top))
    }
    return @(0, 0, 0, 0)
}

# 1) ventana de la suite por título; 2) consola clásica; 3) primer plano.
$origen = "titulo"
$hwnd = [CapturaWin]::FindWindow($null, $Title)
if ($hwnd -eq [IntPtr]::Zero) {
    $origen = "consola"
    $hwnd = [CapturaWin]::GetConsoleWindow()
}
if ($hwnd -eq [IntPtr]::Zero) {
    $origen = "primer-plano"
    $hwnd = [CapturaWin]::GetForegroundWindow()
}

if ([CapturaWin]::IsIconic($hwnd)) {
    [CapturaWin]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE (solo si está minimizada)
}
[CapturaWin]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

$geo = Get-Rect $hwnd
if ($geo[2] -lt 320 -or $geo[3] -lt 200) {
    $origen = "primer-plano"
    $hwnd = [CapturaWin]::GetForegroundWindow()
    $geo = Get-Rect $hwnd
}
if ($geo[2] -lt 320 -or $geo[3] -lt 200) {
    $bordes = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $geo = @($bordes.Left, $bordes.Top, $bordes.Width, $bordes.Height)
    $origen = "pantalla-completa"
}

$bmp = New-Object System.Drawing.Bitmap($geo[2], $geo[3])
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$impreso = [CapturaWin]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
if (-not $impreso) {
    $g.CopyFromScreen($geo[0], $geo[1], 0, 0, (New-Object System.Drawing.Size($geo[2], $geo[3])))
}

$carpeta = Split-Path -Parent $Path
if ($carpeta -and -not (Test-Path $carpeta)) {
    New-Item -ItemType Directory -Force -Path $carpeta | Out-Null
}
$bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()

if ($env:CAPTURA_DIAG -eq "1") {
    "origen=$origen hwnd=$hwnd rect=$($geo -join ',') printwindow=$impreso" |
        Set-Content -Path "$Path.diag.txt" -Encoding UTF8
    Write-Host "Captura: $Path ($($geo[2])x$($geo[3]), $origen, printwindow=$impreso)" -ForegroundColor DarkGray
}
