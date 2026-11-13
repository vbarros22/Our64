/* Our64 player: Our Settings, Our Controllers, Change order and Edit
 * buttons on the console (ux/flows/setup). Each runs on the CPU core like
 * the pause menu: read the controllers, update setup.c's model, show a
 * frame, every 16 ms. Settings go to config/settings.cfg, buttons to
 * config/controllers.cfg or the game's profile, the moment they are applied
 * or saved; a failed write says so with OUR-0203 and changes nothing. */
#include "pause_wiiu.h"
#include "theme.h"
#include "setup.h"
#include "player.h"
#include "gameopt.h"
#include "input.h"
#include "ui.h"
#include "report.h"
#include "saves.h"
#include "usbfs_wiiu.h"

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/sdcard.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BIT(s) (1u << (s))

static setup_view g_view;
static our_settings g_settings;
static char g_settings_path[300];
static int g_from_pause;
static int g_home_asked; /* HOME on a setup screen: the caller handles it */

int settings_take_home(void)
{
	int h = g_home_asked;
	g_home_asked = 0;
	return h;
}

static char g_gameopt_path[300]; /* config/game-options.txt (gameopt.h) */

void setup_start(const char *root, const our_settings *s)
{
	g_settings = *s;
	snprintf(g_settings_path, sizeof(g_settings_path), "%s/config/settings.cfg", root);
	snprintf(g_gameopt_path, sizeof(g_gameopt_path), "%s/config/game-options.txt", root);
	report_line("OPTIONS: %d games with their own", gameopt_read(g_gameopt_path));
	for (int p = 0; p < INPUT_PORTS; p++)
		input_rumble_allow(p, g_settings.rumble[p]);
}

const our_settings *setup_settings(void) { return &g_settings; }

int setup_save_settings(void)
{
	char text[2048];
	int n = settings_format_all(text, sizeof(text), &g_settings);
	int r = savefile_write(g_settings_path, text, (unsigned)n);
	report_line("SETTINGS: %s", r ? "NOT written" : "written");
	return r;
}

static double now_s(void) { return (double)OSTicksToMicroseconds(OSGetSystemTime()) / 1e6; }

static void show(void)
{
	ui_view u;
	memset(&u, 0, sizeof(u));
	u.screen = UI_SETUP;
	u.setup = &g_view;
	ui_show(&u);
}

/* One frame of input: the events of every controller in order, and touch. */
typedef struct {
	int events[24], targets[24], n;
	input_menu m;
} frame_input;

/* events: how a controller's presses become events (pause_events, or Edit buttons' own). */
static void read_frame_with(frame_input *f, int (*events)(int type, uint32_t pressed, int *out, int max),
                            int (*hit)(int x, int y), int *touching, int *target)
{
	player_menu_wait();
	input_read();
	input_menu_state(&f->m);
	f->n = 0;
	if (input_take_home())
		f->events[f->n] = PEV_HOME, f->targets[f->n++] = -1;
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		if (f->m.type[id] < 0 || !f->m.pressed[id])
			continue;
		g_view.glyphs = f->m.type[id];
		int ev[8];
		int k = events(f->m.type[id], f->m.pressed[id], ev, 8);
		for (int i = 0; i < k && f->n < 23; i++)
			f->events[f->n] = ev[i], f->targets[f->n++] = -1;
	}
	if (f->m.touch && !*touching) {
		*touching = 1;
		*target = hit ? hit(f->m.touch_x, f->m.touch_y) : -1;
		f->events[f->n] = PEV_TOUCH_DOWN, f->targets[f->n++] = *target;
	} else if (f->m.touch) {
		*target = hit ? hit(f->m.touch_x, f->m.touch_y) : -1;
	} else if (*touching) {
		*touching = 0;
		f->events[f->n] = PEV_TOUCH_UP, f->targets[f->n++] = *target;
	}
}

static void read_frame(frame_input *f, int (*hit)(int x, int y), int *touching, int *target)
{
	read_frame_with(f, pause_events, hit, touching, target);
}

/* HOME on a setup screen: from Our ROMs it asks "Exit to the Wii U Menu?"
 * here, over the screen, and Cancel stays on it; from a paused game the pause
 * menu asks once the setup screens close. */
static int home_ok(void) { return g_from_pause || player_ask_exit(); }

static void line(const char *s) { snprintf(g_view.line, sizeof(g_view.line), "%s", s); }

static void write_failed(void)
{
	report_line("ERROR settings not written: OUR-0203");
	line("Couldn't save to the SD card: check its lock switch or free space. OUR-0203");
}

/* ---- Change order ---- */

/* 1: HOME, and leaving was confirmed (or the pause menu asks). */
static int run_order(void)
{
	order_open(&g_view.order, input_connected_count());
	g_view.screen = SETUP_ORDER;
	input_menu m;
	input_read();
	input_take_home();
	for (;;) {
		show();
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (player_closing())
			break;
		if (input_take_home() && home_ok()) {
			g_view.screen = SETUP_CONTROLLERS;
			return 1;
		}
		int done = 0;
		for (int id = 0; id < INPUT_CONTROLLERS && !done; id++) {
			if (m.type[id] < 0 || !m.pressed[id])
				continue;
			if (m.pressed[id] & (BIT(SRC_B) | (m.type[id] == INPUT_WIIMOTE ? BIT(SRC_1) : 0))) {
				if (g_view.order.count == 0)
					goto cancel;
				/* B with players set: just those play, the others leave (Zach, 2026-10-20) */
				input_set_players(g_view.order.ids, g_view.order.count);
				line(g_view.order.count == 1 ? "Only that controller plays now: Player 1." : "Only those controllers play now.");
				g_view.screen = SETUP_CONTROLLERS;
				return 0;
			}
			if (m.pressed[id] & (BIT(SRC_A) | (m.type[id] == INPUT_WIIMOTE ? BIT(SRC_2) : 0)))
				done = order_press(&g_view.order, id);
		}
		if (done) {
			input_reorder(g_view.order.ids, g_view.order.count);
			line("Order changed. Rumble stays with each player number.");
			g_view.screen = SETUP_CONTROLLERS;
			return 0;
		}
	}
cancel:
	line("Order kept as it was.");
	g_view.screen = SETUP_CONTROLLERS;
	return 0;
}

/* ---- Edit buttons ---- */

/* What mapping.c asked for. Every change is written at once: the menus read
 * the controllers' own buttons, never the mapping, so none locks a player
 * out. 1 to leave, 2 to leave for HOME (confirmed), else 0. */
