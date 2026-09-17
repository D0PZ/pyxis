# Repliegue por software (--no-hardware).
#
# Es el camino que se usa cuando D3D11VA no puede con el codec, y el unico que
# convierte con swscale y sube la textura a mano. Estaba sin probar, lo que es
# incomodo: es justo el camino que corre en la maquina de quien reporta un
# fallo con una tarjeta rara.
#
# Se prueba con el material HDR ademas del normal porque la normalizacion a P010
# de 10 bits solo existe en esta ruta.

foreach ($media in @($Ctx.AvSync, $Ctx.Hdr)) {
    $name = Split-Path $media -Leaf

    $session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                           -ExtraArguments @('--no-hardware') `
                           -Log (Join-Path $Ctx.Work "software-$name.log")
    try {
        Start-Sleep -Seconds 3

        $decoder = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
        Assert-True ($decoder.Count -gt 0) "$name : no se abrio decodificador"
        Write-Detail "$name -> $($decoder[0] -replace '^.*\] ', '')"

        # Con --no-hardware no debe quedar ni rastro del pool de D3D11VA.
        $hw = @(Get-PyxisLog $session -Pattern 'pool de texturas D3D11VA|pool de \d+ texturas')
        Assert-True ($hw.Count -eq 0) "$name : se intento usar D3D11VA pese a --no-hardware"

        $color = @(Get-PyxisLog $session -Pattern 'Colorimetria:')
        Assert-True ($color.Count -gt 0) "$name : no se decodifico ningun fotograma por software"
        Write-Detail "   $($color[0] -replace '^.*\] ', '')"

        # El avance manual por software es el que mas facil se rompe: no hay
        # pool de texturas que limite el historial y la conversion es sincrona.
        Send-PyxisKey $session -Key 'Space'
        Start-Sleep -Milliseconds 500
        Send-PyxisKey $session -Key 'Right' -Times 20 -PauseMs 100
        Start-Sleep -Seconds 2

        $steps = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms')
        Assert-AtLeast $steps.Count 12 "$name : pasos por software"

        Assert-NoErrors $session
    } finally {
        Stop-Pyxis $session
    }
}
