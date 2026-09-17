<#
=============================================================================
 Pyxis.Testing - utilidades compartidas por los casos de prueba

 Pyxis no tiene interfaz de linea de comandos con la que comprobar nada: es una
 ventana. Las pruebas la manejan como lo haria una persona -enviando teclas y
 clics- y leen el resultado del registro, que para eso lleva `--verbose`.

 Los mensajes se envian con PostMessage en lugar de SendInput porque no
 necesitan el foco: la suite puede correr mientras se trabaja en otra ventana, y
 un clic accidental del usuario no la descarrila.

 La geometria de la barra de controles esta duplicada aqui a partir de
 render/Overlay.cpp. Es la unica duplicacion del arnes y esta reconocida: las
 posiciones no se pueden consultar desde fuera del proceso. Si cambian los
 anchos de Overlay.cpp hay que cambiarlos aqui, y por eso estan en un solo sitio
 en lugar de repartidos por los casos.
=============================================================================
#>

Set-StrictMode -Version Latest

# --- Geometria, copiada de render/Overlay.cpp -------------------------------
$script:Layout = @{
    Margin        = 24.0
    BarHeight     = 108.0
    SeekBarY      = 58.0    # desde el borde inferior
    ButtonRowY    = 26.0    # centro de la fila de botones
    ButtonSize    = 30.0
    ButtonGap     = 6.0
    ControlGap    = 10.0
    SpeedWidth    = 52.0
    SnapshotWidth = 34.0
    TrimWidth     = 34.0
    CropWidth     = 34.0
    FiltersWidth  = 34.0
}

$script:VirtualKeys = @{
    'Space' = 0x20; 'Left' = 0x25; 'Up' = 0x26; 'Right' = 0x27; 'Down' = 0x28
    'Escape' = 0x1B; 'Back' = 0x08; 'Enter' = 0x0D
    'A' = 0x41; 'B' = 0x42; 'C' = 0x43; 'E' = 0x45; 'F' = 0x46; 'G' = 0x47
    'I' = 0x49; 'J' = 0x4A; 'K' = 0x4B; 'L' = 0x4C; 'M' = 0x4D; 'O' = 0x4F
    'Q' = 0x51; 'R' = 0x52; 'S' = 0x53; 'X' = 0x58; 'Z' = 0x5A
    # Digitos: saltan al 0-90 % de la duracion.
    '0' = 0x30; '1' = 0x31; '2' = 0x32; '3' = 0x33; '4' = 0x34
    '5' = 0x35; '6' = 0x36; '7' = 0x37; '8' = 0x38; '9' = 0x39
}

function Initialize-Interop {
    if ('Pyxis.Interop' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Pyxis {
  [StructLayout(LayoutKind.Sequential)]
  public struct Rect { public int Left, Top, Right, Bottom; }

  public static class Interop {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);

    public const uint WM_KEYDOWN     = 0x0100;
    public const uint WM_KEYUP       = 0x0101;
    public const uint WM_MOUSEMOVE   = 0x0200;
    public const uint WM_LBUTTONDOWN = 0x0201;
    public const uint WM_LBUTTONUP   = 0x0202;
    public const uint WM_RBUTTONDOWN = 0x0204;
    public const uint WM_RBUTTONUP   = 0x0205;
    public const uint WM_MOUSEWHEEL  = 0x020A;
  }
}
'@
}

function ConvertTo-LParam([int]$X, [int]$Y) {
    return [IntPtr]((($Y -band 0xFFFF) -shl 16) -bor ($X -band 0xFFFF))
}

<#
 .SYNOPSIS
  Lanza Pyxis sobre un archivo y espera a que la ventana exista.

 .DESCRIPTION
  Devuelve una sesion que el resto de funciones toma como primer argumento. El
  volumen va a cero salvo que se pida otra cosa: una suite que suena a todo
  volumen no la ejecuta nadie dos veces.
