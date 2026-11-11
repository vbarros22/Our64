# Pendências

Bugs e melhorias anotados para corrigir depois.

## Bugs

### Desligar o console depois do HOME trava em "Saving"

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
- **Próximo passo:** linhas de log em cada etapa do fim de `main()` para ver
  onde para.

### Texturas "quadriculadas" no Majora's Mask

- **Onde:** menu de pausa (Quest Status); a confirmar se também acontece no
  jogo em si.
- **Já testado:** `exact-texcoords` não muda nada, então a reescrita do filtro
  (`fetchTexelFilter`) e o caminho rápido de coordenadas estão descartados.
- **Suspeitas:** o jogo pede filtro "point" e a renderização a 640×480 transforma
  cada texel num bloco 2×2; ou quantização de cor para 16 bits sem pontilhado
  (`enableDitheringQuantization=1`, `enableDitheringPattern=0`).
- **A coletar:** screenshots em PNG do GamePad (ScreenshotWUPS).
