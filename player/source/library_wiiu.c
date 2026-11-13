/* Our64 player: Our ROMs on the console (ux/flows/library). The games are
 * the .z64, .n64 and .v64 files in the ROM folder (Settings > Games:
 * sd:/wiiu/our64/roms unless another is chosen, on the SD card or a USB
 * drive), listed A to Z by their file names, # first. Left and right pick a letter on the letter bar
 * (the Favorites star, Recent, All, #, A to Z); up and down move through its
 * games, stacked like a drawer. Y (minus on a Wii Remote) stars a game for
 * the Favorites tab, kept in config/favorites.txt; X (GamePad, Pro and
 * Classic only) opens Game options (its resolution, its cover, and Delete: to trash/N64). A game's header, autosave, save states and cover are read only
 * when it comes into view, so a card with hundreds of ROMs opens at once.
 * Covers are sd:/wiiu/our64/art/<game id>.png (Settings > Games > Fetch
 * covers, covers_wiiu.c; or tools/fetch-art.sh from a computer).
 *
 * The loop runs on the CPU core like the prompt's: read the controllers,
 * update the selection, show a frame, every 16 ms. */
#include "pause_wiiu.h"
#include "theme.h"
#include "library.h"
#include "covers.h"
#include "player.h"
#include "input.h"
#include "ui.h"
#include "report.h"
#include "saves.h"
#include "usbfs_wiiu.h"

#include <coreinit/condition.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <png.h>
#include <whb/sdcard.h>

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define BIT(s) (1u << (s))
#define MAX_GAMES 2000

static library_game *g_games;
static int *g_shown;
static unsigned char *g_info;  /* header and saves read */
static int g_count;
static char g_root[256];
static char g_roms[300];           /* the ROM folder, resolved at each scan; "" when it can't be reached */
static char g_favorites_path[300]; /* config/favorites.txt, beside settings.cfg */
static roms_view g_view;
static int g_deleted;              /* games deleted since the scan: still in g_games, for the loader's indices */

/* Covers: decoded into RGBA on the CPU core, a few at a time. */
#define ART_W 640  /* tools/fetch-art.py keeps them at their own size, at most 357 tall and 640 wide: */
#define ART_H 360  /* the selected cover is drawn about 330 tall (2026-10-30) */
static uint8_t g_art_rgba[ROMS_ART][ART_W * ART_H * 4] __attribute__((aligned(64)));
static unsigned g_art_gen;
static int g_art_use[ROMS_ART]; /* when each slot was last wanted, for replacing the oldest */
static int g_tick;

/* A game's own cover (#29): sd:/wiiu/our64/art/custom/<ROM file name without
 * its extension>.png. Picked in Game options (a copy is put there), or put
 * there by hand with the ROM's name. It follows the file name, so a ROM hack
 * the cover database does not know can have one. */
void library_custom_cover_path(const char *file, char *out, unsigned size)
{
	const char *dot = strrchr(file, '.');
	const int n = dot && dot != file ? (int)(dot - file) : (int)strlen(file);
	snprintf(out, size, "%s/art/custom/%.*s.png", g_root, n, file);
}

/* COVER_ (setup.h) for the game with this ROM file; -1 when it is not listed. */
int library_cover_state(const char *file)
{
	for (int i = 0; i < g_count; i++)
		if (!strcmp(g_games[i].file, file))
			return g_games[i].art_custom ? COVER_CUSTOM : g_games[i].art_fetched ? COVER_FETCHED : COVER_NONE;
	return -1;
}

/* After Game options put a cover of the game's own in place or took it away:
 * the flags again, and its picture read again when next in view. */
void library_cover_changed(const char *file)
{
	char path[600];
	struct stat st;
	for (int i = 0; i < g_count; i++) {
		library_game *g = &g_games[i];
		if (strcmp(g->file, file))
			continue;
		library_custom_cover_path(file, path, sizeof(path));
		g->art_custom = !stat(path, &st);
		g->art = g->art_custom || g->art_fetched;
		for (int s = 0; s < ROMS_ART; s++)
			if (g_view.art[s].game == i)
				g_view.art[s].game = -1;
	}
}

