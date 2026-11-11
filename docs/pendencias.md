# Pendências

Bugs e melhorias anotados para corrigir depois.

## Bugs

### ✅ Resolvido: desligar o console trava em "Saving"

- **Como reproduzir:** num jogo, apertar HOME (abre o menu de pausa do
  Our64) e então desligar o console pelo botão POWER.
- **O que acontece:** o console fica parado na tela de "Saving" e não
  desliga.
- **Esperado:** gravar o autosave e o save do jogo e desligar normalmente.
- **Pistas no código:** o caminho é `leave(LEAVE_SYSTEM)` em
  `player/source/pause_wiiu.c` (`saves_flush`, `writer_drain`,
  `states_autosave`). Suspeitas: espera por gravação no SD que nunca termina
  depois que o sistema pede o fechamento, ou a resposta ao ProcUI
  (release/exit) não chega a tempo.
- **Log de uma reprodução (2026-11-11):** a gravação termina, e a última
  linha é `SYSTEM: game stopped and saved 1021 ms after the system asked`.
  O travamento acontece depois disso, ao devolver o controle ao sistema, já no
  fim de `main()` em `player/source/main.c`: `usbfs_stop`, `audio_stop`,
  `report_save`, o laço `WHBProcIsRunning()` e depois `video_thread_stop`.
- **Suspeita principal:** o `RELEASE_FOREGROUND` é consumido pelo
  `ProcUIProcessMessages(TRUE)` do laço de jogo. Se o `WHBProcIsRunning()`
  não voltar a vê-lo, ninguém chama `ProcUIDrawDoneRelease()` e o sistema
  espera para sempre. Outra possibilidade: o callback de release do WHBGfx
  (`WHBGfxInit`) esbarrando no núcleo gráfico, que ainda desenha
  (`ui_redraw`).
- **Também trava:** POWER em jogo sem HOME antes, e POWER no menu Our ROMs.
  É o mesmo caminho, e isso reforça a suspeita principal.
- **Correção (confirmada no console em 2026-11-11: menu, em jogo e depois do HOME):** `main()` guarda o pedido do sistema
  (`g_asked`) e, depois de salvar, responde ele mesmo: `ProcUIDrawDoneRelease()`
  para "largar a tela", `WHBProcStopRunning()` para "sair". Linhas de log
  `SYSTEM: ...` em cada etapa do encerramento mostram onde para, se ainda
  parar.

### ❎ Não é bug: texturas "quadriculadas" no Majora's Mask

- **Conclusão (screenshots PNG do GamePad, 2026-11-11):** a textura do Quest
  Status está com filtro bilinear normal (degradês suaves entre texels, sem
  blocos nem emendas em grade). O "granulado" é o próprio desenho da pedra,
  igual no N64. Nas fotos pela câmera, a granulação, os pixels da tela e o
  reflexo davam a impressão de quadriculado.
- `exact-texcoords` também não muda nada; o filtro fica como está.

## A investigar

- Caixinha `! e! e! e! e! e!` no canto superior esquerdo de um screenshot em
  jogo (Majora's Mask). Pode ser a notificação do ScreenshotWUPS com a fonte
  quebrada ou um aviso do Our64 que não renderizou.
