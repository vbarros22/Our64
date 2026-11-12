/* Our64 player: Our Settings, Our Controllers, Change order, Edit buttons (see setup.h). */
#include "setup.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define BIT(s) (1u << (s))

static int inside(int x, int y, const int r[4]) { return x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]; }

/* A tap selects; a tap on the selected thing acts. Returns the event to run, or -1. */
static int tap(int *touched, int event, int target, int selected_now)
{
	if (event == PEV_TOUCH_DOWN) {
		*touched = target;
		return -1;
	}
	int was = *touched;
	*touched = -1;
	if (was < 0 || was != target)
		return -1;
	return selected_now ? PEV_OK : -2; /* -2: select it */
}

static void keys_add(pause_key *keys, int *n, int max, int glyph, const char *label, int dim)
{
	if (*n < max)
		keys[(*n)++] = (pause_key){ glyph, label, dim };
}

static int ok_glyph(int type) { return type == INPUT_WIIMOTE ? GLYPH_2 : GLYPH_A; }
static int back_glyph(int type) { return type == INPUT_WIIMOTE ? GLYPH_1 : GLYPH_B; }

/* ---- Our Settings ---- */

static const char *const g_set_row[SET_ROWS] = { "Controllers", "Picture", "Games", "Theme" };

const char *settings_row_name(int row) { return row >= 0 && row < SET_ROWS ? g_set_row[row] : ""; }

void settings_open(settings_ui *s)
{
	memset(s, 0, sizeof(*s));
	s->touched = -1;
}

static int settings_page(int row)
{
	switch (row) {
	case SET_CONTROLLERS: return SACT_CONTROLLERS;
	case SET_PICTURE: return SACT_PICTURE;
	case SET_GAMES: return SACT_GAMES;
	default: return SACT_THEME;
	}
}

int settings_event(settings_ui *s, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int e = tap(&s->touched, event, target, target == s->row);
		if (e == -2) {
			s->row = target;
			return SACT_NONE;
		}
		if (e < 0)
			return SACT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = s->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < SET_ROWS)
			s->row = row;
		break;
	}
	case PEV_OK: return settings_page(s->row);
	case PEV_BACK: return SACT_BACK;
	case PEV_HOME: return SACT_HOME;
	}
	return SACT_NONE;
}

const char *settings_row_about(const settings_ui *s)
{
	switch (s->row) {
	case SET_PICTURE: return "Screens and shape";
	case SET_GAMES: return "ROM Path, box art, and auto-save";
	case SET_THEME: return "Colors and how Our ROMs shows the games";
	default: return "Players, rumble and buttons";
	}
}

#define SROW_X 40
/* Four rows (Theme, 2026-10-30) between the line under the header (what A
 * just applied) and the line above the legend, the selected row's frame clear
 * of both. */
#define SROW_Y 134
#define SROW_W 774
#define SROW_H 54
#define SROW_STEP 62

/* A page of more than 4 rows (Picture, 2026-11-12) has them a little lower and
 * closer, so the fifth and the line under it stay above the legend bar (412). */
#define SROW_Y_TIGHT 122
#define SROW_H_TIGHT 46
#define SROW_STEP_TIGHT 52

void rows_rect(int rows, int row, int r[4])
{
	const int tight = rows > 4;
	r[0] = SROW_X;
	r[1] = (tight ? SROW_Y_TIGHT : SROW_Y) + row * (tight ? SROW_STEP_TIGHT : SROW_STEP);
	r[2] = SROW_W;
	r[3] = tight ? SROW_H_TIGHT : SROW_H;
}

void rows_value_rect(int rows, int row, int i, int r[4])
{
	rows_rect(rows, row, r);
	r[0] = 282 + i * 176;
	r[1] += 5;
	r[2] = 168;
	r[3] -= 10;
}

void settings_row_rect(int row, int r[4]) { rows_rect(4, row, r); }

void settings_value_rect(int row, int i, int r[4]) { rows_value_rect(4, row, i, r); }

int settings_hit(int x, int y)
{
	int r[4];
	for (int row = 0; row < SET_ROWS; row++) {
		settings_row_rect(row, r);
		if (inside(x, y, r))
			return row;
	}
	return -1;
}

int settings_legend(const settings_ui *s, int type, pause_key *keys, int max)
{
	(void)s;
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "OK", 0);
	return n;
}

/* ---- Settings > Picture ---- */

static const char *const g_pic_row[PIC_ROWS] = { "Screen mode", "Shape", "Texture filter", "TV look", "GamePad look" };

int picture_values(int row) { return row == PIC_SCREEN ? MODES : row == PIC_SHAPE ? PICTURES : row == PIC_FILTER ? FILTERS : row == PIC_LOOK || row == PIC_PADLOOK ? LOOKS : 0; }

int picture_in_use(const picture_ui *p, int row)
{
	return row == PIC_SCREEN ? p->mode : row == PIC_SHAPE ? p->shape : row == PIC_FILTER ? p->filter : row == PIC_LOOK ? p->look : row == PIC_PADLOOK ? p->pad_look : -1;
}

int picture_off(const picture_ui *p, int row, int i) { return row == PIC_SCREEN && !p->gamepad && mode_uses_gamepad(i); }

const char *picture_value_name(int row, int i)
{
	return row == PIC_SCREEN ? mode_name(i) : row == PIC_SHAPE ? picture_name(i) : row == PIC_FILTER ? filter_name(i) : row == PIC_LOOK || row == PIC_PADLOOK ? look_name(i) : "";
}

