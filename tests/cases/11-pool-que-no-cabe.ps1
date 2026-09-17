# El pool de texturas no cabe: se reintenta con lo justo, no se cae a software.
#
# Pyxis pide un pool D3D11VA generoso para que el decodificador no se bloquee
# esperando a que el renderizador suelte texturas. Ese pool no siempre cabe:
# FFmpeg lo limita a 64 texturas, y 64 fotogramas de 8K en 10 bits pasan de
# 3 GB, mas de lo que tienen muchas tarjetas.
#
# La respuesta correcta es reintentar con el minimo: la holgura es una comodidad
# y la aceleracion no. Caer a software por haber pedido de mas convertiria un
# problema de rendimiento leve en uno grave.
#
# Provocarlo de verdad exigiria agotar la VRAM, asi que --fault pool-full da por
# fallido el primer intento. Pedir un pool absurdo NO vale: av_hwframe_ctx_init
# recorta a 64 antes de intentar nada y la peticion nunca llega a ser grande.

if ($Ctx.Sample) {
    $media = $Ctx.Sample      # material acelerado de verdad, que es el caso real
} else {
    $media = $Ctx.AvSync      # H.264: tambien lo decodifica la GPU
}

$session = Start-Pyxis -Exe $Ctx.Exe -Media $media `
                       -ExtraArguments @('--fault', 'pool-full') `
                       -Log (Join-Path $Ctx.Work 'pool-full.log')
try {
    Start-Sleep -Seconds 3

    $injected = @(Get-PyxisLog $session -Pattern 'fallo inyectado: se da por fallido el pool')
    Assert-True ($injected.Count -gt 0) "el fallo inyectado no llego a aplicarse"

    $retry = @(Get-PyxisLog $session -Pattern 'no cupo.*se reintenta con')
    Assert-True ($retry.Count -gt 0) `
                "el pool imposible no provoco el reintento; revisa NegotiateFormat"
    Write-Detail ($retry[0] -replace '^.*\] ', '')

    # Lo que importa: que siga acelerado por hardware. Si aparece el repliegue,
    # el reintento no salvo la aceleracion y el remedio fue peor que la averia.
    $fallback = @(Get-PyxisLog $session -Pattern 'se decodificara por software')
    Assert-True ($fallback.Count -eq 0) `
                "se cayo a software en lugar de reintentar con un pool pequeno"

    $decoder = @(Get-PyxisLog $session -Pattern 'Decodificador de video:')
    Assert-True ($decoder.Count -gt 0) "no se abrio ningun decodificador"
    Write-Detail ($decoder[0] -replace '^.*\] ', '')

    # Y que decodifique. Un pool de cuatro texturas es justo, y si el
    # dimensionado del historial no lo respeta, aqui se bloquea.
    $color = @(Get-PyxisLog $session -Pattern 'Colorimetria:')
    Assert-True ($color.Count -gt 0) "no se decodifico ningun fotograma con el pool minimo"

    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 500
    Send-PyxisKey $session -Key 'Right' -Times 20 -PauseMs 110
    Start-Sleep -Seconds 3

    $steps = @(Get-PyxisLog $session -Pattern 'Paso \+1 -> \d+ ms')
    Assert-AtLeast $steps.Count 10 "pasos con el pool reducido"
    Write-Detail "$($steps.Count) pasos con el pool minimo"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
