# Contenido HDR de 10 bits (PQ / BT.2020) en una pantalla SDR.
#
# El camino HDR nunca se habia ejecutado. Es el que mas facil pasa desapercibido
# roto, porque no lanza ningun error: simplemente se ve mal -lavado o
# quemado- y solo lo nota quien tenga material HDR a mano.
#
# OJO con lo que cubre: en un monitor SDR se recorre la decodificacion de 10
# bits, el reconocimiento de las etiquetas y el mapeo de tonos PQ -> sRGB. La
# SALIDA en HDR10 -y la interfaz compuesta en PQ- necesita un panel HDR, y el
# caso lo dice en su salida en lugar de fingir que lo probo.
#
# El archivo va etiquetado como PQ BT.2020 y codificado con FFV1 de 10 bits. El
# codec no importa: lo que elige el camino del shader son las ETIQUETAS. De paso
# se recorre el repliegue por software, porque FFV1 no tiene aceleracion.

$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.Hdr `
                       -Log (Join-Path $Ctx.Work 'hdr.log')
try {
    Start-Sleep -Seconds 4

    $color = @(Get-PyxisLog $session -Pattern 'Colorimetria:')
    Assert-True ($color.Count -gt 0) "no se decodifico ningun fotograma"
    Write-Detail ($color[0] -replace '^.*\] ', '')

    # Si esto falla, o el generador cambio o MapTransfer / MapMatrix dejaron de
    # reconocer las etiquetas, y todo el material HDR se estaria tratando como
    # SDR sin que nada avise.
    Assert-True ($color[0] -match 'PQ \(HDR10\)') `
                "la transferencia PQ no se reconocio: $($color[0])"
    Assert-True ($color[0] -match 'BT\.2020') `
                "la matriz BT.2020 no se reconocio: $($color[0])"
    Assert-True ($color[0] -match '10 bits') `
                "la profundidad de 10 bits no se detecto: $($color[0])"

    # La salida es SDR salvo que el monitor sea HDR; en cualquier caso la
    # cadena tiene que decir cual eligio, porque de ahi depende que rama del
    # shader se usa.
    $output = @(Get-PyxisLog $session -Pattern 'Salida configurada en')
    Assert-True ($output.Count -gt 0) "la cadena de intercambio no declaro el espacio de color"
    Write-Detail ($output[0] -replace '^.*\] ', '')

    # Que camino se recorre no depende solo del contenido: la salida solo pasa a
    # HDR10 si el panel lo admite. Distinguirlo importa porque son dos ramas
    # distintas del shader y una de las dos se queda sin probar.
    $panel = @(Get-PyxisLog $session -Pattern 'Pantalla: HDR10')
    Assert-True ($panel.Count -gt 0) "no se consulto la capacidad HDR del monitor"
    Write-Detail ($panel[0] -replace '^.*\] ', '')

    if ($panel[0] -match 'HDR10 si') {
        # Contenido PQ y panel PQ: la salida TIENE que pasar a HDR10, y con ella
        # la superposicion se compone en PQ en vez de en sRGB.
        Assert-True ($output[-1] -match 'HDR10') `
                    "el monitor admite HDR10 y la salida se quedo en SDR: $($output[-1])"
        Write-Detail "se ejercita el paso directo PQ -> PQ y la interfaz compuesta en PQ"
    } else {
        Assert-True ($output[0] -match 'SDR') `
                    "el monitor no admite HDR10 y aun asi la salida se puso en HDR: $($output[0])"
        Write-Detail "se ejercita el mapeo de tonos PQ -> sRGB"
        Write-Detail "SIN CUBRIR: la salida HDR10 necesita un monitor HDR activado en Windows"
    }

    # Que decodifique de verdad, no que abra y se quede parado.
    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 500
    Send-PyxisKey $session -Key 'Right' -Times 10 -PauseMs 120
    Start-Sleep -Seconds 2

    $steps = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms')
    Assert-AtLeast $steps.Count 5 "pasos sobre material de 10 bits"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