const char *picture_row_name(int row) { return row >= 0 && row < PIC_ROWS ? g_pic_row[row] : ""; }

void picture_open(picture_ui *p, int mode, int shape, int filter, int look, int pad_look, int gamepad)
{
	memset(p, 0, sizeof(*p));
	p->mode = p->pick[PIC_SCREEN] = mode;
	p->shape = p->pick[PIC_SHAPE] = shape;
	p->filter = p->pick[PIC_FILTER] = filter;
	p->look = p->pick[PIC_LOOK] = look;
	p->pad_look = p->pick[PIC_PADLOOK] = pad_look;
	p->gamepad = gamepad;
	p->touched = -1;
}

void picture_applied(picture_ui *p)
{
	p->mode = p->pick[PIC_SCREEN];
	p->shape = p->pick[PIC_SHAPE];
	p->filter = p->pick[PIC_FILTER];
	p->look = p->pick[PIC_LOOK];
	p->pad_look = p->pick[PIC_PADLOOK];
}

static void picture_drop_picks(picture_ui *p)
{
	p->pick[PIC_SCREEN] = p->mode;
	p->pick[PIC_SHAPE] = p->shape;
	p->pick[PIC_FILTER] = p->filter;
	p->pick[PIC_LOOK] = p->look;
	p->pick[PIC_PADLOOK] = p->pad_look;
}

static int picture_apply(const picture_ui *p)
{
	const int i = p->pick[p->row];
	if (i == picture_in_use(p, p->row) || picture_off(p, p->row, i))
		return PICT_NONE;
	return p->row == PIC_SCREEN ? PICT_MODE : p->row == PIC_SHAPE ? PICT_SHAPE : p->row == PIC_FILTER ? PICT_FILTER : p->row == PIC_LOOK ? PICT_LOOK : PICT_PADLOOK;
}

static void picture_select(picture_ui *p, int row)
{
	if (row != p->row)
		picture_drop_picks(p);
	p->row = row;
}

int picture_event(picture_ui *p, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		const int row = target >= 0 ? target / 8 : -1, value = target >= 0 ? target % 8 - 1 : -1;
		const int now = row == p->row && (value < 0 || p->pick[row] == value);
		int e = tap(&p->touched, event, target, now);
		if (e == -2) {
			picture_select(p, row);
			if (value >= 0 && value < picture_values(row) && !picture_off(p, row, value))
				p->pick[row] = value;
			return PICT_NONE;
		}
		if (e < 0)
			return PICT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = p->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < PIC_ROWS)
			picture_select(p, row);
		break;
	}
	case PEV_LEFT:
	case PEV_RIGHT: {
		const int step = event == PEV_LEFT ? -1 : 1, n = picture_values(p->row);
		for (int i = p->pick[p->row] + step; i >= 0 && i < n; i += step)
			if (!picture_off(p, p->row, i)) {
				p->pick[p->row] = i;
				break;
			}
		break;
	}
	case PEV_OK: return picture_apply(p);
	case PEV_BACK: return PICT_BACK;
	case PEV_HOME: return PICT_HOME;
	}
	return PICT_NONE;
}

const char *picture_row_about(const picture_ui *p)
{
	switch (p->row) {
	case PIC_SCREEN:
		/* the same whatever is picked (Zach, 2026-10-23) */
		return "Which screens show the game. The prompt at each start begins on it.";
	case PIC_SHAPE:
		/* the same whatever is picked (Zach, 2026-10-23): it says what each shape is */
		return "4:3 is the games' own shape; 16:9 stretches it to fill a wide TV.";
	case PIC_FILTER:
		return "Smooth: 4-point bilinear. N64: the console's own 3-point filter.";
	case PIC_LOOK:
		return "CRT: scanlines and a phosphor grille; Soft CRT also blurs. On the TV.";
	default:
		return "CRT: scanlines and a phosphor grille; Soft CRT also blurs. On the GamePad.";
	}
}

int picture_hit(int x, int y)
{
	int r[4];
	for (int row = 0; row < PIC_ROWS; row++) {
		for (int i = 0; i < picture_values(row); i++) {
			rows_value_rect(PIC_ROWS, row, i, r);
			if (inside(x, y, r))
				return row * 8 + i + 1;
		}
		rows_rect(PIC_ROWS, row, r);
		if (inside(x, y, r))
			return row * 8;
	}
	return -1;
}

int picture_legend(const picture_ui *p, int type, pause_key *keys, int max)
{
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "Apply", picture_apply(p) == PICT_NONE);
	return n;
}

/* ---- Settings > Games ---- */

static const char *const g_games_row[GAMES_ROWS] = { "ROM folder", "Fetch covers", "Auto-Resume", "Start-up ROM Category" };

const char *games_row_name(int row) { return row >= 0 && row < GAMES_ROWS ? g_games_row[row] : ""; }

void games_open(games_ui *g, const char *folder, int auto_resume, int start_on)
{
	memset(g, 0, sizeof(*g));
	snprintf(g->folder, sizeof(g->folder), "%s", folder ? folder : "");
	g->auto_resume = auto_resume;
	g->start_on = start_on;
	g->touched = -1;
}

void games_reading(games_ui *g, int read, int total)
{
	games_fetching(g, read, total);
	g->reading = 1;
}

void games_fetching(games_ui *g, int done, int total)
{
	g->running = 1;
	g->reading = 0;
	g->done = done;
	g->total = total;
}

void games_fetched(games_ui *g)
{
	g->running = 0;
	g->reading = 0;
}

