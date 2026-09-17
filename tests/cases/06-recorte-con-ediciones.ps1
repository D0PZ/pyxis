# Recorte A/B que ademas lleva encuadre: obliga a recodificar.
#
# En cuanto hay un encuadre o un ajuste de imagen, copiar el flujo dejaria de
# ser fiel a lo que se ve en pantalla, asi que ClipExporter vuelve a codificar
# pasando cada fotograma por el mismo shader que el reproductor. Lo que se
# comprueba aqui es que el resultado sale con las dimensiones RECORTADAS, no con
# las del original.

$media  = if ($Ctx.Sample) { $Ctx.Sample } else { $Ctx.AvSync }
$since  = Get-Date

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -Log (Join-Path $Ctx.Work 'recorte-ediciones.log')
try {
    $decoder = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
    $original = [regex]::Match($decoder[0], '(\d+)x(\d+)')
    $originalW = [int]$original.Groups[1].Value
    $originalH = [int]$original.Groups[2].Value

    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 600

    Send-PyxisKey $session -Key 'A'
    Send-PyxisKey $session -Key 'L'          # +10 s
    Start-Sleep -Seconds 2
    Send-PyxisKey $session -Key 'B'

    # Editor de encuadre: se cierra el marco arrastrando la esquina superior
    # izquierda hacia el centro.
    Send-PyxisKey $session -Key 'E'
    Start-Sleep -Milliseconds 700
    Send-PyxisDrag $session -FromX 4 -FromY 4 `
                            -ToX ([int]($session.Width * 0.3)) -ToY ([int]($session.Height * 0.3))
    Send-PyxisKey $session -Key 'E'          # aplicar
    Start-Sleep -Milliseconds 600

    $point = Get-PyxisControlPoint $session -Control 'Trim'
    Send-PyxisClick $session -X $point.X -Y $point.Y

    # Recodificar tarda: en 8K son decenas de segundos.
    #
    # No se espera por '[ERR]' a secas: el exportador ELIGE el codificador
    # abriendolo, y cuando hevc_mf rechaza la resolucion suelta un
    # MF_E_INVALIDMEDIATYPE por el registro de FFmpeg antes de que se pruebe
    # H.264. Ese error es parte del funcionamiento normal.
    $done = @(Wait-PyxisLog $session `
                  -Pattern 'Recorte con ediciones guardado|la exportacion fallo|no se pudo' `
                  -TimeoutSeconds 300)
    Assert-True ($done.Count -gt 0) "la exportacion no termino en 300 s"
    Assert-True ($done[0] -match 'Recorte con ediciones guardado') "la exportacion fallo: $($done[0])"
    Write-Detail ($done[0] -replace '^.*\] ', '')

    # "... ('ruta' (1920x1080, 300 fotogramas, h264_mf)"
    $exported = [regex]::Match($done[0], '\((\d+)x(\d+), (\d+) fotogramas')
    Assert-True $exported.Success "el registro no dice las dimensiones exportadas"
    $exportedW = [int]$exported.Groups[1].Value
    $exportedH = [int]$exported.Groups[2].Value
    $frames    = [int]$exported.Groups[3].Value

    Write-Detail "original ${originalW}x${originalH} -> exportado ${exportedW}x${exportedH}, $frames fotogramas"
    Assert-True ($exportedW -lt $originalW -and $exportedH -lt $originalH) `
                "el encuadre no llego a la exportacion: salio a ${exportedW}x${exportedH}"
    Assert-AtLeast $frames 10 "fotogramas exportados"

    $file = Wait-NewOutput -Directory $Ctx.Videos -Since $since -TimeoutSeconds 120
    Assert-True ($null -ne $file) "no aparecio ningun archivo en $($Ctx.Videos)"
    Write-Detail "$($file.Name) - $([math]::Round($file.Length / 1MB, 1)) MB"

    # hevc_mf rechazando la resolucion es el sondeo, no un fallo.
    Assert-NoErrors $session -Ignoring 'hevc_mf|MF_E_INVALIDMEDIATYPE'
    Stop-Pyxis $session

    $check = Test-PyxisCanOpen -Exe $Ctx.Exe -Path $file.FullName `
                               -Log (Join-Path $Ctx.Work 'recorte-ediciones-verify.log')
    Assert-True ($check.Opened.Count -gt 0) "el resultado no se puede reabrir"

    $reopened = Get-FirstLine $check.Video
    Assert-True ($reopened -match "${exportedW}x${exportedH}") `
                "al reabrir, el video no mide ${exportedW}x${exportedH}: $reopened"

    # Un recorte de mas de 4096 pixeles de ancho tiene un compromiso conocido:
    # HEVC lo rechaza -Media Foundation no admite esas medidas- y hay que
    # codificar en H.264, que a esa anchura ya no cabe en el decodificador por
    # hardware. Pyxis lo avisa al exportar; al reabrirlo, D3D11VA falla y se
    # replieg a software. Es el comportamiento correcto, no un fallo, pero
    # tiene que estar avisado y el archivo tiene que reproducirse igual.
    if ($exportedW -gt 4096) {
        $warned = @(Get-PyxisLog $session -Pattern 'HEVC rechazo .* se usa H\.264')
        Assert-True ($warned.Count -gt 0) `
                    "se exporto a ${exportedW} px de ancho en H.264 sin avisar de que hara falta software"
        Write-Detail ($warned[0] -replace '^.*\] ', '')

        $ignore = 'hwaccel initialisation|Failed setup for format d3d11'
    } else {
        $ignore = $null
    }

    $reopenErrors = @($check.Errors | Where-Object { -not $ignore -or $_ -notmatch $ignore })
    Assert-True ($reopenErrors.Count -eq 0) "al reabrir: $(Get-FirstLine $reopenErrors)"

    Remove-Item $file.FullName -Force -ErrorAction SilentlyContinue
} finally {
    Stop-Pyxis $session
}