static int mapping_act(mapping_ui *mp, int type, int act)
{
	switch (act) {
	case MACT_BACK: return 1;
	case MACT_HOME: return home_ok() ? 2 : 0;
	case MACT_WRITE:
		if (input_map_save(type, &mp->edit, mp->tab == MAP_TAB_GAME)) {
			report_line("ERROR buttons not written: OUR-0203");
			mapping_fail(mp, "Couldn't save your buttons", "Check the SD card's lock switch or free space, then try again. OUR-0203");
		} else
			mapping_written(mp);
		break;
	case MACT_REMOVE_PROFILE:
		if (input_profile_remove(type)) {
			report_line("ERROR profile not removed: OUR-0203");
			mapping_fail(mp, "Couldn't remove the profile", "Check the SD card's lock switch, then try again. OUR-0203");
		} else
			mapping_profile_removed(mp);
		break;
	}
	return 0;
}

static int run_mapping(int port)
{
	int owner = input_port_owner(port);
	int type = input_controller_type(owner);
	if (type < 0)
		return 0;
	input_map all, in_use;
	int profile;
	input_map_all(type, &all);
	input_map_get(type, &in_use, &profile);
	mapping_ui *mp = &g_view.mapping;
	mapping_open(mp, type, &all, profile ? &in_use : NULL, g_from_pause ? g_view.game : NULL);
	g_view.screen = SETUP_MAPPING;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, leave = 0;
	while (!leave) {
		show();
		read_frame_with(&f, mapping_events, NULL, &touching, &target);
		if (player_closing())
			return 1;
		const int capturing = mp->screen == MAP_CAPTURE; /* this frame's presses are for the capture only */
		if (capturing) {
			for (int id = 0; id < INPUT_CONTROLLERS; id++)
				if (input_controller_type(id) == type && f.m.sources[id])
					mapping_act(mp, type, mapping_capture(mp, f.m.sources[id] & ~0u));
			mapping_tick(mp, now_s());
		}
		for (int i = 0; i < f.n && !leave; i++) {
			if (capturing && (f.events[i] != PEV_HOME || mp->screen != MAP_CAPTURE))
				continue; /* captured above; HOME cancels only a capture still waiting */
			if (f.events[i] == PEV_TOUCH_DOWN || f.events[i] == PEV_TOUCH_UP)
				f.targets[i] = target = mapping_hit(mp, f.m.touch_x, f.m.touch_y);
			leave = mapping_act(mp, type, mapping_event(mp, f.events[i], f.targets[i], now_s()));
		}
	}
	g_view.screen = SETUP_CONTROLLERS;
	input_withhold();
	return leave == 2;
}

/* ---- Our Controllers ---- */

static int run_controllers(void)
{
	controllers_ui *c = &g_view.controllers;
	controllers_open(c);
	g_view.screen = SETUP_CONTROLLERS;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		for (int p = 0; p < INPUT_PORTS; p++) {
			setup_port *sp = &c->ports[p];
			input_port_info(p, &sp->type, &sp->battery, &sp->charging, &sp->gone);
			c->rumble[p] = g_settings.rumble[p];
		}
		show();
		read_frame(&f, controllers_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			switch (controllers_event(c, f.events[i], f.targets[i])) {
			case CACT_BACK: return 0;
			case CACT_HOME:
				if (home_ok())
					return 1;
				break;
			case CACT_RUMBLE: {
				int p = c->card;
				g_settings.rumble[p] = !g_settings.rumble[p];
				if (setup_save_settings()) {
					g_settings.rumble[p] = !g_settings.rumble[p];
					write_failed();
				} else {
					input_rumble_allow(p, g_settings.rumble[p]); /* the motor only: the Rumble Pak stays (gamedb.h) */
					line(g_settings.rumble[p] ? "Rumble on." : "Rumble off.");
				}
				break;
			}
			case CACT_EDIT:
				if (run_mapping(c->card))
					return 1;
				break;
			case CACT_ORDER:
				if (run_order())
					return 1;
				break;
			}
		}
	}
}

/* ---- Theme: the colors and Our ROMs' layout, each saved and shown at once ---- */

static int run_look(void)
{
	look_ui *l = &g_view.look;
	look_open(l, g_settings.theme, g_settings.layout);
	g_view.screen = SETUP_THEME;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		show();
		read_frame(&f, look_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			int act = look_event(l, f.events[i], f.targets[i]);
			switch (act) {
			case LACT_BACK: return 0;
			case LACT_HOME:
				if (home_ok())
					return 1;
				break;
			case LACT_COLORS:
			case LACT_LAYOUT: {
				const our_settings keep = g_settings;
				if (act == LACT_COLORS)
					g_settings.theme = l->pick[LOOK_COLORS];
				else
					g_settings.layout = l->pick[LOOK_LAYOUT];
				if (setup_save_settings()) {
					g_settings = keep;
					l->pick[LOOK_COLORS] = l->theme;
					l->pick[LOOK_LAYOUT] = l->layout;
					write_failed();
				} else {
					look_applied(l);
					theme_use(l->theme); /* the next frame is in it */
					theme_use_layout(l->layout);
					if (act == LACT_COLORS)
						snprintf(g_view.line, sizeof(g_view.line), "Colors: %s", theme_name(l->theme));
					else
						snprintf(g_view.line, sizeof(g_view.line), "Our ROMs: %s", layout_name(l->layout));
				}
				break;
			}
			}
		}
	}
}

/* ---- Settings > Picture: Screen mode at once; Shape and Shader saved at
 * once and shown as the settings close (settings_run) ---- */

static void apply_mode(int mode)
{
	ui_set_mode(mode);
}