/* Fetch covers is always there: whether a game lacks a cover is found on the
 * fetch screen, so opening the page reads no game. */
static int games_act(const games_ui *g)
{
	if (g->running)
		return GACT_NONE;
	switch (g->row) {
	case GAMES_FOLDER: return GACT_FOLDER;
	case GAMES_RESUME: return g->auto_resume ? GACT_RESUME_OFF : GACT_RESUME_ON;
	case GAMES_START: return GACT_START;
	default: return GACT_FETCH;
	}
}

int games_event(games_ui *g, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int e = tap(&g->touched, event, target, target == g->row);
		if (g->running || e == -1)
			return GACT_NONE; /* the fetch screen has no rows: B stops it */
		if (e == -2) {
			g->row = target;
			return GACT_NONE;
		}
		event = e;
	}
	if (g->running) {
		if (event == PEV_BACK)
			return GACT_STOP;
		return event == PEV_HOME ? GACT_HOME : GACT_NONE;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = g->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < GAMES_ROWS)
			g->row = row;
		break;
	}
	case PEV_OK: return games_act(g);
	case PEV_BACK: return GACT_BACK;
	case PEV_HOME: return GACT_HOME;
	}
	return GACT_NONE;
}

void games_value(const games_ui *g, int row, char *out, unsigned size)
{
	snprintf(out, size, "%s", row == GAMES_FOLDER ? g->folder
	                          : row == GAMES_RESUME ? (g->auto_resume ? "On" : "Off")
	                          : row == GAMES_START ? start_name(g->start_on) : "");
}

const char *games_row_about(const games_ui *g)
{
	switch (g->row) {
	case GAMES_FOLDER: return "Where we find your games";
	case GAMES_RESUME: return "Saves where you stop and picks up there next time";
	case GAMES_START: return "The category shown when Our64 starts";
	default: return "Fetches box art from the internet. Requires Wi-Fi";
	}
}

void games_fetch_step(const games_ui *g, char *out, unsigned size)
{
	if (g->reading)
		snprintf(out, size, "Reading your games: %d of %d", g->done, g->total);
	else
		snprintf(out, size, "%d of %d", g->done, g->total);
}

float games_fetch_progress(const games_ui *g)
{
	return g->total > 0 ? (float)g->done / (float)g->total : 0.0f;
}

int games_hit(int x, int y)
{
	int r[4];
	for (int row = 0; row < GAMES_ROWS; row++) {
		settings_row_rect(row, r);
		if (inside(x, y, r))
			return row;
	}
	return -1;
}

int games_legend(const games_ui *g, int type, pause_key *keys, int max)
{
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	if (g->running) {
		keys_add(keys, &n, max, back_glyph(type), "Stop", 0);
		return n;
	}
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "OK", games_act(g) == GACT_NONE);
	return n;
}

/* ---- Settings > Games > Start on ---- */

void start_open(start_ui *s, int in_use)
{
	memset(s, 0, sizeof(*s));
	s->in_use = in_use >= 0 && in_use < STARTS ? in_use : START_RECENT;
	s->row = s->in_use;
	s->touched = -1;
}

int start_event(start_ui *s, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int e = tap(&s->touched, event, target, target == s->row);
		if (e == -2) {
			s->row = target;
			return STACT_NONE;
		}
		if (e < 0)
			return STACT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = s->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < STARTS)
			s->row = row;
		break;
	}
	case PEV_OK: return STACT_PICK;
	case PEV_BACK: return STACT_BACK;
	case PEV_HOME: return STACT_HOME;
	}
	return STACT_NONE;
}

int start_hit(int x, int y)
{
	int r[4];
	for (int row = 0; row < STARTS; row++) {
		settings_row_rect(row, r);
		if (inside(x, y, r))
			return row;
	}
	return -1;
}

int start_legend(const start_ui *s, int type, pause_key *keys, int max)
{
	(void)s;
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "OK", 0);
	return n;
}

/* ---- Settings > Games > ROM folder ---- */

void browse_open(browse_ui *b)
{
	memset(b, 0, sizeof(*b));
	b->touched = -1;
}

void browse_entered(browse_ui *b, int games)
{
	b->games = games;
	b->count = b->row = b->top = 0;
}

static int browse_use_row(const browse_ui *b) { return b->depth > 0; } /* Use this folder, row 0 inside a drive */

int browse_rows(const browse_ui *b) { return b->count + browse_use_row(b); }

static const browse_entry *browse_entry_of(const browse_ui *b, int row)
{
	const int i = row - browse_use_row(b);
	return i >= 0 && i < b->count ? &b->entries[i] : NULL;
}

static void browse_scroll(browse_ui *b)
{
	if (b->row < b->top)
		b->top = b->row;
	if (b->row >= b->top + BROWSE_SHOWN)
		b->top = b->row - BROWSE_SHOWN + 1;
}

void browse_add(browse_ui *b, const char *name, const char *path, int off)
{
	if (b->count >= BROWSE_MAX)
		return;
	browse_entry *e = &b->entries[b->count++];
	snprintf(e->name, sizeof(e->name), "%s", name);
	snprintf(e->path, sizeof(e->path), "%s", path);
	e->off = off;
	if (b->came_from[0] && !strcmp(path, b->came_from)) { /* back on the folder it came out of */
		b->row = b->count - 1 + browse_use_row(b);
		b->came_from[0] = 0;
		browse_scroll(b);
	}
}

