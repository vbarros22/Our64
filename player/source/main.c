/* Our64 player: N64 games with Not64's CPU core and GLideN64 on the GX2
 * backend. Each launch asks how the player is playing (TV only, TV + GamePad,
 * GamePad only); the controller that answers is player 1. Then Our ROMs
 * (library_wiiu.c, the games in sd:/wiiu/our64/roms) until a game is chosen;
 * games load and unload in one session (play_game). HOME opens the pause menu
 * (pause_wiiu.c), whose Exit goes back to Our ROMs; a power press closes the
 * game the same way. In-game saves load from and go
 * to sd:/wiiu/our64/saves (saves_wiiu.c); save states and the autosave to
 * sd:/wiiu/our64/states (states_wiiu.c).
 * Audio goes out through AX and sets the pace while the game sends it;
 * otherwise the game runs paced to the VI rate.
 *
 * One emulation thread on the codegen core (core 1) runs the CPU; generated
 * code may only run there. GLideN64 decodes graphics tasks and presents on
 * the graphics core (core 0), which owns GX2 (video.cpp). The hang watchdog
 * runs on core 2. */
#include <gctypes.h>
#include "main/rom.h"
#include "r4300/r4300.h"
#include "r4300/ppc/Wrappers.h"
#include "r4300/macros.h"
#include "r4300/interupt.h"
#include "gc_memory/memory.h"
#include "gc_memory/tlb.h"
#include "gc_memory/pif.h"
#include "main/ROM-Cache.h"

#include "bench.h"
#include "our64_jit.h"
#include "version.h"
#include "report.h"
#include "capture.h"
#include "player.h"
#include "writer.h"
#include "saves.h"
#include "input.h"
#include "theme.h"
#include "prompt.h"
#include "ui.h"
#include "pause_wiiu.h"
#include "gamedb.h"
#include "tpak_wiiu.h"
#include "usbfs_wiiu.h"

#include <malloc.h>
#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/exception.h>
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/energysaver.h>
#include <coreinit/foreground.h>
#include <coreinit/mcp.h>
#include <coreinit/systeminfo.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sysapp/launch.h>
#include <sysapp/switch.h>
#include <vpad/input.h>
#include <whb/log_cafe.h>
#include <whb/log_udp.h>
#include <proc_ui/procui.h>
#include <whb/proc.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define STATS_EVERY_S 10
#define GRAPHICS_CORE 0

/* ---- storage the Not64 core expects from its platform layer ---- */

char our64_mem2[OUR64_MEM2_SIZE] __attribute__((aligned(64)));
char *our64_sda_base;
char txtbuffer[1024];
bench_counters g_bench; /* the Not64 profiling hooks below fill it */

extern unsigned long dynacore;
extern long long int reg_cop1_fgr_64[32];
extern unsigned int nextLRU;
extern int noCheckInterrupt;
extern const float two16;
extern double our64_cvt_tmp;
extern const double our64_cvt_magic;
void tlb_mem2_init(void);
void format_mempacks(void);
int  rom_read(fileBrowser_file *file);
void cpu_init(void);
void cpu_deinit(void);
void go(void);
int  init_memory(void);
void free_memory(void);
void our64_dynarec_unlink(void);

void DEBUG_print(char *string, int pos) { (void)string; (void)pos; }
void DEBUG_stats(int id, char *info, unsigned int type, unsigned int adj) { (void)id; (void)info; (void)type; (void)adj; }
void InitTimer(void) {}
void new_frame(void) {}

/* ---- profiling sections (Not64's PROFILE hooks) ---- */

/* Timer ticks, not microseconds: the dispatcher enters and leaves a section
 * hundreds of thousands of times a second, and converting each reading took a
 * 64-bit division. Only the sections the log shows are timed: the dispatcher's
 * (TRAMP, FUNCS, INTERP, TLB, FP, IDLE) cost Battle for Naboo 8% of the CPU
 * core in the timer reads and the 64-bit to double conversion (profile-cpu,
 * 2026-10-26). */
static OSTime g_section_start[16];
static double g_us_per_tick;

static inline int section_shown(int s)
{
	return s == GFX_SECTION || s == AUDIO_SECTION || s == COMPILER_SECTION || s == SCREEN_SECTION;
}

void start_section(int s)
{
	if (!section_shown(s))
		return;
	g_bench.section_calls[s]++;
	g_section_start[s] = OSGetSystemTime();
}

void end_section(int s)
{
	if (!section_shown(s))
		return;
	if (g_us_per_tick == 0) g_us_per_tick = 1e6 / OSTimerClockSpeed;
	g_bench.section_us[s] += (double)(uint32_t)(OSGetSystemTime() - g_section_start[s]) * g_us_per_tick;
}

void refresh_stat(void) {}

/* ---- the GamePad as N64 controller 1 ---- */

static volatile int g_quit;   /* leaving the game: QUIT_HOME (Wii U Menu), QUIT_SYSTEM or QUIT_ROMS */
static volatile int g_closing; /* the system asked us to save and close (power button) */
/* What the system asked in the play loop (a ProcUIStatus), 0 for nothing. That
 * loop reads the message, so WHBProcIsRunning never sees it again: main
 * answers it itself once the game has stopped. */
static int g_asked;
static uint32_t g_quit_vi;    /* when */
enum { QUIT_HOME = 1, QUIT_SYSTEM, QUIT_ROMS };
static char g_rom[512]; /* the ROM folder (up to 300) + '/' + a file name (up to 127): fileBrowser_file.name holds it */
static int g_capture_left;  /* graphics tasks still to save (GamePad minus starts 120) */

#define CAPTURE_TASKS 120

int player_capturing(void) { return g_capture_left != 0; }

void player_capture_start(void)
{
#ifdef OUR64_RELEASE
	return; /* the public build has no graphics capture */
#endif
	if (!g_capture_left)
		g_capture_left = CAPTURE_TASKS;
}

/* Captures are kept to the newest CAPTURES_KEPT: a nearly full card made
 * every SD write slow (2026-10-21, 6.3 GB of old captures). frames/captures.txt
 * lists the groups in the order they were taken, so no file dates are read. */
#define CAPTURES_KEPT 4
static void captures_prune(const char *root, const char *group)
{
	char list[300], path[400];
	snprintf(list, sizeof(list), "%s/frames/captures.txt", root);
	static char groups[64][96];
	int n = 0;
	FILE *f = fopen(list, "r");
	if (f) {
		while (n < 64 && fgets(groups[n], sizeof(groups[n]), f)) {
			groups[n][strcspn(groups[n], "\r\n")] = 0;
			if (groups[n][0])
				n++;
		}
		fclose(f);
	}
	if (n < 64)
		snprintf(groups[n++], sizeof(groups[0]), "%s", group);
	int drop = n > CAPTURES_KEPT ? n - CAPTURES_KEPT : 0;
	if (drop) {
		snprintf(path, sizeof(path), "%s/frames", root);
		DIR *d = opendir(path);
		struct dirent *e;
		while (d && (e = readdir(d)))
			for (int i = 0; i < drop; i++) {
				size_t len = strlen(groups[i]);
				if (!strncmp(e->d_name, groups[i], len) && e->d_name[len] == '-') {
					snprintf(path, sizeof(path), "%s/frames/%s", root, e->d_name);
					remove(path);
					break;
				}
			}
		if (d)
			closedir(d);
		report_line("CAPTURE: %d older capture%s deleted (the newest %d are kept)", drop, drop == 1 ? "" : "s",
		            CAPTURES_KEPT);
	}
	if ((f = fopen(list, "w"))) {
		for (int i = drop; i < n; i++)
			fprintf(f, "%s\n", groups[i]);
		fclose(f);
	}
}

void player_capture_task(uint32_t task)
{
	if (!g_capture_left)
		return;
	if (g_capture_left == CAPTURE_TASKS) {
		capture_begin(report_sd_root(), strrchr(g_rom, '/') + 1);
		capture_group(g_bench.vis);
		{
			char name[80], group[96], *dot; /* as capture_begin and capture_group name the files */
			snprintf(name, sizeof(name), "%s", strrchr(g_rom, '/') + 1);
			if ((dot = strrchr(name, '.')))
				*dot = 0;
			snprintf(group, sizeof(group), "%s-v%u", name, (unsigned)g_bench.vis);
			captures_prune(report_sd_root(), group);
		}
		report_line("Capturing %d graphics tasks from VI %u (task %u)", CAPTURE_TASKS, g_bench.vis, task);
		report_save();
	}
	g_capture_left--;
	if (capture_frame(g_bench.vis, task))
		report_line("Capture of task %u failed", task);
	if (!g_capture_left) {
		report_line("Capture done at task %u", task);
		report_save();
	}
}

/* Stops once the last resume point is captured (new_vi). CPU core only. */
static char g_title[64];  /* the game, for people */
static volatile int g_in_game; /* go() is running */

static void show_closing(void)
{
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_CLOSING;
	snprintf(v.title, sizeof(v.title), "%s", g_title);
	ui_show(&v);
}

static void quit(int why)
{
	g_quit = why;
	g_quit_vi = g_bench.vis;
	states_exit();
	if (g_in_game)
		show_closing(); /* the game freezes behind it while the last resume point and saves go out */
}

static volatile int g_write_failures;
void player_write_failed(void) { g_write_failures++; }
int player_write_failures(void) { return g_write_failures; }

static volatile int g_paused;      /* a menu or a missing controller holds the game: no VIs */
static volatile int g_running;     /* go() is running: HOME pauses (else it leaves) */
static int g_pause_wanted;         /* HOME was pressed: the pause menu opens at the next VI */
static int g_exit_warned;          /* the menu showed failed writes and the player left anyway */

int player_closing(void) { return g_closing; }
static unsigned g_holds; /* menus and controller waits so far: a 10 s window with one is not the game's speed */
void player_paused(int on) { g_paused = on; g_holds += on != 0; } /* for the watchdog: no VIs while a menu is up */

void player_left(int where, int warned)
{
	g_quit = where == LEAVE_SYSTEM ? QUIT_SYSTEM : where == LEAVE_ROMS ? QUIT_ROMS : QUIT_HOME;
	g_quit_vi = g_bench.vis;
	g_exit_warned = warned;
}

/* A game started from its autosave opens paused: the pause menu comes up
 * once the first frames are on screen, so the frozen frame is the game's. */
static uint32_t g_pause_at_vi;

/* The TV clock, every NTSC game on a 59.94 Hz TV (2026-11-06): one VI per TV
 * vsync (new_vi), audio follows instead of pacing, every VI is shown for one
 * vsync. With the old pacing (audio or a 60.000 Hz timer) the frame test
 * counted 49-514 frames a minute held a wrong number of vsyncs in Conker,
 * both Banjos, GoldenEye and Mario 64; with the TV clock 0-17
 * (2026-11-05). sd:/wiiu/our64/old-pacing turns it off,
 * for comparing. */
static int g_tv_clock;
static int g_tv_cadence;       /* a PAL game on a 59.94 Hz TV: every fifth VI two vsyncs */

/* ---- frame test (2026-11-05): the folder sd:/wiiu/our64/frametest exists.
 * Each NTSC game's run measures the pacing it plays with (the TV clock, or
 * the old pacing with sd:/wiiu/our64/old-pacing), numbered on from the runs
 * already saved there for it, so the order of games, closing the app or a
 * crash cannot overwrite one. A run settles for
 * FT_SETTLE_VIS VIs (loading, a resume), measures FT_MEASURE_VIS, and writes
 * its own files as it goes: <game id>-run<N>-<tv|old>.txt every 10 s
 * (COMPLETE at the end) and .bin, the flip record, at the end. Notices say
 * which run it is and when it is saved; none show while it measures (a
 * notice changes how pictures are copied). Nothing waits on the PC, FTP or
 * the player log. ---- */
static int is_pal(void);
static void frame_check_line(const char *prefix, const video_stats_t *a, const video_stats_t *b, double max_us,
                             char *out, unsigned size);
#define FT_SETTLE_VIS 600
#define FT_MEASURE_VIS 3600
static int g_ft_on, g_ft_fix, g_ft_run, g_ft_state; /* state: 0 settling, 1 measuring, 2 saved */
static unsigned g_ft_vi0, g_ft_under0;
static char g_ft_base[400], g_ft_name[24], g_ft_l1[96], g_ft_l2[96];
static video_stats_t g_ft_from;

static void ft_notice(const char *line1, const char *line2)
{
	ui_notice(UI_KEY_DEV, NOTICE_INFO, 8, line1, line2);
	report_line("FRAME TEST: %s %s", line1, line2);
}

/* At a game's start: is this a frame test run, and which way? */
static int ft_begin(void)
{
	g_ft_on = 0;
	video_gpu_profile(0);
	const char *root = report_sd_root();
	if (!root || (is_pal() && !g_tv_clock))
		return 0;
	char dir[256], id[64], path[512];
	snprintf(dir, sizeof(dir), "%s/frametest", root);
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
	char prefix[80];
	snprintf(prefix, sizeof(prefix), "%s-run", id);
	int runs = 0, complete = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		const size_t n = strlen(e->d_name);
		if (strncmp(e->d_name, prefix, strlen(prefix)) || n < 4 || strcmp(e->d_name + n - 4, ".txt"))
			continue;
		const int r = atoi(e->d_name + strlen(prefix));
		if (r > runs)
			runs = r;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "r");
		char line[600];
		while (f && fgets(line, sizeof(line), f))
			if (!strncmp(line, "COMPLETE", 8)) {
				complete++;
				break;
			}
		if (f)
			fclose(f);
	}
	closedir(d);
	snprintf(g_ft_name, sizeof(g_ft_name), "%.20s", (const char *)ROM_HEADER.Name);
	for (int k = (int)strlen(g_ft_name) - 1; k >= 0 && g_ft_name[k] == ' '; k--)
		g_ft_name[k] = 0;
	g_ft_on = 1;
	g_ft_fix = g_tv_clock;
	g_ft_run = runs + 1;
	g_ft_state = 0;
	g_ft_vi0 = g_bench.vis;
	snprintf(g_ft_base, sizeof(g_ft_base), "%s/%s-run%d-%s", dir, id, g_ft_run, g_ft_fix ? "tv" : "old");
	/* shown 2 s into play (ft_tick): a game resumed from its autosave opens paused */
	snprintf(g_ft_l1, sizeof(g_ft_l1), "Frame test: %s run %d%s", g_ft_name, g_ft_run, g_ft_fix ? "" : ", old pacing");
	snprintf(g_ft_l2, sizeof(g_ft_l2), "%d saved before. Play until Saved shows", complete);
	report_line("FRAME TEST: %s %s", g_ft_l1, g_ft_l2);
	return 1;
}