int library_is_folder(const char *dir, const struct dirent *e)
{
	if (e->d_type != DT_UNKNOWN)
		return e->d_type == DT_DIR;
	char full[600];
	struct stat st;
	const int n = snprintf(full, sizeof(full), "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
	return n > 0 && (unsigned)n < sizeof(full) && stat(full, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A file in the ROM folder: "usb1p1:/" + "Banjo.z64" without a second slash. */
static void in_roms(const char *file, char *out, unsigned size)
{
	const size_t n = strlen(g_roms);
	snprintf(out, size, "%s%s%s", g_roms, n && g_roms[n - 1] == '/' ? "" : "/", file);
}

static const char *folder_label(const char *f);

int library_scan(const char *root)
{
	snprintf(g_root, sizeof(g_root), "%s", root);
	if (!g_games) {
		g_games = calloc(MAX_GAMES, sizeof(*g_games));
		g_shown = calloc(MAX_GAMES + 1, sizeof(*g_shown)); /* and Search's card */
		g_info = calloc(MAX_GAMES, 1);
		if (!g_games || !g_shown || !g_info)
			return 0;
	}
	/* settings.cfg's ROM folder (usbfs.h): only the default is made when it is not there */
	const char *f = setup_settings()->rom_folder;
	int vols;
	const usbfs_volume *v = usbfs_volumes(&vols);
	g_count = 0;
	g_deleted = 0;
	g_view.usb = !strncmp(f, "usb:", 4);
	g_view.missing = ROMS_FOUND;
	DIR *d = NULL;
	if (usbfs_rom_folder(f, root, WHBGetSdCardMountPath(), v, vols, g_roms, sizeof(g_roms))) {
		g_roms[0] = 0;
		g_view.missing = g_view.usb ? ROMS_NO_DRIVE : ROMS_NO_FOLDER;
	} else {
		if (!f[0])
			mkdir(g_roms, 0777);
		d = opendir(g_roms);
		if (!d && f[0]) /* the default folder that can't be made reads as no games yet, as before */
			g_view.missing = ROMS_NO_FOLDER;
	}
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)) && g_count < MAX_GAMES)
			if (library_is_rom(e->d_name) && !library_is_folder(g_roms, e)) {
				if (strlen(e->d_name) >= sizeof(g_games[0].file)) /* listed cut: it won't open */
					report_line("OUR ROMS: file name too long, cut to %d bytes: %s", (int)sizeof(g_games[0].file) - 1, e->d_name);
				library_game_init(&g_games[g_count++], e->d_name);
			}
		closedir(d);
	}
	snprintf(g_view.folder, sizeof(g_view.folder), "%s", folder_label(f)); /* whole: its sentence wraps */
	if (g_view.missing) {
		report_line("OUR ROMS: can't read the ROM folder %s (%s)", f, g_roms[0] ? g_roms : "drive not found");
		report_line("ERROR ROM folder not found");
	}
	library_sort(g_games, g_count);
	memset(g_info, 0, MAX_GAMES);
	library_mark_recent(g_games, g_count, setup_settings()->recent, RECENT_GAMES);
	snprintf(g_favorites_path, sizeof(g_favorites_path), "%s/config/favorites.txt", root);
	report_line("OUR ROMS: %d favorites", library_favorites_read(g_favorites_path));
	library_mark_favorites(g_games, g_count);
	g_view.category = LIBRARY_ALL;
	g_view.count = library_show(g_games, g_count, LIBRARY_ALL, g_shown);
	library_categories(g_games, g_count, g_view.has);
	report_line("OUR ROMS: %d games in %s", g_count, g_roms);
	return g_count;
}

/* settings.cfg's ROM folder as a full path, for people (usbfs_folder_label):
 * "SD card: /wiiu/our64/roms", "USB drive OUR64 EXFAT: /ROMs/N64", the drive
 * named by its label when plugged in, else by the label kept as it was picked. */
static const char *folder_label(const char *f)
{
	static char label[400];
	int n;
	const usbfs_volume *v = usbfs_volumes(&n);
	usbfs_folder_label(f, setup_settings()->rom_drive, v, n, label, sizeof(label));
	return label;
}

/* The ROM folder in settings.cfg, for people */
const char *library_folder_label(void) { return folder_label(setup_settings()->rom_folder); }

int library_count(void) { return g_count; }
int library_on_usb(void) { return g_view.usb; }
int library_missing(void) { return g_view.missing; }
const library_game *library_get(int i) { return i >= 0 && i < g_count ? &g_games[i] : NULL; }
int library_index_of(const char *file) { return library_find(g_games, g_count, file); }
void library_path(int i, char *out, unsigned size) { in_roms(g_games[i].file, out, size); }

/* The header, the autosave's time, the save states and whether there is a
 * cover, read into g (either thread: it touches only g and the files). */
static void read_info_of(library_game *g)
{
	char path[512];
	in_roms(g->file, path, sizeof(path));
	unsigned char h[64];
	struct stat st;
	FILE *f = fopen(path, "rb");
	g->readable = f && !stat(path, &st) && fread(h, 1, 64, f) == 64 && !library_header(h, (unsigned long)st.st_size, &g->rom);
	if (f)
		fclose(f);
	g->known = 1;
	g->autosave = 0;
	g->autosave_ok = 0;
	g->states = 0;
	g->art = 0;
	if (!g->readable)
		return;
	char dir[400];
	snprintf(dir, sizeof(dir), "%s/states/%s", g_root, g->rom.id);
	for (int k = 0; k < 2; k++) {
		snprintf(path, sizeof(path), "%s/resume-%c.o64s", dir, 'a' + k);
		state_info info;
		int r = statefile_peek(path, g->rom.crc1, g->rom.crc2, &info, NULL);
		time_t when = info.saved_at;
		if (r != STATEFILE_MISSING && !when && !stat(path, &st))
			when = st.st_mtime;
		if ((r == STATEFILE_OK || r == STATEFILE_OTHER_VERSION || r == STATEFILE_DAMAGED) && when > g->autosave) {
			g->autosave = when;
			g->autosave_ok = r == STATEFILE_OK;
		}
	}
	for (int c = 0; c < CARD_SLOTS; c++) {
		state_info info;
		if (card_peek(dir, c, g->rom.crc1, g->rom.crc2, &info, NULL) == STATEFILE_OK)
			g->states++;
	}
	snprintf(path, sizeof(path), "%s/art/%s.png", g_root, g->rom.id);
	g->art_fetched = !stat(path, &st);
	library_custom_cover_path(g->file, path, sizeof(path));
	g->art_custom = !stat(path, &st);
	g->art = g->art_fetched || g->art_custom;
}

/* A cover into out (ART_W x ART_H at most), scaled down by an area average
 * (library_fit_rgba): libretro's 512 x 374 covers draw at 493 x 360, not
 * halved to whole pixels. */
static int decode_png(const char *path, int max_w, int max_h, uint8_t *out, int *pw, int *ph);

static int decode_art(const library_game *g, uint8_t *out, int *pw, int *ph)
{
	char path[600];
	if (g->art_custom) /* the player's own wins (#29) */
		library_custom_cover_path(g->file, path, sizeof(path));
	else
		snprintf(path, sizeof(path), "%s/art/%s.png", g_root, g->rom.id);
	return decode_png(path, ART_W, ART_H, out, pw, ph);
}

/* The cover picker's preview (setup_wiiu.c): a PNG at a real path into out, at most max_w x max_h. */
int library_decode_png(const char *path, int max_w, int max_h, uint8_t *out, int *pw, int *ph)
{
	return decode_png(path, max_w, max_h, out, pw, ph);
}