const char *browse_row_name(const browse_ui *b, int row)
{
	if (browse_use_row(b) && row == 0)
		return "Use this folder";
	const browse_entry *e = browse_entry_of(b, row);
	return e ? e->name : "";
}

int browse_row_off(const browse_ui *b, int row)
{
	if (browse_use_row(b) && row == 0)
		return b->games < 0; /* a folder that couldn't be opened */
	const browse_entry *e = browse_entry_of(b, row);
	return e && e->off;
}

void browse_row_value(const browse_ui *b, int row, char *out, unsigned size)
{
	if (!browse_use_row(b) || row != 0 || b->games < 0)
		snprintf(out, size, "%s", "");
	else if (b->games == 0)
		snprintf(out, size, "No games");
	else
		snprintf(out, size, "%d game%s", b->games, b->games == 1 ? "" : "s");
}

/* What A does on the selected row, without doing it. */
static int browse_act(const browse_ui *b)
{
	if (browse_use_row(b) && b->row == 0)
		return b->games < 0 ? BACT_NONE : BACT_USE;
	const browse_entry *e = browse_entry_of(b, b->row);
	return e && !e->off && e->path[0] ? BACT_OPEN : BACT_NONE;
}

/* "usb1p1:/Games/N64" -> "usb1p1:/Games" -> "usb1p1:/" -> "" (the drive list). */
static void browse_parent(browse_ui *b)
{
	const char *colon = strstr(b->path, ":/");
	const size_t root = colon ? (size_t)(colon - b->path) + 2 : 0;
	char *slash = strrchr(b->path, '/');
	if (!colon || strlen(b->path) <= root || !slash) {
		b->path[0] = 0;
		b->depth = 0;
		return;
	}
	b->path[(size_t)(slash - b->path) < root ? root : (size_t)(slash - b->path)] = 0;
	if (b->depth > 1)
		b->depth--;
}

int browse_event(browse_ui *b, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int e = tap(&b->touched, event, target, target == b->row);
		if (e == -2) {
			if (target >= 0 && target < browse_rows(b)) {
				b->row = target;
				browse_scroll(b);
			}
			return BACT_NONE;
		}
		if (e < 0)
			return BACT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = b->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < browse_rows(b)) {
			b->row = row;
			browse_scroll(b);
		}
		break;
	}
	case PEV_OK: {
		const int act = browse_act(b);
		if (act == BACT_OPEN) {
			const browse_entry *e = browse_entry_of(b, b->row);
			if (b->depth == 0)
				snprintf(b->drive, sizeof(b->drive), "%s", e->name);
			snprintf(b->path, sizeof(b->path), "%s", e->path);
			b->came_from[0] = 0;
			b->depth++;
		}
		return act;
	}
	case PEV_BACK:
		if (b->depth == 0)
			return BACT_BACK;
		snprintf(b->came_from, sizeof(b->came_from), "%s", b->path);
		browse_parent(b);
		return BACT_UP;
	case PEV_HOME: return BACT_HOME;
	}
	return BACT_NONE;
}

const char *browse_title(const browse_ui *b)
{
	static char title[sizeof(b->drive) + sizeof(b->path) + 2];
	if (b->depth == 0)
		return "Choose a drive";
	const char *colon = strstr(b->path, ":/");
	const char *rest = colon ? colon + 2 : "";
	char drive[sizeof(b->drive)];
	if (b->drive[0])
		snprintf(drive, sizeof(drive), "%s", b->drive);
	else if (!strncmp(b->path, "sd:/", 4))
		snprintf(drive, sizeof(drive), "SD card");
	else
		snprintf(drive, sizeof(drive), "%.*s", colon ? (int)(colon - b->path) : 0, b->path);
	snprintf(title, sizeof(title), "%s: /%s", drive, rest); /* a full path, as Settings > Games shows it */
	return title;
}

/* BROWSE_SHOWN rows between the line under the header and the legend, the
 * selected row's frame clear of both. */
void browse_row_rect(int shown, int r[4])
{
	r[0] = 40;
	r[1] = 130 + shown * 44;
	r[2] = 774;
	r[3] = 40;
}

int browse_hit(const browse_ui *b, int x, int y)
{
	int r[4];
	for (int i = 0; i < BROWSE_SHOWN && b->top + i < browse_rows(b); i++) {
		browse_row_rect(i, r);
		if (inside(x, y, r))
			return b->top + i;
	}
	return -1;
}

int browse_legend(const browse_ui *b, int type, pause_key *keys, int max)
{
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), b->depth > 0 ? "Up" : "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "OK", browse_act(b) == BACT_NONE);
	return n;
}

/* ---- Theme ---- */

static const char *const g_look_row[LOOK_ROWS] = { "Colors", "Our ROMs" };

int look_values(int row) { return row == LOOK_COLORS ? THEMES : row == LOOK_LAYOUT ? LAYOUTS : 0; }

int look_in_use(const look_ui *l, int row) { return row == LOOK_COLORS ? l->theme : row == LOOK_LAYOUT ? l->layout : -1; }

const char *look_value_name(int row, int i)
{
	return row == LOOK_COLORS ? theme_name(i) : row == LOOK_LAYOUT ? layout_name(i) : "";
}

const char *look_row_name(int row) { return row >= 0 && row < LOOK_ROWS ? g_look_row[row] : ""; }

void look_open(look_ui *l, int theme, int layout)
{
	memset(l, 0, sizeof(*l));
	l->theme = l->pick[LOOK_COLORS] = theme;
	l->layout = l->pick[LOOK_LAYOUT] = layout;
	l->touched = -1;
}