/* The run's text file, rewritten whole each time. 0 when it could not be written. */
static int ft_write(int done)
{
	char path[420], fc[600], gpu[1600] = "";
	video_stats_t v;
	video_stats(&v);
	frame_check_line("", &g_ft_from, &v, 0, fc, sizeof(fc));
	if (done) { /* the run's GPU profile: read once, at the end (it waits for the graphics core) */
		video_gpu_profile_report(gpu, sizeof(gpu) - 2); /* room for the newline: a cut line ran into COMPLETE */
		video_gpu_profile(0);
		report_line("FRAME TEST: %s", gpu);
		strncat(gpu, "\n", sizeof(gpu) - strlen(gpu) - 1);
	}
	snprintf(path, sizeof(path), "%s.txt", g_ft_base);
	FILE *f = fopen(path, "w");
	if (!f)
		return 0;
	const unsigned measured = g_bench.vis - g_ft_vi0 - FT_SETTLE_VIS;
	double am, a95, a99, amost;
	const unsigned polls = input_age_stats(&am, &a95, &a99, &amost);
	char age[200];
	snprintf(age, sizeof(age), "input age at the game's polls: median %.2f ms, 95%% %.2f, 99%% %.2f, most %.2f (%u polls)\n",
	         am, a95, a99, amost, polls);
	int ok = fprintf(f, "game %s\nrun %d\npacing %s\nmeasured %u VIs of %u\naudio underruns %u\n%s\n%s%s%s", g_ft_name,
	                 g_ft_run, g_ft_fix ? "TV clock" : "old", measured, FT_MEASURE_VIS,
	                 audio_underruns() - g_ft_under0, fc, age, gpu, done ? "COMPLETE\n" : "") > 0;
	ok = fclose(f) == 0 && ok;
	return ok;
}

/* Each VI: settle, measure, save. */
static void ft_tick(void)
{
	if (!g_ft_on || g_ft_state == 2)
		return;
	const unsigned n = g_bench.vis - g_ft_vi0;
	if (g_ft_state == 0 && n == 120)
		ui_notice(UI_KEY_DEV, NOTICE_INFO, 7, g_ft_l1, g_ft_l2);
	if (g_ft_state == 0 && n >= FT_SETTLE_VIS) {
		ui_notice_clear(UI_KEY_DEV);
		video_stats(&g_ft_from);
		g_ft_under0 = audio_underruns();
		video_pace_start();
		video_gpu_profile(1);
		input_age_reset();
		g_ft_state = 1;
		if (!ft_write(0))
			ft_notice("Frame test: could not write", "to sd:/wiiu/our64/frametest");
		return;
	}
	if (g_ft_state != 1 || (n - FT_SETTLE_VIS) % 600)
		return;
	const int done = n - FT_SETTLE_VIS >= FT_MEASURE_VIS;
	int ok = ft_write(done);
	if (!done)
		return;
	char path[420], l1[96];
	snprintf(path, sizeof(path), "%s.bin", g_ft_base);
	ok = video_pace_write_to(path) && ok;
	g_ft_state = 2;
	snprintf(l1, sizeof(l1), "Frame test: %s run %d%s", g_ft_name, g_ft_run, g_ft_fix ? "" : " (old pacing)");
	if (ok)
		ft_notice(l1, "Saved. Exit any way you like");
	else
		ft_notice(l1, "NOT SAVED: see player-log.txt");
}

/* The flip record, if one runs, as pace-<game id>.bin (video.cpp). */
static void pace_write(void)
{
	char id[64];
	saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
	if (g_tv_clock)
		strncat(id, "-tv", sizeof(id) - strlen(id) - 1);
	video_pace_write(id);
}

/* Controllers once per VI (input_wiiu.c): HOME on any of them pauses. */
static void pad_read(void)
{
	input_read();
	if (input_take_flipq()) {
		const int on = video_flipq_toggle();
		ui_notice(UI_KEY_DEV, NOTICE_INFO, 3, on ? "GPU flip queue: on" : "GPU flip queue: off", "");
	}
	if (input_take_capture()) {
		if (video_frame_dump_on())
			video_frame_dump_start();
		else
			player_capture_start();
	}
	if (input_take_home() && !g_quit) {
		if (g_running)
			g_pause_wanted = 1;
		else
			quit(QUIT_HOME);
	}
}

/* A port's controller is gone: the game waits, frozen, until it comes back or
 * another controller presses a button to take the port. No VIs pass, so no
 * resume point records a game nobody controls. */
static OSTime g_next_vi;    /* when the next VI is due (defined again below) */
static void wait_for_controller(void)
{
	int port = input_waiting_port();
	report_line("INPUT: paused at VI %u until port %d has a controller again", g_bench.vis, port + 1);
	report_save();
	g_paused = 1;
	g_holds++;
	OSTime t0 = OSGetSystemTime();
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_PAUSED;
	while (input_waiting_port() >= 0 && !g_quit && !g_pause_wanted) {
		ui_show(&v); /* the frozen game, dimmed, with the notice saying what to do */
		player_menu_wait();
		pad_read();
		if (g_closing && !g_quit)
			quit(QUIT_SYSTEM);
	}
	g_paused = 0;
	if (!g_quit && !g_pause_wanted) /* HOME: the pause menu takes over the frozen frame */
		ui_resume();
	g_next_vi = OSGetSystemTime(); /* the VI schedule starts again from now */
	report_line("INPUT: %s after %.1f s", g_quit ? "leaving" : g_pause_wanted ? "pause menu" : "resumed",
	            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e6);
}

/* ---- each VI: input, pacing to the VI rate, statistics ---- */

static OSTime g_next_vi;    /* when the next VI is due */
static OSTime g_vi_period;
static uint32_t g_tv_vi_k; /* TV clock: the vsync the current VI is due at */
extern int video_tv_vsync(uint32_t *count, OSTime *t); /* video.cpp */
extern int video_tv_slip(void);
extern unsigned video_tv_lead_dropped(void);
static double g_stats_from_us;
static double g_vi_wait_us;
/* TV clock pacing in the last 10 s (the stats line "TV clock pacing"): sleeps to the VI's vsync, waits for
 * the TV (none since VIs are dropped instead, video_update_screen), schedule moves when behind or ahead, slips, VIs due 2+ vsyncs on, the oldest vsync seen */
static struct { double sched_us, vs_age_max_us; unsigned behind, ahead, slips, far, n, dropped0; } g_tvp;
static unsigned long g_threads_at; /* sd:/wiiu/our64/threads.txt: the game's __osActiveQueue, in hex */
static int g_interpreter;          /* sd:/wiiu/our64/interpreter exists: pure interpreter, no recompiler */
static int g_sync_gfx;             /* the CPU waits for each graphics task: this game needs it, or the switch */
static int g_sync_switch;          /* sd:/wiiu/our64/sync-gfx exists: every game waits */
static int g_sync_before;          /* sd:/wiiu/our64/sync-before exists: the graphics core is idle before each task is posted, as during a capture */
static int g_probe_overlap;        /* sd:/wiiu/our64/probe-overlap exists: no wait and no RDRAM writes by the decoder (read_test_switches) */
static int g_capture_at_start;     /* sd:/wiiu/our64/capture-at-start exists: a capture as each game starts or resets */
static int g_tlb_links_switch;     /* sd:/wiiu/our64/tlb-links exists: every game links its TLB code */
static int g_count_switch;         /* sd:/wiiu/our64/count-per-op holds 1-4: every game's counter factor */
static int g_build_shaders;        /* sd:/wiiu/our64/build-shaders exists: build every game's shaders, then stop */
/* sd:/wiiu/our64/sweep exists: play every game for g_sweep_s seconds with nobody at the controls, then stop */
static int g_sweep_s;
static char g_game_fc[640];  /* the last game's pacing and frame check, for its sweep line */
static double g_sweep_t0_us, g_sweep_low;  /* when the game started playing; its lowest 10 s VI/s (no menu in them) */
static uint32_t g_sweep_vis0, g_sweep_tasks0, g_sweep_under0, g_sweep_yields0, g_sweep_links0;

int player_sync_gfx(void) { return g_sync_gfx; }
int player_sync_before(void) { return g_sync_before; }

/* The last VI had no idle time: the game is behind real time. */
static int g_vi_late;

/* How far emulated time is ahead of real time: the last VI's real time plus
 * the part of a VI period the CPU has run since, minus now. A game behind
 * is never ahead: the schedule restarts from each late VI, so early in a VI
 * it looked ahead, and the CPU waited on graphics tasks it could have run
 * past (Gauntlet Legends: 5-30% of each second, at 40-58 VIs a second). */
double player_ahead_us(void)
{
	unsigned long delay = vi_register.vi_delay;
	if (!delay || g_vi_late)
		return 0;
	long elapsed = (long)(Count - (next_vi - delay));
	if (elapsed < 0)
		elapsed = 0;
	OSTime target = g_next_vi + (OSTime)((double)elapsed / delay * (double)g_vi_period);
	OSTime now = OSGetSystemTime();
	return target > now ? (double)OSTicksToMicroseconds(target - now) : 0;
}

static int g_writer_ok; /* the SD writer thread runs: saves and autosaves work */
static void save_log(void) { report_save(); }

/* The frame check between two readings (video.cpp g_vi_marks): did the TV
 * show every emulated VI for one vsync, as the N64 would? */
static video_stats_t g_fc_window, g_fc_game; /* at the last stats line; at the game's start */
static double g_fc_game_max_us;
static void frame_check_line(const char *prefix, const video_stats_t *a, const video_stats_t *b, double max_us,
                             char *out, unsigned size)
{
	unsigned lat_n = b->latency_n - a->latency_n;
	unsigned pend_n = b->tv_pending_n - a->tv_pending_n;
	char tv[224] = "";
	if (g_tv_clock)
		snprintf(tv, sizeof(tv), "; TV clock: swaps waiting %.2f a present, %u extra vsyncs (delay back), %u waits, "
		         "%u VIs dropped for the TV (total)",
		         pend_n ? (double)(b->tv_pending_sum - a->tv_pending_sum) / pend_n : 0.0, b->tv_trimmed - a->tv_trimmed,
		         b->tv_waited - a->tv_waited, video_tv_lead_dropped());
	snprintf(out, size, "%sframe check: %u N64 VIs (%u game frames), %u TV vsyncs, %u flips; TV skipped %u VIs, repeated %u; "
	            "game frames never shown %u, held wrong %u of %u (short %u, long %u); VIs not presented %u; "
	            "VI to TV %.1f ms (most %.1f)%s",
	            prefix, b->vis_marked - a->vis_marked, b->game_frames - a->game_frames, b->tv_vsyncs - a->tv_vsyncs,
	            b->fc_flips - a->fc_flips, b->fc_skipped - a->fc_skipped, b->fc_repeated - a->fc_repeated,
	            b->fc_never - a->fc_never, (b->frames_short - a->frames_short) + (b->frames_long - a->frames_long),
	            b->frames_new - a->frames_new, b->frames_short - a->frames_short, b->frames_long - a->frames_long,
	            b->vis_unpresented - a->vis_unpresented,
	            lat_n ? (b->latency_us - a->latency_us) / lat_n / 1e3 : 0.0, max_us / 1e3, tv);
	report_line("%s", out);
}

/* Where the time went, per second of the last STATS_EVERY_S seconds. What
 * is left after the listed parts is the CPU core running game code. Saved to
 * SD each time, so a session that ends badly still has its log. */