static int run_picture(void)
{
	picture_ui *p = &g_view.picture;
	frame_input f;
	memset(&f, 0, sizeof(f));
	input_menu_state(&f.m);
	picture_open(p, ui_mode(), g_settings.picture, g_settings.shader, g_settings.speed, f.m.type[0] >= 0);
	g_view.screen = SETUP_PICTURE;
	g_view.line[0] = 0;
	int touching = 0, target = -1;
	for (;;) {
		p->gamepad = f.m.type[0] >= 0;
		show();
		read_frame(&f, picture_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			int act = picture_event(p, f.events[i], f.targets[i]);
			switch (act) {
			case PICT_BACK: return 0;
			case PICT_HOME:
				if (home_ok())
					return 1;
				break;
			case PICT_MODE: {
				int keep = g_settings.mode;
				g_settings.mode = p->pick[PIC_SCREEN];
				if (setup_save_settings()) {
					g_settings.mode = keep;
					p->pick[PIC_SCREEN] = p->mode;
					write_failed();
				} else {
					picture_applied(p);
					apply_mode(p->mode);
					snprintf(g_view.line, sizeof(g_view.line), "Screen mode: %s", mode_name(p->mode));
				}
				break;
			}
			case PICT_SPEED: {
				int keep = g_settings.speed;
				g_settings.speed = p->pick[PIC_SPEED];
				if (setup_save_settings()) {
					g_settings.speed = keep;
					p->pick[PIC_SPEED] = p->speed;
					write_failed();
				} else {
					picture_applied(p);
					player_speed_display(p->speed);
					snprintf(g_view.line, sizeof(g_view.line), "Speed display: %s", p->speed ? "On" : "Off");
				}
				break;
			}
			case PICT_SHAPE:
			case PICT_SHADER: {
				const our_settings keep = g_settings;
				if (act == PICT_SHAPE)
					g_settings.picture = p->pick[PIC_SHAPE];
				else
					g_settings.shader = p->pick[PIC_SHADER];
				if (setup_save_settings()) {
					g_settings = keep;
					p->pick[PIC_SHAPE] = p->shape;
					p->pick[PIC_SHADER] = p->shader;
					write_failed();
				} else {
					picture_applied(p);
					if (act == PICT_SHAPE)
						snprintf(g_view.line, sizeof(g_view.line), "Shape: %s%s", picture_name(p->shape),
						         g_from_pause ? ", on Resume" : "");
					else
						snprintf(g_view.line, sizeof(g_view.line), "Shader: %s%s", shader_name(p->shader),
						         g_from_pause ? ", on Resume" : "");
				}
				break;
			}
			}
		}
	}
}

/* ---- Settings > Games > ROM folder: drives, then folders, listed once per
 * level (BACT_OPEN, BACT_UP), not per frame ---- */

/* The drive list: the SD card, then each mounted USB partition (usbfs_wiiu.h,
 * scanned as the browser opens), then greyed rows for a USB drive with
 * nothing to read and for USB without Mocha. No row when no drive is in. */
static void list_drives(browse_ui *b)
{
	browse_entered(b, 0);
	browse_add(b, "SD card", "sd:/", 0);
	int n;
	const usbfs_volume *v = usbfs_volumes(&n);
	for (int i = 0; i < n; i++) {
		char name[64], dev[8], path[16];
		usbfs_name(&v[i], name, sizeof(name));
		usbfs_device(&v[i], dev, sizeof(dev));
		snprintf(path, sizeof(path), "%s:/", dev);
		browse_add(b, name, path, 0);
	}
	/* a Wii U-formatted drive, or one whose only partition homebrew sees is WFS */
	for (int d = 1; d <= USBFS_DRIVES; d++) {
		int mounted = 0;
		for (int i = 0; i < n; i++)
			mounted |= v[i].drive == d;
		if (usbfs_drive_open(d) && !mounted) {
			char name[64];
			snprintf(name, sizeof(name), "USB drive %d: no FAT32 or exFAT partition", d);
			browse_add(b, name, "", 1);
		}
	}
	if (usbfs_why() == USBFS_NO_MOCHA)
		browse_add(b, "USB drives need Aroma's Mocha: update Aroma", "", 1);
	else if (usbfs_why() != USBFS_READY) /* no FSA client, or no memory */
		browse_add(b, "USB drives couldn't be read", "", 1);
}

/* A browser path as the player opens it. "sd:/x/y": the SD card is mounted by
 * WHBMountSdCard (report_sd_root), at "fs:/vol/external01", not "sd:".
 * "usb1p1:/x/y": a USB partition's own device, as it is. */
static int open_path(const char *path, char *out, unsigned size)
{
	if (usbfs_volume_index(path) >= 0) {
		int n = snprintf(out, size, "%s", path);
		return n > 0 && (unsigned)n < size;
	}
	return !usbfs_sd_path(WHBGetSdCardMountPath(), path, out, size);
}

static int by_name(const void *a, const void *b) { return strcasecmp((const char *)a, (const char *)b); }

/* The folder b->path: its folders A to Z (not those starting with "."), and
 * the ROM files right in it counted for Use this folder, as Our ROMs counts
 * them (library_is_rom, library_is_folder). One it can't open greys Use. */
static void list_folder(browse_ui *b)
{
	static char names[BROWSE_MAX][256];
	char dir[300], full[600];
	int n = 0, games = 0;
	DIR *d = open_path(b->path, dir, sizeof(dir)) ? opendir(dir) : NULL;
	if (!d) {
		report_line("ROM FOLDER: can't read %s", b->path);
		line("Couldn't open this folder.");
		games = -1;
	} else {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (!library_is_folder(dir, e))
				games += library_is_rom(e->d_name);
			else if (e->d_name[0] != '.' && n < BROWSE_MAX && strlen(b->path) + strlen(e->d_name) + 2 <= sizeof(b->path))
				snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
		}
		closedir(d);
	}
	qsort(names, (size_t)n, sizeof(names[0]), by_name);
	browse_entered(b, games);
	const size_t len = strlen(b->path);
	for (int i = 0; i < n; i++) {
		/* "sd:/" + "Games", "sd:/Games" + "/" + "N64" */
		snprintf(full, sizeof(full), "%s%s%.*s", b->path, len && b->path[len - 1] == '/' ? "" : "/", 255, names[i]);
		browse_add(b, names[i], full, 0);
	}
}