void look_applied(look_ui *l)
{
	l->theme = l->pick[LOOK_COLORS];
	l->layout = l->pick[LOOK_LAYOUT];
}

static void look_drop_picks(look_ui *l)
{
	l->pick[LOOK_COLORS] = l->theme;
	l->pick[LOOK_LAYOUT] = l->layout;
}

static int look_apply(const look_ui *l)
{
	if (l->pick[l->row] == look_in_use(l, l->row))
		return LACT_NONE;
	return l->row == LOOK_COLORS ? LACT_COLORS : LACT_LAYOUT;
}

int look_event(look_ui *l, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int row = target >= 0 ? target / 8 : -1, value = target >= 0 ? target % 8 - 1 : -1;
		int now = row == l->row && (value < 0 || l->pick[row] == value);
		int e = tap(&l->touched, event, target, now);
		if (e == -2) {
			if (row != l->row)
				look_drop_picks(l);
			l->row = row;
			if (value >= 0 && value < look_values(row))
				l->pick[row] = value;
			return LACT_NONE;
		}
		if (e < 0)
			return LACT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_UP:
	case PEV_DOWN: {
		int row = l->row + (event == PEV_UP ? -1 : 1);
		if (row >= 0 && row < LOOK_ROWS) {
			look_drop_picks(l);
			l->row = row;
		}
		break;
	}
	case PEV_LEFT:
	case PEV_RIGHT: {
		int i = l->pick[l->row] + (event == PEV_LEFT ? -1 : 1);
		if (i >= 0 && i < look_values(l->row))
			l->pick[l->row] = i;
		break;
	}
	case PEV_OK: return look_apply(l);
	case PEV_BACK: return LACT_BACK;
	case PEV_HOME: return LACT_HOME;
	}
	return LACT_NONE;
}

const char *look_row_about(const look_ui *l)
{
	return l->row == LOOK_COLORS ? "Our64's colors" : "A row of large covers, or a list of names beside the covers";
}

int look_hit(int x, int y)
{
	int r[4];
	for (int row = 0; row < LOOK_ROWS; row++) {
		for (int i = 0; i < look_values(row); i++) {
			settings_value_rect(row, i, r);
			if (inside(x, y, r))
				return row * 8 + i + 1;
		}
		settings_row_rect(row, r);
		if (inside(x, y, r))
			return row * 8;
	}
	return -1;
}

int look_legend(const look_ui *l, int type, pause_key *keys, int max)
{
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "Apply", look_apply(l) == LACT_NONE);
	return n;
}

/* ---- Our Controllers ---- */

void controllers_open(controllers_ui *c)
{
	memset(c, 0, sizeof(*c));
	c->touched = -1;
	for (int i = 0; i < INPUT_PORTS; i++) {
		c->ports[i].type = -1;
		c->ports[i].battery = -1;
		c->rumble[i] = 1;
	}
}

int controllers_action_enabled(const controllers_ui *c, int action)
{
	/* Rumble and Edit buttons need a controller in that player's slot (Zach, 2026-10-29) */
	return action == CTL_ORDER || c->ports[c->card].type >= 0;
}

const char *controllers_action_reason(const controllers_ui *c, int action)
{
	return controllers_action_enabled(c, action) ? "" : "No controller"; /* the card says how to join */
}

/* Every action shows in Our64, in the CTL_ order (the shared UI hides
 * Rumble for a system without it). */
int controllers_action_count(void) { return CTL_ACTIONS; }
int controllers_action_id(int slot) { return slot >= 0 && slot < CTL_ACTIONS ? slot : -1; }

const char *controllers_action_name(const controllers_ui *c, int action)
{
	switch (action) {
	case CTL_RUMBLE: return c->rumble[c->card] ? "Rumble: On" : "Rumble: Off";
	case CTL_EDIT: return "Edit buttons";
	default: return "Change order";
	}
}

static int controllers_ok(controllers_ui *c)
{
	if (!c->in_actions) { /* A on a card: down to its actions */
		c->in_actions = 1;
		c->action = CTL_RUMBLE;
		return CACT_NONE;
	}
	if (!controllers_action_enabled(c, c->action)) {
		c->refused++;
		return CACT_NONE;
	}
	static const int act[CTL_ACTIONS] = { CACT_RUMBLE, CACT_EDIT, CACT_ORDER };
	return act[c->action];
}

int controllers_event(controllers_ui *c, int event, int target)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		int now = target >= 10 ? c->in_actions && c->action == target - 10 : !c->in_actions && c->card == target;
		int e = tap(&c->touched, event, target, now);
		if (e == -2) {
			if (target >= 10) {
				c->in_actions = 1;
				c->action = target - 10;
			} else {
				c->in_actions = 0;
				c->card = target;
			}
			return CACT_NONE;
		}
		if (e < 0)
			return CACT_NONE;
		event = e;
	}
	switch (event) {
	case PEV_LEFT:
		if (c->in_actions && c->action > 0)
			c->action--;
		else if (!c->in_actions && c->card > 0)
			c->card--;
		break;
	case PEV_RIGHT:
		if (c->in_actions && c->action < CTL_ACTIONS - 1)
			c->action++;
		else if (!c->in_actions && c->card < INPUT_PORTS - 1)
			c->card++;
		break;
	case PEV_DOWN:
		if (!c->in_actions) {
			c->in_actions = 1;
			c->action = CTL_RUMBLE;
		}
		break;
	case PEV_UP:
		c->in_actions = 0;
		break;
	case PEV_OK: return controllers_ok(c);
	case PEV_BACK: return CACT_BACK;
	case PEV_HOME: return CACT_HOME;
	}
	return CACT_NONE;
}