static void stats(double t)
{
	static uint32_t vis, gfx_calls, underruns, links, yields;
	static double sect[16], audio_wait, link_us, gfx_wait, task_us, screen_us, cpu_wait, gpu_wait, wait_part[4];
	static unsigned shown, late, dropped, trimmed, taken, fnew, fshort, flong;
	static double queue_wait;
	extern unsigned our64_gx2_links;
	extern double our64_gx2_link_us;
	double s = (t - g_stats_from_us) / 1e6, ms = 1e-3 / s;
	double d[16];
	for (int i = 0; i < 16; i++) {
		d[i] = g_bench.section_us[i] - sect[i];
		sect[i] = g_bench.section_us[i];
	}
	video_stats_t v;
	video_stats(&v);
	double pacing = audio_wait_us() - audio_wait + g_vi_wait_us;
	double compile = our64_gx2_link_us - link_us;
	double event_wait = gfx_event_wait_us() - gfx_wait;
	static unsigned holds;
	if (g_holds == holds && (g_bench.vis - vis) / s < g_sweep_low)
		g_sweep_low = (g_bench.vis - vis) / s;
	holds = g_holds;
	report_line("%.0f s: %.1f VI/s, %.1f frames/s, %u audio underruns, %u graphics yields", t / 1e6,
	            (g_bench.vis - vis) / s, (gfx_tasks_posted() - gfx_calls) / s,
	            audio_underruns() - underruns, (unsigned)(gfx_yields() - yields));
	/* Graphics core: decoding tasks, VIs (presenting), idle. CPU core: posting
	 * graphics work, waiting for it, audio tasks, JIT, pacing, and the rest:
	 * game code. Times are ms per second. */
	/* The color copy to RDRAM (GLideN64 ColorBufferToRDRAM.cpp) is part of
	 * tasks or screen, whichever asked for it. */
	extern volatile unsigned long long our64_color_copy_ticks;
	extern volatile unsigned our64_color_copies;
	static unsigned long long copy_ticks;
	static unsigned copies;
	const unsigned long long copy_now = our64_color_copy_ticks;
	const unsigned copies_now = our64_color_copies;
	report_line("  graphics core: tasks %.0f (worst %.0f, %u shaders compiled in %.0f), screen %.0f, idle %.0f; "
	            "GPU wait %.0f; frames shown %u (late %u), replaced unshown %u, trimmed %u; "
	            "game frames %u (held short %u, long %u); queue wait %.1f ms a VI; "
	            "color copies %u in %.0f",
	            (v.task_us - task_us) * ms, v.worst_task_us / 1e3, our64_gx2_links - links, compile / 1e3,
	            (v.screen_us - screen_us) * ms, 1e3 - (v.task_us - task_us + v.screen_us - screen_us) * ms,
	            (v.gpu_wait_us - gpu_wait) * ms, v.frames_shown - shown, v.frames_late - late,
	            v.frames_dropped - dropped, v.frames_trimmed - trimmed,
	            v.frames_new - fnew, v.frames_short - fshort, v.frames_long - flong,
	            v.vis_taken != taken ? (v.queue_wait_us - queue_wait) / 1e3 / (v.vis_taken - taken) : 0.0,
	            copies_now - copies, OSTicksToMicroseconds(copy_now - copy_ticks) * ms);
	copy_ticks = copy_now;
	copies = copies_now;
	char fc[600];
	frame_check_line("  ", &g_fc_window, &v, v.latency_max_us, fc, sizeof(fc));
	if (v.latency_max_us > g_fc_game_max_us)
		g_fc_game_max_us = v.latency_max_us;
	g_fc_window = v;
	double posting = d[GFX_SECTION] + d[SCREEN_SECTION] + event_wait;
	report_line("  CPU core: graphics %.0f (tasks %.0f, VIs %.0f, waiting %.0f: ring full %.0f, calls %.0f, a task while ahead %.0f, "
	            "a task needed %.0f), audio tasks %.0f, JIT %.0f, pacing %.0f, game %.0f",
	            posting * ms, d[GFX_SECTION] * ms, d[SCREEN_SECTION] * ms, (v.cpu_wait_us - cpu_wait) * ms,
	            (v.cpu_wait_part_us[0] - wait_part[0]) * ms, (v.cpu_wait_part_us[1] - wait_part[1]) * ms,
	            (v.cpu_wait_part_us[2] - wait_part[2]) * ms, (v.cpu_wait_part_us[3] - wait_part[3]) * ms,
	            d[AUDIO_SECTION] * ms, d[COMPILER_SECTION] * ms,
	            pacing * ms, 1e3 - (posting + d[AUDIO_SECTION] + d[COMPILER_SECTION] + pacing) * ms);
	extern volatile unsigned long long our64_alloc_ticks[3];
	extern volatile unsigned our64_alloc_calls[3];
	static unsigned long long alloc_ticks[3];
	static unsigned alloc_calls[3];
	double alloc_ms[3];
	unsigned calls[3];
	for (int c = 0; c < 3; c++) {
		unsigned long long ticks = our64_alloc_ticks[c];
		unsigned n = our64_alloc_calls[c];
		alloc_ms[c] = OSTicksToMicroseconds(ticks - alloc_ticks[c]) / 1e3;
		calls[c] = n - alloc_calls[c];
		alloc_ticks[c] = ticks;
		alloc_calls[c] = n;
	}
	report_line("  malloc and free: core 0 %.0f ms (%u calls), core 1 %.0f ms (%u), core 2 %.0f ms (%u)",
	            alloc_ms[0], calls[0], alloc_ms[1], calls[1], alloc_ms[2], calls[2]);
	static unsigned pushed0, dropped0;
	unsigned pushed, lost, rate;
	int playing;
	audio_debug(&pushed, &lost, &rate, &playing);
	char resume[160];
	states_stats(resume, sizeof(resume));
	static unsigned dips0;
	int lowest_pm, highest_pm;
	unsigned dips;
	audio_smoothing(&lowest_pm, &highest_pm, &dips);
	report_line("  audio: %.0f frames/s pushed (%.0f dropped) at %u Hz, voices %s, played at %.1f-%.1f%%, %u dips; %s",
	            (pushed - pushed0) / s, (lost - dropped0) / s, rate, playing ? "playing" : "stopped",
	            100 + lowest_pm / 10.0, 100 + highest_pm / 10.0, dips - dips0, resume);
	dips0 = dips;
	pushed0 = pushed;
	dropped0 = lost;
	char state[256];
	player_core_state(state, sizeof(state));
	report_line("  %s", state);
	if (g_threads_at)
		player_thread_dump(g_threads_at);
	/* On the SD writer's thread: the whole log is rewritten each time, and on
	 * the CPU core that stopped the game for 0.1-2.4 s every 10 s on a slow
	 * card (Zach, 2026-10-21: odd stalls in Mario Golf at odd times). */
	if (g_writer_ok)
		writer_post(WRITER_LOG, save_log);
	else
		report_save();
	vis = g_bench.vis;
	gfx_calls = gfx_tasks_posted();
	underruns = audio_underruns();
	yields = gfx_yields();
	links = our64_gx2_links;
	link_us = our64_gx2_link_us;
	audio_wait = audio_wait_us();
	gfx_wait = gfx_event_wait_us();
	task_us = v.task_us;
	screen_us = v.screen_us;
	gpu_wait = v.gpu_wait_us;
	shown = v.frames_shown;
	dropped = v.frames_dropped;
	trimmed = v.frames_trimmed;
	fnew = v.frames_new;
	fshort = v.frames_short;
	flong = v.frames_long;
	taken = v.vis_taken;
	queue_wait = v.queue_wait_us;
	late = v.frames_late;
	cpu_wait = v.cpu_wait_us;
	for (int i = 0; i < 4; i++)
		wait_part[i] = v.cpu_wait_part_us[i];
	if (g_tv_clock)
		report_line("  TV clock pacing: %u VIs, sleeping to the vsync %.0f, %u dropped for the TV; schedule moved "
		            "%u behind, %u ahead; %u slips; %u due 2+ vsyncs on; oldest vsync %.1f ms",
		            g_tvp.n, g_tvp.sched_us * ms, video_tv_lead_dropped() - g_tvp.dropped0, g_tvp.behind, g_tvp.ahead,
		            g_tvp.slips, g_tvp.far, g_tvp.vs_age_max_us / 1e3);
	memset(&g_tvp, 0, sizeof(g_tvp));
	g_tvp.dropped0 = video_tv_lead_dropped();
	g_vi_wait_us = 0;
	g_stats_from_us = t;
}

unsigned player_vis(void) { return g_bench.vis; }

/* How long audio_push waited per VI, smoothed over about 16 VIs. */
static double g_audio_wait_per_vi;

double player_idle_left_us(void)
{
	if (audio_paces())
		return g_audio_wait_per_vi;
	OSTime due = g_next_vi + g_vi_period, now = OSGetSystemTime(); /* what new_vi will sleep until */
	return due > now ? (double)OSTicksToMicroseconds(due - now) : 0;
}

void new_vi(void)
{
	g_bench.vis++;
	timeline_note('V', vi_register.vi_origin, vi_register.vi_width, vi_register.vi_status, vi_register.vi_delay);
	if (g_ai_stream)
		ai_stream_tick();
	gfx_watch_vi(g_bench.vis);
	pad_read();
	if (g_closing && !g_quit)
		quit(QUIT_SYSTEM);
	if (g_sweep_s && !g_quit && report_now_us() - g_sweep_t0_us >= g_sweep_s * 1e6)
		quit(QUIT_ROMS); /* the sweep's time for this game is up */
	if (g_pause_at_vi && g_bench.vis >= g_pause_at_vi) {
		g_pause_at_vi = 0;
		g_pause_wanted = 1;
	}
	if (!g_quit && !g_pause_wanted && input_waiting_port() >= 0)
		wait_for_controller();
	/* HOME: the menu opens at the next instruction boundary, with the game captured there. */
	if (g_pause_wanted && !g_quit) {
		g_pause_wanted = 0;
		report_line("PAUSE: HOME at VI %u", g_bench.vis);
		timeline_write(); /* a frozen game is left with HOME, and the power button loses it */
		pace_write();
		states_pause();
	}
	/* Leaving: once the last resume point is captured, or after a second without the moment for it. */
	if (g_quit && (states_exit_done() || g_bench.vis - g_quit_vi >= 60))
		stop = 1;

	/* While the game sends audio, audio_push waits for room and so sets the
	 * pace. Otherwise wait until the VI is due; a frame that ran late moves
	 * the schedule instead of rushing to catch up. Either wait is idle time
	 * a resume point capture can use. */
	static double audio_waited;
	double idle_us = audio_wait_us() - audio_waited;
	audio_waited += idle_us;
	g_audio_wait_per_vi += (idle_us - g_audio_wait_per_vi) / 16;
	OSTime now = OSGetSystemTime();
	uint32_t vs_count;
	OSTime vs_t;
	if (g_tv_clock && video_tv_vsync(&vs_count, &vs_t)) {
		/* The vsync now, counted on from the last one known; this VI is due at
		 * the next of its own. Behind by more than two: the schedule moves (the
		 * game runs slower, the TV keeps pictures longer; nothing is skipped). */
		const uint32_t k_now = vs_count + (uint32_t)(now > vs_t ? (now - vs_t) / g_vi_period : 0);
		g_tv_vi_k += video_last_vi_hold(); /* the VI just run is due this many vsyncs: 2 for a PAL game's every fifth */
		const int slip = video_tv_slip();
		g_tv_vi_k += slip; /* the TV's queue is deep: one vsync more, unseen */
		g_tvp.n++;
		g_tvp.slips += slip;
		if (now > vs_t && OSTicksToMicroseconds(now - vs_t) > g_tvp.vs_age_max_us)
			g_tvp.vs_age_max_us = OSTicksToMicroseconds(now - vs_t);
		/* Behind by up to two vsyncs the CPU core catches up (kTvLeadMax in
		 * video.cpp bounds the delay that adds); more, and the schedule moves. Moving it
		 * after one vsync starved audio where Conker is CPU-bound: 10 underruns
		 * a minute instead of 1 (2026-11-05). */
		if ((int32_t)(k_now - g_tv_vi_k) > 2 || (int32_t)(g_tv_vi_k - k_now) > 4) {
			g_tvp.behind += (int32_t)(k_now - g_tv_vi_k) > 2;
			g_tvp.ahead += (int32_t)(g_tv_vi_k - k_now) > 4;
			g_tv_vi_k = k_now;
		}
		g_tvp.far += (int32_t)(g_tv_vi_k - k_now) >= 2;
		g_next_vi = vs_t + (OSTime)(int32_t)(g_tv_vi_k - vs_count) * g_vi_period;
		if (g_next_vi > now) {
			OSSleepTicks(g_next_vi - now);
			g_tvp.sched_us += OSTicksToMicroseconds(g_next_vi - now);
			g_vi_wait_us += OSTicksToMicroseconds(g_next_vi - now);
			idle_us += OSTicksToMicroseconds(g_next_vi - now);
		}
	} else if (audio_paces()) {
		/* Audio sets the pace only while it blocks: a game sending a trickle
		 * (Perfect Dark and Conker loading, 2,000 frames/s of 22,000) never
		 * fills the queue, and VIs ran at 250-320/s. A VI clock 1.25 times
		 * real time caps it, leaving audio the room it needs: never more than
		 * 2 VIs ahead. Once the ring is full the clock keeps real time:
		 * Chameleon Twist makes its audio per game frame, not per VI, and at
		 * 1.25 times its frames took 2.5 VIs instead of 2, so it sent less per
		 * VI, the ring never filled and the VIs stayed at 75/s (2026-10-29). */
		g_next_vi += audio_ring_full() ? g_vi_period : g_vi_period * 4 / 5;
		if (now + 2 * g_vi_period < g_next_vi) {
			OSTime wait = g_next_vi - 2 * g_vi_period - now;
			OSSleepTicks(wait);
			g_vi_wait_us += OSTicksToMicroseconds(wait);
			idle_us += OSTicksToMicroseconds(wait);
		} else if (now > g_next_vi)
			g_next_vi = now;
	} else if ((g_next_vi += g_vi_period) > now) {
		OSSleepTicks(g_next_vi - now);
		g_vi_wait_us += OSTicksToMicroseconds(g_next_vi - now);
		idle_us += OSTicksToMicroseconds(g_next_vi - now);
	} else if (now - g_next_vi > 4 * g_vi_period)
		g_next_vi = now;
	g_vi_late = idle_us < 1000;
	/* The controllers again after the wait, so the frame the game runs next
	 * gets input from now, not from before the sleep: Smash slept ~6.7 ms a
	 * VI between the read above (HOME, pauses) and its next frame (400 ms a
	 * second, 2026-11-06). */
	if (idle_us >= 1000)
		input_read();

	double t = report_now_us();
	saves_poll(t);
	states_vi(t, idle_us);
	if (t - g_stats_from_us >= STATS_EVERY_S * 1e6)
		stats(t);
	ft_tick();
}

/* ---- hang watchdog: on core 2, so it still runs when the emulation thread
 * is stuck. Without a VI for 10 s it saves the log with where things stand. ---- */

static volatile int g_watch_on;
static OSThread g_watch_thread __attribute__((aligned(16)));
static uint8_t g_watch_stack[64 * 1024] __attribute__((aligned(16)));
extern volatile long long our64_gx2_compile_start; /* gx2_GL.cpp: non-zero while CafeGLSL compiles */
extern volatile const char *our64_at_file;            /* GLideN64's our64_at.h: the last mark passed */
extern volatile int our64_at_line;
extern volatile unsigned our64_vi_w, our64_vi_h;
extern volatile unsigned our64_gx2_compile_vs_bytes, our64_gx2_compile_ps_bytes;

