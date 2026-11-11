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
- **A coletar:** log (UDP / `sd:/wiiu/our64`) de uma reprodução.

### Texturas "quadriculadas" no Majora's Mask

- **Onde:** menu de pausa (Quest Status); a confirmar se também acontece no
  jogo em si.
- **Já testado:** `exact-texcoords` não muda nada, então a reescrita do filtro
  (`fetchTexelFilter`) e o caminho rápido de coordenadas estão descartados.
- **Suspeitas:** o jogo pede filtro "point" e a renderização a 640×480 transforma
  cada texel num bloco 2×2; ou quantização de cor para 16 bits sem pontilhado
  (`enableDitheringQuantization=1`, `enableDitheringPattern=0`).
- **A coletar:** screenshots em PNG do GamePad (ScreenshotWUPS).