void controllers_card_rect(int card, int r[4])
{
	r[0] = 29 + card * 202;
	r[1] = 124;
	r[2] = 190;
	r[3] = 180;
}

void controllers_action_rect(int action, int r[4])
{
	/* three equal buttons, evenly spaced under the cards (Zach, 2026-10-29) */
	r[0] = 29 + action * 272; /* the cards span 29 to 825 */
	r[1] = 324;
	r[2] = 252;
	r[3] = 62;
}

int controllers_hit(int x, int y)
{
	int r[4];
	for (int i = 0; i < INPUT_PORTS; i++) {
		controllers_card_rect(i, r);
		if (inside(x, y, r))
			return i;
	}
	for (int a = 0; a < CTL_ACTIONS; a++) {
		controllers_action_rect(a, r);
		if (inside(x, y, r))
			return 10 + a;
	}
	return -1;
}

int controllers_legend(const controllers_ui *c, int type, pause_key *keys, int max)
{
	int n = 0;
	keys_add(keys, &n, max, GLYPH_HOME, "Wii U Menu", 0);
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	keys_add(keys, &n, max, GLYPH_DPAD, "Choose", 0);
	keys_add(keys, &n, max, ok_glyph(type), "OK", c->in_actions && !controllers_action_enabled(c, c->action));
	return n;
}

/* ---- Change order ---- */

void order_open(order_ui *o, int connected)
{
	memset(o, 0, sizeof(*o));
	o->total = connected < INPUT_PORTS ? connected : INPUT_PORTS;
}

int order_press(order_ui *o, int id)
{
	for (int i = 0; i < o->count; i++)
		if (o->ids[i] == id)
			return o->count >= o->total;
	if (o->count < INPUT_PORTS)
		o->ids[o->count++] = id;
	return o->count >= o->total;
}

/* ---- Edit buttons ---- */

void mapping_open(mapping_ui *m, int type, const input_map *all, const input_map *profile, const char *game)
{
	memset(m, 0, sizeof(*m));
	m->other_row = -1;
	m->type = type;
	m->all = *all;
	m->has_profile = profile != NULL;
	if (profile)
		m->profile = *profile;
	m->has_game = game && game[0];
	snprintf(m->game, sizeof(m->game), "%s", m->has_game ? game : "");
	/* opens on the buttons in use: the game's profile when it has one */
	m->tab = m->has_game && m->has_profile ? MAP_TAB_GAME : MAP_TAB_ALL;
	m->edit = m->tab == MAP_TAB_GAME ? m->profile : m->all;
	m->touched = -1;
	m->screen = MAP_LIST;
}

int mapping_events(int type, uint32_t pressed, int *out, int max)
{
	int ev[8], n = 0;
	int k = pause_events(type, pressed, ev, 8);
	for (int i = 0; i < k && n < max; i++)
		if (ev[i] != PEV_DELETE) /* Y, or minus on a Wii Remote: Edit buttons' own below */
			out[n++] = ev[i];
	const int wii = type == INPUT_WIIMOTE || type == INPUT_NUNCHUK;
	const struct { uint32_t bits; int event; } map[] = {
		{ wii ? BIT(SRC_PLUS) : BIT(SRC_Y), MEV_ADD },
		{ wii ? BIT(SRC_MINUS) : BIT(SRC_X), MEV_CLEAR },
		{ wii ? 0 : BIT(SRC_MINUS), MEV_RESET },
		{ wii ? 0 : BIT(SRC_L) | BIT(SRC_R), MEV_TAB },
	};
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]) && n < max; i++)
		if (pressed & map[i].bits)
			out[n++] = map[i].event;
	return n;
}

static int at_defaults(const mapping_ui *m)
{
	input_map def;
	input_default_map(m->type, &def);
	return input_map_equal(&def, &m->edit);
}

int mapping_can_reset(const mapping_ui *m)
{
	if (m->can_undo)
		return 1;
	return m->tab == MAP_TAB_GAME ? m->has_profile : !at_defaults(m);
}

static void scroll(mapping_ui *m)
{
	if (m->row < m->top)
		m->top = m->row;
	if (m->row >= m->top + MAP_SHOWN)
		m->top = m->row - MAP_SHOWN + 1;
}

/* Before a change: what to go back to if its write fails. */
static int change(mapping_ui *m, int pending)
{
	m->before = m->edit;
	m->pending = pending;
	m->line[0] = 0;
	return MACT_WRITE;
}

void mapping_written(mapping_ui *m)
{
	if (m->tab == MAP_TAB_GAME) {
		m->profile = m->edit;
		m->has_profile = 1; /* the first change on the game's tab makes its profile */
	} else {
		m->all = m->edit;
	}
	m->can_undo = m->pending == MPEND_RESET;
	m->pending = MPEND_CHANGE;
}

void mapping_profile_removed(mapping_ui *m)
{
	m->has_profile = 0;
	m->edit = m->all;
	m->can_undo = 1;
}

void mapping_fail(mapping_ui *m, const char *title, const char *body)
{
	m->edit = m->before;
	m->screen = MAP_FAILED;
	snprintf(m->failed_title, sizeof(m->failed_title), "%s", title);
	snprintf(m->failed_body, sizeof(m->failed_body), "%s", body);
}

