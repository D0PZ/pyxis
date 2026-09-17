# Contenido HDR de 10 bits (PQ / BT.2020) en una pantalla SDR.
#
# El camino HDR nunca se habia ejecutado. Es el que mas facil pasa desapercibido
# roto, porque no lanza ningun error: simplemente se ve mal -lavado o
# quemado- y solo lo nota quien tenga material HDR a mano.
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

    if ($output[0] -match 'SDR') {
        Write-Detail "monitor SDR: se ejercita el mapeo de tonos PQ -> sRGB"
    } else {
        Write-Detail "monitor HDR: se ejercita el paso directo PQ -> PQ"
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
