/* Our64 player: Our Settings, Our Controllers, Change order and Edit buttons
 * (ux/flows/setup; rulings in docs/ux/requirements/2026-10-16-setup.md):
 * what is selected, what each press does, layout for touch, and the words.
 * Plain C, so tests/ runs it on the PC; drawing is ui_wiiu.cpp's business,
 * the loops and the files setup_wiiu.c's.
 *
 * Presses come in as pause.h's PEV_ events (pause_events turns a
 * controller's buttons into them). No shortcut buttons (Zach, 2026-10-16):
 * every action is a named item chosen with A, except in Edit buttons' test
 * layout (2026-10-29), whose actions are on buttons. HOME means Wii U Menu. */
#ifndef OUR64_SETUP_H
#define OUR64_SETUP_H
#include "input.h"
#include "pause.h"
#include "prompt.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- Our Settings ---- */

/* Every row opens a page (2026-11-04): Controllers at the top (2026-10-17),
 * Theme at the bottom (2026-10-30). */
enum { SET_CONTROLLERS, SET_PICTURE, SET_GAMES, SET_THEME, SET_ROWS };
typedef struct {
	int row;
	int touched;
} settings_ui;
enum { SACT_NONE, SACT_BACK, SACT_HOME, SACT_CONTROLLERS, SACT_PICTURE, SACT_GAMES, SACT_THEME };
void settings_open(settings_ui *s);
int  settings_event(settings_ui *s, int event, int target);
const char *settings_row_name(int row);
const char *settings_row_about(const settings_ui *s); /* the line under the rows */
void settings_row_rect(int row, int r[4]);
void settings_value_rect(int row, int i, int r[4]); /* kept: the value pages' boxes */
int  settings_hit(int x, int y);               /* the row, or -1 */
int  settings_legend(const settings_ui *s, int type, pause_key *keys, int max);

/* ---- Settings > Picture (2026-11-04): Screen mode, Shape (the old
 * Picture row) and Texture filter, each applied with A, on the Settings rows'
 * layout. ---- */

enum { PIC_SCREEN, PIC_SHAPE, PIC_FILTER, PIC_LOOK, PIC_PADLOOK, PIC_ROWS };
typedef struct {
	int row;
	int mode, shape, filter, look, pad_look; /* in use */
	int pick[PIC_ROWS];       /* the value left/right is on, per row; A applies it */
	int gamepad;              /* a GamePad is connected: GamePad only can be picked */
	int touched;
} picture_ui;
enum { PICT_NONE, PICT_BACK, PICT_HOME, PICT_MODE, PICT_SHAPE, PICT_FILTER, PICT_LOOK, PICT_PADLOOK };
void picture_open(picture_ui *p, int mode, int shape, int filter, int look, int pad_look, int gamepad);
int  picture_event(picture_ui *p, int event, int target); /* PICT_MODE/_SHAPE/_FILTER: apply pick[], then picture_applied */
void picture_applied(picture_ui *p);
int  picture_values(int row);
int  picture_in_use(const picture_ui *p, int row);
int  picture_off(const picture_ui *p, int row, int i); /* greyed: shown, A does nothing */
const char *picture_value_name(int row, int i);
const char *picture_row_name(int row);           /* "Screen mode", "Shape", "Texture filter", "TV look", "GamePad look" */
const char *picture_row_about(const picture_ui *p);
int  picture_hit(int x, int y);                  /* row * 8 + value + 1, or row * 8 for the row itself; -1 */
int  picture_legend(const picture_ui *p, int type, pause_key *keys, int max);

/* ---- Settings > Games (2026-11-04): where the ROMs are, and fetching
 * missing covers (only when asked). Both rows act on A; the ROM folder's
 * value is text, Fetch covers has none (the page reads no game: Zach,
 * 2026-11-04). A fetch runs on its own screen, like the game loading screen:
 * the games' headers read first ("Reading your games: 80 of 200"), then a
 * bar, "12 of 40" under it, and HOME or B to stop (SETUP_FETCH).
 * Auto-Resume (2026-11-06) switches resume points on or off with A: turning
 * it off asks first, since every game's resume points are deleted
 * (GACT_RESUME_OFF: the caller asks); on asks nothing. Start on opens its own
 * page: All, Recent or Favorites, A picks one and goes back. ---- */
