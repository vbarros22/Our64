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

- **Sintoma:** linhas retas de 1 pixel na página do Quest Status (y = 139, 205,
  271… e x = 344, 509 no GamePad, blocos de ~80×32 texels) e degraus em
  zigue-zague no topo de troncos.
- **Testes (screenshots PNG do GamePad, 2026-11-11):** Smooth, N64 (3-point),
  `rw2` e `exact-texcoords` deixam a página **pixel a pixel idêntica**
  (`rw2` e `exact-texcoords` contra N64: 100% dos pixels iguais).
- **Referência:** o ares (RDP fiel ao hardware) mostra os mesmos blocos e o
  mesmo zigue-zague. É a arte do jogo; a 640×480 só fica mais nítido.
- Ficou a chave de diagnóstico `vshighp` (vertex shader em `highp`) para
  casos de emendas que dependam da precisão do vértice.

## A investigar

- Log público: as linhas `GX2 debug: ...` e `Texture filter: ...` não aparecem
  no `player-log.txt` da versão pública, então não dá para confirmar se uma
  chave de diagnóstico foi lida.

## Ideias para depois

- **TV look com intensidade:** além de Clean e CRT, opções como "CRT leve" e
  "CRT forte". O shader já tem os dois parâmetros: `edge` (brilho das bordas
  da linha, hoje 0,45 a 0,75) e a força da grade (hoje 0,9 a 1,0). Um nível
  forte teria bordas em ~0,3 a 0,6, com o ganho repondo o brilho do mesmo
  jeito. A primeira versão (gaussiana sem compensação, bordas na metade)
  ficou escura demais na TV (2026-11-12).
- **CRT em TV 1080p:** hoje o quadro CRT é feito em 1280×720 e a cópia para o
  scan buffer estica para 1080; vale medir se as linhas ficam irregulares e,
  se ficarem, desenhar direto em 1440×1080 (4,5 linhas da TV por linha do N64).
