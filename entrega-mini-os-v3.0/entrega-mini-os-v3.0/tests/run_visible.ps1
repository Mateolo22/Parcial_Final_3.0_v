param(
    [string]$Evidencia = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) "capturas-pruebas"),
    [string[]]$Etapas = @("build", "base", "login", "rbac", "crud", "signals",
                          "game", "raid", "proc", "race", "edge"),
    [switch]$NoPause
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $Evidencia | Out-Null

# Evidencia anterior: solo se borran los archivos que genera este script.
Get-ChildItem -Path $Evidencia -File |
    Where-Object { $_.Name -match '^(etapa_.*\.(png|log)|resumen\.png|captura-\d+.*\.png)$' } |
    Remove-Item -Force

# UTF-8 para que los acentos de la suite se vean bien en la consola.
chcp 65001 | Out-Null
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)

function Get-Slug([string]$texto) {
    $norm = $texto.Normalize([Text.NormalizationForm]::FormD)
    $sb = New-Object System.Text.StringBuilder
    foreach ($c in $norm.ToCharArray()) {
        $cat = [Globalization.CharUnicodeInfo]::GetUnicodeCategory($c)
        if ($cat -ne [Globalization.UnicodeCategory]::NonSpacingMark) {
            [void]$sb.Append($c)
        }
    }
    $s = $sb.ToString().ToLowerInvariant()
    $s = $s -replace '[^a-z0-9]+', '-'
    $s = $s.Trim('-')
    if ($s.Length -gt 60) { $s = $s.Substring(0, 60).Trim('-') }
    if ([string]::IsNullOrWhiteSpace($s)) { $s = "paso" }
    return $s
}

try {
    # El buffer va primero: la ventana no puede ser más alta que el buffer.
    $Host.UI.RawUI.BufferSize = New-Object System.Management.Automation.Host.Size(120, 4000)
    $Host.UI.RawUI.WindowSize = New-Object System.Management.Automation.Host.Size(120, 42)
} catch {
    # Algunos hosts no permiten fijar el tamaño; no es crítico.
}

# Capturas en la ventana completa: maximizar la consola de las pruebas.
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class ConsolaMax {
    [DllImport("kernel32.dll")] public static extern IntPtr GetConsoleWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
}
"@
$hwndConsola = [ConsolaMax]::GetConsoleWindow()
if ($hwndConsola -ne [IntPtr]::Zero) {
    [ConsolaMax]::ShowWindow($hwndConsola, 3) | Out-Null   # SW_MAXIMIZE
    Start-Sleep -Milliseconds 300
}

Clear-Host
$Host.UI.RawUI.WindowTitle = "MiniOS v3.0 - pruebas"
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  MiniOS v3.0 - SUITE COMPLETA DE PRUEBAS" -ForegroundColor Yellow
Write-Host "  Contenedor: minios-dev | evidencia: $Evidencia" -ForegroundColor Gray
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host "  $ comando   = comando ejecutado" -ForegroundColor Gray
Write-Host "  > entrada   = tecla o dato enviado a MiniOS" -ForegroundColor Gray
Write-Host "  | salida    = pantalla real de MiniOS" -ForegroundColor Gray
Write-Host "  [CAPTURA N] = captura N guardada en disco" -ForegroundColor Gray
Write-Host "================================================================" -ForegroundColor Cyan
Write-Host ""

Write-Host "[preparacion] copiando el proyecto a /work..." -ForegroundColor DarkGray
Write-Host '  $ docker exec minios-dev bash -lc "rm -rf /work && cp -r /src /work"' -ForegroundColor Yellow
docker exec minios-dev bash -lc "rm -rf /work && cp -r /src /work"
if ($LASTEXITCODE -ne 0) {
    Write-Host "No se pudo preparar /work. ¿Está corriendo el contenedor minios-dev?" -ForegroundColor Red
    Read-Host "Enter para cerrar"
    exit 1
}

$resumen = @()
$script:n = 0

function Save-Captura([string]$descripcion) {
    $script:n += 1
    $slug = Get-Slug $descripcion
    $nombre = "captura-{0:D2}-{1}.png" -f $script:n, $slug
    Write-Host ""
    Write-Host ("  [CAPTURA {0}] {1}" -f $script:n, $descripcion) -ForegroundColor Magenta
    Start-Sleep -Milliseconds 500
    & (Join-Path $PSScriptRoot "screenshot.ps1") -Path (Join-Path $Evidencia $nombre)
    Start-Sleep -Milliseconds 150
}

foreach ($etapa in $etapas) {
    Clear-Host
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host "  ETAPA: $etapa" -ForegroundColor Yellow
    Write-Host "================================================================" -ForegroundColor Cyan
    Write-Host ""

    $comando = "docker exec -e MINIOS_VISUAL=1 -e MINIOS_CAPTURAS=1 -t minios-dev python3 /work/tests/run_all.py --stage $etapa"
    Write-Host "  $ $comando" -ForegroundColor Yellow
    Write-Host ""
    docker exec -e MINIOS_VISUAL=1 -e MINIOS_CAPTURAS=1 -t minios-dev python3 /work/tests/run_all.py --stage $etapa 2>&1 |
        ForEach-Object {
            $linea = "$_".TrimEnd("`r")
            if ($linea -match '^@@CAPTURA:(.+)$') {
                Save-Captura $Matches[1].Trim()
            } else {
                Write-Host $linea
            }
        }
    $codigo = $LASTEXITCODE

    $resumen += [pscustomobject]@{ Etapa = $etapa; Resultado = $(if ($codigo -eq 0) { "OK" } else { "FALLO" }) }
}

Clear-Host
Write-Host "================================================================" -ForegroundColor Green
Write-Host "  RESUMEN DE LA SUITE" -ForegroundColor Yellow
Write-Host "================================================================" -ForegroundColor Green
$resumen | Format-Table -AutoSize
$fallidas = @($resumen | Where-Object { $_.Resultado -ne "OK" })
if ($fallidas.Count -eq 0) {
    Write-Host "Todas las etapas terminaron correctamente." -ForegroundColor Green
} else {
    Write-Host "Hay etapas con fallos." -ForegroundColor Red
}
Write-Host ""
Write-Host "Capturas numeradas en: $Evidencia" -ForegroundColor Gray
Save-Captura "resumen final de la suite"

if (-not $NoPause) { Read-Host "Enter para cerrar esta ventana" }