static int watchdog(int argc, const char **argv)
{
	(void)argc; (void)argv;
	uint32_t last_vis = 0;
	int quiet_s = 0;
	while (g_watch_on) {
		OSSleepTicks(OSSecondsToTicks(5));
		uint32_t v = g_bench.vis;
		/* only while a game runs: menus, a missing controller and loading (a resume point took 7.5 s) are not hangs */
		quiet_s = v == last_vis && g_running && !g_paused ? quiet_s + 5 : 0;
		last_vis = v;
		/* The CPU core runs on while the graphics core is stuck, so this is a check of its own. */
		unsigned cmd;
		double busy = video_busy_s(&cmd);
		if (busy >= 10 && (int)busy % 10 < 5) {
			report_line("WATCHDOG: graphics core in one command (type %u: 0 call, 1 task, 2 VI) for %.0f s%s", cmd,
			            busy, our64_gx2_compile_start ? ", compiling a shader" : "");
			/* GLideN64's last mark, the VI size and the heap: safe to read, so first */
			struct mallinfo mi = mallinfo();
			report_line("  graphics core passed %s:%d last; VI %ux%u; heap %u KB in use of %u KB taken from the system",
			            (const char *)our64_at_file, our64_at_line, our64_vi_w, our64_vi_h, (unsigned)(mi.uordblks / 1024),
			            (unsigned)(mi.arena / 1024));
			report_save();
			if (busy < 15) { /* then its stack, once: stopping it can hang this thread too */
				char where[512];
				video_backtrace(where, sizeof(where));
				report_line("  %s (code at %08X)", where, (unsigned)(uintptr_t)&watchdog);
				report_save();
			}
		}
		if (quiet_s < 10 || quiet_s % 10)
			continue;
		long long c = our64_gx2_compile_start;
		char state[256];
		player_core_state(state, sizeof(state));
		if (c)
			report_line("WATCHDOG: no VI for %d s, in a shader compile for %.1f s (vertex %u bytes, pixel %u bytes)",
			            quiet_s, (double)OSTicksToMicroseconds(OSGetSystemTime() - (OSTime)c) / 1e6,
			            our64_gx2_compile_vs_bytes, our64_gx2_compile_ps_bytes);
		else
			report_line("WATCHDOG: no VI for %d s, not compiling a shader", quiet_s);
		report_line("  %s", state);
		report_save();
	}
	return 0;
}

static void watchdog_start(void)
{
	g_watch_on = 1;
	if (OSCreateThread(&g_watch_thread, (OSThreadEntryPointFn)watchdog, 0, NULL, g_watch_stack + sizeof(g_watch_stack),
	                   sizeof(g_watch_stack), 20, OS_THREAD_ATTRIB_AFFINITY_CPU2))
		OSResumeThread(&g_watch_thread);
}

static void watchdog_stop(void)
{
	g_watch_on = 0;
	int ret;
	OSJoinThread(&g_watch_thread, &ret);
}

/* ---- small data base for generated code ---- */

static int sda_setup(void)
{
	struct { const void *p; unsigned n; } v[] = {
		{ reg, sizeof(long long) * 34 }, { reg_cop0, 4 * 32 },
		{ reg_cop1_simple, 4 * 32 }, { reg_cop1_double, 4 * 32 },
		{ reg_cop1_fgr_64, 8 * 32 }, { &llbit, 4 }, { &local_rs32, 4 },
		{ &FCR31, 4 }, { &last_addr, 4 }, { &next_interupt, 4 },
		{ &nextLRU, 4 }, { &noCheckInterrupt, 4 }, { &two16, 4 },
		{ &our64_cvt_tmp, 8 }, { &our64_cvt_magic, 8 },
	};
	uintptr_t first = ~(uintptr_t)0, end = 0;
	for (unsigned i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		uintptr_t a = (uintptr_t)v[i].p;
		if (a < first) first = a;
		if (a + v[i].n > end) end = a + v[i].n;
	}
	if (end - first > 0xFFF0)
		return -1;
	our64_sda_base = (char *)first + 0x8000;
	return 0;
}

/* ---- crash report: saved to SD before Cafe OS stops the app ---- */

static const char *where(uint32_t a)
{
	uint32_t base = (uint32_t)our64_jit_heap_start() - 64 * 1024;
	if (a >= base && a < base + our64_jit_heap_size() + 64 * 1024) return "generated code";
	if (a >= 0x02000000 && a < 0x10000000) return "app code";
	return "elsewhere";
}

/* A crash. The exception callback runs for every core, where taking a lock
 * or touching the SD card can wait forever (the console froze with nothing
 * logged, 2026-10-17): it only notes what happened, then sends the thread
 * on, on a stack of its own, to crash_after, which logs it in the open and
 * shows it with OSFatal. */
static char g_crash_text[900];
static volatile int g_crashing;
static uint8_t g_crash_stack[64 * 1024] __attribute__((aligned(16)));

static void crash_after(void)
{
	report_line("%s", g_crash_text);
	extern void gfx_notes_report(const char *prefix, unsigned n);
	gfx_notes_report("CRASH: ", 16); /* the last RSP tasks and interrupts before it */
	report_save();
	OSFatal(g_crash_text);
}

static BOOL crash_report(const char *what, OSContext *c)
{
	if (__sync_lock_test_and_set(&g_crashing, 1))
		return FALSE; /* a crash while handling one: the system's own handling */
	snprintf(g_crash_text, sizeof(g_crash_text),
	         "CRASH: %s at 0x%08X (%s), lr 0x%08X (%s), dar 0x%08X, dsisr 0x%08X\n"
	         "CRASH: r1 0x%08X r3 0x%08X r4 0x%08X r13 0x%08X; N64 PC 0x%08X; this code is at 0x%08X\n"
	         "CRASH: thread %s on core %u",
	         what, (unsigned)c->srr0, where(c->srr0), (unsigned)c->lr, where(c->lr), (unsigned)c->dar,
	         (unsigned)c->dsisr, (unsigned)c->gpr[1], (unsigned)c->gpr[3], (unsigned)c->gpr[4], (unsigned)c->gpr[13],
	         (unsigned)interp_addr, (unsigned)(uintptr_t)&crash_report,
	         OSGetThreadName(OSGetCurrentThread()) ? OSGetThreadName(OSGetCurrentThread()) : "?", OSGetCoreId());
	/* The calls that led here: the stack's back chain, return addresses only. */
	int n = (int)strlen(g_crash_text);
	n += snprintf(g_crash_text + n, sizeof(g_crash_text) - n, "\nCRASH: stack");
	uint32_t sp = c->gpr[1];
	for (int depth = 0; depth < 20 && n < (int)sizeof(g_crash_text) - 10; depth++) {
		if (sp < 0x10000000 || sp >= 0x50000000 || (sp & 7))
			break;
		uint32_t up = *(const uint32_t *)(uintptr_t)sp;
		if (up <= sp || up - sp > 0x100000)
			break;
		n += snprintf(g_crash_text + n, sizeof(g_crash_text) - n, " %08X", *(const uint32_t *)(uintptr_t)(up + 4));
		sp = up;
	}
	c->srr0 = (uint32_t)(uintptr_t)&crash_after;
	c->gpr[1] = (uint32_t)(uintptr_t)(g_crash_stack + sizeof(g_crash_stack) - 64);
	return TRUE;
}

static BOOL on_dsi(OSContext *c)     { return crash_report("DSI (bad data access)", c); }
static BOOL on_isi(OSContext *c)     { return crash_report("ISI (bad code address)", c); }
static BOOL on_program(OSContext *c) { return crash_report("program exception (illegal or trap instruction)", c); }
static BOOL on_align(OSContext *c)   { return crash_report("alignment exception", c); }

static void crash_handlers_install(void)
{
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
	OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT, on_align);
}

/* ---- the ROM ---- */


/* Switches on the SD card for testing: sd:/wiiu/our64/threads.txt,
 * interpreter, sync-gfx, no-jr-cache, capture-at-start, tlb-links, count-per-op. */
static void read_test_switches(const char *root)
{
	char path[256];
	struct stat st;
	snprintf(path, sizeof(path), "%s/threads.txt", root);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fscanf(f, "%lx", &g_threads_at) != 1) g_threads_at = 0;
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/interpreter", root);
	g_interpreter = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/timeline", root);
	g_timeline_on = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/audio-dump", root);
	g_audio_dump_on = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/sync-gfx", root);
	g_sync_switch = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/sync-before", root);
	g_sync_before = !stat(path, &st);
	if (g_sync_before)
		report_line("TEST: the graphics core is idle before each graphics task is posted (sync-before)");
	/* probe-overlap: Perfect Dark runs without the CPU waiting for each
	 * graphics task, and GLideN64 writes nothing into RDRAM (the color and
	 * depth copies, the software depth, fills, the depth-line copy). If the
	 * intro survives, the no-wait crash is a late RDRAM write; if not, the
	 * yield protocol is the suspect (2026-10-29). */
	snprintf(path, sizeof(path), "%s/probe-overlap", root);
	g_probe_overlap = !stat(path, &st);
	if (g_probe_overlap) {
		extern int our64_rdram_writes; /* GLideN64's FrameBuffer.cpp */
		our64_rdram_writes = 0;
		report_line("TEST: no CPU wait for graphics tasks and no decoder writes into RDRAM (probe-overlap)");
	}
	{
		extern int our64_tlb_links; /* Not64's Recomp-Cache-Heap.c */
		snprintf(path, sizeof(path), "%s/tlb-links", root);
		g_tlb_links_switch = !stat(path, &st);
	}
	snprintf(path, sizeof(path), "%s/capture-at-start", root);
	g_capture_at_start = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/no-jr-cache", root);
	jr_cache_on = stat(path, &st) != 0;
	snprintf(path, sizeof(path), "%s/build-shaders", root);
	g_build_shaders = !stat(path, &st);
	snprintf(path, sizeof(path), "%s/sweep", root);
	g_sweep_s = 0;
	if ((f = fopen(path, "r"))) { /* the seconds each game plays, 30 when the file is empty */
		if (fscanf(f, "%d", &g_sweep_s) != 1 || g_sweep_s < 5 || g_sweep_s > 600)
			g_sweep_s = 30;
		fclose(f);
	}
	snprintf(path, sizeof(path), "%s/count-per-op", root);
	g_count_switch = 0;
	if ((f = fopen(path, "r"))) {
		if (fscanf(f, "%d", &g_count_switch) != 1 || g_count_switch < 1 || g_count_switch > 4)
			g_count_switch = 0;
		fclose(f);
	}
}

static int is_pal(void)
{
	switch (ROM_HEADER.Country_code) {
	case 'D': case 'F': case 'I': case 'P': case 'S': case 'U': case 'X': case 'Y':
		return 1;
	}
	return 0;
}

int player_is_pal(void) { return is_pal(); }

static int g_us_console; /* US region: 12-hour clock, month first */

int player_us(void) { return g_us_console; }
const char *player_title(void) { return g_title; }

/* The N64 as at power on, as Not64's own Restart Game does. The in-game save
 * memory is not touched: the saves stay. */
void player_reset_machine(void)
{
	cpuprof_restart(); /* the samples so far point at code cpu_deinit frees */
	cpu_deinit();
	free_memory();
	init_memory();
	tlb_mem2_init();
	cpu_init();
	our64_dynarec_unlink();
}

static int us_console(void)
{
	int us = 0, h = MCP_Open();
	if (h >= 0) {
		static MCPSysProdSettings p __attribute__((aligned(0x40)));
		if (MCP_GetSysProdSettings(h, &p) == 0)
			us = p.product_area == MCP_REGION_USA;
		MCP_Close(h);
	}
	return us;
}

#define BIT(s) (1u << (s))

/* Asks how the player is playing, until a controller answers (it becomes
 * player 1). Returns the mode, or -1 when HOME or the system ends it. */
static int run_prompt(our_settings *s, int damaged, const char *line)
{
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_PROMPT;
	snprintf(v.title, sizeof(v.title), "Our ROMs");
	if (damaged)
		snprintf(v.warning, sizeof(v.warning), "Settings file damaged: your choice here replaces it.");
	snprintf(v.line, sizeof(v.line), "%s", line);
	input_menu m;
	input_read();
	input_menu_state(&m);
	prompt_init(&v.prompt, s->mode, m.type[0] >= 0);
	report_line("PROMPT: %s; last choice %s, GamePad %s", v.line, s->mode >= 0 ? mode_name(s->mode) : "none",
	            m.type[0] >= 0 ? "on" : "off");
	int touching = 0, touch_card = -1, chosen = -1, by = -1;
	g_paused = 1; /* for the watchdog: no VIs while the prompt waits */
	while (chosen < 0) {
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (g_closing || (input_take_home() && player_ask_exit())) {
			g_paused = 0;
			return -1;
		}
		prompt_set_gamepad(&v.prompt, m.type[0] >= 0);
		for (int id = 0; id < 5 && chosen < 0; id++) {
			uint32_t p = m.pressed[id];
			if (p & (BIT(SRC_LEFT) | BIT(SRC_LSTICK_LEFT)))
				prompt_event(&v.prompt, PROMPT_LEFT, -1);
			if (p & (BIT(SRC_RIGHT) | BIT(SRC_LSTICK_RIGHT)))
				prompt_event(&v.prompt, PROMPT_RIGHT, -1);
			/* A, or 2 on a Wii Remote held sideways */
			if (p & (BIT(SRC_A) | (m.type[id] == INPUT_WIIMOTE ? BIT(SRC_2) : 0)))
				if ((chosen = prompt_event(&v.prompt, PROMPT_ACCEPT, -1)) >= 0)
					by = id;
		}
		if (chosen < 0 && m.touch && !touching) {
			touching = 1;
			touch_card = prompt_card_at(m.touch_x, m.touch_y);
			prompt_event(&v.prompt, PROMPT_TOUCH_DOWN, touch_card);
		} else if (chosen < 0 && m.touch) {
			touch_card = prompt_card_at(m.touch_x, m.touch_y);
		} else if (chosen < 0 && touching) {
			touching = 0;
			if ((chosen = prompt_event(&v.prompt, PROMPT_TOUCH_UP, touch_card)) >= 0)
				by = 0;
		}
		v.glyphs = m.last_id >= 0 ? m.type[m.last_id] : -1;
		if (chosen < 0)
			ui_show(&v);
	}
	g_paused = 0;
	report_line("PROMPT: %s chosen with controller %d (0 is the GamePad)", mode_name(chosen), by);
	if (chosen != s->mode || damaged) {
		s->mode = chosen;
		setup_start(report_sd_root(), s);
		if (setup_save_settings())
			report_line("PROMPT: settings not written");
	}
	input_claim(by);
	return chosen;
}

