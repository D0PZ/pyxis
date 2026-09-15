<#
.SYNOPSIS
    Compila Pyxis. Prepara el entorno de MSVC y llama a CMake.

.DESCRIPTION
    Ninja no hereda el entorno de Visual Studio como si hace MSBuild, asi que
    hay que importar vcvars64.bat antes de configurar. Este script localiza el
    Visual Studio instalado con vswhere, importa sus variables y lanza la
    compilacion.

    La primera ejecucion compila FFmpeg desde fuente con vcpkg y tarda entre 15
    y 45 minutos segun la maquina. Las siguientes reutilizan la cache binaria de
    vcpkg y son casi instantaneas.

.PARAMETER Configuration
    Release (por defecto), Debug o RelWithDebInfo.

.PARAMETER Clean
    Borra el directorio de compilacion antes de empezar.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Configuration Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug', 'relwithdebinfo')]
    [string]$Configuration = 'release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# --- Herramientas -----------------------------------------------------------
# winget modifica el PATH del sistema, pero las sesiones ya abiertas no se
# enteran hasta reiniciarse. En lugar de exigir al usuario que reinicie la
# terminal, se buscan los ejecutables en sus rutas de instalacion conocidas.
function Resolve-Tool {
    param([string]$Name, [string[]]$Candidates)

    $found = Get-Command $Name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }

    # Los candidatos admiten comodines: winget guarda cada paquete bajo un
    # directorio cuyo nombre incluye el identificador de la fuente, que varia.
    foreach ($candidate in $Candidates) {
        $match = Get-Item -Path $candidate -ErrorAction SilentlyContinue |
                 Select-Object -First 1
        if ($match) {
            $env:PATH = "$($match.DirectoryName);$env:PATH"
            return $match.FullName
        }
    }
    return $null
}

$cmakeExe = Resolve-Tool 'cmake' @(
    (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\cmake.exe')
)
if (-not $cmakeExe) {
    throw 'No se encontro CMake. Instalalo con: winget install Kitware.CMake'
}

$ninjaExe = Resolve-Tool 'ninja' @(
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\ninja.exe'),
    (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe'),
    (Join-Path $env:ProgramFiles 'Ninja\ninja.exe')
)
if (-not $ninjaExe) {
    throw 'No se encontro Ninja. Instalalo con: winget install Ninja-build.Ninja'
}

Write-Host "CMake      : $cmakeExe" -ForegroundColor DarkGray
Write-Host "Ninja      : $ninjaExe" -ForegroundColor DarkGray

# --- vcpkg ------------------------------------------------------------------
if (-not $env:VCPKG_ROOT) {
    $candidates = @(
        (Join-Path $env:USERPROFILE 'vcpkg'),
        'C:\vcpkg',
        (Join-Path $repoRoot 'vcpkg')
    )
    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate 'vcpkg.exe')) { $env:VCPKG_ROOT = $candidate; break }
    }
}

if (-not $env:VCPKG_ROOT) {
    Write-Host 'No se encontro vcpkg. Instalandolo en ~\vcpkg ...' -ForegroundColor Yellow
    $target = Join-Path $env:USERPROFILE 'vcpkg'
    git clone --depth 1 https://github.com/microsoft/vcpkg.git $target
    & (Join-Path $target 'bootstrap-vcpkg.bat') -disableMetrics
    $env:VCPKG_ROOT = $target
}
Write-Host "vcpkg      : $env:VCPKG_ROOT" -ForegroundColor DarkGray

# --- Entorno de MSVC --------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'No se encontro vswhere.exe. Instala Visual Studio Build Tools con la carga "Desarrollo para el escritorio con C++".'
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw 'No se encontro ninguna instalacion de MSVC x64.'
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "No se encontro vcvars64.bat en $vsPath" }
Write-Host "MSVC       : $vsPath" -ForegroundColor DarkGray

# vcvars64.bat solo sabe exportar a cmd, asi que se ejecuta ahi y se vuelcan las
# variables resultantes de vuelta a esta sesion de PowerShell.
& cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
    }
}

# --- Compilacion ------------------------------------------------------------
$buildDir = Join-Path $repoRoot "build\$Configuration"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Limpiando $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}

# Windows PowerShell 5.1 convierte cada linea de stderr de un ejecutable nativo
# en un ErrorRecord. Con ErrorActionPreference = 'Stop', un simple aviso de
# CMake abortaria la compilacion. Aqui se desactiva esa conversion y se usa el
# codigo de salida, que es la unica senal fiable de un proceso externo.
$failure = $null
$previousPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'

Push-Location $repoRoot
try {
    & $cmakeExe --preset $Configuration
    if ($LASTEXITCODE -ne 0) { throw "La configuracion de CMake fallo ($LASTEXITCODE)" }

    & $cmakeExe --build --preset $Configuration
    if ($LASTEXITCODE -ne 0) { throw "La compilacion fallo ($LASTEXITCODE)" }

    $exe = Join-Path $buildDir 'bin\pyxis.exe'
    if (Test-Path $exe) {
        $size = [math]::Round((Get-Item $exe).Length / 1MB, 1)
        Write-Host ''
        Write-Host "Listo: $exe  ($size MB, autocontenido)" -ForegroundColor Green
    }
} catch {
    $failure = $_.Exception.Message
} finally {
    Pop-Location
    $ErrorActionPreference = $previousPreference
}

if ($failure) {
    Write-Host ''
    Write-Host $failure -ForegroundColor Red
    exit 1
}
