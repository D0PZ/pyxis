# La barra inferior se esconde a los 2 s sin actividad, y al perder el foco.
#
# Dos reglas con el mismo fin: que el video se vea entero cuando nadie esta
# tocando nada. La segunda importa mas de lo que parece, porque con la ventana
# en segundo plano el raton se mueve en OTRA aplicacion: la inactividad nunca
# llegaria a cumplirse y la barra se quedaria encendida para siempre.
#
# Todo se mide contando TRANSICIONES, no comprobando si el mensaje aparece. Con
# lo segundo, el ocultado que provoca la prueba no se distingue de cualquier
# otro anterior, y el caso pasa sin haber demostrado nada.

$session = Start-Pyxis -Exe $Ctx.Exe -Media $Ctx.AvSync `
                       -Log (Join-Path $Ctx.Work 'autoocultar.log')
try {
    $VISIBLE  = 'Controles visibles'
    $BY_IDLE  = 'Controles ocultos por inactividad'
    $BY_FOCUS = 'Controles ocultos por perder el foco'

    $middle = [int]($session.Width / 2)
    $upper  = [int]($session.Height / 3)

    # --- 1. Aparece al mover el raton --------------------------------------
    Send-PyxisClick $session -X $middle -Y $upper
    $shown = Wait-PyxisLogCount $session -Pattern $VISIBLE -AtLeast 1 -TimeoutSeconds 8
    Assert-AtLeast $shown 1 "la barra no llego a mostrarse con el raton encima"

    # --- 2. Se esconde sola ------------------------------------------------
    #
    # El plazo es de 2 s; se conceden 8 porque en una maquina cargada el hilo de
    # presentacion puede tardar en pasar por la decision. Lo que se comprueba es
    # que ocurre, no cuando exactamente: atar la prueba al reloj la volveria
    # intermitente sin vigilar nada mas.
    $idle = Wait-PyxisLogCount $session -Pattern $BY_IDLE -AtLeast 1 -TimeoutSeconds 8
    Assert-AtLeast $idle 1 "la barra no se escondio estando el raton quieto"
    Write-Detail "se escondio sola tras la inactividad"

    # --- 3. Vuelve al moverse el raton -------------------------------------
    $before = @(Get-PyxisLog $session -Pattern $VISIBLE).Count
    Send-PyxisClick $session -X ($middle + 40) -Y $upper
    $after = Wait-PyxisLogCount $session -Pattern $VISIBLE -AtLeast ($before + 1) -TimeoutSeconds 6
    Assert-AtLeast $after ($before + 1) "la barra no volvio al mover el raton"

    # --- 4. Se esconde al perder el foco, sin esperar los 2 s --------------
    #
    # La pulsacion previa reinicia el reloj de inactividad a proposito: si la
    # barra se escondiera igualmente por tiempo, la prueba no distinguiria una
    # causa de la otra.
    Send-PyxisKey $session -Key 'Space'
    Start-Sleep -Milliseconds 200

    $beforeFocus = @(Get-PyxisLog $session -Pattern $BY_FOCUS).Count
    Send-PyxisFocus $session -Focused $false

    $lost = Wait-PyxisLogCount $session -Pattern $BY_FOCUS `
                -AtLeast ($beforeFocus + 1) -TimeoutSeconds 5
    Assert-AtLeast $lost ($beforeFocus + 1) `
                   "la barra siguio visible con la ventana en segundo plano"
    Write-Detail "se escondio al perder el foco antes de cumplirse los 2 s"

    # --- 5. Y vuelve al recuperarlo ----------------------------------------
    $before = @(Get-PyxisLog $session -Pattern $VISIBLE).Count
    Send-PyxisFocus $session -Focused $true
    Send-PyxisClick $session -X $middle -Y $upper
    $after = Wait-PyxisLogCount $session -Pattern $VISIBLE -AtLeast ($before + 1) -TimeoutSeconds 6
    Assert-AtLeast $after ($before + 1) "la barra no volvio al recuperar el foco"

    Assert-NoErrors $session
} finally {
    Stop-Pyxis $session
}