enum { GAMES_FOLDER, GAMES_COVERS, GAMES_RESUME, GAMES_START, GAMES_ROWS };
typedef struct {
	int row;
	char folder[400];     /* "SD card: /wiiu/our64/roms", "USB drive OUR64 EXFAT: /ROMs/N64" (usbfs_folder_label) */
	int auto_resume;      /* on: resume points written and loaded */
	int start_on;         /* START_ (prompt.h) */
	int running, done, total; /* a fetch in progress: SETUP_FETCH */
	int reading;          /* running, and done of total are headers read, not covers */
	int touched;
} games_ui;
/* GACT_RESUME_OFF: ask, then turn it off; GACT_RESUME_ON: turn it on; GACT_START: the Start on page */
enum { GACT_NONE, GACT_BACK, GACT_HOME, GACT_FOLDER, GACT_FETCH, GACT_STOP, GACT_RESUME_OFF, GACT_RESUME_ON, GACT_START };
void games_open(games_ui *g, const char *folder, int auto_resume, int start_on);
int  games_event(games_ui *g, int event, int target);
void games_reading(games_ui *g, int read, int total); /* the headers being read for the fetch */
void games_fetching(games_ui *g, int done, int total);
void games_fetched(games_ui *g);                /* the fetch ended or stopped */
const char *games_row_name(int row);
void games_value(const games_ui *g, int row, char *out, unsigned size);
const char *games_row_about(const games_ui *g);
void games_fetch_step(const games_ui *g, char *out, unsigned size); /* under the bar: "Reading your games: 80 of 200", "12 of 40" */
float games_fetch_progress(const games_ui *g);                    /* 0..1 */
int  games_hit(int x, int y);                   /* row, or -1 */
int  games_legend(const games_ui *g, int type, pause_key *keys, int max); /* running: HOME, Stop */

/* ---- Settings > Games > Start on (2026-11-06): the tab Our ROMs opens on,
 * rows All, Recent and Favorites (START_, in that order), the one in use
 * ticked. A picks the selected one and goes back; B goes back unchanged. ---- */
typedef struct {
	int row;     /* START_ */
	int in_use;  /* START_ */
	int touched;
} start_ui;
enum { STACT_NONE, STACT_BACK, STACT_HOME, STACT_PICK }; /* STACT_PICK: row is the choice */
void start_open(start_ui *s, int in_use);
int  start_event(start_ui *s, int event, int target);
int  start_hit(int x, int y);                   /* row, or -1 */
int  start_legend(const start_ui *s, int type, pause_key *keys, int max);

/* ---- Settings > Games > ROM folder (2026-11-04): drives, then folders.
 * A opens a folder; "Use this folder", the first row inside a drive, picks
 * the one shown; B goes up a level. The caller lists the drives and each
 * folder (paths "sd:/...", "usb1p1:/..."): this never touches files. ---- */
#define BROWSE_MAX 256
#define BROWSE_SHOWN 6
typedef struct { char name[64]; char path[256]; int off; } browse_entry;
typedef struct {
	int depth;              /* 0: the drives */
	char path[256];         /* the folder shown ("" on the drive list) */
	char came_from[256];    /* after BACT_UP: the folder to select again */
	char drive[64];         /* the drive's name, from the drive list: browse_title */
	int games;              /* ROM files right in path; -1: it couldn't be opened, Use this folder is greyed */
	browse_entry entries[BROWSE_MAX];
	int count, row, top, touched;
} browse_ui;
enum { BACT_NONE, BACT_BACK, BACT_HOME, BACT_OPEN, BACT_UP, BACT_USE };
void browse_open(browse_ui *b);                       /* the drive list, empty */
void browse_entered(browse_ui *b, int games);         /* after OPEN/UP: clears entries; caller adds; games -1: unreadable */
void browse_add(browse_ui *b, const char *name, const char *path, int off);
int  browse_rows(const browse_ui *b);                 /* entries, plus Use this folder inside a drive */
int  browse_event(browse_ui *b, int event, int target);
const char *browse_row_name(const browse_ui *b, int row);
int  browse_row_off(const browse_ui *b, int row);     /* greyed: shown, A does nothing */
void browse_row_value(const browse_ui *b, int row, char *out, unsigned size); /* "4 games", "1 game", "" */
const char *browse_title(const browse_ui *b);         /* the line under the header: "SD card: /Games", "Choose a drive" */
void browse_row_rect(int shown, int r[4]);
int  browse_hit(const browse_ui *b, int x, int y);    /* row, or -1 */
int  browse_legend(const browse_ui *b, int type, pause_key *keys, int max);

