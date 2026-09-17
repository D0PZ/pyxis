# Retroceso fotograma a fotograma, y que el avance siga vivo despues del final.
#
# Retroceder es lo caro: no se puede decodificar hacia atras, asi que cada paso
# fuera del historial obliga a rebobinar y redecodificar el grupo de imagenes.
# Este caso vigila dos cosas distintas:
#
#   - que el historial absorba la mayoria de los pasos (si cada paso rebobina,
#     el dimensionado por VRAM de ComputeHistoryLimit dejo de funcionar);
#   - que pedir pasos adelante al final del archivo no deje colgado el avance.
#     Ese fue un fallo real: stepPending_ se quedaba levantado para siempre y a
#     partir de ahi ninguna flecha volvia a responder.

$media = if ($Ctx.Sample) { $Ctx.Sample } else { $Ctx.AvSync }

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -Log (Join-Path $Ctx.Work 'retroceso.log')
try {
    $limit = @(Get-PyxisLog $session -Pattern 'Historial de avance manual')
    if ($limit.Count -gt 0) { Write-Detail ($limit[0] -replace '^.*\] ', '') }

    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 500

    # Material por detras que retroceder.
    Send-PyxisKey $session -Key 'L' -Times 2 -PauseMs 400
    Start-Sleep -Seconds 2

    $requested = 60
    Send-PyxisKey $session -Key 'Left' -Times $requested -PauseMs 150
    Start-Sleep -Seconds 3

    $back = @(Get-PyxisLog $session -Pattern 'Paso -1 -> \d+ ms')
    Assert-AtLeast $back.Count ([int]($requested * 0.75)) "pasos atras completados"

    $rewinds = @($back | Where-Object { $_ -match 'rebobinado' }).Count
    $ratio   = if ($rewinds -gt 0) { [math]::Round($back.Count / $rewinds, 1) } else { [double]$back.Count }
    Write-Detail "$($back.Count) pasos atras, $rewinds rebobinados ($ratio pasos por rebobinado)"

    # Con el historial dimensionado por VRAM se median 20 pasos por rebobinado.
    # Se exige la mitad para no atar la prueba a una tarjeta concreta.
    Assert-AtLeast $ratio 8 "pasos por rebobinado (el historial no esta absorbiendo)"

    # --- El avance sobrevive al final del archivo ---------------------------
    Send-PyxisKey $session -Key 'Right' -Times 1   # limpia el estado
    Start-Sleep -Milliseconds 500

    Send-PyxisKey $session -Key '9'      # salto al 90 %
    Start-Sleep -Seconds 3

    Send-PyxisKey $session -Key 'Right' -Times 25 -PauseMs 120
    Start-Sleep -Seconds 2

    # Vuelta al medio: si el avance quedo colgado, aqui no aparece ni un paso.
    $middle = Get-PyxisControlPoint $session -Control 'SeekBar' -Fraction 0.5
    Send-PyxisClick $session -X $middle.X -Y $middle.Y
    Start-Sleep -Seconds 3

    $beforeRevive = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms').Count
    Send-PyxisKey $session -Key 'Right' -Times 6 -PauseMs 350
    Start-Sleep -Seconds 2
    $afterRevive = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms').Count

    Write-Detail "tras el final: $($afterRevive - $beforeRevive) de 6 pasos volvieron a responder"
    Assert-AtLeast ($afterRevive - $beforeRevive) 3 `
                   "el avance quedo colgado despues de llegar al final"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
