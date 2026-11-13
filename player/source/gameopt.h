/* Our64 player: per-game options (Game options: X on a game in Our ROMs, or
 * the pause menu; 2026-11-11). Kept in config/game-options.txt, one game a
 * line, by ROM file name as favorites are, then what is set for it, tabs
 * between:
 *   <file name><TAB>720p<TAB>mode=2<TAB>shape=1<TAB>speed=1
 * Only what the player set is written; a game with nothing set has no line.
 * native: the game draws at its own resolution, most often 320x240 (GLideN64's
 * native resolution factor; 2026-11-13: Automobili Lamborghini's letters and
 * F-1 World Grand Prix's menu line are right with it, as on the N64), and the
 * picture is enlarged after. 720p: the game draws at 720 lines (480 without
 * it). 720p is never chosen
 * for the player: 5 of 16 games tested held their speed with it
 * (results/res720-2026-11-11, light-2026-11-11). mode, shape, speed: this
 * game's own Screen mode (MODE_), Shape (PICTURE_) and Speed display (0 off,
 * 1 on) in place of Settings > Picture's; left out, Settings' apply.
 * Plain C: tests/ runs it. */
#ifndef OUR64_GAMEOPT_H
#define OUR64_GAMEOPT_H
#ifdef __cplusplus
extern "C" {
#endif

enum { GAMEOPT_NATIVE = 240 }; /* gameopt_lines: the game's own resolution */
enum { GAMEOPT_MODE, GAMEOPT_SHAPE, GAMEOPT_SPEED, GAMEOPT_FILTER, GAMEOPT_PICTURE_KEYS };

void gameopt_parse(const char *text);                 /* replaces what is held */
int  gameopt_read(const char *path);                  /* games with options, 0 without a file */
int  gameopt_text(char *out, unsigned size);          /* the file's text; its length (out NULL: only that) */
int  gameopt_lines(const char *file);                 /* 480 or 720, GAMEOPT_NATIVE for native */
int  gameopt_set_lines(const char *file, int lines);  /* 0, or -1 when no room is left */
int  gameopt_picture(const char *file, int key);      /* the game's own value, -1: Settings' applies */
int  gameopt_set_picture(const char *file, int key, int value); /* value -1 clears it; 0, or -1 when no room is left */
void gameopt_renamed(const char *from, const char *to); /* the ROM file was renamed: its line follows */

#ifdef __cplusplus
}
#endif
#endif