/* ---- Theme (Settings > Theme, 2026-10-30): the colors and Our ROMs' layout,
 * each applied with A, on the Settings rows' layout. ---- */

enum { LOOK_COLORS, LOOK_LAYOUT, LOOK_ROWS };
typedef struct {
	int row;
	int theme, layout;    /* in use */
	int pick[LOOK_ROWS];  /* the value left/right is on, per row; A applies it */
	int touched;
} look_ui;
enum { LACT_NONE, LACT_BACK, LACT_HOME, LACT_COLORS, LACT_LAYOUT };
void look_open(look_ui *l, int theme, int layout);
int  look_event(look_ui *l, int event, int target); /* LACT_COLORS / _LAYOUT: apply pick[], then look_applied */
void look_applied(look_ui *l);
int  look_values(int row);
int  look_in_use(const look_ui *l, int row);
const char *look_value_name(int row, int i);
const char *look_row_name(int row);
const char *look_row_about(const look_ui *l);
int  look_hit(int x, int y);                   /* row * 8 + value + 1, or row * 8; -1 */
int  look_legend(const look_ui *l, int type, pause_key *keys, int max);

/* ---- Our Controllers ---- */

typedef struct {
	int type;      /* INPUT_, -1 for an empty port */
	int battery;   /* 0-4 bars, -1 unknown */
	int charging;
	int gone;      /* its controller disconnected; the port waits */
} setup_port;
/* No Pair a controller: the Wii U Menu's own Controller Settings pairs them
 * (Zach, 2026-10-24: opening the pairing screen from a paused game froze the console). */
enum { CTL_RUMBLE, CTL_EDIT, CTL_ORDER, CTL_ACTIONS };
typedef struct {
	int card;            /* the selected player 0-3 */
	int in_actions;      /* the selection is in the action row */
	int action;          /* CTL_ */
	setup_port ports[INPUT_PORTS];
	int rumble[INPUT_PORTS];
	int touched;
	int refused;       /* A on something that does nothing now, counted: the screen flashes it */
} controllers_ui;
enum { CACT_NONE, CACT_BACK, CACT_HOME, CACT_RUMBLE, CACT_EDIT, CACT_ORDER };
void controllers_open(controllers_ui *c);
int  controllers_event(controllers_ui *c, int event, int target);
int  controllers_action_enabled(const controllers_ui *c, int action);
const char *controllers_action_reason(const controllers_ui *c, int action); /* why not now; "" when it works */
const char *controllers_action_name(const controllers_ui *c, int action); /* "Rumble: On" */
int controllers_action_count(void);     /* 3 */
int controllers_action_id(int slot);    /* the CTL_ in that place from the left; -1 past the ends */
void controllers_card_rect(int card, int r[4]);
void controllers_action_rect(int action, int r[4]);
int  controllers_hit(int x, int y);            /* cards 0-3, actions 10 + CTL_; -1 */
int  controllers_legend(const controllers_ui *c, int type, pause_key *keys, int max);

/* ---- Change order ---- */

typedef struct {
	int ids[INPUT_PORTS]; /* controllers in their new order */
	int count;            /* assigned so far */
	int total;            /* connected controllers: the step ends after the last */
} order_ui;
void order_open(order_ui *o, int connected);
int  order_press(order_ui *o, int id); /* A on controller id; 1 when every controller is assigned */

/* ---- Edit buttons ---- */

/* Edit buttons opens on the button list (test layout 2026-10-29,
 * docs/plans/2026-10-29-stable-release-decisions.md item 2): the 18 N64
 * inputs, then Dead zone and Range. A captures a new button, Y adds one, X
 * clears; on a Wii Remote or Nunchuk plus adds and minus clears. L or R
 * switches between All games and the paused game's tab. Minus resets the tab
 * shown (All games to Our64's buttons; the game's tab removes its profile)
 * and then undoes that reset until the next change. Every change is written
 * at once: menus read the controllers' own buttons, never the mapping, so no
 * change can lock a player out. The actions are on the legend, not rows in
 * the list (Zach, 2026-10-18). */