static void show_tab(mapping_ui *m, int tab)
{
	if (!m->has_game || tab == m->tab)
		return;
	m->tab = tab;
	m->edit = tab == MAP_TAB_GAME && m->has_profile ? m->profile : m->all;
	m->can_undo = 0;
	m->line[0] = 0;
}

/* Dead zone in steps of 2% up to 40%; range from 60 to 100 in steps of 5. */
static int step_value(mapping_ui *m, int dir)
{
	if (m->row == MAP_DEAD_ZONE) {
		int was = (int)lroundf(m->edit.dead_zone * 100), pct = was + 2 * dir;
		pct = pct < 0 ? 0 : pct > 40 ? 40 : pct;
		if (pct == was)
			return MACT_NONE;
		change(m, MPEND_CHANGE);
		m->edit.dead_zone = pct / 100.0f;
		return MACT_WRITE;
	}
	if (m->row == MAP_RANGE) {
		int r = m->edit.range + 5 * dir;
		r = r < 60 ? 60 : r > 100 ? 100 : r;
		if (r == m->edit.range)
			return MACT_NONE;
		change(m, MPEND_CHANGE);
		m->edit.range = r;
		return MACT_WRITE;
	}
	return MACT_NONE;
}

static int capture(mapping_ui *m, int add, double now)
{
	if (m->row >= N64_INPUTS) {
		m->refused++;
		return MACT_NONE;
	}
	m->screen = MAP_CAPTURE;
	m->capture_add = add;
	m->capture_until = now + 5;
	return MACT_CAPTURE;
}

static int reset(mapping_ui *m)
{
	if (!mapping_can_reset(m)) {
		m->refused++;
		return MACT_NONE;
	}
	if (m->can_undo) { /* minus again: the buttons before the reset (on the game's tab, its profile again) */
		input_map back = m->undo;
		change(m, MPEND_UNDO);
		m->edit = back;
		return MACT_WRITE;
	}
	m->undo = m->edit;
	if (m->tab == MAP_TAB_GAME) {
		change(m, MPEND_RESET);
		return MACT_REMOVE_PROFILE;
	}
	change(m, MPEND_RESET);
	input_default_map(m->type, &m->edit);
	return MACT_WRITE;
}

int mapping_event(mapping_ui *m, int event, int target, double now)
{
	if (event == PEV_TOUCH_DOWN || event == PEV_TOUCH_UP) {
		const int list = m->screen == MAP_LIST, tab = list && target >= MAP_TAB_TARGET;
		int e = tap(&m->touched, event, target, !list || tab || m->row == target);
		if (e == -2) { /* a row not selected yet: select it */
			m->row = target;
			scroll(m);
			return MACT_NONE;
		}
		if (e < 0)
			return MACT_NONE;
		if (tab) { /* a tab switches at the first tap */
			show_tab(m, target - MAP_TAB_TARGET);
			return MACT_NONE;
		}
		event = e;
	}
	switch (m->screen) {
	case MAP_CAPTURE:
		if (event == PEV_HOME) /* the only way out besides waiting; every other button is bound */
			m->screen = MAP_LIST;
		return MACT_NONE;
	case MAP_FAILED:
		if (event == PEV_OK || event == PEV_BACK || event == PEV_HOME)
			m->screen = MAP_LIST;
		return MACT_NONE;
	}
	switch (event) {
	/* The list wraps: up from A goes to Range, down from Range to A. */
	case PEV_UP:
		m->row = m->row > 0 ? m->row - 1 : MAP_ROWS - 1;
		scroll(m);
		break;
	case PEV_DOWN:
		m->row = m->row < MAP_ROWS - 1 ? m->row + 1 : 0;
		scroll(m);
		break;
	case PEV_LEFT:
	case PEV_RIGHT: return step_value(m, event == PEV_LEFT ? -1 : 1);
	case PEV_OK: return capture(m, 0, now);
	case MEV_ADD: return capture(m, 1, now);
	case MEV_CLEAR:
		if (m->row >= N64_INPUTS || !m->edit.sources[m->row]) {
			m->refused++;
			return MACT_NONE;
		}
		change(m, MPEND_CHANGE);
		m->edit.sources[m->row] = 0;
		return MACT_WRITE;
	case MEV_RESET: return reset(m);
	case MEV_TAB:
		if (!m->has_game) {
			m->refused++;
			return MACT_NONE;
		}
		show_tab(m, !m->tab);
		break;
	case PEV_BACK: return MACT_BACK;
	case PEV_HOME: return MACT_HOME;
	}
	return MACT_NONE;
}

/* "C-Up" for N64 C-Up: the rows already say N64 */
static const char *short_n64(int n64)
{
	const char *s = input_n64_label(n64);
	return strncmp(s, "N64 ", 4) ? s : s + 4;
}

