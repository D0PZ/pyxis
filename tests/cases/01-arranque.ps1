# Abre un archivo, comprueba que hay ventana, decodificador y cadena de intercambio.
#
# Es el caso mas barato y el que mas veces ha salvado la tarde: si este falla,
# el resto de la suite solo va a repetir el mismo fallo doce veces.

$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.AvSync `
                       -Log (Join-Path $Ctx.Work 'arranque.log')
try {
    $decoder = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
    Assert-True ($decoder.Count -gt 0) "no se abrio ningun decodificador de video"
    Write-Detail $decoder[0]

    $swap = @(Get-PyxisLog $session -Pattern 'Cadena de intercambio')
    Assert-True ($swap.Count -gt 0) "no se creo la cadena de intercambio"
    Write-Detail $swap[0]

    $color = @(Get-PyxisLog $session -Pattern 'Colorimetria:')
    Assert-True ($color.Count -gt 0) "no se decodifico ningun fotograma (falta la colorimetria)"
    Write-Detail $color[0]

    # av-sync.mp4 no lleva etiquetas de color, asi que se deduce por altura:
    # 360 lineas estan por debajo de 576 y toca BT.601. Los 8 bits tienen que
    # salir aunque el fotograma venga de la GPU, donde el formato del pixel es
    # opaco y su descriptor declara cero.
    Assert-True ($color[0] -match 'BT\.601 SDR 8 bits') `
                "colorimetria inesperada para av-sync.mp4: $($color[0])"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