enum { MAP_DEAD_ZONE = N64_INPUTS, MAP_RANGE, MAP_ROWS };
enum { MAP_LIST, MAP_CAPTURE, MAP_FAILED };
enum { MAP_TAB_ALL, MAP_TAB_GAME };
/* Edit buttons' own presses, beside pause.h's PEV_ ones (mapping_events) */
enum { MEV_ADD = 32, MEV_CLEAR, MEV_RESET, MEV_TAB };
#define MAP_TAB_TARGET 100 /* a tab's touch target: MAP_TAB_TARGET + MAP_TAB_ */
#define MAP_SHOWN 4 /* rows on screen at once */
typedef struct {
	int type;                  /* the controller type edited */
	int has_game;              /* opened from a paused game: its tab is there */
	int has_profile;           /* that game has a profile for this type */
	char game[64];             /* that game, for its tab */
	int tab;                   /* MAP_TAB_ */
	input_map all, profile;    /* on the card: the all-games buttons, the game's (when has_profile) */
	input_map edit;            /* the tab shown */
	input_map before;          /* the tab before the change being written: back to it if the write fails */
	int pending;               /* what is being written (MPEND_) */
	int can_undo;              /* minus undoes the last reset */
	input_map undo;            /* the tab before that reset */
	int row, top;              /* the selection in the list, and the first row shown */
	int screen;                /* MAP_ */
	int capture_add;           /* the capture adds a button instead of replacing */
	double capture_until;      /* when the capture gives up (seconds) */
	char line[128];            /* what the last action did */
	int other_row;             /* the input the last capture took a button from (-1 none) */
	unsigned other_seq;        /* counts those captures: the photo blinks other_row once per count */
	char failed_title[64], failed_body[256];
	int touched;
	int refused;       /* a press that does nothing now, counted: the row flashes it */
} mapping_ui;
enum { MPEND_CHANGE, MPEND_RESET, MPEND_UNDO };
/* MACT_WRITE: write m->edit (as the game's profile on its tab), then
 * mapping_written or mapping_fail. MACT_REMOVE_PROFILE: remove the game's
 * profile, then mapping_profile_removed or mapping_fail. */
enum { MACT_NONE, MACT_BACK, MACT_HOME, MACT_WRITE, MACT_REMOVE_PROFILE, MACT_CAPTURE };
/* all: the all-games buttons; profile: the game's, NULL when it has none;
 * game: the paused game, NULL or "" from Our ROMs (no game tab). */
void mapping_open(mapping_ui *m, int type, const input_map *all, const input_map *profile, const char *game);
/* One controller's presses as events: pause_events' moves, A and B, and MEV_. */
int  mapping_events(int type, uint32_t pressed, int *out, int max);
int  mapping_event(mapping_ui *m, int event, int target, double now);
/* While capturing: the buttons and stick directions the edited type's
 * controller pressed (SRC_ bits). MACT_WRITE when one was bound. */
int  mapping_capture(mapping_ui *m, uint32_t pressed);
int  mapping_tick(mapping_ui *m, double now);   /* 1 when the capture just gave up */
void mapping_written(mapping_ui *m);
void mapping_profile_removed(mapping_ui *m);
void mapping_fail(mapping_ui *m, const char *title, const char *body);
int  mapping_can_reset(const mapping_ui *m);    /* minus does something: a reset, or its undo */
void mapping_row_text(const mapping_ui *m, int row, char *label, unsigned label_size, char *value, unsigned value_size);
void mapping_headline(const mapping_ui *m, char *out, unsigned size, char *sub, unsigned sub_size);
const char *mapping_tab_name(const mapping_ui *m, int tab);
void mapping_row_rect(int shown, int r[4]);     /* the shown-th row on screen */
void mapping_tab_rect(int tab, int r[4]);
int  mapping_hit(const mapping_ui *m, int x, int y); /* list: row, or MAP_TAB_TARGET + tab; failed: 0; -1 */
int  mapping_legend(const mapping_ui *m, int type, pause_key *keys, int max);

#ifdef __cplusplus
}
#endif
#endif
