# Avance fotograma a fotograma hacia adelante: 120 pasos sin quedarse clavado.
#
# El fallo que persigue este caso es el que llevo tres intentos arreglar: la
# flecha derecha llegaba al final del segmento precargado y se plantaba, porque
# el demultiplexor se dormia empujando audio que nadie consumia.

$media = if ($Ctx.Sample) { $Ctx.Sample } else { $Ctx.AvSync }

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -Log (Join-Path $Ctx.Work 'avance.log')
try {
    Send-PyxisKey $session -Key 'Space'          # pausar
    Start-Sleep -Milliseconds 700

    $requested = 120
    Send-PyxisKey $session -Key 'Right' -Times $requested -PauseMs 60
    Start-Sleep -Seconds 2

    $steps = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms' |
               ForEach-Object { [int]([regex]::Match($_, 'Paso \+1 -> (\d+) ms').Groups[1].Value) })

    Assert-AtLeast $steps.Count ([int]($requested * 0.9)) "pasos completados"

    # El sintoma de "se pega" es el mismo milisegundo repetido: el paso se da
    # por hecho pero el fotograma no cambia.
    $stuck = 0
    for ($i = 1; $i -lt $steps.Count; $i++) {
        if ($steps[$i] -le $steps[$i - 1]) { $stuck++ }
    }

    Write-Detail "$($steps.Count) pasos de $requested; $($steps[0]) ms -> $($steps[-1]) ms"
    Assert-True ($stuck -eq 0) "$stuck pasos no avanzaron (el avance se queda clavado)"
    Assert-True ($steps[-1] -gt $steps[0]) "la posicion no avanzo en absoluto"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