/* 1: HOME, and leaving was confirmed (or the pause menu asks). */
static int run_browse(void)
{
	browse_ui *b = &g_view.browse;
	browse_open(b);
	library_usb_rescan(); /* drives plugged in, pulled or moved since */
	list_drives(b);
	g_view.screen = SETUP_BROWSE;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		show();
		read_frame(&f, NULL, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			if (f.events[i] == PEV_TOUCH_DOWN || f.events[i] == PEV_TOUCH_UP)
				f.targets[i] = target = browse_hit(b, f.m.touch_x, f.m.touch_y);
			switch (browse_event(b, f.events[i], f.targets[i])) {
			case BACT_BACK:
				g_view.line[0] = 0;
				return 0;
			case BACT_HOME:
				if (home_ok())
					return 1;
				break;
			case BACT_OPEN:
			case BACT_UP:
				g_view.line[0] = 0; /* the line under the header shows the folder again */
				if (b->depth == 0)
					list_drives(b);
				else
					list_folder(b);
				i = f.n; /* the rest of this frame's presses were for the old list */
				break;
			case BACT_USE: {
				const our_settings keep = g_settings;
				g_settings.rom_drive[0] = 0;
				if (usbfs_volume_index(b->path) >= 0) { /* "usb1p1:/x" kept as "usb:<serial>/x" */
					int n;
					const usbfs_volume *v = usbfs_volumes(&n);
					const int at = usbfs_find(v, n, b->path);
					if (at < 0 || usbfs_store_path(v, n, b->path, g_settings.rom_folder, sizeof(g_settings.rom_folder))) {
						g_settings = keep;
						report_line("ROM FOLDER: can't keep %s", b->path);
						line("Couldn't use this folder.");
						break;
					}
					/* its label too, so the folder reads in full while the drive is unplugged */
					snprintf(g_settings.rom_drive, sizeof(g_settings.rom_drive), "%s", v[at].label);
				} else {
					snprintf(g_settings.rom_folder, sizeof(g_settings.rom_folder), "%s", b->path);
				}
				if (setup_save_settings()) {
					g_settings = keep;
					write_failed();
					break;
				}
				report_line("ROM FOLDER: %s%s%s", g_settings.rom_folder, g_settings.rom_drive[0] ? ", drive " : "", g_settings.rom_drive);
				library_rescan(); /* its games listed now */
				char games[24];
				browse_row_value(b, 0, games, sizeof(games)); /* Use this folder's: "No games", "1 game", "4 games" */
				snprintf(g_view.line, sizeof(g_view.line), "ROM folder: %s (%s)", library_folder_label(), games);
				return 0;
			}
			}
		}
	}
}

int settings_pick_rom_folder(int glyphs)
{
	g_from_pause = 0; /* HOME asks "Exit to the Wii U Menu?" over the picker */
	g_view.game[0] = 0;
	g_view.glyphs = glyphs;
	input_take_home();
	const int home = run_browse();
	input_withhold();
	return home;
}

/* ---- Settings > Games: the ROM folder, and Fetch covers (covers_wiiu.c) ---- */

/* The headers being read for Fetch covers, on this thread: the fetch screen
 * drawn with how far it got, so hundreds of games on USB don't stall it. B or
 * HOME stops the reading: this runs about every 100 ms, not every frame, so a
 * B held down counts as well as one pressed. */
static int g_read_stop; /* GACT_STOP or GACT_HOME: how the reading was stopped */
static int reading_shown(int read, int total)
{
	games_reading(&g_view.games, read, total);
	show();
	input_read();
	input_menu m;
	input_menu_state(&m);
	if (input_take_home())
		return g_read_stop = GACT_HOME;
	for (int id = 0; id < INPUT_CONTROLLERS; id++) {
		if (m.type[id] < 0)
			continue;
		int ev[8];
		const int n = pause_events(m.type[id], m.pressed[id] | m.held[id], ev, 8);
		for (int k = 0; k < n; k++)
			if (ev[k] == PEV_BACK)
				return g_read_stop = GACT_STOP;
	}
	return 0;
}

/* Fetch covers: the n games without one, to the fetch's thread. */
static void fetch_start(games_ui *g, int n)
{
	covers_game *list = malloc(sizeof(*list) * (size_t)n);
	int k = list ? library_missing_list(list, n, NULL) : 0; /* n again: every header is read by now */
	if (k > n)
		k = n;
	if (k > 0 && !covers_start(report_sd_root(), list, k)) {
		games_fetching(g, 0, k);
	} else { /* no memory or no thread */
		games_fetched(g);
		report_line("ERROR Fetch covers not started: OUR-0403");
		covers_summary(COVERS_NO_START, 0, 0, 0, 0, g_view.line, sizeof(g_view.line));
	}
	free(list);
}

/* Each frame of a fetch: the count, and the covers saved since into the list. */
static void fetch_follow(games_ui *g)
{
	int done, total, saved;
	covers_progress(&done, &total, &saved);
	games_fetching(g, done, total);
	for (const char *id; (id = covers_take_saved());)
		library_cover_saved(id);
}

/* The fetch ended (stopped: B, HOME or closing): the thread joined, and
 * what happened under the header. */
static void fetch_end(games_ui *g, int stopped)
{
	covers_stop();
	int done, total, saved;
	covers_progress(&done, &total, &saved);
	for (const char *id; (id = covers_take_saved());)
		library_cover_saved(id);
	const int failed = covers_failed(), result = covers_final(covers_result(), saved, failed);
	if (result == COVERS_NO_NETWORK)
		report_line("ERROR no internet for Fetch covers: OUR-0401");
	else if (result == COVERS_NO_SERVER)
		report_line("ERROR cover server not reached: OUR-0402");
	else if (result == COVERS_CARD_FULL)
		report_line("ERROR cover not written: OUR-0205");
	covers_summary(result, stopped, saved, covers_not_found(), failed, g_view.line, sizeof(g_view.line));
	games_fetched(g);
}

/* Fetch covers on its own screen, laid out like the game loading screen
 * (Zach, 2026-11-04): the games' headers not read yet first ("Reading your
 * games: 80 of 200", the bar following), then the bar and "12 of 40" until it
 * ends, or B or HOME stops it; then the Games page with what happened under
 * the header. No game without a cover: straight back, "All games have
 * covers.". 1: HOME, and leaving was confirmed (or the pause menu asks), or
 * closing. */
static int run_fetch(games_ui *g)
{
	g_view.line[0] = 0;
	g_view.screen = SETUP_FETCH; /* drawn only while headers are read, or a fetch runs */
	g_read_stop = GACT_NONE;
	const int n = library_missing_covers(reading_shown);
	if (n <= 0) {
		games_fetched(g);
		g_view.screen = SETUP_GAMES;
		if (player_closing())
			return 1;
		if (g_read_stop == GACT_HOME)
			return home_ok(); /* the question over the Games page, Cancel stays on it */
		covers_summary(COVERS_OK, n < 0, 0, 0, 0, g_view.line, sizeof(g_view.line)); /* "Stopped.", "All games have covers." */
		return 0;
	}
	fetch_start(g, n);
	if (!g->running) {
		g_view.screen = SETUP_GAMES;
		return 0; /* it couldn't start: the line says so */
	}
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, home = 0;
	while (g->running) {
		if (!covers_running()) {
			fetch_end(g, 0);
			break;
		}
		fetch_follow(g);
		show();
		read_frame(&f, NULL, &touching, &target);
		if (player_closing()) { /* the system waits for the game to stop: the fetch stops first */
			fetch_end(g, 1);
			g_view.screen = SETUP_GAMES;
			return 1;
		}
		for (int i = 0; i < f.n && g->running; i++) {
			switch (games_event(g, f.events[i], f.targets[i])) {
			case GACT_STOP:
				fetch_end(g, 1);
				break;
			case GACT_HOME:
				fetch_end(g, 1);
				g_view.screen = SETUP_GAMES; /* the question over the Games page, Cancel stays on it */
				home = home_ok();
				break;
			}
		}
	}
	g_view.screen = SETUP_GAMES;
	return home;
}