static int decode_png(const char *path, int max_w, int max_h, uint8_t *out, int *pw, int *ph)
{
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&im, path))
		return -1;
	im.format = PNG_FORMAT_RGBA;
	uint8_t *full = (uint64_t)im.width * im.height <= LIBRARY_FIT_MAX_PIXELS ? malloc(PNG_IMAGE_SIZE(im)) : NULL;
	int ok = full && png_image_finish_read(&im, NULL, full, 0, NULL) &&
	         !library_fit_rgba(full, (int)im.width, (int)im.height, max_w, max_h, out, pw, ph);
	free(full);
	png_image_free(&im);
	return ok ? 0 : -1;
}

/* ---- the loader: headers, saves and covers come off the SD card on a
 * thread of their own, one job at a time, so the menu never waits on the
 * card and moves at the screen's 60 frames a second. The menu posts the job
 * nearest the selection and applies each result between frames; only the
 * menu's thread touches g_games, the art slots and the view. ---- */

enum { JOB_NONE, JOB_INFO, JOB_ART };
static OSThread g_loader __attribute__((aligned(16)));
static uint8_t g_loader_stack[32 * 1024] __attribute__((aligned(16)));
static OSMutex g_load_lock;
static OSCondition g_load_wake;
static int g_loader_on;
static int g_job, g_job_game;            /* posted by the menu */
static library_game g_job_info;          /* the game's file and id, copied when posted */
static int g_done, g_done_game, g_done_ok; /* the loader's result, until the menu takes it */
static library_game g_done_info;
static uint8_t g_stage[ART_W * ART_H * 4] __attribute__((aligned(64)));
static int g_stage_w, g_stage_h;

static int loader(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	for (;;) {
		OSLockMutex(&g_load_lock);
		while (g_job == JOB_NONE || g_done != JOB_NONE)
			OSWaitCond(&g_load_wake, &g_load_lock);
		const int job = g_job, game = g_job_game;
		library_game g = g_job_info;
		OSUnlockMutex(&g_load_lock);
		int ok = 1, w = 0, h = 0;
		if (job == JOB_INFO)
			read_info_of(&g);
		else
			ok = !decode_art(&g, g_stage, &w, &h); /* the menu reads g_stage only after g_done */
		OSLockMutex(&g_load_lock);
		g_done = job;
		g_done_game = game;
		g_done_ok = ok;
		g_done_info = g;
		g_stage_w = w;
		g_stage_h = h;
		g_job = JOB_NONE;
		OSUnlockMutex(&g_load_lock);
	}
	return 0;
}