/* A whole screen of words: an error, with what to do and its code (docs/errors.md). */
static void show_message(const char *headline, const char *body, const char *code, int failure, const char *legend)
{
	ui_view v;
	memset(&v, 0, sizeof(v));
	v.screen = UI_MESSAGE;
	snprintf(v.headline, sizeof(v.headline), "%s", headline);
	snprintf(v.body, sizeof(v.body), "%s", body);
	snprintf(v.code, sizeof(v.code), "%s", code);
	snprintf(v.legend, sizeof(v.legend), "%s", legend);
	v.failure = failure;
	if (code[0])
		report_line("ERROR %s: %s", code, headline);
	ui_show(&v);
}

#define LEGEND_HOME " Wii U Menu"

/* ---- the running marker: a crash leaves it, so the next start can say so ---- */

static void running_mark(const char *file)
{
	char path[300];
	snprintf(path, sizeof(path), "%s/config/running", report_sd_root());
	if (!file) {
		remove(path);
		return;
	}
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", file);
		fclose(f);
	}
}

/* The game that was running when Our64 last stopped without closing, or "". */
static void running_left(char *out, unsigned size)
{
	char path[300];
	out[0] = 0;
	snprintf(path, sizeof(path), "%s/config/running", report_sd_root());
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(out, (int)size, f))
		out[strcspn(out, "\r\n")] = 0;
	fclose(f);
	remove(path);
}

/* ---- loading ---- */

#define LOADING_MIN_S 2.0 /* cat2.jpg is always seen (Zach, 2026-10-16) */
static ui_view g_load;
static OSTime g_load_t0;
static int g_home_while_loading;

static void load_poll(void);

static void loading(const char *step, float progress)
{
	g_load.screen = UI_LOADING;
	snprintf(g_load.title, sizeof(g_load.title), "%s", g_title);
	snprintf(g_load.loading.step, sizeof(g_load.loading.step), "%s", step);
	g_load.loading.progress = progress;
	g_load.legend[0] = 0; /* nothing to press while it loads (HOME still pauses once ready) */
	ui_show(&g_load);
	load_poll();
}

/* One bar for the whole load, its words saying the step (Zach, 2026-10-17):
 * the SD card once a session (sd_ready) to 0.25, reading the ROM from there
 * to 0.6 by bytes read, the autosave to 0.7, GLideN64's saved shaders the
 * rest (ui_gfx_loading_progress). At most every tenth of a second. */
static float g_read_from = 0.02f;
extern void (*romcache_progress)(unsigned done, unsigned total);
static void reading_progress(unsigned done, unsigned total)
{
	static OSTime last;
	OSTime now = OSGetSystemTime();
	if (done < total && now - last < OSMillisecondsToTicks(100))
		return;
	last = now;
	loading("Reading the game", g_read_from + (0.6f - g_read_from) * (float)done / (float)total);
}

/* The first write in a session that needs more SD space than it freed took
 * 20 s (a resume point growing from 557 to 1102 KB; the same file's later
 * growth, 0.3 s), and every SD access waited behind it, the game's included.
 * Counting the free space first did not help (24 s, then the write still took
 * 20). So the first load of a session pays it here, in the bar: a new 2 MB
 * file is written and deleted, and the space it frees serves later writes. */
static int g_sd_ready;
static void sd_ready(void)
{
	if (g_sd_ready) {
		g_read_from = 0.02f;
		return;
	}
	g_sd_ready = 1;
	loading("Preparing the SD card", 0.02f);
	char path[320];
	snprintf(path, sizeof(path), "%s/sd-ready-%08x.tmp", report_sd_root(), (unsigned)OSGetSystemTime());
	static unsigned char block[64 * 1024];
	OSTime t0 = OSGetSystemTime();
	FILE *f = fopen(path, "wb");
	int ok = f != NULL;
	for (int i = 0; ok && i < 32; i++)
		ok = fwrite(block, 1, sizeof(block), f) == sizeof(block);
	if (f)
		ok &= fclose(f) == 0;
	double ms = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3;
	remove(path);
	report_line("SD: 2 MB written and deleted in %.0f ms%s (first load this session)", ms, ok ? "" : ", NOT all written");
	g_read_from = 0.25f;
	loading("Reading the game", g_read_from);
}

/* HOME during the load is kept and pauses the game once it is ready. No B
 * back to Our ROMs: the load can only stop between its steps, and a back that
 * came seconds later read as a broken promise (Zach, 2026-10-20). */
static void load_poll(void)
{
	input_read();
	if (input_take_home() && !g_home_while_loading) {
		g_home_while_loading = 1;
		snprintf(g_load.loading.step, sizeof(g_load.loading.step), "Pausing once ready");
		ui_show(&g_load);
	}
}

/* The system asked Our64 to close (power button) while it loads. */
static int loading_cancelled(void)
{
	load_poll();
	return g_closing;
}

/* The autosave cannot be used: say why and wait for A (start fresh) or B (back). */
static int autosave_unusable(int launch, time_t when)
{
	char t[48] = "";
	if (when)
		ui_when(t, sizeof(t), time(NULL), when, g_us_console);
	const char *why = launch == LAUNCH_OTHER_VERSION ? "it was made by another Our64 version"
	                : launch == LAUNCH_OTHER_GAME ? "it belongs to another game" : "the file is damaged";
	snprintf(g_load.loading.stopped, sizeof(g_load.loading.stopped),
	         "The autosave%s%s can't be used: %s. It stays on the SD card. OUR-0301", t[0] ? " from " : "", t, why);
	ui_show(&g_load);
	input_menu m;
	input_take_home();
	for (;;) {
		player_menu_wait();
		input_read();
		input_menu_state(&m);
		if (g_closing)
			return 0;
		for (int id = 0; id < INPUT_CONTROLLERS; id++) {
			if (m.type[id] < 0)
				continue;
			const int wm = m.type[id] == INPUT_WIIMOTE;
			if (m.pressed[id] & (BIT(SRC_B) | (wm ? BIT(SRC_1) : 0)))
				return 0;
			if (m.pressed[id] & (BIT(SRC_A) | (wm ? BIT(SRC_2) : 0))) {
				g_load.loading.stopped[0] = 0;
				return 1;
			}
		}
	}
}

/* ---- one game: from its ROM to its teardown ---- */

enum { PLAY_ROMS, PLAY_EXIT, PLAY_BACK };

void player_menu_wait(void)
{
	static OSTime last;
	const OSTime frame = OSNanosecondsToTicks(16666667);
	OSTime now = OSGetSystemTime();
	if (last && now - last < frame)
		OSSleepTicks(frame - (now - last));
	last = OSGetSystemTime();
}

/* The Paks the game in play can use (gamedb.h), -1 between games. */
static int g_game_paks = -1;

