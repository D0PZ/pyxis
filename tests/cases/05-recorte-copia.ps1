# Recorte A/B sin recodificar: copia de flujo.
#
# Sin encuadre ni ajustes, el recorte copia los paquetes tal cual. Eso lo hace
# instantaneo y sin perdida, y es el camino que debe tomarse siempre que no haya
# ediciones que obliguen a lo contrario.

$media  = if ($Ctx.Sample) { $Ctx.Sample } else { $Ctx.AvSync }
$since  = Get-Date

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -Log (Join-Path $Ctx.Work 'recorte-copia.log')
try {
    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 600

    # Marcar A cerca del principio y B mas adelante, por la barra.
    $a = Get-PyxisControlPoint $session -Control 'SeekBar' -Fraction 0.15
    Send-PyxisClick $session -X $a.X -Y $a.Y
    Start-Sleep -Seconds 2
    Send-PyxisKey $session -Key 'A'

    $b = Get-PyxisControlPoint $session -Control 'SeekBar' -Fraction 0.55
    Send-PyxisClick $session -X $b.X -Y $b.Y
    Start-Sleep -Seconds 2
    Send-PyxisKey $session -Key 'B'

    $point = Get-PyxisControlPoint $session -Control 'Trim'
    Send-PyxisClick $session -X $point.X -Y $point.Y

    $done = @(Wait-PyxisLog $session -Pattern 'Recorte guardado en|\[ERR\]' -TimeoutSeconds 90)
    Assert-True ($done.Count -gt 0) "el recorte no termino en 90 s"
    Assert-True ($done[0] -notmatch '\[ERR\]') "el recorte fallo: $($done[0])"
    Write-Detail ($done[0] -replace '^.*\] ', '')

    # Es copia de flujo: si aparece el mensaje de recodificacion, el programa
    # eligio el camino caro sin motivo.
    $reencoded = @(Get-PyxisLog $session -Pattern 'Recorte con ediciones')
    Assert-True ($reencoded.Count -eq 0) "se recodifico sin haber ninguna edicion"

    $file = Wait-NewOutput -Directory $Ctx.Videos -Since $since -TimeoutSeconds 60
    Assert-True ($null -ne $file) "no aparecio ningun archivo en $($Ctx.Videos)"
    Write-Detail "$($file.Name) - $([math]::Round($file.Length / 1MB, 1)) MB"

    Assert-NoErrors $session
    Stop-Pyxis $session

    # La comprobacion que importa: que el propio Pyxis sepa reabrirlo.
    $check = Test-PyxisCanOpen -Exe $Ctx.Exe -Path $file.FullName `
                               -Log (Join-Path $Ctx.Work 'recorte-copia-verify.log')
    Assert-True ($check.Opened.Count -gt 0) "el recorte no se puede reabrir"
    Assert-True ($check.Errors.Count -eq 0) "al reabrir el recorte: $(Get-FirstLine $check.Errors)"
    Write-Detail ($check.Opened[0] -replace '^.*\] ', '')

    Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
} finally {
    Stop-Pyxis $session
}
