# Recuperacion tras perder el dispositivo grafico (--fault device-loss).
#
# Windows invalida el dispositivo al actualizar el controlador, al despertar de
# suspension o cuando el TDR corta un cuelgue de la GPU. Pyxis lo reconstruye
# todo -dispositivo, cadena, renderizador, reproductor- y retoma en la misma
# posicion. Ese codigo existia desde el primer dia y no se habia ejecutado
# jamas: ni siquiera se sabia si compilaba en el sentido de hacer lo que dice.
#
# El fallo inyectado lanza DXGI_ERROR_DEVICE_REMOVED desde dentro de Present,
# que es exactamente por donde llega el de verdad.

$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.AvSync `
                       -ExtraArguments @('--fault', 'device-loss') `
                       -Log (Join-Path $Ctx.Work 'device-loss.log')
try {
    # El fallo esta armado con unos segundos de margen para que salte con la
    # reproduccion ya en marcha; recuperarse de una ventana vacia no prueba nada.
    $lost = @(Wait-PyxisLog $session -Pattern 'se perdio el dispositivo grafico' -TimeoutSeconds 30)
    Assert-True ($lost.Count -gt 0) "el fallo inyectado no llego a dispararse"
    Write-Detail ($lost[0] -replace '^.*\] ', '')

    # La reconstruccion vuelve a crear la cadena y a abrir el decodificador. Si
    # cualquiera de las dos cosas no aparece, se quedo a medias.
    Start-Sleep -Seconds 5

    $swapChains = @(Get-PyxisLog $session -Pattern 'Cadena de intercambio')
    Assert-AtLeast $swapChains.Count 2 "cadenas de intercambio creadas (la original y la reconstruida)"

    $decoders = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
    Assert-AtLeast $decoders.Count 2 "aperturas del decodificador"
    Write-Detail "reconstruido: $($swapChains[-1] -replace '^.*\] ', '')"

    Assert-True ($session.Process.HasExited -eq $false) `
                "el proceso murio en lugar de recuperarse"

    # Y sigue vivo de verdad: que responda al avance manual despues.
    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 700
    Send-PyxisKey $session -Key 'Right' -Times 10 -PauseMs 130
    Start-Sleep -Seconds 2

    $stepsAfter = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms')
    Assert-AtLeast $stepsAfter.Count 5 "pasos despues de la reconstruccion"
    Write-Detail "$($stepsAfter.Count) pasos tras recuperar el dispositivo"

    # Una sola perdida: el fallo se desarma solo. Si aparecen varias es que
    # FaultFires no esta desarmandose y el bucle de reconstruccion se realimenta.
    $losses = @(Get-PyxisLog $session -Pattern 'se perdio el dispositivo grafico')
    Assert-True ($losses.Count -eq 1) `
                "el dispositivo se perdio $($losses.Count) veces; deberia ser una"

    Assert-NoErrors $session -Ignoring 'perdida de dispositivo inyectada'
} finally {
    Stop-Pyxis $session
}
