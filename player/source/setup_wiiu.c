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

void setup_start(const char *root, const our_settings *s)
{
	g_settings = *s;
	snprintf(g_settings_path, sizeof(g_settings_path), "%s/config/settings.cfg", root);
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

/* ---- Settings > Picture: Screen mode at once; Shape saved at once and
 * shown as the settings close (settings_run) ---- */

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
	picture_open(p, ui_mode(), g_settings.picture, g_settings.filter, g_settings.look, g_settings.pad_look, f.m.type[0] >= 0);
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
			case PICT_SHAPE: {
				int keep = g_settings.picture;
				g_settings.picture = p->pick[PIC_SHAPE];
				if (setup_save_settings()) {
					g_settings.picture = keep;
					p->pick[PIC_SHAPE] = p->shape;
					write_failed();
				} else {
					picture_applied(p);
					snprintf(g_view.line, sizeof(g_view.line), "Shape: %s%s", picture_name(p->shape),
					         g_from_pause ? ", on Resume" : "");
				}
				break;
			}
			case PICT_LOOK: { /* on the TV's next frame */
				int keep = g_settings.look;
				g_settings.look = p->pick[PIC_LOOK];
				if (setup_save_settings()) {
					g_settings.look = keep;
					p->pick[PIC_LOOK] = p->look;
					write_failed();
				} else {
					picture_applied(p);
					video_set_crt(p->look, p->pad_look);
					snprintf(g_view.line, sizeof(g_view.line), "TV look: %s%s", look_name(p->look),
					         g_from_pause ? ", on Resume" : "");
				}
				break;
			}
			case PICT_PADLOOK: { /* on the GamePad's next frame */
				int keep = g_settings.pad_look;
				g_settings.pad_look = p->pick[PIC_PADLOOK];
				if (setup_save_settings()) {
					g_settings.pad_look = keep;
					p->pick[PIC_PADLOOK] = p->pad_look;
					write_failed();
				} else {
					picture_applied(p);
					video_set_crt(p->look, p->pad_look);
					snprintf(g_view.line, sizeof(g_view.line), "GamePad look: %s%s", look_name(p->pad_look),
					         g_from_pause ? ", on Resume" : "");
				}
				break;
			}
			case PICT_FILTER: { /* GLideN64 builds its shaders with it: from the next game start */
				int keep = g_settings.filter;
				g_settings.filter = p->pick[PIC_FILTER];
				if (setup_save_settings()) {
					g_settings.filter = keep;
					p->pick[PIC_FILTER] = p->filter;
					write_failed();
				} else {
					picture_applied(p);
					snprintf(g_view.line, sizeof(g_view.line), "Texture filter: %s%s", filter_name(p->filter),
					         g_from_pause ? ", from the next game start" : "");
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
	/* Shape is applied once, as the settings close: each change rebuilds
	 * the graphics core's frame buffers, a wait the player should not get at
	 * every press while trying both (Zach, 2026-10-27). */
	const int picture_at_open = g_settings.picture;
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
	if (g_settings.picture != picture_at_open)
		video_set_picture(g_settings.picture == PICTURE_WIDE);
	input_withhold();
	if (home) /* HOME means Wii U Menu: the caller asks first (a paused game) or leaves */
		g_home_asked = 1;
}