/* Settings > Games > Start on: A picks the tab and goes back, saved at once
 * (a write that fails changes nothing). 1: HOME, and leaving was confirmed. */
static int run_start(games_ui *g)
{
	start_ui *st = &g_view.start;
	start_open(st, g_settings.start_on);
	g_view.screen = SETUP_START;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		show();
		read_frame(&f, start_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			switch (start_event(st, f.events[i], f.targets[i])) {
			case STACT_BACK: return 0;
			case STACT_HOME:
				if (home_ok())
					return 1;
				break;
			case STACT_PICK: {
				if (st->row == g_settings.start_on)
					return 0;
				const int keep = g_settings.start_on;
				g_settings.start_on = st->row;
				if (setup_save_settings()) {
					g_settings.start_on = keep;
					write_failed();
					return 0;
				}
				g->start_on = st->row;
				report_line("START ON: %s", start_name(st->row));
				snprintf(g_view.line, sizeof(g_view.line), "Start-up ROM Category: %s", start_name(st->row));
				return 0;
			}
			}
		}
	}
}

/* ---- Game options (X on a game in Our ROMs, Game options in the pause
 * menu): this game's own settings ---- */

static int options_hit_now(int x, int y) { return options_hit(&g_view.options, x, y); }
static int choice_hit_now(int x, int y) { return choice_hit(&g_view.choice, x, y); }

static int gameopt_save(void)
{
	const int n = gameopt_text(NULL, 0);
	char *text = malloc((size_t)n + 1);
	int r = -1;
	if (text) {
		gameopt_text(text, (unsigned)n + 1);
		r = savefile_write(g_gameopt_path, text, (unsigned)n);
	}
	free(text);
	return r;
}

/* Sets the game's lines and writes the file; on a failed write the old value stays. */
static void options_set_lines(options_ui *o, const char *file, int lines)
{
	const int keep = o->lines;
	if (lines == keep)
		return;
	if (gameopt_set_lines(file, lines) || gameopt_save()) {
		gameopt_set_lines(file, keep);
		write_failed();
		return;
	}
	o->lines = lines;
	if (lines == GAMEOPT_NATIVE) {
		report_line("OPTIONS: %s draws at its own resolution", file);
		snprintf(g_view.line, sizeof(g_view.line), "Resolution: Native");
	} else {
		report_line("OPTIONS: %s draws at %dp", file, lines);
		snprintf(g_view.line, sizeof(g_view.line), "Resolution: %dp", lines);
	}
}

/* A page of choices over Game options: the row picked, -1 for B, -2 for HOME (confirmed, or the pause menu's). */
static int run_choice(const char *title, const char *about, const char *const *names, int count, int in_use)
{
	choice_ui *c = &g_view.choice;
	choice_open(c, title, about, names, count, in_use);
	g_view.screen = SETUP_CHOICE;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, result = -3;
	while (result == -3) {
		show();
		read_frame(&f, choice_hit_now, &touching, &target);
		if (player_closing())
			result = -2;
		for (int i = 0; i < f.n && result == -3; i++) {
			switch (choice_event(c, f.events[i], f.targets[i])) {
			case CHACT_BACK: result = -1; break;
			case CHACT_HOME:
				if (home_ok())
					result = -2;
				break;
			case CHACT_PICK: result = c->row; break;
			}
		}
	}
	g_view.screen = SETUP_OPTIONS;
	return result;
}

/* The folder b->path for the cover picker: its folders A to Z, then its PNG
 * files A to Z (covers are read as PNG: library_wiiu.c decode_art). */
static int is_png_name(const char *name)
{
	const size_t n = strlen(name);
	return n > 4 && !strcasecmp(name + n - 4, ".png") && name[0] != '.';
}

static void list_images(browse_ui *b)
{
	static char names[BROWSE_MAX][256], files[BROWSE_MAX][256];
	char dir[300], full[600];
	int n = 0, nf = 0;
	DIR *d = open_path(b->path, dir, sizeof(dir)) ? opendir(dir) : NULL;
	if (!d) {
		report_line("COVER: can't read %s", b->path);
		line("Couldn't open this folder.");
	} else {
		struct dirent *e;
		while ((e = readdir(d))) {
			if (strlen(b->path) + strlen(e->d_name) + 2 > sizeof(b->path))
				continue;
			if (library_is_folder(dir, e)) {
				if (e->d_name[0] != '.' && n < BROWSE_MAX)
					snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
			} else if (is_png_name(e->d_name) && nf < BROWSE_MAX) {
				snprintf(files[nf++], sizeof(files[0]), "%s", e->d_name);
			}
		}
		closedir(d);
	}
	qsort(names, (size_t)n, sizeof(names[0]), by_name);
	qsort(files, (size_t)nf, sizeof(files[0]), by_name);
	browse_entered(b, 0);
	const size_t len = strlen(b->path);
	const char *sep = len && b->path[len - 1] == '/' ? "" : "/";
	for (int i = 0; i < n; i++) {
		snprintf(full, sizeof(full), "%s%s%.*s", b->path, sep, 255, names[i]);
		browse_add(b, names[i], full, 0);
	}
	for (int i = 0; i < nf; i++) {
		snprintf(full, sizeof(full), "%s%s%.*s", b->path, sep, 255, files[i]);
		browse_add_file(b, files[i], full);
	}
	if (d && n + nf == 0)
		line("No folders or PNG images here.");
}

/* The picker's preview: the selected image, read once the selection has
 * rested a quarter of a second (a big PNG takes a moment off the card), so
 * moving through a long folder stays quick. */
#define PREVIEW_W 320
#define PREVIEW_H 256
static uint8_t g_preview[PREVIEW_W * PREVIEW_H * 4] __attribute__((aligned(64)));
static char g_preview_of[256];  /* the path shown, or tried */
static char g_preview_sel[256]; /* the path selected now */
static double g_preview_since;

