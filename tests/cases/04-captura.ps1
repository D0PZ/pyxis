# La captura se guarda a resolucion NATIVA, no a la de la ventana.
#
# Es lo que distingue la captura de un recorte de pantalla: el fotograma se
# vuelve a dibujar contra una textura del tamano del video, con el encuadre
# aplicado pero sin la interfaz encima.

Add-Type -AssemblyName System.Drawing

$media = if ($Ctx.Sample) { $Ctx.Sample } else { $Ctx.AvSync }
$since  = Get-Date

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -Log (Join-Path $Ctx.Work 'captura.log')
try {
    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 800

    # Por el boton de la barra y no por la tecla: asi se prueba tambien el
    # reparto del clic, que es donde se cuelan los errores al anadir controles.
    $point = Get-PyxisControlPoint $session -Control 'Snapshot'
    Send-PyxisClick $session -X $point.X -Y $point.Y

    $saved = @(Wait-PyxisLog $session -Pattern 'Captura guardada en' -TimeoutSeconds 30)
    Assert-True ($saved.Count -gt 0) "no se registro ninguna captura"
    Write-Detail ($saved[0] -replace '^.*\] ', '')

    # Resolucion del video segun el propio decodificador.
    $decoder = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
    $expected = [regex]::Match($decoder[0], '(\d+)x(\d+)')
    $expectedW = [int]$expected.Groups[1].Value
    $expectedH = [int]$expected.Groups[2].Value

    # Si el video es mas pequeno que la ventana, la comprobacion sigue valiendo
    # -una captura de la ventana daria 1280x720 y no las medidas del video- pero
    # es menos contundente. Con -Sample se prueba el caso que importa: 8K en una
    # ventana de 720p.
    if ($expectedW -le $session.Width) {
        Write-Detail "el video ($expectedW px) cabe en la ventana ($($session.Width) px); con -Sample la prueba es mas fuerte"
    }

    $file = Wait-NewOutput -Directory $Ctx.Images -Since $since -TimeoutSeconds 30
    Assert-True ($null -ne $file) "no aparecio ningun PNG en $($Ctx.Images)"

    try {
        $image = [System.Drawing.Image]::FromFile($file.FullName)
        $w = $image.Width; $h = $image.Height
        $image.Dispose()
    } catch {
        throw "el PNG no se puede leer: $($_.Exception.Message)"
    }

    Write-Detail "ventana $($session.Width)x$($session.Height) -> PNG ${w}x${h} (video ${expectedW}x${expectedH})"
    Assert-True ($w -eq $expectedW -and $h -eq $expectedH) `
                "la captura salio a ${w}x${h} y el video es ${expectedW}x${expectedH}"

    Assert-NoErrors $session
    Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
} finally {
    Stop-Pyxis $session
}