static int play_game(int index)
{
	const library_game *lg = library_get(index);
	/* its file name for the end: Settings can list another ROM folder during the game (library_rescan) */
	char played[sizeof(lg->file)];
	memcpy(played, lg->file, sizeof(played));
	library_path(index, g_rom, sizeof(g_rom));
	snprintf(g_title, sizeof(g_title), "%s", lg->title);
	struct stat st;
	unsigned size = stat(g_rom, &st) ? 0 : (unsigned)st.st_size;
	memset(&g_load, 0, sizeof(g_load));
	{ /* the game's cover beside the portrait while it loads (Zach, 2026-10-20) */
		extern const roms_view *library_view(void);
		g_load.roms = library_view();
		g_load.loading.game = index;
	}
	g_load_t0 = OSGetSystemTime();
	g_home_while_loading = 0;
	g_quit = 0;
	g_exit_warned = 0;
	g_pause_wanted = 0;
	g_pause_at_vi = 0;
	loading("Reading the game", 0.02f);
	report_line("Loading %s", lg->file);
	report_save();

	/* Remembered as the last game and the newest recent one, and marked running until it ends. */
	our_settings s = *setup_settings();
	if (!g_sweep_s) { /* the sweep leaves Recent and the last game alone */
		settings_played(&s, lg->file);
		setup_start(report_sd_root(), &s);
		setup_save_settings();
	}

	static fileBrowser_file f;
	memset(&f, 0, sizeof(f));
	snprintf(f.name, sizeof(f.name), "%s", g_rom);
	f.size = size;
	format_mempacks();
	tlb_mem2_init();
	sd_ready();
	romcache_progress = reading_progress;
	OSTime read_t0 = OSGetSystemTime();
	int unread = size <= 0x1000 || rom_read(&f);
	romcache_progress = NULL;
	if (!unread) {
		double read_ms = OSTicksToMicroseconds(OSGetSystemTime() - read_t0) / 1e3;
		report_line("LOAD: ROM, %.1f MB, read in %.0f ms (%.1f MB/s), %.0f ms into the load", size / 1048576.0, read_ms,
		            read_ms > 0 ? size / 1048576.0 / (read_ms / 1e3) : 0.0,
		            OSTicksToMicroseconds(OSGetSystemTime() - g_load_t0) / 1e3);
	}
	if (unread) {
		report_line("Cannot read %s", g_rom);
		if (g_sweep_s)
			return PLAY_BACK; /* nobody to press B: the sweep notes it */
		char body[320];
		if (library_on_usb())
			snprintf(body, sizeof(body), "Our64 could not read %s. The file may be damaged, or the USB drive could not "
			         "be read: check that the USB drive is plugged in firmly, or copy the file again from a computer.",
			         lg->file);
		else
			snprintf(body, sizeof(body), "Our64 could not read %s. The file may be damaged, or the SD card could not be "
			         "read: put the card back in firmly, or copy the file again from a computer.", lg->file);
		show_message("CAN'T READ THE GAME", body, "OUR-0104", 1, " Back to Our ROMs");
		for (;;) {
			player_menu_wait();
			input_menu m;
			input_read();
			input_menu_state(&m);
			int back = g_closing;
			for (int id = 0; id < INPUT_CONTROLLERS; id++)
				back |= m.type[id] >= 0 && (m.pressed[id] & (BIT(SRC_B) | BIT(SRC_A) | BIT(SRC_1) | BIT(SRC_2)));
			if (back)
				return PLAY_BACK;
		}
	}
	init_memory();
	player_plugins_init();
	input_game_start(); /* after the plugins: their init has only port 1 present */
	running_mark(lg->file);
	/* The game's counter factor (mupen64plus's CountPerOp): the recompiler
	 * builds it into each block, so it is set before the first one. */
	int cpo = gamedb_count_per_op(ROM_HEADER.CRC1, ROM_HEADER.CRC2);
	/* Gauntlet Legends: mupen64plus.ini's factor 1 doubles the CPU's work per
	 * VI, and the Wii U fell to 44 VIs a second with choppy audio (2026-10-20). */
	if (!strncmp((const char *)ROM_HEADER.Name, "GAUNTLET LEGENDS", 16))
		cpo = 2;
	/* Star Wars Episode I Racer: with factor 2 its own pod was missing for up
	 * to 10 frames after a switch to the chase camera (a load the game spreads
	 * over frames, short of CPU time per VI); with 1 it shows at once, at 60 VIs
	 * a second (2026-10-22). */
	if (!strncmp((const char *)ROM_HEADER.Name, "STAR WARS EP1 RACER", 19))
		cpo = 1;
	/* Conker's Bad Fur Day: not in mupen64plus.ini, so 2, and in play it drew
	 * 25.4 frames a second, 70% of them held 2 VIs and 27% 3. ares, timed
	 * closer to the hardware, drew 28.1 at the same spot (88% and 11%);
	 * factor 1 drew 27.4 (86.5% and 9%), at 60 VIs a second (2026-11-02). */
	if (!strncmp((const char *)ROM_HEADER.Name, "CONKER BFD", 10))
		cpo = 1;
	/* V-Rally Edition 99: at factor 2 its menus wrote audio every other VI in
	 * places (11,000 frames a second of 21,617), and the VIs ran on at 65-77 a
	 * second with the sound breaking up (GitHub issue 17); at factor 1 those
	 * stretches lack at most 3,600 frames a second and the underruns fell from
	 * up to 15 to 0-3 in 10 s, at 60 VIs a second (2026-11-02). */
	if (!strncmp((const char *)ROM_HEADER.Name, "V-RALLY", 7))
		cpo = 1;
	/* Banjo-Tooie: at factor 2 it drew 24-28 graphics tasks a second in play,
	 * at factor 1 29-30, at 60 VIs a second, and it played clearly smoother
	 * (Zach on the console, 2026-11-02). Japan's is "BANJO KAZOOIE 2". */
	if (!strncmp((const char *)ROM_HEADER.Name, "BANJO TOOIE", 11) ||
	    !strncmp((const char *)ROM_HEADER.Name, "BANJO KAZOOIE 2", 15))
		cpo = 1;
	/* Nightmare Creatures: at factor 2 it drew 569 game frames in 3,241 VIs
	 * (about 10.5 a second) at 60 VIs a second, and it played with heavy lag;
	 * with factor 1 (the count-per-op switch) it played smoothly (Vinicius on
	 * the console, 2026-11-12). */
	if (!strncmp((const char *)ROM_HEADER.Name, "NIGHTMARE CREATURES", 19))
		cpo = 1;
	/* Castlevania and Legacy of Darkness ("CASTLEVANIA2") swap code overlays
	 * at 0x0F000000 that begin with the same instructions: the recompiler
	 * checks whole functions there (Wrappers.c), or the main thread runs an old
	 * overlay's code and dies at the start of play (GitHub issue 15). Japan's
	 * are "DRACULA MOKUSHIROKU" and "DRACULA MOKUSHIROKU2". */
	{
		extern int our64_full_code_check;
		our64_full_code_check = !strncmp((const char *)ROM_HEADER.Name, "CASTLEVANIA", 11) ||
		                        !strncmp((const char *)ROM_HEADER.Name, "DRACULA MOKUSHIROKU", 19);
	}
	if (g_count_switch)
		cpo = g_count_switch;
	count_per_op = cpo ? cpo : 2;
	if (cpo)
		report_line("TIMING: counter factor %d (%s; Not64's own is 2)", cpo, g_count_switch ? "the count-per-op switch" : "mupen64plus.ini");
	/* Perfect Dark once stopped after the Expansion Pak screen while its
	 * graphics tasks ran beside the CPU, and waited for each task until
	 * 2026-10-30: the stop was its DP unfreeze at a retrace starting the task
	 * in flight again (__wrap_doRspCycles). It now overlaps like every other
	 * game (full speed, Zach on the console, 2026-10-30); the sync-gfx switch
	 * brings the wait back. */
	g_sync_gfx = g_sync_switch;
	/* Known before the first task is decoded: a state saved with signal 0
	 * set (ZSortBOSS's "carry on") made the first task after a resume look
	 * like a yield request, and Stunt Racer waited for it forever. */
	video_set_signal_ucode(!strncmp((const char *)ROM_HEADER.Name, "Stunt Racer 64", 14) ||
	                       !strncasecmp((const char *)ROM_HEADER.Name, "World Driver Champ", 18));
	/* The same studio's audio driver plays one ring with the AI and mixes
	 * just ahead of the DMA (plugins.c ai_stream_tick). */
	/* mupen64plus.ini's DisableExtraMem: these games get 4 MB (Not64's dma.c). */
	{
		extern int our64_ram_4mb;
		static const uint32_t ram_4mb[][2] = {
			{ 0xE616B5BC, 0xC9658B88 }, { 0xD692CC5E, 0xEC58D072 }, { 0x69458B9E, 0xFC95F936 }, /* Iggy's Reckin' Balls U, E, J */
		};
		our64_ram_4mb = 0;
		for (unsigned i = 0; i < sizeof(ram_4mb) / sizeof(ram_4mb[0]); i++)
			our64_ram_4mb |= ROM_HEADER.CRC1 == ram_4mb[i][0] && ROM_HEADER.CRC2 == ram_4mb[i][1];
		if (our64_ram_4mb)
			report_line("MEMORY: 4 MB, no Expansion Pak (mupen64plus.ini DisableExtraMem)");
	}
	g_ai_stream = !strncmp((const char *)ROM_HEADER.Name, "Stunt Racer 64", 14) ||
	              !strncasecmp((const char *)ROM_HEADER.Name, "World Driver Champ", 18);
	/* Gauntlet Legends runs its code at TLB addresses: 41-56 VIs a second
	 * with choppy audio without links there, a steady 60 with them, resumes
	 * included (2026-10-20). Conker crashed on resume with them, so only
	 * Gauntlet has them. */
	{
		extern int our64_tlb_links; /* Not64's Recomp-Cache-Heap.c */
		/* Factor 5's Indiana Jones and Battle for Naboo run their code from TLB
		 * addresses too: unlinked, Indy ran at 34 VI/s, linked at 60 (Zach,
		 * 2026-10-26). Perfect Dark: 46-52 VI/s unlinked (a third of the CPU
		 * core's emulation in the dispatcher), 59-60 linked, resume included
		 * (2026-10-27). Japan's Perfect Dark names itself in capitals.
		 * Conker and GoldenEye: unlinked, the dispatcher took about a third
		 * of Conker's busy CPU core and 45% of GoldenEye's (7% linked); linked,
		 * Conker played 15 minutes over three runs with a resume, no missed
		 * timer and its best frame test (2026-11-06). Conker's crash with
		 * links on 2026-10-20 was GLideN64's mip tile bug (be7206d). */
		our64_tlb_links = g_tlb_links_switch || !strncmp((const char *)ROM_HEADER.Name, "GAUNTLET LEGENDS", 16) ||
		                  !strncmp((const char *)ROM_HEADER.Name, "CONKER BFD", 10) ||
		                  !strncmp((const char *)ROM_HEADER.Name, "GOLDENEYE", 9) ||
		                  !strncmp((const char *)ROM_HEADER.Name, "Indiana Jones", 13) ||
		                  !strncmp((const char *)ROM_HEADER.Name, "Battle for Naboo", 16) ||
		                  !strncasecmp((const char *)ROM_HEADER.Name, "Perfect Dark", 12);
		if (our64_tlb_links)
			report_line("CPU: code at TLB addresses is linked; a TLB write cuts the links into its pages (tlb-links)");
	}
	int paks[4];
	g_game_paks = gamedb_paks(ROM_HEADER.CRC1, ROM_HEADER.CRC2);
	for (int p = 0; p < 4; p++)
		paks[p] = gamedb_pak_for(g_game_paks);
	player_set_paks(paks);
	if ((g_game_paks & GAMEDB_RUMBLE) && !strncmp((const char *)ROM_HEADER.Name, "Chameleon Twist ", 16))
		player_pak_rumble_only(); /* not Chameleon Twist 2 */
	/* A Transfer Pak game: a Game Boy game's save from RetroArch, a Virtual
	 * Console inject or OurGB, when there is one, goes in port 1's Transfer
	 * Pak (tpak_wiiu.c). */
	if ((g_game_paks & GAMEDB_TRANSFER) && !g_sweep_s) { /* the sweep never touches the Game Boy saves */
		loading("Finding the Game Boy game", 0.6f);
		tpak_wiiu_start(report_sd_root(), ROM_HEADER.Name, ROM_HEADER.CRC1);
	}
	int cpu_up = 0, video_up = 0, launch = LAUNCH_FIRST;
	time_t when = 0;
	/* The sweep starts each game fresh with saves of its own (sd:/wiiu/our64/sweep-data): the
	 * player's saves and autosaves are never read or written by it. */
	char sweep_root[256];
	snprintf(sweep_root, sizeof(sweep_root), "%s/sweep-data", report_sd_root());
	if (g_sweep_s)
		mkdir(sweep_root, 0777);
	if (g_writer_ok && !saves_start(g_sweep_s ? sweep_root : report_sd_root()) && !g_interpreter && !g_sweep_s) {
		states_start(report_sd_root(), 0); /* the interpreter test mode never loads one either */
		launch = states_launch(&when);
	}
	{
		char id[64];
		saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
		input_start(report_sd_root(), id);
	}
	int result = PLAY_BACK;
	if (loading_cancelled())
		goto teardown;
	if (launch == LAUNCH_OTHER_VERSION || launch == LAUNCH_OTHER_GAME || launch == LAUNCH_DAMAGED) {
		if (!autosave_unusable(launch, when))
			goto teardown;
		states_set_aside(); /* kept, renamed, so the fresh game's autosaves cannot replace it */
	}
	if (launch == LAUNCH_RESUME)
		loading("Loading the autosave", 0.6f);
	cpu_init();
	cpu_up = 1;
	if (loading_cancelled())
		goto teardown;
	loading("Preparing graphics", 0.7f);
	video_set_picture(s.picture == PICTURE_WIDE);
	video_set_filter(s.filter == FILTER_N64);
	video_set_crt(s.look, s.pad_look);
	if (video_start()) {
		report_line("Graphics did not start.");
		goto teardown;
	}
	video_up = 1;
	video_stats(&g_fc_game); /* the frame check counts this game from here */
	g_fc_window = g_fc_game;
	g_fc_game_max_us = 0;
	loading("Ready", 1.0f);
	while (OSTicksToMicroseconds(OSGetSystemTime() - g_load_t0) < LOADING_MIN_S * 1e6) {
		if (loading_cancelled())
			goto teardown;
		player_menu_wait();
	}
	if (loading_cancelled())
		goto teardown;
	report_line("Playing %s (%s, %s)%s%s", lg->file, ROM_HEADER.Name, is_pal() ? "PAL" : "NTSC",
	            g_interpreter ? " on the pure interpreter" : "",
	            g_sync_gfx ? ", the CPU waiting for each graphics task" : "");
	report_line("LOAD: %.0f ms from the choice to play (the bar stays up at least %.0f s)",
	            OSTicksToMicroseconds(OSGetSystemTime() - g_load_t0) / 1e3, LOADING_MIN_S);
	report_save();

	g_vi_period = OSSecondsToTicks(1) / (is_pal() ? 50 : 60);
	g_next_vi = OSGetSystemTime();
	{ /* the TV clock: NTSC games on a 59.94 Hz TV (a PAL game's 50 VIs, or a 50 Hz TV, cannot follow it) */
		char path[256];
		struct stat st;
		snprintf(path, sizeof(path), "%s/old-pacing", report_sd_root() ? report_sd_root() : "");
		const int old = report_sd_root() && !stat(path, &st);
		/* Racer kept the old pacing (5ef8a2e) while the CPU core waited for the
		 * TV: 55-58 VI/s and choppy sound. VIs are now dropped instead
		 * (video_update_screen), so it is on the TV clock again, to be checked. */
		/* PAL games too (2026-11-06):
		 * on a 59.94 Hz TV every fifth VI stays two vsyncs (5 VIs in 6 vsyncs,
		 * 49.95 VI/s, 0.06% fast against a PAL N64's 49.92); on a 50 Hz TV
		 * (576i) one VI a vsync. An NTSC game on a 50 Hz TV keeps the old pacing. */
		const int tv60 = video_tv_is_60hz();
		g_tv_clock = !old && (is_pal() || tv60);
		g_tv_cadence = g_tv_clock && is_pal() && tv60;
		video_set_pal_cadence(g_tv_cadence);
		ft_begin();
		if (g_tv_clock) {
			g_vi_period = OSNanosecondsToTicks(video_tv_period_ns()); /* the TV's 59.94 or 50 Hz */
			uint32_t c;
			OSTime t;
			g_tv_vi_k = video_tv_vsync(&c, &t) ? c : 0;
			report_line("VIDEO: the TV's vsync is the VI clock, audio follows%s",
			            g_tv_cadence ? " (a PAL game on a 59.94 Hz TV: every fifth VI two vsyncs)" : is_pal() ? " (a PAL game on a 50 Hz TV)" : "");
		} else
			report_line("VIDEO: the old pacing (%s)", old ? "old-pacing" : "an NTSC game on a 50 Hz TV");
		video_set_tv_clock(g_tv_clock);
		video_set_frame_check(!is_pal() || g_tv_clock);
		audio_set_unpaced(g_tv_clock);
	}
	g_stats_from_us = report_now_us();
	g_sweep_t0_us = g_stats_from_us;
	g_sweep_low = 1e9;
	g_sweep_vis0 = g_bench.vis;
	g_sweep_tasks0 = gfx_tasks_posted();
	g_sweep_under0 = audio_underruns();
	g_sweep_yields0 = gfx_yields();
	{
		extern unsigned our64_gx2_links;
		g_sweep_links0 = our64_gx2_links;
	}
	dynacore = g_interpreter ? 2 : 1;
	stop = 0;
	/* From its autosave, a game opens paused, Resume selected; HOME during loading pauses it too. */
	if (launch == LAUNCH_RESUME || g_home_while_loading)
		g_pause_at_vi = g_bench.vis + 4;
	input_withhold();
	ui_resume();
	g_in_game = 1;
	g_running = 1;
	input_set_notices(1);
	cpuprof_start(report_sd_root(), &g_paused);
	if (g_capture_at_start) /* the first graphics tasks, e.g. a game that goes black at once */
		player_capture_start();
	/* go() stops for a load or a reset chosen in the pause menu, and runs again. */
	for (;;) {
		go();
		int action = states_take_action();
		if (action == ACTION_NONE || g_quit)
			break;
		OSTime t0 = OSGetSystemTime();
		gfx_reset();
		input_rumble_reset();
		if (action == ACTION_LOAD) {
			states_load_now();
			our64_dynarec_unlink();
		} else {
			player_reset_machine();
			states_after_reset();
			if (g_capture_at_start) { /* a fresh one: the start's may never have filled */
				if (g_capture_left)
					report_line("Capture stopped at the reset, %d graphics tasks short", g_capture_left);
				g_capture_left = 0;
				player_capture_start();
			}
		}
		report_line("PAUSE: %s done in %.0f ms; the game runs on", action == ACTION_LOAD ? "load" : "reset",
		            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
		g_paused = 0;
		g_next_vi = OSGetSystemTime();
		ui_resume();
	}
	g_running = 0;
	input_set_notices(0);
	result = g_quit == QUIT_ROMS ? PLAY_ROMS : PLAY_EXIT;
	report_line("Stopped after %u VIs (%s).", g_bench.vis, result == PLAY_ROMS ? "to Our ROMs" : "leaving");
	{ /* the one line a public build keeps per game: which ROM and how it ran */
		char low[48] = "", id[64];
		if (g_sweep_low < 1e9)
			snprintf(low, sizeof(low), ", slowest 10 s %.1f VI/s of %d", g_sweep_low, is_pal() ? 50 : 60);
		saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
		report_line("GAME: %s (%s): %.0f min%s, %u audio underruns", played, id,
		            (report_now_us() - g_sweep_t0_us) / 60e6, low, audio_underruns() - g_sweep_under0);
	}
	{
		video_stats_t v;
		video_stats(&v);
		if (v.latency_max_us > g_fc_game_max_us)
			g_fc_game_max_us = v.latency_max_us;
		char fc[600];
		frame_check_line("GAME: ", &g_fc_game, &v, g_fc_game_max_us, fc, sizeof(fc));
		snprintf(g_game_fc, sizeof(g_game_fc), "%s; %s", g_tv_clock ? "TV clock" : "old pacing", fc + 6);
		/* every game's line kept in sd:/wiiu/our64/frame-check.txt (the log keeps two launches) */
		char path[256], id[64];
		saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
		snprintf(path, sizeof(path), "%s/frame-check.txt", report_sd_root() ? report_sd_root() : "");
		FILE *f = report_sd_root() ? fopen(path, "a") : NULL;
		if (f) {
			fprintf(f, "%s%s, %.1f min: %s\n", id, g_tv_clock ? " (tv-clock)" : "",
			        (report_now_us() - g_sweep_t0_us) / 60e6, fc + 6);
			fclose(f);
		}
	}
	timeline_write();
	audio_dump_write();
	pace_write();
teardown:
	if (g_capture_left) { /* a capture belongs to one game: the next one's tasks would join its files */
		report_line("Capture stopped with the game, %d graphics tasks short", g_capture_left);
		g_capture_left = 0;
	}
	g_game_paks = -1;
	tpak_wiiu_stop(); /* a Game Boy save the game changed goes back where it came from */
	input_rumble_reset();
	if (video_up) {
		gfx_reset(); /* nothing in flight carries over to the next game */
		video_stop();
	}
	saves_stop();
	states_stop();
	{ /* the run's CPU profile, named for the game and the VI clock */
		char id[64], name[80];
		saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
		snprintf(name, sizeof(name), "%s%s", id, g_tv_clock ? "-tv" : "");
		cpuprof_save(name);
	}
	if (cpu_up)
		cpu_deinit();
	free_memory();
	g_in_game = 0;
	input_start(report_sd_root(), NULL); /* Our ROMs edits the all-games buttons, never the last game's profile */
	running_mark(NULL);
	report_save();
	return result == PLAY_BACK && g_closing ? PLAY_EXIT : result;
}

/* ---- the shader builder (sd:/wiiu/our64/build-shaders) ----
 *
 * For the public download's pre-built shaders (docs/plans/2026-10-27-
 * prebuilt-shaders.md). Every ROM is opened the way play_game opens it, up to
 * GLideN64's RomOpen, whose preload makes the game's combiners from its keys
 * (shaders/games/<game>.keys) in the game's own setup: its header, GLideN64's
 * per-game hacks and ini entries, Our64's per-game shader choices. The keys
 * missing from the shader cache compile on GLideN64's worker; the game closes
 * once they are done. A second open must then find every key in the cache:
 * it writes the game's complete pack, and its "not in the cache" count is the
 * check (0). No game runs. Progress survives a crash the way tlb-count's does:
 * shader-build-done lists the ROMs done, shader-build-current the one open. */

/* GLideN64's last preload (gx2_ContextImpl.cpp Our64Preload), and its worker's queue (gx2_GL.cpp). */
typedef struct { unsigned keys, loaded, from_pack, from_files, missing, ms; } shader_preload;
extern shader_preload our64_gx2_preload;
extern unsigned our64_gx2_jobs_pending(void);
extern unsigned our64_gx2_links;

static void build_note(const char *file, const char *mode, const char *line)
{
	char p[256];
	snprintf(p, sizeof(p), "%s/%s", report_sd_root(), file);
	FILE *f = fopen(p, mode);
	if (f) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
}

static int build_listed(const char *file, const char *name)
{
	char p[256], line[160];
	snprintf(p, sizeof(p), "%s/%s", report_sd_root(), file);
	FILE *f = fopen(p, "r");
	if (!f)
		return 0;
	int found = 0;
	while (!found && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		found = !strcmp(line, name);
	}
	fclose(f);
	return found;
}

/* One ROM: read once, opened twice. 0 when it opened. */
static int build_rom_shaders(int index, char *line, unsigned size_line)
{
	const library_game *lg = library_get(index);
	library_path(index, g_rom, sizeof(g_rom));
	struct stat st;
	unsigned size = stat(g_rom, &st) ? 0 : (unsigned)st.st_size;
	static fileBrowser_file f;
	memset(&f, 0, sizeof(f));
	snprintf(f.name, sizeof(f.name), "%s", g_rom);
	f.size = size;
	format_mempacks();
	tlb_mem2_init();
	if (size <= 0x1000 || rom_read(&f)) {
		snprintf(line, size_line, "%s: could not read the ROM", lg->file);
		return -1;
	}
	init_memory();
	player_plugins_init();
	char name[24];
	snprintf(name, sizeof(name), "%.20s", (const char *)ROM_HEADER.Name);
	for (int k = (int)strlen(name) - 1; k >= 0 && name[k] == ' '; k--)
		name[k] = 0;
	const unsigned links0 = our64_gx2_links;
	const OSTime t0 = OSGetSystemTime();
	if (video_start()) {
		snprintf(line, size_line, "%s (%s): graphics did not start", lg->file, name);
		free_memory();
		return -1;
	}
	const shader_preload first = our64_gx2_preload;
	while (our64_gx2_jobs_pending() && !g_closing)
		OSSleepTicks(OSMillisecondsToTicks(50));
	const unsigned compiled = our64_gx2_links - links0;
	const double first_s = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e6;
	gfx_reset();
	video_stop();
	shader_preload second = first;
	if (first.keys && !g_closing) {
		if (!video_start()) {
			second = our64_gx2_preload;
			gfx_reset();
			video_stop();
		} else {
			second.missing = first.keys;
		}
	}
	free_memory();
	if (!first.keys)
		snprintf(line, size_line, "%s (%s): no keys", lg->file, name);
	else
		snprintf(line, size_line, "%s (%s): %u keys; first open %.1f s, %u compiled; second open %u ms: %u from the pack, "
		         "%u from single files, %u not in the cache%s", lg->file, name, first.keys, first_s, compiled, second.ms,
		         second.from_pack, second.from_files, second.missing, second.missing ? "  CHECK" : "");
	return 0;
}

static void build_shaders(void)
{
	const int n = library_count();
	char path[256];
	struct stat st;
	snprintf(path, sizeof(path), "%s/shader-build-done", report_sd_root());
	const int resume = !stat(path, &st);
	if (!resume)
		build_note("shader-build-results.txt", "w", "Our64 shader build: each game opened twice; the second open must find every key in the cache.");
	/* A ROM that was open when Our64 stopped (a crash) is skipped. */
	{
		char p[256], cur[160] = "";
		snprintf(p, sizeof(p), "%s/shader-build-current", report_sd_root());
		FILE *f = fopen(p, "r");
		if (f) {
			if (fgets(cur, sizeof(cur), f))
				cur[strcspn(cur, "\r\n")] = 0;
			fclose(f);
		}
		if (cur[0] && !build_listed("shader-build-done", cur)) {
			char line[256];
			snprintf(line, sizeof(line), "%s: Our64 stopped while it was open, skipped", cur);
			build_note("shader-build-results.txt", "a", line);
			build_note("shader-build-done", "a", cur);
			report_line("SHADERS: %s", line);
		}
	}
	report_line("SHADERS: building every game's shaders (build-shaders)%s", resume ? ", going on" : "");
	player_paused(1); /* no VIs here: not a hang */
	unsigned done = 0, checks = 0;
	for (int i = 0; i < n && !g_closing; i++) {
		const library_game *lg = library_get(i);
		if (build_listed("shader-build-done", lg->file))
			continue;
		input_read();
		if (input_take_home())
			break;
		char body[200];
		snprintf(body, sizeof(body), "%d of %d: %s. HOME stops; starting Our64 again goes on.", i + 1, n, lg->title);
		show_message("BUILDING SHADERS", body, "", 0, LEGEND_HOME);
		build_note("shader-build-current", "w", lg->file);
		char line[320];
		build_rom_shaders(i, line, sizeof(line));
		build_note("shader-build-results.txt", "a", line);
		build_note("shader-build-done", "a", lg->file);
		build_note("shader-build-current", "w", "");
		report_line("SHADERS: %s", line);
		report_save();
		done++;
		checks += strstr(line, "  CHECK") != NULL;
	}
	int finished = 1;
	for (int i = 0; i < n && finished; i++)
		finished = build_listed("shader-build-done", library_get(i)->file);
	if (finished)
		build_note("shader-build-results.txt", "a", "Done. Press HOME");
	player_paused(0);
	char body[200];
	snprintf(body, sizeof(body), finished ? "Every game's shaders are built (%u this time, %u to check). Remove "
	         "build-shaders from the SD card to play." : "Stopped after %u games (%u to check). Start Our64 again to go on.",
	         done, checks);
	show_message(finished ? "SHADERS BUILT" : "SHADER BUILD STOPPED", body, "", 0, LEGEND_HOME);
	while (!g_closing) {
		player_menu_wait();
		input_read();
		if (input_take_home())
			break;
	}
}

/* ---- the sweep (sd:/wiiu/our64/sweep) ----
 *
 * Every game in Our ROMs plays for g_sweep_s seconds (the file's number, 30
 * when it is empty) with nobody at the controls, each from a fresh start with
 * saves of its own (sweep-data), and sweep-results.txt gets a line per game:
 * speed, its lowest 10 s, graphics tasks, underruns, yields, shaders compiled,
 * and CHECK where something needs a look. Progress survives a crash or a hang
 * the way the shader builder's does: sweep-done lists the games done,
 * sweep-current the one playing; starting Our64 again notes that game as
 * stopped and goes on. The console's auto power-down is off meanwhile. */
static void sweep(void)
{
	g_game_fc[0] = 0;
	char path[256];
	struct stat st;
	snprintf(path, sizeof(path), "%s/sweep-done", report_sd_root());
	const int resume = !stat(path, &st);
	if (!resume) {
		char head[160];
		snprintf(head, sizeof(head), "Our64 sweep: each game %d s from a fresh start, nobody at the controls.", g_sweep_s);
		build_note("sweep-results.txt", "w", head);
	}
	{ /* a game that was playing when Our64 stopped: a crash, a hang or a power-off */
		char p[256], cur[160] = "";
		snprintf(p, sizeof(p), "%s/sweep-current", report_sd_root());
		FILE *f = fopen(p, "r");
		if (f) {
			if (fgets(cur, sizeof(cur), f))
				cur[strcspn(cur, "\r\n")] = 0;
			fclose(f);
		}
		if (cur[0] && !build_listed("sweep-done", cur)) {
			char line[256];
			snprintf(line, sizeof(line), "%s: Our64 stopped while it played (crash, hang or power-off)  CHECK", cur);
			build_note("sweep-results.txt", "a", line);
			build_note("sweep-done", "a", cur);
			report_line("SWEEP: %s", line);
		}
	}
	report_line("SWEEP: every game for %d s%s", g_sweep_s, resume ? ", going on" : "");
	IMDisableAPD();
	unsigned done = 0, checks = 0;
	/* the count each time: Settings can list another ROM folder during a game (library_rescan) */
	for (int i = 0; i < library_count() && !g_closing; i++) {
		const library_game *lg = library_get(i);
		if (!lg || build_listed("sweep-done", lg->file))
			continue;
		char file[sizeof(lg->file)]; /* lg can be another game's after play_game */
		memcpy(file, lg->file, sizeof(file));
		input_read();
		if (input_take_home())
			break;
		build_note("sweep-current", "w", file);
		report_restart(); /* each game's lines reach the card: the 256 KB buffer held only 24 games */
		g_sweep_t0_us = 0;
		g_game_fc[0] = 0; /* before the game: play_game fills it (cleared after, every line lost it) */
		const int r = play_game(i);
		char line[1100];
		if (!g_sweep_t0_us) {
			snprintf(line, sizeof(line), "%s: did not start (could not read it, or graphics did not start)  CHECK", file);
		} else {
			extern unsigned our64_gx2_links;
			const double s = (report_now_us() - g_sweep_t0_us) / 1e6;
			const double vis = (g_bench.vis - g_sweep_vis0) / s, tasks = (gfx_tasks_posted() - g_sweep_tasks0) / s;
			const unsigned under = audio_underruns() - g_sweep_under0, yields = gfx_yields() - g_sweep_yields0;
			const double full = is_pal() ? 50 : 60;
			/* The slowest 10 s, not the average: a first-time shader build pauses the game about 3 s. One or
			 * two underruns at boot are normal (90 of 217 games had one). */
			const int check = (g_sweep_low < 1e9 ? g_sweep_low < full - 5 || g_sweep_low > full + 3 : vis < full - 2) ||
			                  tasks < 1 || under > 2;
			char low[32] = "";
			if (g_sweep_low < 1e9)
				snprintf(low, sizeof(low), " (lowest 10 s %.1f)", g_sweep_low);
			snprintf(line, sizeof(line), "%s (%.20s): %.0f s, %.1f VI/s%s, %.1f graphics tasks/s, %u underruns, %u yields, "
			         "%u shaders compiled%s", file, (const char *)ROM_HEADER.Name, s, vis, low, tasks, under, yields,
			         our64_gx2_links - g_sweep_links0, check ? "  CHECK" : "");
			if (g_game_fc[0]) /* the pacing and frame check: games that do worse on the TV clock than the old pacing */
				snprintf(line + strlen(line), sizeof(line) - strlen(line), " | %s", g_game_fc);
			checks += check;
		}
		build_note("sweep-results.txt", "a", line);
		build_note("sweep-done", "a", file);
		build_note("sweep-current", "w", "");
		report_line("SWEEP: %s", line);
		report_save();
		done++;
		if (r == PLAY_EXIT)
			break; /* HOME or the power button while it played */
		g_quit = 0;
	}
	IMEnableAPD();
	int finished = 1;
	for (int i = 0; i < library_count() && finished; i++)
		finished = build_listed("sweep-done", library_get(i)->file);
	if (finished)
		build_note("sweep-results.txt", "a", "Done.");
	char body[200];
	snprintf(body, sizeof(body), finished ? "Every game has played (%u this time, %u to check). Remove sweep from the "
	         "SD card to play." : "Stopped after %u games (%u to check). Start Our64 again to go on.", done, checks);
	show_message(finished ? "SWEEP DONE" : "SWEEP STOPPED", body, "", 0, LEGEND_HOME);
	while (!g_closing) {
		player_menu_wait();
		input_read();
		if (input_take_home())
			break;
	}
}

/* ---- emulation thread on the codegen core ---- */

static int emu_thread(int argc, const char **argv)
{
	(void)argc; (void)argv;
	/* Generated code reads core state through r13; nothing else in this app uses it. */
	__asm__ volatile("mr 13,%0" : : "r"(our64_sda_base));

	const char *root = report_sd_root();
	g_writer_ok = !writer_start();
	if (!g_writer_ok)
		report_line("SAVE FAILED: no SD writer thread, so saves and autosaves are off this session.");
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/config", root);
	mkdir(dir, 0777);
	snprintf(dir, sizeof(dir), "%s/config/settings.cfg", root);
	our_settings s;
	int damaged = settings_read(dir, &s) == SETTINGS_DAMAGED;
	theme_use(s.theme); /* before the first screen */
	theme_use_layout(s.layout);
	setup_start(root, &s);
	input_start(root, NULL);
	if (!strncmp(s.rom_folder, "usb:", 4))
		usbfs_start(); /* the ROM folder is on a USB drive */
	library_scan(root);
	int game = library_index_of(s.last_game);
	if (game >= 0)
		library_refresh(game);
	char crashed[160];
	running_left(crashed, sizeof(crashed));
	if (g_build_shaders) {
		build_shaders();
		g_quit = g_closing ? QUIT_SYSTEM : QUIT_HOME;
		writer_stop();
		return 0;
	}

	/* The line under the prompt's cards: only a crash or an empty card (the legend already says what A does). */
	char line[256] = "", notice[160] = "";
	if (crashed[0]) {
		snprintf(line, sizeof(line), "Our64 stopped unexpectedly last time. Your autosave is safe.");
		report_line("CRASH: Our64 stopped while %s was running", crashed);
	} else if (library_count() == 0) { /* the folder not found: Our ROMs' headline, in a sentence */
		snprintf(line, sizeof(line), "A: Our ROMs. %s", library_missing() ? "No ROMs found." : "No games yet.");
	}
	if (s.last_game[0] && game < 0) {
		char title[64];
		ui_game_title(title, sizeof(title), s.last_game);
		snprintf(notice, sizeof(notice), "%.60s is no longer in the ROM folder. Its saves are kept: copy it back to play on.",
		         title);
	}
	/* Test switch sd:/wiiu/our64/autostart: a ROM file name in Our ROMs.
	 * That game plays at once, without the prompt or Our ROMs, and Our64
	 * closes after it (Cemu runs with nobody at the controls). */
	int autostart = -1;
	{
		char path[256], file[128] = "";
		snprintf(path, sizeof(path), "%s/autostart", root);
		FILE *f = fopen(path, "r");
		if (f) {
			if (fgets(file, sizeof(file), f))
				file[strcspn(file, "\r\n")] = 0;
			fclose(f);
			autostart = library_index_of(file);
			report_line("AUTOSTART: %s%s", file, autostart < 0 ? " is not in Our ROMs" : "");
		}
	}
	int mode = autostart >= 0 || g_sweep_s ? (s.mode >= 0 ? s.mode : 0) : run_prompt(&s, damaged, line);
	if (mode < 0) {
		report_line("PROMPT: left without playing");
		g_quit = g_closing ? QUIT_SYSTEM : QUIT_HOME;
		writer_stop();
		return 0;
	}
	ui_set_mode(mode);
	input_set_joining(1);
	if (autostart >= 0) {
		play_game(autostart);
		writer_stop();
		return 0;
	}
	if (g_sweep_s) {
		sweep();
		g_quit = g_closing ? QUIT_SYSTEM : QUIT_HOME;
		writer_stop();
		return 0;
	}
	if (game < 0)
		game = 0;
	for (;;) {
		int r = library_run(&game, notice);
		notice[0] = 0;
		if (r != LIB_PLAY) {
			g_quit = r == LIB_SYSTEM ? QUIT_SYSTEM : QUIT_HOME;
			break;
		}
		char file[sizeof(library_get(game)->file)];
		snprintf(file, sizeof(file), "%s", library_get(game)->file);
		r = play_game(game);
		game = library_index_of(file); /* Settings may have listed another ROM folder meanwhile: -1 when it lacks the game */
		if (game >= 0)
			library_refresh(game);
		if (r == PLAY_EXIT)
			break;
		g_quit = 0; /* back on Our ROMs: main reads system messages again (power press) */
	}
	writer_stop();
	return 0;
}

static OSThread g_thread __attribute__((aligned(16)));
static uint8_t g_stack[8 * 1024 * 1024] __attribute__((aligned(16)));
static int g_playing; /* g_thread was started */

/* The system calls this before it closes Our64 after SYSLaunchMenu and
 * closes nothing until it returns. A power press does not come here: it
 * arrives as a foreground release during play (main). Either way, a running
 * game stops the way HOME stops it: last resume point, in-game saves, all
 * on the SD before Our64 lets go. */
static uint32_t save_request(void *arg)
{
	(void)arg;
	OSTime t0 = OSGetSystemTime();
	int was_playing = g_playing && !OSIsThreadTerminated(&g_thread);
	if (was_playing) {
		g_closing = 1;
		while (!OSIsThreadTerminated(&g_thread) && OSTicksToMilliseconds(OSGetSystemTime() - t0) < 10000)
			OSSleepTicks(OSMillisecondsToTicks(5));
	}
	report_line("SYSTEM: asked to close%s; ready after %.0f ms%s", was_playing ? " during play" : "",
	            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3,
	            g_playing && !OSIsThreadTerminated(&g_thread) ? " (the game did not stop in 10 s)" : "");
	report_save();
	OSSavesDone_ReadyToRelease();
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	/* HOME is Our64's own button (it leaves through a last resume point), so
	 * the system HOME menu stays off; this must come before ProcUI starts. */
	OSEnableHomeButtonMenu(FALSE);
	WHBProcInit();
	ProcUISetSaveCallback(save_request, NULL);
	WHBLogCafeInit();
	WHBLogUdpInit();
	int sound = !audio_start(); /* also ends the launch jingle */
	input_init();
	g_us_console = us_console();

	report_line("Our64 " OUR64_VERSION " (" __DATE__ " " __TIME__ ")");
	if (!sound)
		report_line("No sound: AX did not start.");
	const char *root = report_sd_root();
#ifdef OUR64_RELEASE
	if (root) /* the earlier starts stay above this one's lines, about the last 10-20 games */
		report_continue(48 * 1024);
#else
	if (root) { /* the previous run's log stays as player-log-prev.txt */
		char now[256], prev[256];
		snprintf(now, sizeof(now), "%s/player-log.txt", root);
		snprintf(prev, sizeof(prev), "%s/player-log-prev.txt", root);
		remove(prev);
		rename(now, prev);
	}
#endif
	crash_handlers_install();
	uint32_t core = OSGetCodegenCore();
	/* Cemu gives code generation core 0: graphics then go to core 2, so Our64's screens can be checked there. */
	unsigned gfx_core = core == GRAPHICS_CORE ? 2 : GRAPHICS_CORE;
	int gfx = !video_thread_start(gfx_core);
	if (!gfx) {
		report_line("Graphics did not start (GX2 on core %u, codegen core %u).", gfx_core, core);
	} else if (our64_jit_init()) {
		report_line("No JIT area: launch from the Wii U Menu under Aroma.");
		show_message("START OUR64 FROM THE WII U MENU", "Our64 needs Aroma to run its games. Start it from its "
		             "icon on the Wii U Menu with Aroma running, not from another launcher.", "OUR-0103", 1, LEGEND_HOME);
	} else if (sda_setup()) {
		report_line("Core variables too far apart for r13 addressing: build problem.");
		show_message("THIS BUILD IS BROKEN", "This copy of Our64 was built wrong and cannot run games. "
		             "Install a release build.", "OUR-0901", 1, LEGEND_HOME);
	} else if (!root) {
		report_line("No SD card.");
		show_message("NO SD CARD", "Our64 keeps its games and saves on the SD card. Put the card back in, "
		             "then start Our64 again.", "OUR-0102", 1, LEGEND_HOME);
	} else if ((read_test_switches(root), 1) && OSCreateThread(&g_thread, (OSThreadEntryPointFn)emu_thread, 0, NULL, g_stack + sizeof(g_stack), sizeof(g_stack), 16,
	                          (OSThreadAttributes)(1u << core))) {
		g_playing = 1;
		OSSetThreadName(&g_thread, "Our64 CPU");
		OSResumeThread(&g_thread);
		watchdog_start();
		/* System messages during play. A power press arrives as a foreground
		 * release: the game stops first, and the loop below lets go of the
		 * screen (WHBProcIsRunning) only after that. The system waits. */
		OSTime asked = 0;
		while (!OSIsThreadTerminated(&g_thread)) {
			if (!g_quit && !g_closing) {
				ProcUIStatus st = ProcUIProcessMessages(TRUE);
				if (st == PROCUI_STATUS_RELEASE_FOREGROUND || st == PROCUI_STATUS_EXITING) {
					report_line("SYSTEM: told to %s during play (power button?)", st == PROCUI_STATUS_EXITING ? "exit" : "give up the screen");
					asked = OSGetSystemTime();
					g_asked = st;
					g_closing = 1;
				}
			}
			OSSleepTicks(OSMillisecondsToTicks(20));
		}
		int ret;
		OSJoinThread(&g_thread, &ret);
		if (asked)
			report_line("SYSTEM: game stopped and saved %.0f ms after the system asked", OSTicksToMicroseconds(OSGetSystemTime() - asked) / 1e3);
		watchdog_stop();
	}
	usbfs_stop(); /* every thread that read a USB file is done */
	report_line("SYSTEM: USB files closed");
	audio_stop();
	report_line("SYSTEM: sound stopped");
	report_save();

	/* Back to the Wii U Menu on HOME. Dark red: stopped by an error, see
	 * sd:/wiiu/our64/player-log.txt; the system takes the screen back
	 * between these frames. They are drawn on the graphics core, which owns
	 * GX2; WHBGfx's foreground callbacks run there too. */
	/* A write that failed this session: HOME waits on a screen saying so
	 * (a power press cannot wait; its log has it). */
	pause_autosave safe;
	states_autosave(&safe);
	int told = g_quit != QUIT_HOME || g_exit_warned || !(safe.autosave_failed || safe.ingame_failed);
	if (!told) {
		report_line("Closing after %d failed SD writes: telling the player first", g_write_failures);
		show_message("COULDN'T SAVE", "Our64 could not write to the SD card: it may be full, locked or removed. "
		             "Progress since the last good save is not on the card. Free up space on a computer, "
		             "then play again.", "OUR-0201", 1, "\uE044 Close anyway");
		input_take_home();
	}
	/* The system's request, read in the play loop, answered now that the game
	 * is saved. Unanswered, WHBProcIsRunning waited for a message the system
	 * never sent (it waits for this answer first), and a power press left the
	 * console on the last screen (2026-11-11). */
	if (g_asked == PROCUI_STATUS_RELEASE_FOREGROUND) {
		report_line("SYSTEM: letting go of the screen");
		report_save();
		ProcUIDrawDoneRelease();
	} else if (g_asked == PROCUI_STATUS_EXITING) {
		report_line("SYSTEM: exiting as asked");
		report_save();
		WHBProcStopRunning();
	}
	int leaving = 0;
	while (WHBProcIsRunning()) {
		if (!told) {
			input_read();
			told = input_take_home();
		}
		if (told && !leaving && g_quit == QUIT_HOME) {
			leaving = 1;
			ui_restore_screens();
			SYSLaunchMenu();
		}
		if (!g_quit) {
			pad_read();
			if (g_quit) continue;
		}
		if (gfx && ProcUIInForeground())
			ui_redraw();
		OSSleepTicks(OSMillisecondsToTicks(16)); /* not player_menu_wait: that paces the CPU core's menus */
	}
	report_line("SYSTEM: closing");
	ui_restore_screens();
	video_thread_stop();
	report_line("SYSTEM: graphics stopped");
	report_save();
	WHBLogUdpDeinit();
	WHBLogCafeDeinit();
	WHBProcShutdown();
	return 0;
}

/* ---- failure reporting: report and stop here ---- */

static void __attribute__((noreturn)) halt(void)
{
	report_save();
	for (;;)
		OSSleepTicks(OSSecondsToTicks(1));
}

void abort(void)
{
	report_line("FAILED: abort() called from %p, N64 PC 0x%08X", __builtin_return_address(0), (unsigned)interp_addr);
	halt();
}

void __assert_func(const char *file, int line, const char *func, const char *expr)
{
	report_line("FAILED: assert \"%s\" in %s (%s:%d), N64 PC 0x%08X", expr, func, file, line, (unsigned)interp_addr);
	halt();
}

/* Diagnostic builds (OUR64_DIV_CHECK): a DIV/DIVU where the recompiler's
 * PowerPC sequence would give a different result than the interpreter. */
void our64_div_mismatch(unsigned pc, int is_unsigned, unsigned rs, unsigned rt,
                        unsigned lo_interp, unsigned hi_interp, unsigned lo_ppc, unsigned hi_ppc)
{
	static unsigned count;
	if (++count <= 64)
		report_line("DIV%s at 0x%08X: 0x%08X / 0x%08X: interpreter lo 0x%08X hi 0x%08X, PowerPC lo 0x%08X hi 0x%08X",
		            is_unsigned ? "U" : "", pc, rs, rt, lo_interp, hi_interp, lo_ppc, hi_ppc);
	else if (count == 65)
		report_line("DIV: more mismatches not logged");
}

void our64_bad_mem_call(unsigned addr, unsigned value, int count, int type, unsigned pc, int delay)
{
	report_line("FAILED: memory helper got addr 0x%08X reg %u count %d type %d pc 0x%08X delay %d",
	            addr, value, count, type, pc, delay);
}