static void preview_tick(const browse_ui *b)
{
	const char *sel = browse_picked(b);
	if (strcmp(sel, g_preview_sel)) {
		snprintf(g_preview_sel, sizeof(g_preview_sel), "%s", sel);
		g_preview_since = now_s();
	}
	if (!strcmp(g_preview_of, g_preview_sel) || now_s() - g_preview_since < 0.25)
		return;
	snprintf(g_preview_of, sizeof(g_preview_of), "%s", g_preview_sel);
	char real[600];
	int w = 0, h = 0;
	if (g_preview_sel[0] && open_path(g_preview_sel, real, sizeof(real)) &&
	    !library_decode_png(real, PREVIEW_W, PREVIEW_H, g_preview, &w, &h)) {
		g_view.preview_rgba = g_preview;
		g_view.preview_w = w;
		g_view.preview_h = h;
	} else {
		g_view.preview_rgba = NULL; /* a folder, or a file that is not a PNG it can read */
	}
	g_view.preview_gen++;
}

/* The folder finder, picking a PNG: 1 with its path (as the finder names it) in out; 0 for B; -2 for HOME. */
static int run_pick_image(char *out, unsigned size)
{
	browse_ui *b = &g_view.browse;
	g_preview_of[0] = g_preview_sel[0] = 0;
	g_view.preview_rgba = NULL;
	g_view.preview_gen++;
	browse_open(b);
	b->files = 1;
	library_usb_rescan();
	list_drives(b);
	g_view.screen = SETUP_BROWSE;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, result = -3;
	while (result == -3) {
		preview_tick(b);
		show();
		read_frame(&f, NULL, &touching, &target);
		if (player_closing())
			result = -2;
		for (int i = 0; i < f.n && result == -3; i++) {
			if (f.events[i] == PEV_TOUCH_DOWN || f.events[i] == PEV_TOUCH_UP)
				f.targets[i] = target = browse_hit(b, f.m.touch_x, f.m.touch_y);
			switch (browse_event(b, f.events[i], f.targets[i])) {
			case BACT_BACK: result = 0; break;
			case BACT_HOME:
				if (home_ok())
					result = -2;
				break;
			case BACT_OPEN:
			case BACT_UP:
				g_view.line[0] = 0;
				if (b->depth == 0)
					list_drives(b);
				else
					list_images(b);
				i = f.n;
				break;
			case BACT_PICK:
				snprintf(out, size, "%s", browse_picked(b));
				result = 1;
				break;
			}
		}
	}
	b->files = 0;
	g_view.preview_rgba = NULL;
	g_view.line[0] = 0;
	g_view.screen = SETUP_OPTIONS;
	return result;
}

/* Game options > Picture: this game's own Screen mode, Shape and Speed
 * display. Stored at once; in a paused game they also apply at once (Shape as
 * the page closes: each change rebuilds frame buffers). -2 for HOME. */
static int gamepic_hit_now(int x, int y) { return gamepic_hit(x, y); }

static int picture_set_any(const char *file)
{
	return gameopt_picture(file, GAMEOPT_MODE) >= 0 || gameopt_picture(file, GAMEOPT_SHAPE) >= 0 ||
	       gameopt_picture(file, GAMEOPT_SPEED) >= 0 || gameopt_picture(file, GAMEOPT_FILTER) >= 0;
}

static int run_gamepic(const char *file, int paused)
{
	static const int key_of[PIC_ROWS] = { GAMEOPT_MODE, GAMEOPT_SHAPE, GAMEOPT_SPEED, GAMEOPT_FILTER };
	static const char *const row_words[PIC_ROWS] = { "Screen mode", "Shape", "Speed display", "Filter" };
	gamepic_ui *g = &g_view.gamepic;
	frame_input f;
	memset(&f, 0, sizeof(f));
	input_menu_state(&f.m);
	gamepic_open(g, gameopt_picture(file, GAMEOPT_MODE), gameopt_picture(file, GAMEOPT_SHAPE),
	             gameopt_picture(file, GAMEOPT_SPEED), gameopt_picture(file, GAMEOPT_FILTER), f.m.type[0] >= 0);
	const int shape_at_open = g->set[PIC_SHAPE];
	g_view.screen = SETUP_GAMEPIC;
	g_view.line[0] = 0;
	int touching = 0, target = -1, result = -3;
	while (result == -3) {
		g->gamepad = f.m.type[0] >= 0;
		show();
		read_frame(&f, gamepic_hit_now, &touching, &target);
		if (player_closing())
			result = -2;
		for (int i = 0; i < f.n && result == -3; i++) {
			switch (gamepic_event(g, f.events[i], f.targets[i])) {
			case GPACT_BACK: result = 0; break;
			case GPACT_HOME:
				if (home_ok())
					result = -2;
				break;
			case GPACT_SET: {
				const int row = g->row, value = g->pick[row] - 1, keep = g->set[row];
				if (gameopt_set_picture(file, key_of[row], value) || gameopt_save()) {
					gameopt_set_picture(file, key_of[row], keep);
					g->pick[row] = keep + 1;
					write_failed();
					break;
				}
				gamepic_applied(g);
				report_line("OPTIONS: %s: %s %s", file, row_words[row], gamepic_column_name(row, value + 1));
				snprintf(g_view.line, sizeof(g_view.line), "%s: %s%s", row_words[row], gamepic_column_name(row, value + 1),
				         paused && (row == PIC_SHAPE || row == PIC_SHADER) ? ", on Resume" : "");
				if (paused && row == PIC_SHADER)
					video_set_shader(value >= 0 ? value : g_settings.shader);
				if (paused && row == PIC_SCREEN)
					apply_mode(value >= 0 ? value : g_settings.mode >= 0 ? g_settings.mode : ui_mode());
				if (paused && row == PIC_SPEED)
					player_speed_display(value >= 0 ? value : g_settings.speed);
				break;
			}
			}
		}
	}
	if (paused && g->set[PIC_SHAPE] != shape_at_open)
		video_set_picture((g->set[PIC_SHAPE] >= 0 ? g->set[PIC_SHAPE] : g_settings.picture) == PICTURE_WIDE);
	g_view.line[0] = 0;
	g_view.screen = SETUP_OPTIONS;
	return result;
}

/* The picked image becomes the game's own cover: a copy at
 * art/custom/<ROM name>.png (library_custom_cover_path), so it stays when
 * its drive is unplugged. Only a PNG of at most 8 MB. */
