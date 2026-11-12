/* Our64 player: the screen mode prompt ("How are you playing?") and the
 * words around it. Plain C, so tests/ runs it on the PC; drawing it is
 * ui_wiiu.cpp's business, reading controllers input_wiiu.c's.
 *
 * Coordinates are the UI canvas: 854x480, the GamePad's own pixels. The TV
 * shows the same canvas scaled. */
#ifndef OUR64_PROMPT_H
#define OUR64_PROMPT_H
#include <time.h>
#ifdef __cplusplus
extern "C" {
#endif

#define UI_W 854
#define UI_H 480

/* Screen modes, in card order, left to right. */
enum { MODE_TV, MODE_BOTH, MODE_GAMEPAD, MODES };
const char *mode_name(int mode); /* "TV only", "TV + GamePad", "GamePad only" */
int mode_uses_gamepad(int mode);

/* config/settings.cfg: "Screen mode = TV + GamePad". settings_read_mode
 * gives the mode, SETTINGS_MISSING when there is no such file or line, or
 * SETTINGS_DAMAGED when a line is there and cannot be read. */
enum { SETTINGS_MISSING = -1, SETTINGS_DAMAGED = -2 };
int settings_parse_mode(const char *text);
int settings_read_mode(const char *path);
int settings_format(char *out, unsigned size, int mode); /* the file's text; its length */

/* Every line of settings.cfg: "Screen mode = TV + GamePad", "Picture = 4:3",
 * "Last game = Super Mario 64 (USA).z64", "Recent games = ...". A missing line keeps its default
 * (mode -1: never chosen); a line that cannot be read is skipped and the
 * rest kept. Returns 0, SETTINGS_MISSING (nothing set) or SETTINGS_DAMAGED. */
enum { PICTURE_43, PICTURE_WIDE, PICTURES };
/* "Recent games = Banjo.z64 | Blast Corps.z64": Our ROMs' Recent tab. Five
 * fit the drawer without scrolling. '|' cannot be in a FAT file name. */
#define RECENT_GAMES 5
const char *picture_name(int picture); /* "4:3", "16:9 stretched" */
/* "Texture filter = Smooth": Settings > Picture. Smooth is GLideN64's standard
 * 4-point bilinear; N64 is the console's own 3-point filter. */
enum { FILTER_SMOOTH, FILTER_N64, FILTERS };
const char *filter_name(int filter); /* "Smooth", "N64" */
/* "TV look = Clean": Settings > Picture. CRT gives the TV scanlines and a
 * light aperture grille; the GamePad stays clean. */
enum { LOOK_CLEAN, LOOK_CRT, LOOKS };
const char *look_name(int look); /* "Clean", "CRT" */
/* "Theme = Standard", "Our ROMs = Covers": Settings > Theme (theme.h). */
enum { THEME_STANDARD, THEME_COMRADE, THEMES };
const char *theme_name(int theme); /* "Standard", "Comrade" */
enum { LAYOUT_COVERS, LAYOUT_LIST, LAYOUTS };
const char *layout_name(int layout); /* "Covers": the row of covers; "List": the drawer and names */
/* "Start on = recent": Settings > Games, the tab Our ROMs opens on (library_start_category). */
enum { START_ALL, START_RECENT, START_FAVORITES, STARTS };
const char *start_name(int start); /* "All", "Recent", "Favorites" */
typedef struct {
	int mode;            /* MODE_, or -1 */
	int picture;         /* PICTURE_ */
	int theme;           /* THEME_ */
	int layout;          /* LAYOUT_: Our ROMs */
	char last_game[128]; /* the ROM file played last, "" none */
	int rumble[4];       /* players 1-4: rumble on (the default) or off */
	char recent[RECENT_GAMES][128]; /* the ROM files played last, newest first, "" ends */
	char rom_folder[256]; /* "ROM folder = ...": "" is sd:/wiiu/our64/roms; "sd:/path" or "usb:<serial>/path" (usbfs.h) */
	char rom_drive[34];   /* "ROM drive = ...": the USB partition's label when its folder was picked, shown while
	                       * it is unplugged; "" none (an SD folder, no label, or a settings.cfg from before) */
	int auto_resume;      /* "Auto-resume = on|off": resume points written and loaded (the default), or neither */
	int start_on;         /* START_ */
	int filter;           /* FILTER_: "Texture filter", last so the positional initializers keep their order */
	int look;             /* LOOK_: "TV look" */
} our_settings;
int settings_parse(const char *text, our_settings *s);
/* A game started: it becomes the last game and the newest recent one. */
void settings_played(our_settings *s, const char *file);
int settings_read(const char *path, our_settings *s);
int settings_format_all(char *out, unsigned size, const our_settings *s);

typedef struct {
	int selected;  /* MODE_ */
	int gamepad;   /* a GamePad is connected: its cards can be chosen */
	int touched;   /* the card a touch went down on, -1 */
} prompt;

/* last: the last choice, or < 0 on the first launch (TV + GamePad then).
 * Without a GamePad both GamePad cards are off and TV only is selected. */
void prompt_init(prompt *p, int last, int gamepad);
void prompt_set_gamepad(prompt *p, int connected);
int  prompt_enabled(const prompt *p, int mode);

enum { PROMPT_LEFT, PROMPT_RIGHT, PROMPT_ACCEPT, PROMPT_TOUCH_DOWN, PROMPT_TOUCH_UP };
/* card: for touches, the card under the touch (prompt_card_at), -1 for none.
 * Returns the chosen mode, or -1 while nothing is chosen. A tap selects a
 * card; a tap on the selected card chooses it. A touch counts only when it
 * goes down and comes up on the same card. */
int prompt_event(prompt *p, int event, int card);

/* Card i's rectangle {x, y, w, h}, and the card under a point (-1 in the gaps). */
void prompt_card_rect(int card, int r[4]);
int  prompt_card_at(int x, int y);

/* "today 9:40 PM", "yesterday 21:40", "14 Oct 21:40", "Oct 14, 2025, 9:40 PM".
 * Times are the console's wall clock (both read as UTC). us: the US style,
 * 12-hour with the month first. */
void ui_when(char *out, unsigned size, time_t now, time_t then, int us);
void ui_clock(char *out, unsigned size, time_t t, int us); /* "9:40 PM", "21:40" */

/* A title from the ROM's file name: no folder, no extension, no trailing
 * "(USA)" or "[!]" tags. */
void ui_game_title(char *out, unsigned size, const char *file);
/* The title without a leading The, A or An: what Our ROMs sorts and files under a letter by. */
const char *ui_sort_title(const char *title);

/* What the launch will do, for the line under the cards. */
enum { LAUNCH_RESUME, LAUNCH_FIRST, LAUNCH_FRESH, LAUNCH_DAMAGED, LAUNCH_OTHER_VERSION, LAUNCH_OTHER_GAME };
/* when: ui_when of the resume point (LAUNCH_RESUME and LAUNCH_FRESH). */
void prompt_line(char *out, unsigned size, const char *title, int launch, const char *when);

#ifdef __cplusplus
}
#endif
#endif
