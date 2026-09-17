# La exportacion con ediciones conserva la pista de AUDIO.
#
# ClipExporter recodifica el video -no le queda otra si hay encuadre- pero COPIA
# el audio: ni el encuadre ni el brillo lo afectan, y recodificarlo solo
# degradaria el sonido. Esa copia nunca se habia ejecutado, porque el material
# con el que se probo el exportador era mudo.
#
# Lo que se hace mal aqui no da error: el multiplexor acepta encantado un
# archivo sin audio. El fallo aparece al reproducirlo, en silencio.

$since  = Get-Date

# Este caso EXIGE audio, asi que usa siempre el material generado.
$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.AvSync `
                       -Log (Join-Path $Ctx.Work 'audio-export.log')
try {
    $opened = @(Get-PyxisLog $session -Pattern 'Medio abierto')
    Assert-True ($opened[0] -notmatch 'audio=ninguno') `
                "el material de prueba no tiene audio; regenera tests/media con mkmedia"
    Write-Detail ($opened[0] -replace '^.*\] ', '')

    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 600

    # Intervalo: del 10 % al 80 %.
    $a = Get-PyxisControlPoint $session -Control 'SeekBar' -Fraction 0.10
    Send-PyxisClick $session -X $a.X -Y $a.Y
    Start-Sleep -Seconds 1
    Send-PyxisKey $session -Key 'A'

    $b = Get-PyxisControlPoint $session -Control 'SeekBar' -Fraction 0.80
    Send-PyxisClick $session -X $b.X -Y $b.Y
    Start-Sleep -Seconds 1
    Send-PyxisKey $session -Key 'B'

    # Un encuadre cualquiera fuerza el camino de recodificacion, que es el que
    # tiene la copia de audio. Sin edicion se iria por Trimmer, que copia los
    # dos flujos y no probaria nada nuevo.
    Send-PyxisKey $session -Key 'E'
    Start-Sleep -Milliseconds 700
    Send-PyxisDrag $session -FromX 4 -FromY 4 `
                            -ToX ([int]($session.Width * 0.25)) -ToY ([int]($session.Height * 0.25))
    Send-PyxisKey $session -Key 'E'
    Start-Sleep -Milliseconds 600

    $point = Get-PyxisControlPoint $session -Control 'Trim'
    Send-PyxisClick $session -X $point.X -Y $point.Y

    $done = @(Wait-PyxisLog $session `
                  -Pattern 'Recorte con ediciones guardado|la exportacion fallo|no se pudo' `
                  -TimeoutSeconds 180)
    Assert-True ($done.Count -gt 0) "la exportacion no termino en 180 s"
    Assert-True ($done[0] -match 'Recorte con ediciones guardado') "la exportacion fallo: $($done[0])"
    Write-Detail ($done[0] -replace '^.*\] ', '')

    $file = Wait-NewOutput -Directory $Ctx.Videos -Since $since -TimeoutSeconds 120
    Assert-True ($null -ne $file) "no aparecio ningun archivo en $($Ctx.Videos)"

    Assert-NoErrors $session
    Stop-Pyxis $session

    # La prueba de verdad: reabrirlo y ver que la pista sigue ahi.
    $check = Test-PyxisCanOpen -Exe $Ctx.Exe -Path $file.FullName `
                               -Log (Join-Path $Ctx.Work 'audio-export-verify.log') -Seconds 6
    Assert-True ($check.Opened.Count -gt 0) "el resultado no se puede reabrir"
    Write-Detail ($check.Opened[0] -replace '^.*\] ', '')

    Assert-True ($check.Opened[0] -notmatch 'audio=ninguno') `
                "la exportacion perdio la pista de audio"

    # Que el contenedor declare la pista no basta: WASAPI tiene que poder
    # abrirla y el decodificador entregar muestras.
    Assert-True ($check.Audio.Count -gt 0) `
                "el audio esta declarado pero no se pudo reproducir"
    Write-Detail ($check.Audio[0] -replace '^.*\] ', '')

    Assert-True ($check.Errors.Count -eq 0) "al reabrir: $(Get-FirstLine $check.Errors)"

    Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
} finally {
    Stop-Pyxis $session
}