static void cover_use(options_ui *o, const char *file, const char *picked)
{
	char from[600], to[600], dir[400];
	static unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
	if (!open_path(picked, from, sizeof(from))) {
		line("Couldn't open that image.");
		return;
	}
	FILE *f = fopen(from, "rb");
	long n = -1;
	unsigned char *data = NULL;
	if (f && !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 8 && n <= 8 * 1024 * 1024 && !fseek(f, 0, SEEK_SET) &&
	    (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) != (size_t)n) {
		free(data);
		data = NULL;
	}
	if (f)
		fclose(f);
	if (!data || memcmp(data, png, 8)) {
		report_line("COVER: %s not used (%ld bytes%s)", picked, n, data ? ", not a PNG" : "");
		line(n > 8 * 1024 * 1024 ? "That image is over 8 MB: use a smaller one." : "Couldn't read that image as a PNG.");
		free(data);
		return;
	}
	snprintf(dir, sizeof(dir), "%s/art", report_sd_root());
	mkdir(dir, 0777);
	snprintf(dir, sizeof(dir), "%s/art/custom", report_sd_root());
	mkdir(dir, 0777);
	library_custom_cover_path(file, to, sizeof(to));
	const int r = savefile_write(to, data, (unsigned)n);
	free(data);
	if (r) {
		write_failed();
		return;
	}
	library_cover_changed(file);
	o->cover = COVER_CUSTOM;
	report_line("COVER: %s is %s's cover", picked, file);
	line("Cover: Custom");
}

/* Back to the fetched cover (or none): the game's own is removed. */
static void cover_fetched(options_ui *o, const char *file)
{
	char path[600];
	if (o->cover != COVER_CUSTOM)
		return;
	library_custom_cover_path(file, path, sizeof(path));
	if (remove(path)) {
		write_failed();
		return;
	}
	library_cover_changed(file);
	const int now = library_cover_state(file);
	o->cover = now < 0 ? COVER_NONE : now;
	report_line("COVER: %s has no cover of its own any more", file);
	line(o->cover == COVER_FETCHED ? "Cover: Fetched" : "Cover: None");
}

int game_options_run(const char *file, const char *title, int glyphs, int paused, int usb)
{
	static const char *const res_names[3] = { "Native", "480p", "720p" };
	static const char *const cover_names[2] = { "Fetched cover", "Choose an image" };
	options_ui *o = &g_view.options;
	g_from_pause = paused;
	snprintf(g_view.game, sizeof(g_view.game), "%s", title);
	g_view.glyphs = glyphs;
	g_view.line[0] = 0;
	g_view.screen = SETUP_OPTIONS;
	const int cover = library_cover_state(file);
	options_open(o, gameopt_lines(file), picture_set_any(file), cover < 0 ? COVER_NONE : cover, !paused, usb, paused);
	input_take_home();
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, result = -1;
	while (result < 0) {
		show();
		read_frame(&f, options_hit_now, &touching, &target);
		if (player_closing()) {
			result = 1;
			break;
		}
		for (int i = 0; i < f.n && result < 0; i++) {
			switch (options_event(o, f.events[i], f.targets[i])) {
			case OACT_BACK: result = 0; break;
			case OACT_HOME:
				if (home_ok())
					result = 1;
				break;
			case OACT_RES: {
				/* Native (2026-11-13): the N64's own resolution, the picture enlarged after. */
				const int pick = run_choice("RESOLUTION", "Native is the N64's own. 720p is sharper, but many games slow down",
				                            res_names, 3, o->lines == GAMEOPT_NATIVE ? 0 : o->lines == 720 ? 2 : 1);
				const int lines = pick == 0 ? GAMEOPT_NATIVE : pick == 2 ? 720 : 480;
				if (pick == -2) {
					result = 1;
				} else if (pick >= 0 && lines != o->lines) {
					/* 720p is never on by itself: the player reads the warning first. In a
					 * running game the change needs a restart, which the player accepts
					 * here too (Zach, 2026-11-11): it picks up from its resume point, or
					 * from the beginning with Auto-Resume off. */
					const char *restart = !paused ? "" : g_settings.auto_resume ? " The game restarts now."
					                                                             : " The game restarts from its beginning.";
					char body[160];
					snprintf(body, sizeof(body), "%s%s", lines == 720 ? "Many games slow down or stutter with it." : "", restart);
					const int ok = (lines != 720 && !paused) ||
					               player_ask(lines == 720 ? "Use 720p for this game?" : lines == GAMEOPT_NATIVE ? "Use Native for this game?"
					                          : "Use 480p for this game?", body[0] == ' ' ? body + 1 : body,
					                          lines == 720 ? "Use 720p" : "Restart");
					if (ok) {
						options_set_lines(o, file, lines);
						if (paused && o->lines == lines)
							result = 3;
					}
				}
				i = f.n;
				break;
			}
			case OACT_COVER: {
				const int pick = run_choice("COVER", "A PNG image from the SD card or a USB drive", cover_names, 2,
				                            o->cover == COVER_CUSTOM);
				if (pick == -2) {
					result = 1;
				} else if (pick == 0) {
					cover_fetched(o, file);
				} else if (pick == 1) {
					char picked[256];
					const int r = run_pick_image(picked, sizeof(picked));
					if (r == -2)
						result = 1;
					else if (r == 1)
						cover_use(o, file, picked);
				}
				i = f.n;
				break;
			}
			case OACT_PICTURE:
				if (run_gamepic(file, paused) == -2)
					result = 1;
				o->picture_set = picture_set_any(file);
				i = f.n;
				break;
			case OACT_RENAME: {
				/* The system keyboard, starting from the name as it reads, without its
				 * extension; the new name keeps the extension. */
				char name[200], typed[256], clean[200], to[260];
				const char *dot = strrchr(file, '.');
				ui_name_for_edit(name, sizeof(name), file);
				const int k = keyboard_ask(name, typed, sizeof(typed));
				const int home = input_take_home(); /* HOME closed the keyboard: kept for this screen */
				input_withhold(); /* the press that closed the keyboard is not a press here */
				if (home)
					input_post_home();
				if (k < 0) {
					line("Couldn't open the keyboard.");
				} else if (k > 0) {
					/* characters a card refuses are kept as %XX and shown as typed (prompt.h) */
					ui_name_from_edit(clean, sizeof(clean), typed);
					const char *start = clean;
					snprintf(to, sizeof(to), "%s%s", start, dot && dot != file ? dot : "");
					if (!*start) {
						line("A name needs at least one letter.");
					} else if (strlen(to) >= 128) {
						line("That name is too long.");
					} else if (!strcmp(to, file)) {
						;
					} else {
						char old[128];
						snprintf(old, sizeof(old), "%s", file);
						const int r = library_rename(old, to);
						if (r == 1) {
							line("A ROM with that name is already there.");
						} else if (r) {
							write_failed();
						} else {
							/* the name also lives in Recent, the last game and its own options */
							for (int i = 0; i < RECENT_GAMES; i++)
								if (!strcmp(g_settings.recent[i], old))
									snprintf(g_settings.recent[i], sizeof(g_settings.recent[i]), "%s", to);
							if (!strcmp(g_settings.last_game, old))
								snprintf(g_settings.last_game, sizeof(g_settings.last_game), "%s", to);
							setup_save_settings();
							gameopt_renamed(old, to);
							gameopt_save();
							result = 4;
						}
					}
				}
				i = f.n;
				break;
			}
			case OACT_DELETE: {
				/* asked here: Cancel stays in Game options (Zach, 2026-11-11) */
				char question[96];
				snprintf(question, sizeof(question), "Delete %s?", title);
				if (player_ask(question, "Its saves and save states stay.", "Delete"))
					result = 2;
				i = f.n;
				break;
			}
			}
		}
	}
	g_view.game[0] = 0;
	g_from_pause = 0;
	input_withhold();
	return result;
}