#>
function Start-Pyxis {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]   $Media,
        [Parameter(Mandatory)][string]   $Exe,
        [Parameter(Mandatory)][string]   $Log,
        [string[]] $ExtraArguments = @(),
        [int]      $Volume         = 0,
        [int]      $TimeoutSeconds = 30
    )

    Initialize-Interop

    if (-not (Test-Path $Exe))   { throw "No existe el ejecutable: $Exe" }
    if (-not (Test-Path $Media)) { throw "No existe el medio: $Media" }
    if (Test-Path $Log)          { Remove-Item $Log -Force }

    # Cada argumento va entrecomillado: Start-Process los une con espacios sin
    # citarlos, y una ruta con espacios llegaria partida en varios.
    $arguments = @('--verbose', '--log', "`"$Log`"", '--volume', "$Volume") +
                 $ExtraArguments + @("`"$Media`"")

    $process = Start-Process -FilePath $Exe -ArgumentList $arguments -PassThru

    $session = [pscustomobject]@{
        Process   = $process
        Handle    = [IntPtr]::Zero
        Log       = $Log
        Media     = $Media
        Width     = 0
        Height    = 0
        StartedAt = Get-Date
    }

    # La ventana no existe hasta que el hilo de presentacion ha creado el
    # dispositivo grafico, que con material 8K puede tardar varios segundos.
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($process.HasExited) {
            $tail = Get-PyxisLog $session -Last 12
            throw "Pyxis salio con codigo $($process.ExitCode) antes de abrir ventana.`n$($tail -join "`n")"
        }
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            $session.Handle = $process.MainWindowHandle
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if ($session.Handle -eq [IntPtr]::Zero) {
        Stop-Pyxis $session
        throw "Pyxis no abrio ninguna ventana en $TimeoutSeconds s."
    }

    [void][Pyxis.Interop]::SetForegroundWindow($session.Handle)

    $rect = New-Object Pyxis.Rect
    [void][Pyxis.Interop]::GetClientRect($session.Handle, [ref]$rect)
    $session.Width  = $rect.Right
    $session.Height = $rect.Bottom

    # Abrir el medio va despues de crear la ventana. Esperar a verlo en el
    # registro evita que el caso empiece a teclear sobre un reproductor vacio,
    # que es el origen de la mitad de las pruebas intermitentes.
    [void](Wait-PyxisLog $session -Pattern 'Medio abierto|Decodificador de video' -TimeoutSeconds 25)

    return $session
}

function Stop-Pyxis {
    param([Parameter(Mandatory)]$Session)

    if ($null -eq $Session -or $null -eq $Session.Process) { return }
    if (-not $Session.Process.HasExited) {
        Stop-Process -Id $Session.Process.Id -Force -ErrorAction SilentlyContinue
    }
    # El registro se escribe con bufer: sin esta pausa, las ultimas lineas -que
    # suelen ser las interesantes- no han llegado al disco cuando se leen.
    Start-Sleep -Milliseconds 600
}

function Send-PyxisKey {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$Key,
        [int]$Times      = 1,
        [int]$PauseMs    = 60,
        [int]$HoldMs     = 45
    )

    if (-not $script:VirtualKeys.ContainsKey($Key)) {
        throw "Tecla desconocida: $Key"
    }
    $vk = [IntPtr]$script:VirtualKeys[$Key]

    for ($i = 0; $i -lt $Times; $i++) {
        [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_KEYDOWN, $vk, [IntPtr]0)
        Start-Sleep -Milliseconds $HoldMs
        [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_KEYUP, $vk, [IntPtr]0)
        Start-Sleep -Milliseconds $PauseMs
    }
}

function Send-PyxisClick {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][int]$X,
        [Parameter(Mandatory)][int]$Y,
        [ValidateSet('Left','Right')][string]$Button = 'Left',
        [int]$PauseMs = 350
    )

    $lp   = ConvertTo-LParam $X $Y
    $down = if ($Button -eq 'Left') { [Pyxis.Interop]::WM_LBUTTONDOWN } else { [Pyxis.Interop]::WM_RBUTTONDOWN }
    $up   = if ($Button -eq 'Left') { [Pyxis.Interop]::WM_LBUTTONUP }   else { [Pyxis.Interop]::WM_RBUTTONUP }

    # El movimiento previo importa: la barra solo se muestra cuando el raton
    # esta encima, y un clic sobre controles ocultos no lo recoge nadie.
    [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_MOUSEMOVE, [IntPtr]0, $lp)
    Start-Sleep -Milliseconds 180
    [void][Pyxis.Interop]::PostMessage($Session.Handle, $down, [IntPtr]1, $lp)
    Start-Sleep -Milliseconds 90
    [void][Pyxis.Interop]::PostMessage($Session.Handle, $up, [IntPtr]0, $lp)
    Start-Sleep -Milliseconds $PauseMs
}

function Send-PyxisDrag {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][int]$FromX, [Parameter(Mandatory)][int]$FromY,
        [Parameter(Mandatory)][int]$ToX,   [Parameter(Mandatory)][int]$ToY,
        [int]$Steps = 6
    )

    [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_MOUSEMOVE,
                                       [IntPtr]0, (ConvertTo-LParam $FromX $FromY))
    Start-Sleep -Milliseconds 150
    [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_LBUTTONDOWN,
                                       [IntPtr]1, (ConvertTo-LParam $FromX $FromY))
    Start-Sleep -Milliseconds 120

    # Por pasos y no de un salto: el arrastre de las esquinas del encuadre y el
    # de la barra reaccionan a cada WM_MOUSEMOVE, y un unico mensaje no se
    # parece en nada a lo que hace una mano.
    for ($i = 1; $i -le $Steps; $i++) {
        $x = [int]($FromX + ($ToX - $FromX) * $i / $Steps)
        $y = [int]($FromY + ($ToY - $FromY) * $i / $Steps)
        [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_MOUSEMOVE,
                                           [IntPtr]1, (ConvertTo-LParam $x $y))
        Start-Sleep -Milliseconds 70
    }

    [void][Pyxis.Interop]::PostMessage($Session.Handle, [Pyxis.Interop]::WM_LBUTTONUP,
                                       [IntPtr]0, (ConvertTo-LParam $ToX $ToY))
    Start-Sleep -Milliseconds 400
}

<#
 .SYNOPSIS
  Centro en pixeles de un control de la barra inferior.

 .DESCRIPTION
  Los casos piden 'Trim' o 'Snapshot' en lugar de calcular restas a partir del
  borde derecho. Cuando la barra cambie, se corrige una vez aqui.
#>
function Get-PyxisControlPoint {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)]
        [ValidateSet('Play','StepBack','StepForward','Speed','Snapshot','Trim','Crop','Filters','SeekBar')]
        [string]$Control,
        # Solo para SeekBar: fraccion de la duracion, de 0 a 1.
        [double]$Fraction = 0.0
    )

    $L = $script:Layout
    $w = $Session.Width
    $h = $Session.Height
    $rowY = [int]($h - $L.ButtonRowY)

    # Los controles de la derecha se colocan de derecha a izquierda, en el mismo
    # orden que Overlay::DrawRightControls.
    $right        = $w - $L.Margin
    $filtersLeft  = $right - $L.FiltersWidth
    $cropLeft     = $filtersLeft - $L.ControlGap - $L.CropWidth
    $trimLeft     = $cropLeft - $L.ControlGap - $L.TrimWidth
    $snapshotLeft = $trimLeft - $L.ControlGap - $L.SnapshotWidth
    $speedLeft    = $snapshotLeft - $L.ControlGap - $L.SpeedWidth

    switch ($Control) {
        'Play'        { return @{ X = [int]($L.Margin + $L.ButtonSize / 2); Y = $rowY } }
        'StepBack'    { return @{ X = [int]($L.Margin + $L.ButtonSize * 1.5 + $L.ButtonGap); Y = $rowY } }
        'StepForward' { return @{ X = [int]($L.Margin + $L.ButtonSize * 2.5 + 2 * $L.ButtonGap); Y = $rowY } }
        'Speed'       { return @{ X = [int]($speedLeft    + $L.SpeedWidth    / 2); Y = $rowY } }
        'Snapshot'    { return @{ X = [int]($snapshotLeft + $L.SnapshotWidth / 2); Y = $rowY } }
        'Trim'        { return @{ X = [int]($trimLeft     + $L.TrimWidth     / 2); Y = $rowY } }
        'Crop'        { return @{ X = [int]($cropLeft     + $L.CropWidth     / 2); Y = $rowY } }
        'Filters'     { return @{ X = [int]($filtersLeft  + $L.FiltersWidth  / 2); Y = $rowY } }
        'SeekBar'     {
            $barLeft  = $L.Margin
            $barRight = $w - $L.Margin
            return @{ X = [int]($barLeft + ($barRight - $barLeft) * $Fraction)
                      Y = [int]($h - $L.SeekBarY) }
        }
    }
}

function Get-PyxisLog {
    param(
        [Parameter(Mandatory)]$Session,
        [string]$Pattern,
        [int]$Last = 0
    )

    if (-not (Test-Path $Session.Log)) { return @() }

    # El proceso mantiene el archivo abierto; sin ReadWrite el acceso falla.
    $lines  = @()
    $stream = $null
    $reader = $null
    try {
        $stream = [System.IO.File]::Open($Session.Log, 'Open', 'Read', 'ReadWrite')
        $reader = New-Object System.IO.StreamReader($stream)
        $lines  = $reader.ReadToEnd() -split "`r?`n" | Where-Object { $_ -ne '' }
    } finally {
        if ($reader) { $reader.Dispose() }
        if ($stream) { $stream.Dispose() }
    }

    if ($Pattern) { $lines = @($lines | Where-Object { $_ -match $Pattern }) }
    if ($Last -gt 0 -and $lines.Count -gt $Last) { $lines = @($lines | Select-Object -Last $Last) }

    # OJO: PowerShell desenvuelve los arrays de un solo elemento al devolverlos,
    # asi que un unico acierto llega al caso como una cadena suelta. Envolverlo
    # aqui con la coma unaria NO es la solucion -produce un array dentro de otro
    # en cuanto quien llama pone su propio @()-. La regla es la contraria: los
    # casos envuelven con @() todo lo que vayan a contar o indexar.
    return $lines
}

<#
 .SYNOPSIS
  Espera a que aparezca una linea en el registro. Devuelve las coincidencias, o
  un array vacio si se agota el plazo.
#>
function Wait-PyxisLog {
    param(
        [Parameter(Mandatory)]$Session,
        [Parameter(Mandatory)][string]$Pattern,
        [int]$TimeoutSeconds = 20,
        [int]$PollMs = 400
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $hits = @(Get-PyxisLog $Session -Pattern $Pattern)
        if ($hits.Count -gt 0) { return $hits }
        if ($Session.Process.HasExited) { break }
        Start-Sleep -Milliseconds $PollMs
    }
    return @()
}

<#
 .SYNOPSIS
  Espera a que aparezca un archivo de salida nuevo y termine de escribirse.

 .DESCRIPTION
  Se busca por MARCA DE TIEMPO y no por nombre. Pyxis nombra los recortes con la
  posicion del intervalo, asi que dos ejecuciones de la misma prueba producen el
  mismo nombre: comparando contra una lista previa, la segunda vez el archivo se
  sobrescribe y la prueba concluye que no se genero nada.

  Las capturas y los recortes van a las carpetas del usuario, donde puede haber
  material suyo; por eso se compara contra el instante en que empezo el caso y
  nunca se toca nada anterior.
#>
function Wait-NewOutput {
    param(
        [Parameter(Mandatory)][string]$Directory,
        [Parameter(Mandatory)][datetime]$Since,
        [int]$TimeoutSeconds = 120
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $found = $null

    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Directory) {
            $found = Get-ChildItem $Directory -File |
                     Where-Object { $_.LastWriteTime -ge $Since } |
                     Sort-Object LastWriteTime -Descending |
                     Select-Object -First 1
            if ($found) { break }
        }
        Start-Sleep -Milliseconds 500
    }
    if (-not $found) { return $null }

    # Un archivo que aparece no es un archivo terminado: recodificar tarda, y
    # leerlo a medias da un fallo que no tiene nada que ver con lo que se prueba.
    $previous = -1
    while ((Get-Date) -lt $deadline) {
        $found.Refresh()
        if ($found.Length -eq $previous -and $found.Length -gt 0) { break }
        $previous = $found.Length
        Start-Sleep -Milliseconds 900
    }
    return $found
}

<#
 .SYNOPSIS
  Reabre un archivo con el propio Pyxis y devuelve las lineas de su registro.

 .DESCRIPTION
  Es la forma mas honesta de verificar lo que produce el programa: si Pyxis no
  sabe volver a abrir lo que acaba de escribir, el archivo esta mal, diga lo
  que diga cualquier otra herramienta.
#>
function Test-PyxisCanOpen {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Log,
        [int]$Seconds = 5
    )

    $verify = Start-Pyxis -Exe $Exe -Media $Path -Log $Log
    try {
        Start-Sleep -Seconds $Seconds
        return @{
            Opened = @(Get-PyxisLog $verify -Pattern 'Medio abierto')
            Audio  = @(Get-PyxisLog $verify -Pattern 'Audio: \d+ Hz')
            Video  = @(Get-PyxisLog $verify -Pattern 'Decodificador de video:')
            Color  = @(Get-PyxisLog $verify -Pattern 'Colorimetria:')
            Errors = @(Get-PyxisLog $verify -Pattern '\[ERR\]')
        }
    } finally {
        Stop-Pyxis $verify
    }
}

# --- Aserciones -------------------------------------------------------------
#
# Fallan lanzando. El corredor captura y marca el caso en rojo; lo que interesa
# de un fallo es la primera linea, no las veinte siguientes ejecutandose sobre
# un estado ya roto.

function Assert-True {
    param([Parameter(Mandatory)][bool]$Condition, [Parameter(Mandatory)][string]$Because)
    if (-not $Condition) { throw $Because }
}

function Assert-AtLeast {
    param(
        [Parameter(Mandatory)]$Actual,
        [Parameter(Mandatory)]$Expected,
        [Parameter(Mandatory)][string]$What
    )
    if ($Actual -lt $Expected) { throw "$What : se esperaban $Expected o mas, hubo $Actual" }
}

function Assert-NoErrors {
    param(
        [Parameter(Mandatory)]$Session,
        # Errores conocidos e inofensivos para este caso concreto.
        [string]$Ignoring
    )
    $errors = @(Get-PyxisLog $Session -Pattern '\[ERR\]')
    if ($Ignoring) { $errors = @($errors | Where-Object { $_ -notmatch $Ignoring }) }
    if ($errors.Count -gt 0) {
        throw "el registro tiene $($errors.Count) error(es):`n  $(Get-FirstLine $errors)"
    }
}