int mapping_capture(mapping_ui *m, uint32_t pressed)
{
	if (m->screen != MAP_CAPTURE || !pressed)
		return MACT_NONE;
	uint32_t one = pressed & (~pressed + 1); /* one button: the lowest pressed */
	change(m, MPEND_CHANGE);
	const uint32_t old = m->edit.sources[m->row];
	m->edit.sources[m->row] = m->capture_add ? old | one : one;
	/* Add shares the button (Zach, 2026-10-31: one button may press several
	 * inputs): the inputs that have it keep it. Change takes it from them, and
	 * the first gets this row's old buttons (a swap, Zach, 2026-10-29), even
	 * when that leaves one with none (not every game needs every input). The
	 * photo blinks the other input. */
	int given = 0;
	for (int r = 0; r < N64_INPUTS; r++)
		if (r != m->row && (m->edit.sources[r] & one)) {
			m->other_row = r;
			m->other_seq++;
			/* said beside the tabs while the photo blinks (Zach, 2026-10-29) */
			if (m->capture_add) {
				snprintf(m->line, sizeof(m->line), "Shared with %s", short_n64(r));
				continue;
			}
			m->edit.sources[r] &= ~one;
			if (!given++)
				m->edit.sources[r] |= old & ~one;
			if (!m->edit.sources[r])
				snprintf(m->line, sizeof(m->line), "Unmapped %s", short_n64(r));
			else if (given == 1 && (old & ~one)) /* the other input got this row's old buttons */
				snprintf(m->line, sizeof(m->line), "Swapped %s and %s", short_n64(m->row), short_n64(r));
			else /* only the button changed hands (Zach, 2026-10-29: "Swapped" was untrue here) */
				snprintf(m->line, sizeof(m->line), "Moved from %s to %s", short_n64(r), short_n64(m->row));
		}
	m->screen = MAP_LIST;
	return MACT_WRITE;
}

int mapping_tick(mapping_ui *m, double now)
{
	if (m->screen != MAP_CAPTURE || now < m->capture_until)
		return 0;
	m->screen = MAP_LIST;
	snprintf(m->line, sizeof(m->line), "Nothing pressed: unchanged");
	return 1;
}

void mapping_row_text(const mapping_ui *m, int row, char *label, unsigned label_size, char *value, unsigned value_size)
{
	value[0] = 0;
	if (row < N64_INPUTS) {
		snprintf(label, label_size, "%s", input_n64_label(row));
		input_sources_text(value, value_size, m->edit.sources[row], m->type);
		return;
	}
	switch (row) {
	case MAP_DEAD_ZONE:
		snprintf(label, label_size, "Dead zone");
		snprintf(value, value_size, "%d%%", (int)lroundf(m->edit.dead_zone * 100));
		break;
	case MAP_RANGE:
		snprintf(label, label_size, "Range");
		snprintf(value, value_size, "%d", m->edit.range);
		break;
	}
}

/* The headline names the type; the line under it: the last action, else
 * "All games" with no game tab (the tabs themselves name the scope). */
void mapping_headline(const mapping_ui *m, char *out, unsigned size, char *sub, unsigned sub_size)
{
	const char *name = input_type_name(m->type);
	unsigned i = 0;
	for (; name && name[i] && i + 1 < size; i++)
		out[i] = (char)(name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i]);
	out[i] = 0;
	if (!m->has_game) /* the last action's line is not shown (Zach, 2026-10-29: it explained too little) */
		snprintf(sub, sub_size, "All games");
	else
		sub[0] = 0;
}

const char *mapping_tab_name(const mapping_ui *m, int tab)
{
	return tab == MAP_TAB_GAME ? m->game : "All games";
}

void mapping_row_rect(int shown, int r[4])
{
	r[0] = 40;
	r[1] = 160 + shown * 56; /* below the tabs and the line under them (Zach, 2026-10-29) */
	r[2] = 430;
	r[3] = 50;
}

/* The tabs under the headline, between the L and R glyphs. */
void mapping_tab_rect(int tab, int r[4])
{
	r[0] = tab == MAP_TAB_GAME ? 338 : 190; /* 20 px inside each tab around its words */
	r[1] = 100;
	r[2] = tab == MAP_TAB_GAME ? 290 : 136;
	r[3] = 28;
}

int mapping_hit(const mapping_ui *m, int x, int y)
{
	int r[4];
	if (m->screen == MAP_FAILED) {
		pause_button_rect(2, r);
		return inside(x, y, r) ? 0 : -1;
	}
	if (m->screen != MAP_LIST)
		return -1;
	for (int t = 0; m->has_game && t < 2; t++) {
		mapping_tab_rect(t, r);
		if (inside(x, y, r))
			return MAP_TAB_TARGET + t;
	}
	for (int i = 0; i < MAP_SHOWN && m->top + i < MAP_ROWS; i++) {
		mapping_row_rect(i, r);
		if (inside(x, y, r))
			return m->top + i;
	}
	return -1;
}

int mapping_legend(const mapping_ui *m, int type, pause_key *keys, int max)
{
	int n = 0;
	const int wii = type == INPUT_WIIMOTE || type == INPUT_NUNCHUK;
	switch (m->screen) {
	case MAP_CAPTURE:
		keys_add(keys, &n, max, GLYPH_HOME, "Cancel", 0);
		return n;
	case MAP_FAILED:
		keys_add(keys, &n, max, ok_glyph(type), "OK", 0);
		return n;
	}
	keys_add(keys, &n, max, back_glyph(type), "Back", 0);
	if (m->row < N64_INPUTS) {
		keys_add(keys, &n, max, ok_glyph(type), "Change", 0);
		keys_add(keys, &n, max, wii ? GLYPH_PLUS : GLYPH_Y, "Add", 0);
		keys_add(keys, &n, max, wii ? GLYPH_MINUS : GLYPH_X, "Unmap", !m->edit.sources[m->row]);
	} else {
		keys_add(keys, &n, max, GLYPH_DPAD, "Change", 0);
	}
	if (!wii)
		keys_add(keys, &n, max, GLYPH_MINUS, m->can_undo ? "Undo reset" : "Reset", !mapping_can_reset(m));
	return n;
}