/* Auto-Resume off: the Deleting Auto-Saves screen follows each game folder
 * (about 50 ms an SD delete; 94 games took long enough to look frozen). */
static void deleting_shown(int done, int total, void *ctx)
{
	(void)ctx;
	static OSTime last;
	g_view.deleting_done = done;
	g_view.deleting_total = total;
	const OSTime now = OSGetSystemTime();
	if (done == 0 || done == total || now - last >= OSMillisecondsToTicks(33)) {
		last = now;
		show();
	}
}

/* Auto-Resume: off only after "Are you sure?", which deletes every game's
 * resume points once the setting is saved (a write that fails leaves it on
 * and deletes nothing); on at once. */
static void toggle_auto_resume(games_ui *g, int on)
{
	/* No Auto-Saves on the card: nothing to lose, so no question and no screen. */
	const int saves = on ? 0 : states_autosaves(report_sd_root());
	if (saves > 0 && !player_ask("Are you sure?", "You will lose all your current Auto-Saves.", "Turn Off"))
		return; /* Cancel: still on */
	g_settings.auto_resume = on;
	if (setup_save_settings()) {
		g_settings.auto_resume = !on;
		write_failed();
		return;
	}
	g->auto_resume = on;
	report_line("AUTO-RESUME: %s", on ? "on" : "off");
	if (on) {
		line("Auto-Resume on.");
	} else if (saves > 0) {
		const int screen = g_view.screen;
		g_view.screen = SETUP_DELETING;
		states_forget_all(report_sd_root(), deleting_shown, NULL);
		g_view.screen = screen;
		line("Auto-Resume off. Auto-Saves deleted.");
	} else {
		states_forget_all(report_sd_root(), NULL, NULL); /* the game paused now forgets its plan */
		line("Auto-Resume off.");
	}
}

static int run_games(void)
{
	games_ui *g = &g_view.games;
	games_open(g, library_folder_label(), g_settings.auto_resume, g_settings.start_on); /* no game read here: Fetch covers reads them */
	g_view.screen = SETUP_GAMES;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1;
	for (;;) {
		show();
		read_frame(&f, games_hit, &touching, &target);
		if (player_closing())
			return 1;
		for (int i = 0; i < f.n; i++) {
			switch (games_event(g, f.events[i], f.targets[i])) {
			case GACT_BACK: return 0;
			case GACT_HOME:
				if (home_ok())
					return 1;
				break;
			case GACT_FOLDER: {
				const int home = run_browse();
				g_view.screen = SETUP_GAMES;
				snprintf(g->folder, sizeof(g->folder), "%s", library_folder_label());
				if (home)
					return 1;
				i = f.n; /* presses after A were the browser's */
				break;
			}
			case GACT_FETCH:
				if (run_fetch(g))
					return 1;
				i = f.n; /* presses after A were the fetch screen's */
				break;
			case GACT_RESUME_OFF:
			case GACT_RESUME_ON:
				toggle_auto_resume(g, !g->auto_resume);
				i = f.n; /* presses after A were the question's */
				break;
			case GACT_START: {
				const int home = run_start(g);
				g_view.screen = SETUP_GAMES;
				if (home)
					return 1;
				i = f.n; /* presses after A were the Start on page's */
				break;
			}
			}
		}
	}
}

/* ---- Our Settings: every row opens a page ---- */

void settings_run(int from_pause)
{
	g_from_pause = from_pause;
	snprintf(g_view.game, sizeof(g_view.game), "%s", from_pause ? player_title() : "");
	settings_ui *s = &g_view.settings;
	settings_open(s);
	/* Shape and Shader are applied once, as the settings close: a change can
	 * rebuild the graphics core's frame buffers, a wait the player should not
	 * get at every press while trying them (Shape: Zach, 2026-10-27). */
	const int picture_at_open = g_settings.picture, shader_at_open = g_settings.shader;
	g_view.screen = SETUP_SETTINGS;
	g_view.line[0] = 0;
	frame_input f;
	memset(&f, 0, sizeof(f));
	int touching = 0, target = -1, home = 0;
	input_take_home();
	while (!home) {
		show();
		read_frame(&f, settings_hit, &touching, &target);
		if (player_closing())
			break;
		for (int i = 0; i < f.n && !home; i++) {
			int page = 0; /* 1: HOME on a page; -1: no page opened */
			switch (settings_event(s, f.events[i], f.targets[i])) {
			case SACT_BACK:
				goto done;
			case SACT_HOME:
				home = home_ok();
				page = -1;
				break;
			case SACT_CONTROLLERS: page = run_controllers(); break;
			case SACT_PICTURE: page = run_picture(); break;
			case SACT_GAMES: page = run_games(); break;
			case SACT_THEME: page = run_look(); break;
			default: page = -1; break;
			}
			if (page >= 0) { /* back from a page */
				if (page)
					home = 1;
				g_view.screen = SETUP_SETTINGS;
				g_view.line[0] = 0;
			}
		}
	}
done:
	if (g_settings.picture != picture_at_open && !(from_pause && gameopt_picture(player_rom_file(), GAMEOPT_SHAPE) >= 0))
		video_set_picture(g_settings.picture == PICTURE_WIDE); /* not over a game's own Shape (Game options > Picture) */
	if (g_settings.shader != shader_at_open && !(from_pause && gameopt_picture(player_rom_file(), GAMEOPT_FILTER) >= 0))
		video_set_shader(g_settings.shader); /* nor over its own Filter */
	input_withhold();
	if (home) /* HOME means Wii U Menu: the caller asks first (a paused game) or leaves */
		g_home_asked = 1;
}
