<#
.SYNOPSIS
  Ejecuta la suite funcional de Pyxis.

.DESCRIPTION
  Pyxis se prueba como se usa: abriendo la ventana y mandandole teclas y clics.
  No hay pruebas unitarias porque casi todo lo interesante del programa -el
  zero-copy, la sincronia con el reloj de audio, el avance manual- solo existe
  con un dispositivo grafico real por debajo y un archivo real delante.

  Cada caso vive en cases/ y recibe un contexto con las rutas. Falla lanzando
  una excepcion; se omite llamando a Skip-Case.

.PARAMETER Case
  Ejecuta solo los casos cuyo nombre contenga este texto.

.PARAMETER Sample
  Archivo grande y acelerado por hardware para los casos de rendimiento. Los
  archivos que genera mkmedia son diminutos a proposito y no sirven para medir
  el avance manual a 8K. Sin este parametro, esos casos se omiten.

.EXAMPLE
  .\tests\run-tests.ps1
  .\tests\run-tests.ps1 -Case audio
  .\tests\run-tests.ps1 -Sample 'D:\pelicula 8K.mkv'
#>
[CmdletBinding()]
param(
    [string] $Case,
    [string] $Sample,
    [string] $Exe,
    [switch] $List,
    [switch] $KeepArtifacts
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root      = Split-Path -Parent $PSScriptRoot
$testsDir  = $PSScriptRoot
$casesDir  = Join-Path $testsDir 'cases'
$mediaDir  = Join-Path $testsDir 'media'
$workDir   = Join-Path $testsDir '.work'

if (-not $Exe) { $Exe = Join-Path $root 'build\release\bin\pyxis.exe' }
$mkmedia = Join-Path $root 'build\release\bin\tools\mkmedia.exe'

Import-Module (Join-Path $testsDir 'Pyxis.Testing.psm1') -Force

# ---------------------------------------------------------------------------
#  Descubrimiento
# ---------------------------------------------------------------------------
$cases = @(Get-ChildItem $casesDir -Filter '*.ps1' | Sort-Object Name)
if ($Case) { $cases = @($cases | Where-Object { $_.BaseName -like "*$Case*" }) }

if ($List) {
    Write-Host "Casos disponibles:" -ForegroundColor Cyan
    foreach ($c in $cases) {
        $synopsis = (Get-Content $c.FullName -TotalCount 3 |
                     Where-Object { $_ -match '^\s*#\s*(.+)' } |
                     Select-Object -First 1) -replace '^\s*#\s*', ''
        '  {0,-28} {1}' -f $c.BaseName, $synopsis
    }
    exit 0
}

if ($cases.Count -eq 0) { Write-Host "Ningun caso coincide con '$Case'."; exit 1 }
if (-not (Test-Path $Exe)) { Write-Host "No existe $Exe. Compila con scripts\build.ps1."; exit 1 }

# ---------------------------------------------------------------------------
#  Material de prueba
#
#  Se genera si falta. Los archivos son pequenos y no se versionan: tener el
#  generador en el repositorio vale mas que tener los archivos, porque el
#  generador dice exactamente que propiedades se estan probando.
# ---------------------------------------------------------------------------
$avSync = Join-Path $mediaDir 'av-sync.mp4'
$hdr    = Join-Path $mediaDir 'hdr-pq.mkv'

if (-not (Test-Path $avSync) -or -not (Test-Path $hdr)) {
    if (-not (Test-Path $mkmedia)) {
        Write-Host "Falta el material de prueba y no esta $mkmedia." -ForegroundColor Red
        Write-Host "Compila con scripts\build.ps1 (PYXIS_BUILD_TOOLS esta activado por defecto)."
        exit 1
    }
    Write-Host "Generando material de prueba..." -ForegroundColor Cyan
    & $mkmedia $mediaDir
    if ($LASTEXITCODE -ne 0) { Write-Host "mkmedia fallo." -ForegroundColor Red; exit 1 }
}

# Un ejemplar grande ayuda pero no es obligatorio. Se acepta por parametro o por
# variable de entorno para que nadie tenga que editar el script.
if (-not $Sample -and $env:PYXIS_TEST_SAMPLE) { $Sample = $env:PYXIS_TEST_SAMPLE }
if ($Sample -and -not (Test-Path $Sample)) {
    Write-Host "Aviso: no existe $Sample; los casos que lo necesitan se omitiran." -ForegroundColor Yellow
    $Sample = $null
}

if (Test-Path $workDir) { Remove-Item $workDir -Recurse -Force }
New-Item -ItemType Directory -Path $workDir -Force | Out-Null

$context = [pscustomobject]@{
    Exe     = $Exe
    Root    = $root
    Work    = $workDir
    AvSync  = $avSync
    Hdr     = $hdr
    Sample  = $Sample
    Videos  = Join-Path ([Environment]::GetFolderPath('MyVideos'))   'Pyxis'
    Images  = Join-Path ([Environment]::GetFolderPath('MyPictures')) 'Pyxis'
}

# ---------------------------------------------------------------------------
#  Ejecucion
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "Pyxis - suite funcional" -ForegroundColor Cyan
Write-Host "  ejecutable : $Exe"
Write-Host "  ejemplar   : $(if ($Sample) { $Sample } else { '(ninguno: se omitiran los casos de 8K)' })"
Write-Host "  casos      : $($cases.Count)"
Write-Host ""

$results = @()

foreach ($file in $cases) {
    $name = $file.BaseName
    Write-Host ("  {0,-30}" -f $name) -NoNewline
    $started = Get-Date

    try {
        # Cada caso corre en su propio ambito para que una variable olvidada no
        # se filtre al siguiente y produzca un verde que no es tal.
        & {
            param($Ctx)
            . $file.FullName
        } $context

        $elapsed = ((Get-Date) - $started).TotalSeconds
        Write-Host ("  OK     {0,5:N1} s" -f $elapsed) -ForegroundColor Green
        $results += [pscustomobject]@{ Name = $name; Status = 'OK'; Seconds = $elapsed; Message = '' }

    } catch [System.Management.Automation.ItemNotFoundException] {
        $reason = ($_.Exception.Message -replace '^OMITIDO:\s*', '')
        Write-Host ("  OMITIDO       {0}" -f $reason) -ForegroundColor DarkYellow
        $results += [pscustomobject]@{ Name = $name; Status = 'OMITIDO'; Seconds = 0; Message = $reason }

    } catch {
        $elapsed = ((Get-Date) - $started).TotalSeconds
        Write-Host ("  FALLO  {0,5:N1} s" -f $elapsed) -ForegroundColor Red
        Write-Host "      $($_.Exception.Message)" -ForegroundColor Red
        $results += [pscustomobject]@{
            Name = $name; Status = 'FALLO'; Seconds = $elapsed
            Message = $_.Exception.Message
        }
    }

    # Un proceso huerfano de un caso que fallo a medias se comeria la ventana
    # del siguiente y encadenaria fallos que no son reales.
    Get-Process pyxis -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 400
}

# ---------------------------------------------------------------------------
#  Resumen
# ---------------------------------------------------------------------------
$ok      = @($results | Where-Object Status -eq 'OK').Count
$failed  = @($results | Where-Object Status -eq 'FALLO')
$skipped = @($results | Where-Object Status -eq 'OMITIDO')

Write-Host ""
Write-Host ("  {0} correctos, {1} fallidos, {2} omitidos  ({3:N0} s en total)" -f
            $ok, $failed.Count, $skipped.Count, (($results | Measure-Object Seconds -Sum).Sum)) -ForegroundColor Cyan

if ($failed.Count -gt 0) {
    Write-Host ""
    foreach ($f in $failed) { Write-Host "  FALLO $($f.Name): $($f.Message)" -ForegroundColor Red }
}

if (-not $KeepArtifacts) {
    Remove-Item $workDir -Recurse -Force -ErrorAction SilentlyContinue
} else {
    Write-Host ""
    Write-Host "  Registros y salidas en $workDir"
}

exit ($(if ($failed.Count -gt 0) { 1 } else { 0 }))