# Marca el caso como omitido en lugar de fallado: falta algo del entorno (un
# monitor HDR, un archivo que el usuario no tiene), no del programa.
function Skip-Case {
    param([Parameter(Mandatory)][string]$Reason)
    throw [System.Management.Automation.ItemNotFoundException]::new("OMITIDO: $Reason")
}

<#
 .SYNOPSIS
  Primera linea de una coleccion, o una cadena vacia si no hay ninguna.

 .DESCRIPTION
  Existe por una trampa de PowerShell: el mensaje de un Assert se interpola
  ANTES de evaluar la condicion, asi que "$($errores[0])" revienta con "indice
  fuera de los limites" justo cuando NO hay errores, que es el caso que pasa.
#>
function Get-FirstLine {
    param([Parameter(Mandatory)][AllowEmptyCollection()][AllowNull()]$Lines)
    $array = @($Lines)
    if ($array.Count -eq 0) { return '' }
    return [string]$array[0]
}

function Write-Detail {
    param([Parameter(Mandatory)][string]$Text)
    Write-Host "      $Text" -ForegroundColor DarkGray
}

Export-ModuleMember -Function Start-Pyxis, Stop-Pyxis, Send-PyxisKey, Send-PyxisClick,
                              Send-PyxisDrag, Get-PyxisControlPoint, Get-PyxisLog,
                              Wait-PyxisLog, Wait-NewOutput, Test-PyxisCanOpen,
                              Get-FirstLine, Assert-True, Assert-AtLeast,
                              Assert-NoErrors, Skip-Case, Write-Detail