static void loader_start(void)
{
	if (g_loader_on)
		return;
	OSInitMutex(&g_load_lock);
	OSInitCond(&g_load_wake);
	/* below the menu's priority, on the core the game is not using now */
	if (OSCreateThread(&g_loader, loader, 0, NULL, g_loader_stack + sizeof(g_loader_stack), sizeof(g_loader_stack), 26,
	                   OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		OSSetThreadName(&g_loader, "Our64 covers");
		OSResumeThread(&g_loader);
		g_loader_on = 1;
	}
}

/* Synchronously, when the menu needs the answer now (A on a game). */
static void need_info(int i)
{
	if (i >= 0 && i < g_count && !g_info[i]) {
		read_info_of(&g_games[i]);
		g_info[i] = 1;
	}
}

/* The loader's job finished (Settings posts none, so it stays idle after);
 * drop: its result is thrown away too. */
static void loader_wait(int drop)
{
	while (g_loader_on) {
		OSLockMutex(&g_load_lock);
		const int busy = g_job != JOB_NONE;
		if (!busy && drop)
			g_done = JOB_NONE;
		OSUnlockMutex(&g_load_lock);
		if (!busy)
			break;
		OSSleepTicks(OSMillisecondsToTicks(1));
	}
}

/* The ROM folder browser's rescan of the USB drives. The loader finishes its
 * job first (it may be reading a game there), then the ROM folder's device is
 * looked up again by its serial: a drive moved to another port is another
 * usb<D>p<P>:/ now, and the old path would give OUR-0104 at the next game. A
 * drive no longer there leaves no path, so another drive in its port is never
 * read in its place. */
int library_usb_rescan(void)
{
	loader_wait(0);
	const int n = usbfs_rescan();
	if (g_view.usb) {
		char roms[sizeof(g_roms)];
		int vols;
		const usbfs_volume *v = usbfs_volumes(&vols);
		if (usbfs_rom_folder(setup_settings()->rom_folder, g_root, WHBGetSdCardMountPath(), v, vols, roms, sizeof(roms)))
			roms[0] = 0;
		if (strcmp(roms, g_roms)) {
			report_line("OUR ROMS: after the USB rescan the ROM folder is %s", roms[0] ? roms : "not found");
			snprintf(g_roms, sizeof(g_roms), "%s", roms);
		}
	}
	return n;
}

/* Fetch covers: the readable games without a cover, one per game id (two
 * files of one game share its cover), into out (max of them; out NULL only
 * counts). The headers come in lazily as games come into view, so the games
 * not read yet are read here, on the menu thread: once per scan, since what
 * is read stays (about 8 small file reads a game: a second or two for 200
 * games on the SD card, longer on USB). reading, when given, is called before
 * the first is read and about every 100 ms after, so the fetch screen can show
 * how far it got; a nonzero return stops the reading, and the count is -1.
 * The loader finishes its job first, and its result, taken later, agrees with
 * what is read here. */
int library_missing_list(covers_game *out, int max, int (*reading)(int read, int total))
{
	const OSTime t0 = OSGetSystemTime();
	int n = 0, read = 0, unread = 0;
	loader_wait(0);
	for (int i = 0; i < g_count; i++)
		unread += !g_games[i].deleted && !g_info[i];
	OSTime shown = t0;
	int stopped = unread && reading && reading(0, unread);
	for (int i = 0; i < g_count && !stopped; i++) {
		if (g_games[i].deleted)
			continue;
		if (!g_info[i]) {
			if (player_closing()) {
				stopped = 1; /* the count no longer matters */
				break;
			}
			need_info(i);
			read++;
			if (reading && OSGetSystemTime() - shown >= OSMillisecondsToTicks(100)) {
				stopped = reading(read, unread);
				shown = OSGetSystemTime();
			}
		}
		const library_game *g = &g_games[i];
		if (!g->readable || g->art)
			continue;
		int shared = 0;
		for (int j = 0; j < i && !shared; j++)
			shared = !g_games[j].deleted && g_games[j].readable && !g_games[j].art && !strcmp(g_games[j].rom.id, g->rom.id);
		if (shared)
			continue;
		if (out && n < max) {
			covers_game *c = &out[n];
			memset(c, 0, sizeof(*c));
			snprintf(c->id, sizeof(c->id), "%s", g->rom.id);
			c->crc1 = g->rom.crc1;
			c->crc2 = g->rom.crc2;
			c->country = g->rom.country;
			snprintf(c->code, sizeof(c->code), "%s", g->rom.code);
			snprintf(c->file, sizeof(c->file), "%s", g->file);
		}
		n++;
	}
	if (read)
		report_line("OUR ROMS: %d games without a cover%s; %d headers read for it in %.0f ms", n, stopped ? " so far, stopped" : "", read,
		            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	return stopped ? -1 : n;
}

int library_missing_covers(int (*reading)(int read, int total)) { return library_missing_list(NULL, 0, reading); }

/* A cover Fetch covers saved: every game with that id has one now, and the
 * loader draws it when it comes into view. The loader's result not taken yet
 * was read before the cover was there, so it says so too. */
void library_cover_saved(const char *id)
{
	for (int i = 0; i < g_count; i++)
		if (g_info[i] && g_games[i].readable && !strcmp(g_games[i].rom.id, id))
			g_games[i].art = 1;
	if (g_loader_on) {
		OSLockMutex(&g_load_lock);
		if (g_done == JOB_INFO && g_done_info.readable && !strcmp(g_done_info.rom.id, id))
			g_done_info.art = 1;
		OSUnlockMutex(&g_load_lock);
	}
}

void library_refresh(int i)
{
	if (i >= 0 && i < g_count) {
		read_info_of(&g_games[i]);
		g_info[i] = 1;
		for (int s = 0; s < ROMS_ART; s++) /* its cover may be new */
			if (g_view.art[s].game == i)
				g_view.art[s].game = -1;
	}
}

static int art_slot_of(int game)
{
	for (int s = 0; s < ROMS_ART; s++)
		if (g_view.art[s].game == game)
			return s;
	return -1;
}

/* Games around the selection, nearest first: 0, +1, -1, +2, -2... */
static int around(int sel, int k)
{
	int at = sel + (k & 1 ? (k + 1) / 2 : -k / 2);
	return at >= 0 && at < g_view.count ? g_shown[at] : -1;
}
#define INFO_AROUND 13 /* the selection and six each side */
#define ART_AROUND 9   /* and four each side: the drawer shows three */

/* A finished job into the menu's state. Called with g_load_lock held. */
static void take_result(int sel)
{
	const int game = g_done_game;
	if (g_done == JOB_INFO) {
		library_game *g = &g_games[game];
		g->readable = g_done_info.readable;
		g->rom = g_done_info.rom;
		g->known = 1;
		g->autosave = g_done_info.autosave;
		g->autosave_ok = g_done_info.autosave_ok;
		g->states = g_done_info.states;
		g->art = g_done_info.art;
		g->art_custom = g_done_info.art_custom;
		g->art_fetched = g_done_info.art_fetched;
		g_info[game] = 1;
	} else if (!g_done_ok) {
		g_games[game].art = 0; /* not a PNG it can read: the poster card instead */
	} else if (art_slot_of(game) < 0) {
		/* the slot whose game is furthest from the drawer's window, oldest first */
		int pick = -1;
		for (int s = 0; s < ROMS_ART && pick < 0; s++)
			if (g_view.art[s].game < 0)
				pick = s;
		for (int s = 0; s < ROMS_ART && pick < 0; s++) {
			int near = 0;
			for (int k = 0; k < ART_AROUND; k++)
				near |= around(sel, k) == g_view.art[s].game;
			if (!near && (pick < 0 || g_art_use[s] < g_art_use[pick]))
				pick = s;
		}
		if (pick >= 0) {
			memcpy(g_art_rgba[pick], g_stage, (size_t)g_stage_w * g_stage_h * 4);
			g_view.art[pick].game = game;
			g_view.art[pick].rgba = g_art_rgba[pick];
			g_view.art[pick].w = g_stage_w;
			g_view.art[pick].h = g_stage_h;
			g_view.art[pick].gen = ++g_art_gen;
			g_art_use[pick] = g_tick;
		}
	}
	g_done = JOB_NONE;
}

/* Each frame: take what the loader finished, and give it the next job,
 * nearest the selection first: a game's header and saves, then its cover. */
static void prepare(int sel)
{
	g_tick++;
	for (int k = 0; k < ART_AROUND; k++) { /* covers in view stay */
		int s = art_slot_of(around(sel, k));
		if (s >= 0 && around(sel, k) >= 0)
			g_art_use[s] = g_tick;
	}
	if (!g_loader_on) { /* no thread: the old way, on this one */
		for (int k = 0; k < 3; k++)
			need_info(around(sel, k));
		return;
	}
	OSLockMutex(&g_load_lock);
	if (g_done != JOB_NONE)
		take_result(sel);
	if (g_job == JOB_NONE) {
		for (int k = 0; k < INFO_AROUND && g_job == JOB_NONE; k++) {
			int game = around(sel, k);
			if (game < 0)
				continue;
			if (!g_info[game])
				g_job = JOB_INFO;
			else if (k < ART_AROUND && g_games[game].readable && g_games[game].art && art_slot_of(game) < 0)
				g_job = JOB_ART;
			if (g_job != JOB_NONE) {
				g_job_game = game;
				g_job_info = g_games[game];
			}
		}
		if (g_job != JOB_NONE)
			OSSignalCond(&g_load_wake);
	}
	OSUnlockMutex(&g_load_lock);
}

/* The Search tab (2026-11-11, #22; the card 2026-11-12, Zach): its first
 * entry is a card, "Press A to search". A on it (or a tap) brings the
 * keyboard, starting from the last search; then come the games whose title
 * or file name holds every word typed, the first of them selected. No game
 * found: the card says so, and A on it asks again. Cancel changes nothing. */
static void search_ask(void)
{
	roms_view *v = &g_view;
	char typed[128];
	const int k = keyboard_ask(library_search_text(), typed, sizeof(typed));
	input_read(); /* the press that closed the keyboard is not a press here */
	if (k < 0)
		snprintf(v->notice, sizeof(v->notice), "Couldn't open the keyboard.");
	if (k <= 0)
		return;
	library_search_set(typed);
	v->category = LIBRARY_SEARCH;
	v->count = library_show(g_games, g_count, LIBRARY_SEARCH, g_shown);
	v->searched = typed[strspn(typed, " ")] != 0;
	v->sel = v->count > 1 ? 1 : 0;
	v->pos = (float)v->sel;
	if (v->count > 1)
		v->bar_focus = 0; /* onto the games found */
	report_line("OUR ROMS: search \"%s\": %d games", typed, v->count - 1);
}

/* The entry selected is Search's card, not a game. */
static int on_card(void) { return g_view.count > 0 && g_shown[g_view.sel] == LIBRARY_CARD; }

/* The letter bar's category: its games, with the game the player was on
 * still selected when it is among them (All), else the first. */
static void pick_category(int category)
{
	roms_view *v = &g_view;
	int game = v->count > 0 ? g_shown[v->sel] : -1;
	v->category = category;
	v->count = library_show(g_games, g_count, category, g_shown);
	v->sel = 0;
	for (int i = 0; i < v->count; i++)
		if (g_shown[i] == game && game >= 0 && category != LIBRARY_SEARCH) /* Search opens on its card, ready for a new search */
			v->sel = i;
	v->pos = (float)v->sel;
}

static void show(void)
{
	roms_view *v = &g_view;
	v->games = g_games;
	v->shown = g_shown;
	v->total = g_count - g_deleted;
	v->now = time(NULL);
	v->us = player_us();
	ui_view u;
	memset(&u, 0, sizeof(u));
	u.screen = UI_ROMS;
	snprintf(u.title, sizeof(u.title), "Our ROMs");
	u.roms = v;
	ui_show(&u);
}

/* Y (minus on a Wii Remote) on a game: stars or unstars it, and the file
 * follows at once, written as settings.cfg is. In Favorites an unstarred
 * game stays in the list until the player leaves the tab, so a second press
 * undoes the first. */
static int write_favorites(void)
{
	const int n = library_favorites_text(NULL, 0);
	char *text = malloc((size_t)n + 1);
	int r = -1;
	if (text) {
		library_favorites_text(text, (unsigned)n + 1);
		r = savefile_write(g_favorites_path, text, (unsigned)n);
	}
	free(text);
	return r;
}

static void rescan(void);

/* Game options > Rename: the ROM file `file` becomes `to` (both names in the
 * ROM folder). Its favorite star and its own cover go with it; saves follow
 * the ROM's contents, not its name. The list is read again, the game under
 * its new name selected. 0 done; 1 a file of that name is there already;
 * 2 the card refused; 3 not on a USB drive. */
int library_rename(const char *file, const char *to)
{
	roms_view *v = &g_view;
	char from_path[512], to_path[512], cover_from[600], cover_to[600];
	struct stat st;
	if (v->usb)
		return 3;
	int at = -1;
	for (int i = 0; i < g_count; i++)
		if (!strcmp(g_games[i].file, file))
			at = i;
	in_roms(file, from_path, sizeof(from_path));
	in_roms(to, to_path, sizeof(to_path));
	if (at < 0 || strlen(to) >= sizeof(g_games[0].file))
		return 2;
	if (strcasecmp(file, to) && !stat(to_path, &st))
		return 1;
	loader_wait(1); /* the loader reads games by file name */
	const int was_favorite = g_games[at].favorite;
	if (was_favorite)
		library_favorite_toggle(&g_games[at]);
	if (rename(from_path, to_path)) {
		if (was_favorite)
			library_favorite_toggle(&g_games[at]);
		report_line("OUR ROMS: rename %s to %s: NOT renamed", file, to);
		return 2;
	}
	library_custom_cover_path(file, cover_from, sizeof(cover_from));
	library_custom_cover_path(to, cover_to, sizeof(cover_to));
	if (!stat(cover_from, &st))
		rename(cover_from, cover_to);
	snprintf(g_games[at].file, sizeof(g_games[at].file), "%s", to);
	if (was_favorite) {
		library_favorite_toggle(&g_games[at]);
		write_favorites();
	}
	report_line("OUR ROMS: renamed %s to %s", file, to);
	rescan();
	const int game = library_index_of(to);
	int found = 0;
	for (int k = 0; k < v->count; k++)
		if (g_shown[k] == game) {
			v->sel = k;
			found = 1;
		}
	if (!found && game >= 0) { /* its new first letter is another tab: All has it */
		v->category = LIBRARY_ALL;
		v->count = library_show(g_games, g_count, LIBRARY_ALL, g_shown);
		for (int k = 0; k < v->count; k++)
			if (g_shown[k] == game)
				v->sel = k;
	}
	v->pos = (float)v->sel;
	return 0;
}

static void toggle_favorite(void)
{
	roms_view *v = &g_view;
	library_game *g = &g_games[g_shown[v->sel]];
	library_favorite_toggle(g);
	const int was = v->has[v->category];
	library_categories(g_games, g_count, v->has);
	v->has[v->category] |= was;
	int r = write_favorites();
	report_line("OUR ROMS: %s %s; favorites %s", g->favorite ? "starred" : "unstarred", g->file, r ? "NOT written" : "written");
	if (r)
		snprintf(v->notice, sizeof(v->notice), "Couldn't save to the SD card: check its lock switch or free space. OUR-0203");
}

/* X on a game (Zach, 2026-10-25: the bulk import left junk), as ui/'s Our
 * ROMs: after "Delete <title>?" its file moves to trash/N64 (a rename on the
 * card, so a computer can put it back), it leaves every list, and
 * trash/deleted.txt names it ("N64/<file>", tools/rom-upload.py's rule) so
 * the ROM sort never sends it again. Its saves and save states stay. A
 * favorite is unstarred. The game stays in g_games, marked, because the
 * loader reads games by index; the question's own loop posts no jobs, so the
 * loader is idle by the time the file moves. */
static void delete_game(void)
{
	roms_view *v = &g_view;
	const int i = g_shown[v->sel];
	library_game *g = &g_games[i];
	if (v->usb) { /* Our64 never writes to a USB drive (usbfs.h: read-only) */
		snprintf(v->notice, sizeof(v->notice), "On a USB drive: delete it on a computer");
		return;
	}
	/* asked in Game options, where Cancel stays (Zach, 2026-11-11) */
	char trash[300], from[512], to[450];
	snprintf(trash, sizeof(trash), "%s/trash", g_root);
	mkdir(trash, 0777);
	snprintf(trash, sizeof(trash), "%s/trash/N64", g_root);
	mkdir(trash, 0777);
	in_roms(g->file, from, sizeof(from));
	snprintf(to, sizeof(to), "%s/%s", trash, g->file);
	remove(to); /* an older copy deleted before: the trash keeps the newest */
	const int failed = rename(from, to) != 0;
	report_line("OUR ROMS: delete %s to %s%s", g->file, trash, failed ? ": NOT moved" : "");
	if (failed) {
		snprintf(v->notice, sizeof(v->notice), "Couldn't delete %s: check the SD card's lock switch. OUR-0204", g->title);
		return;
	}
	char list[300];
	snprintf(list, sizeof(list), "%s/trash/deleted.txt", g_root);
	FILE *f = fopen(list, "a");
	if (f) {
		fprintf(f, "N64/%s\n", g->file);
		fclose(f);
	}
	if (g->favorite)
		toggle_favorite(); /* the file follows */
	g->deleted = 1;
	g_deleted++;
	library_categories(g_games, g_count, v->has);
	if (!v->has[v->category])
		v->category = LIBRARY_ALL;
	v->count = library_show(g_games, g_count, v->category, g_shown);
	/* the game after it in list order, else the one before (the last) */
	v->sel = v->count > 0 ? v->count - 1 : 0;
	for (int k = v->count - 1; k >= 0; k--)
		if (g_shown[k] > i)
			v->sel = k;
	v->pos = (float)v->sel;
}

/* After a ROM folder is chosen (Settings > Games, or A on a folder not found): the games
 * listed again from nothing, in All, the game selected before still selected
 * when it is listed (by file name), else the first. The loader reads games by
 * index, so its job finishes first and its result is dropped, and the covers
 * in the art slots go with the old indices. Settings posts no jobs, so the
 * loader is idle by now, or nearly. */
static void rescan(void)
{
	roms_view *v = &g_view;
	char file[sizeof(g_games[0].file)] = "";
	if (v->count > 0 && !on_card())
		snprintf(file, sizeof(file), "%s", g_games[g_shown[v->sel]].file);
	loader_wait(1);
	for (int s = 0; s < ROMS_ART; s++)
		v->art[s].game = -1;
	library_scan(g_root);
	if (g_count == 0) /* + from the letter bar: no bar now, so A goes to the screen's own key */
		v->bar_focus = 0;
	const int game = library_index_of(file);
	v->sel = 0;
	for (int k = 0; k < v->count && file[0]; k++)
		if (g_shown[k] == game)
			v->sel = k;
	v->pos = (float)v->sel;
}

/* The ROM folder picker chose a folder: its games listed at once. From a
 * paused game too: the game runs from memory and nothing of it reads the list
 * until it ends, when main.c finds the game played again by its file name. */
void library_rescan(void) { rescan(); }

/* Our ROMs until A on a game (LIB_PLAY, *game its index), HOME (LIB_WIIU) or
 * the system closing (LIB_SYSTEM). *game in: the game to select. */
int library_run(int *game, const char *notice)
{
	roms_view *v = &g_view;
	static int first = 1;
	const int first_time = first;
	v->bar_focus = 0; /* Our ROMs always opens on the covers */
	if (first) {
		for (int s = 0; s < ROMS_ART; s++)
			v->art[s].game = -1;
		first = 0;
	}
	loader_start();
	/* Recent follows the games played since the list was last up. The first
	 * time, it opens on Settings > Games' Start on (2026-11-06): Recent when
	 * the last game is among them (Zach, 2026-10-18), Favorites when there
	 * are some; otherwise All. */
	library_mark_recent(g_games, g_count, setup_settings()->recent, RECENT_GAMES);
	library_categories(g_games, g_count, v->has);
	if (first_time)
		v->category = library_start_category(setup_settings()->start_on, v->has,
		                                     *game >= 0 && *game < g_count && g_games[*game].recent);
	if (!v->has[v->category])
		v->category = LIBRARY_ALL;
	v->count = library_show(g_games, g_count, v->category, g_shown);
	/* the game to select is in this letter, or All shows it */
	int in = 0;
	for (int i = 0; i < v->count; i++)
		if (g_shown[i] == *game) {
			v->sel = i;
			in = 1;
		}
	/* Opening on Favorites without the last game among them: the first favorite */
	if (!in && first_time && v->category == LIBRARY_FAVORITES && v->count > 0) {
		v->sel = 0;
		in = 1;
	}
	if (!in) { /* All: the game, or the one after it when it was deleted */
		v->count = library_show(g_games, g_count, LIBRARY_ALL, g_shown);
		v->category = LIBRARY_ALL;
		v->sel = v->count > 0 ? v->count - 1 : 0;
		for (int i = v->count - 1; i >= 0; i--)
			if (g_shown[i] >= *game)
				v->sel = i;
	}
	v->pos = (float)v->sel;
	snprintf(v->notice, sizeof(v->notice), "%s", notice && !v->missing ? notice : ""); /* the folder's message says it all */
	input_menu m;
	input_menu_state(&m);
	v->glyphs = m.last_id >= 0 && m.type[m.last_id] >= 0 ? m.type[m.last_id] : INPUT_GAMEPAD;
	player_paused(1);
	input_take_home();
	double held_since = -1, next_repeat = 0;
	int held_dir = 0, touching = 0, touch_x = 0, touch_target = -1, dragged = 0;
	int result = -1;
	while (result < 0) {
		prepare(v->sel);
		show();
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		const double now = (double)OSTicksToMicroseconds(OSGetSystemTime()) / 1e6;
		if (player_closing()) {
			result = LIB_SYSTEM;
			break;
		}
		if (input_take_home() && player_ask_exit()) {
			result = LIB_WIIU;
			break;
		}
		int pressed_any = 0, dir = 0, repeat = 0, side = 0, ok = 0, plus = 0, star = 0, del = 0, up = 0, down = 0, back = 0;
		const int list = theme_roms_list(); /* Settings > Theme > Our ROMs */
		uint32_t held = 0;
		for (int id = 0; id < INPUT_CONTROLLERS; id++) {
			if (m.type[id] < 0)
				continue;
			held |= m.held[id];
			if (!m.pressed[id])
				continue;
			pressed_any = 1;
			v->glyphs = m.type[id];
			int ev[8];
			int n = pause_events(m.type[id], m.pressed[id], ev, 8);
			for (int k = 0; k < n; k++) {
				/* the row of covers runs left to right, up goes onto the letter
				 * bar and down back (Zach, 2026-10-30); the list runs up and
				 * down, left and right change the tab */
				if (!list) {
					if (ev[k] == PEV_LEFT) dir = -1;
					if (ev[k] == PEV_RIGHT) dir = 1;
					if (ev[k] == PEV_UP) up = 1;
					if (ev[k] == PEV_DOWN) down = 1;
				} else {
					if (ev[k] == PEV_UP) dir = -1;
					if (ev[k] == PEV_DOWN) dir = 1;
					if (ev[k] == PEV_LEFT) side = -1;
					if (ev[k] == PEV_RIGHT) side = 1;
				}
				if (ev[k] == PEV_BACK) back = 1;
				if (ev[k] == PEV_OK) ok = 1;
				if (ev[k] == PEV_DELETE) star = 1; /* Y, minus on a Wii Remote or Nunchuk */
			}
			if (m.pressed[id] & BIT(SRC_L)) /* L and R change the tab too: a sideways Wii Remote has neither */
				side = -1;
			if (m.pressed[id] & BIT(SRC_R))
				side = 1;
			if (m.pressed[id] & BIT(SRC_PLUS))
				plus = 1;
			if ((m.pressed[id] & BIT(SRC_X)) && m.type[id] != INPUT_WIIMOTE && m.type[id] != INPUT_NUNCHUK)
				del = 1; /* a Wii Remote's buttons are all taken */
		}
		if (pressed_any)
			v->notice[0] = 0;
		/* Holding the way the games run: after half a second it repeats, faster the longer it is held. */
		const uint32_t back_keys = list ? BIT(SRC_UP) | BIT(SRC_LSTICK_UP) : BIT(SRC_LEFT) | BIT(SRC_LSTICK_LEFT);
		const uint32_t on_keys = list ? BIT(SRC_DOWN) | BIT(SRC_LSTICK_DOWN) : BIT(SRC_RIGHT) | BIT(SRC_LSTICK_RIGHT);
		int hold_dir = held & back_keys ? -1 : held & on_keys ? 1 : 0;
		if (dir) {
			held_dir = dir;
			held_since = now;
			next_repeat = now + 0.5;
		} else if (hold_dir && hold_dir == held_dir && held_since >= 0 && now >= next_repeat) {
			dir = hold_dir;
			repeat = 1;
			double held_for = now - held_since;
			next_repeat = now + (held_for > 2.5 ? 0.05 : held_for > 1.2 ? 0.08 : 0.12);
		} else if (!hold_dir) {
			held_dir = 0;
			held_since = -1;
		}
		/* On the bar, left/right (held too) move along it; down, A or B go back
		 * to the covers. L and R change the tab from either. */
		if (list) {
			v->bar_focus = 0;
		} else if (v->bar_focus) {
			if (dir)
				side = dir;
			dir = 0;
			if (ok && v->category == LIBRARY_SEARCH) { /* A on the magnifier: the keyboard */
				search_ask();
				continue;
			}
			if (down || ok || back)
				v->bar_focus = 0;
			ok = 0;
		} else if (up && g_count > 0) {
			v->bar_focus = 1;
		}
		if (side && g_count > 0) {
			int c = library_next_category(v->has, v->category, side);
			if (c != v->category)
				pick_category(c);
		}
		/* A press past either end of the list goes to the other end (Zach,
		 * 2026-10-27); a held repeat stops at the end, so it never overshoots. */
		if (dir && v->count > 0) {
			int to = v->sel + dir;
			if (to < 0 || to >= v->count)
				to = repeat ? v->sel : to < 0 ? v->count - 1 : 0;
			v->sel = to;
		}
		/* Touch: a letter on the bar picks it; the selected cover plays; the
		 * covers beside it select those; dragging along the covers scrolls:
		 * sideways on the row, up and down on the list's drawer. */
		if (m.touch && g_count > 0) {
			const int along = list ? m.touch_y : m.touch_x;
			if (!touching) {
				touching = 1;
				dragged = 0;
				touch_x = along;
				int bar = library_bar_hit(m.touch_x, m.touch_y);
				if (list)
					touch_target = bar >= 0 ? 100 + bar : m.touch_x >= ROMS_LIST_RIGHT ? -9
					             : m.touch_y < ROMS_LIST_COVER_TOP ? -1 : m.touch_y > ROMS_LIST_COVER_BOTTOM ? 1 : 0;
				else
					touch_target = bar >= 0 ? 100 + bar : m.touch_y < ROMS_STAGE_TOP || m.touch_y > ROMS_STAGE_BOTTOM ? -9
					             : m.touch_x < ROMS_OUTER_LEFT ? -2 : m.touch_x < ROMS_COVER_LEFT ? -1
					             : m.touch_x > ROMS_OUTER_RIGHT ? 2 : m.touch_x > ROMS_COVER_RIGHT ? 1 : 0;
			}
			if (touch_target < 100 && touch_target != -9 && (along - touch_x >= 40 || touch_x - along >= 40)) {
				int step = along < touch_x ? 1 : -1; /* drag left (up on the list): later games come */
				v->sel = v->sel + step < 0 ? 0 : v->sel + step >= v->count ? v->count - 1 : v->sel + step;
				touch_x = along;
				dragged = 1;
			}
		} else if (touching) {
			touching = 0;
			if (touch_target == 100 + LIBRARY_SEARCH && v->category == LIBRARY_SEARCH) { /* the magnifier again: the keyboard */
				search_ask();
				continue;
			} else if (touch_target >= 100 && v->has[touch_target - 100]) {
				pick_category(touch_target - 100);
			} else if (!dragged && touch_target == 0) {
				ok = 1;
			} else if (!dragged && touch_target >= -2 && touch_target <= 2 && touch_target != 0 && v->count > 0) {
				v->sel = v->sel + touch_target < 0 ? 0 : v->sel + touch_target >= v->count ? v->count - 1 : v->sel + touch_target;
				ok = on_card(); /* a tap on the search card beside a game: the keyboard at once */
			}
		}
		/* The drawer glides to the selection; far behind, it jumps closer first. */
		float gap = (float)v->sel - v->pos;
		if (gap > 3) v->pos = (float)v->sel - 3;
		if (gap < -3) v->pos = (float)v->sel + 3;
		v->pos += ((float)v->sel - v->pos) * 0.35f;
		if (fabsf((float)v->sel - v->pos) < 0.02f)
			v->pos = (float)v->sel;
		/* Search's card: A (or a tap on it) brings the keyboard; it has no
		 * favorite star and no options. */
		if (on_card()) {
			if (ok && !v->missing) {
				search_ask();
				continue;
			}
			star = del = 0;
		}
		if (star && v->count > 0)
			toggle_favorite();
		/* X: Game options (2026-11-11): the game's resolution and cover, and Delete. */
		if (del && v->count > 0) {
			const library_game *lg = &g_games[g_shown[v->sel]];
			need_info(g_shown[v->sel]); /* its cover flags, if the loader has not come by yet */
			const int r = game_options_run(lg->file, lg->title, v->glyphs, 0, v->usb);
			input_take_home();
			if (r == 1 && !player_closing())
				result = LIB_WIIU;
			if (r == 2)
				delete_game();
			if (r == 4) /* renamed: the list was read again (library_rename) */
				snprintf(v->notice, sizeof(v->notice), "Renamed");
			continue;
		}
		if (plus) {
			settings_run(0);
			input_take_home();
			const int home = settings_take_home();
			/* a folder not found is looked for again: the browser rescans the
			 * drives (a folder chosen there was listed at once: library_rescan) */
			if (v->missing && !home && !player_closing())
				rescan();
			if (home)
				result = LIB_WIIU;
			continue;
		}
		/* A on "No ROMs found.": the ROM folder picker of Settings > Games. Use
		 * this folder saves it and lists its games (library_rescan); B comes
		 * back here, and the folder is looked for again, as after + */
		if (ok && v->missing) {
			report_line("OUR ROMS: A on the folder not found: the ROM folder picker");
			const int home = settings_pick_rom_folder(v->glyphs);
			input_take_home();
			if (v->missing && !home && !player_closing())
				rescan();
			if (home && !player_closing())
				result = LIB_WIIU;
			continue;
		}
		if (ok && v->count > 0 && !on_card()) {
			int i = g_shown[v->sel];
			need_info(i);
			*game = i;
			result = LIB_PLAY;
		}
	}
	player_paused(0);
	if (v->count > 0 && !on_card())
		*game = g_shown[v->sel];
	report_line("OUR ROMS: %s %s", result == LIB_PLAY ? "play" : result == LIB_WIIU ? "HOME" : "system closing",
	            g_count > 0 && *game >= 0 ? g_games[*game].file : "");
	return result;
}

/* Our ROMs' covers, for the loading screen: the game launched is still in a slot. */
const roms_view *library_view(void) { return &g_view; }
