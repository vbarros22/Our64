// Our64 player: GLideN64 (linked in, GX2 backend) as the graphics plugin, on
// a core of its own: the graphics core. GX2 is set up on that core, so
// drawing and presenting happen there too.
//
// The CPU thread posts commands into a ring: graphics tasks, VIs, VI register
// changes and plain calls. The graphics core runs them in order.
// - RDRAM is shared, not copied: GLideN64 reads display lists, vertices and
//   textures from live RDRAM. That is safe because the CPU side holds the
//   task's SP and DP interrupts until the task is decoded (plugins.c), so
//   the game reuses the task's memory only when real hardware would let it.
// - DMEM and IMEM are copied with each task (8 KB): audio tasks run on the
//   CPU core meanwhile and overwrite them.
// - GLideN64 gets private copies of the registers it reads or writes (MI
//   interrupt, SP status, DP and VI registers), filled from each command's
//   snapshot. What a task does to them goes back to the CPU side as a result.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_common.h"
#include "m64p_plugin.h"

#include <Graphics/GX2Context/gx2_ContextImpl.h>
#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/event.h>
#include <coreinit/thread.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/display.h>
#include <gx2/mem.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <whb/gfx.h>

#include <RSP.h>
#include <GBI.h>

#include "player.h"
#include "prompt.h"
#include "video_ui.h"
#include "our64_at.h"
extern "C" {
#include "report.h"
extern "C" unsigned our64_vi_holds, our64_vi_from_rdram, our64_vi_found_start, our64_vi_found_width, our64_vi_found_valid; /* GLideN64's VI.cpp */
extern "C" unsigned our64_vi_cfb, our64_vi_swaps, our64_cfb_calls, our64_cfb_nobuffer, our64_cfb_noheight, our64_cfb_zero,
	our64_cfb_drawn, our64_cfb_alpha, our64_cfb_last_height, our64_cfb_last_width; /* VI.cpp, RDRAMtoColorBuffer.cpp */
}

extern "C" void our64_settle_rdram_copies(void); // GLideN64's FrameBuffer.cpp: GPU copies into RDRAM land before a task reads it
extern "C" int our64_config_override(const char * list); // core_config.cpp
extern "C" void our64_config_shared_dir(const char * dir);
extern const char * our64_gx2_log_path;
extern const char * our64_gx2_debug;
extern "C" const char * our64_gx2_shader_dir; // gx2_ShaderCache.cpp
extern "C" void our64_gx2_shader_bench(const char * dir); // gx2_GL.cpp
extern "C" void our64_gx2_prof_mark(const char * what); // gx2_ContextImpl.cpp: the frame test's GPU profile

namespace {

enum Command : uint32_t { CMD_CALL, CMD_TASK, CMD_SCREEN, CMD_VI_STATUS, CMD_VI_WIDTH, CMD_RDP_LIST };

struct Slot {
	uint32_t cmd;
	uint32_t task;             // CMD_TASK: graphics tasks posted before this one
	void (*fn)(void *);        // CMD_CALL
	void * arg;
	uint32_t vi[14], dpc[8], sp_status;
	uint8_t dmem[0x1000], imem[0x1000]; // CMD_TASK; dmem for CMD_RDP_LIST from DMEM too
};

// What a task did to the registers, for the CPU side to apply.
struct Result {
	uint32_t sp_set, sp_clear;
	bool dp;                   // the task ended with a full sync: DP interrupt
	bool running;              // it ended on a display list branching to itself: the RSP runs on
};

int g_trace_tasks; // tasks after a reset or load still to log

// The last task's microcode talks with the game through SP signals mid-task
// (ZSortBOSS: Stunt Racer 64, World Driver Championship), see plugins.c.
volatile bool g_signal_ucode;

constexpr uint32_t SLOTS = 8;
constexpr uint32_t RESULTS = VIDEO_RESULTS;

Slot g_slots[SLOTS] __attribute__((aligned(64)));
Result g_results[RESULTS];
volatile uint32_t g_posted, g_done;       // commands
volatile uint32_t g_tasks_posted, g_tasks_done;
volatile uint32_t g_screens_posted, g_screens_run; // CMD_SCREENs: posted (CPU core), run (graphics core)
OSEvent g_cmd_event, g_done_event;

// The graphics core's view of the N64 registers and RSP memory.
struct Regs {
	unsigned mi_intr, sp_status, dpc[8], vi[14];
} g_regs;
uint8_t g_dmem[0x1000] __attribute__((aligned(64))), g_imem[0x1000] __attribute__((aligned(64)));
player_memory g_mem;                      // Not64's, read on the CPU core only

OSThread g_thread __attribute__((aligned(16)));
uint8_t g_stack[1024 * 1024] __attribute__((aligned(16)));
volatile bool g_quit, g_ready, g_gfx_ok;

// Statistics, written by the graphics core; times in microseconds.
volatile double g_task_us, g_screen_us, g_worst_task_us, g_cpu_wait_us;
// The CPU core's waits by cause: the ring full, a call or sync, a task while
// ahead of time, a task it must have (Gauntlet Legends, 2026-10-20).
volatile double g_cpu_wait_part_us[4];
volatile long long g_busy_since;          // OSTime the current command started, 0 when idle
volatile uint32_t g_busy_cmd;

unsigned g_rdram_size = 0x800000;
bool g_started = false;
bool g_plugin_up = false; // PluginStartup ran: once a session; each game opens and closes its ROM
const int g_aspect = 1;   // GLideN64's AspectRatio, force 4:3: its 640x480 window either way (Picture: ui_gfx_scan)

double now_us() { return double(OSTicksToMicroseconds(OSGetSystemTime())); }

volatile unsigned g_frames_shown, g_frames_late, g_frames_dropped, g_frames_trimmed;

volatile double g_queue_wait_us; // time VIs waited in the queue, summed over the VIs taken
volatile unsigned g_vis_taken;    // queued VIs the TV took (shown or repeated)
bool g_present_waits;  // sd:/wiiu/our64/present-waits: show each frame at once, waiting for the flip
GX2ColorBuffer * g_last_frame; // the last frame GLideN64 presented
bool g_hold;              // Our64's own screens own the screens: game frames are not shown

// GLideN64 presents once per emulated VI, and the TV flips once per vsync.
// Each present goes into a queue and the TV takes one per vsync, so a game's
// own rhythm (a new picture every second VI at 30 frames a second) reaches
// the TV unchanged. VIs come in bursts (the CPU core runs ahead while audio
// has room, the graphics core decodes in bursts), and showing the newest
// frame at each flip dropped some and showed others twice: Mario Golf lost
// 2-18 of its 30 frames a second (its swing meter jumped) and ran smoothly
// only when a light scene kept up (Zach, 2026-10-21: the ball's flight
// looked faster than everything else). A queued VI that repeats the picture
// before it costs no copy. With more than kQueue waiting (the emulated VIs
// run 60.0 a second, the TV 59.94), the oldest repeat goes, else the oldest.
// A burst that fills the queue leaves it full for good (VIs come as fast as
// the TV takes them): Star Wars Episode I Racer showed every frame late,
// 33-50 ms after it was drawn (Zach, 2026-10-22: input lag). So when a VI
// has waited behind the shown one at every vsync of a second, one queued VI
// goes, the oldest repeat if there is one (a 30-frames-a-second picture then
// shows for one vsync instead of two, once), else the oldest. Three queued
// VIs still kept Mario Tennis's rallies 37-53 ms behind (bursts refilled the
// queue faster than the trim emptied it; showing at once felt quicker, Zach,
// 2026-10-25), so one waits at most: a burst drops a picture instead.
constexpr int kQueue = 1;
constexpr int kTrimWindow = 60;           // vsyncs
constexpr int kPictures = kQueue + 1;     // one more for the present being queued
GX2ColorBuffer g_pictures[kPictures];
struct Queued {
	int picture;          // index in g_pictures
	bool fresh;           // a new picture, not a repeat of the VI before
	OSTime at;            // when it was queued
	unsigned vi;          // g_presents at its present
	uint32_t origin;      // the VI origin then: a new value is a new game frame
};
// Holds: each new game frame (a new VI origin) should stay on the TV for as
// many vsyncs as the game kept it for VIs. Counted at the TV's real flips
// (GX2GetSwapStatus), from what each swap showed (Conker judders at a steady
// 30 frames a second, 2026-11-02).
unsigned g_presents;        // VIs presented, this core's count
struct SwapInfo { uint32_t swap; unsigned vi; uint32_t origin; uint32_t slot; }; // slot: the vsync it is for (see ViMark)
SwapInfo g_swap_info[16];   // by swap count: what each swap showed (swap: the count it was written for)

// Frame check (2026-11-05): does the TV show each VI when the N64 would?
// The N64 shows VI n's frame buffer (the VI origin then) for one VI. Each
// emulated VI is numbered on the CPU core (video_update_screen) with its
// origin and time, each swap carries the number of the VI it shows, and each
// TV flip (GX2's flip callback) is checked: k vsyncs after the last flip the
// TV should show the VI k later. A later one means VIs skipped, an earlier
// one VIs repeated (shown late). A game frame (a run of VIs with one origin)
// none of whose VIs reached the TV was dropped. Testers saw frames skipped
// at full speed in Conker, the Banjos and GoldenEye.
// slot: the TV vsync the VI is due at, counted from any start: the last VI's
// plus its hold. A PAL game on a 59.94 Hz TV holds every fifth VI two vsyncs
// (5 VIs in 6, video_set_pal_cadence), and that VI is swapped twice, so a swap
// waits at every vsync as with NTSC: holding it by sending nothing left the
// queue almost empty, and a late frame showed (89-475 times a minute in four
// PAL games, 2026-11-06). The frame check compares slots, so one rule covers
// NTSC (one VI a vsync) and PAL.
struct ViMark { uint32_t origin; OSTime t; uint32_t slot; uint32_t hold; }; // hold: vsyncs it is due on the TV
constexpr uint32_t kViMarks = 1024;
ViMark g_vi_marks[kViMarks];        // VI n's at n % kViMarks
bool g_pal_cadence;                 // a PAL game on a 59.94 Hz TV (video_set_pal_cadence)
uint32_t g_cadence_n;               // CPU core: VIs numbered since it was set
volatile uint32_t g_vi_marked;      // CPU core: VIs numbered
volatile uint32_t g_vis_unpresented; // CPU core: VIs never given to the graphics core (it still had one)
volatile uint32_t g_game_frames;    // CPU core: VIs with a new origin
uint32_t g_last_marked_origin;
uint32_t g_screen_vi;               // graphics core: the VI the current screen command is

struct FlipMark { uint32_t flips; OSTime t; };
constexpr uint32_t kFlipMarks = 64;
FlipMark g_flip_marks[kFlipMarks];  // written by the flip callback
volatile uint32_t g_flip_marks_n;
volatile uint32_t g_vsyncs;         // vsync callback: the TV's own count
volatile uint32_t g_tv_swaps, g_tv_flips; // TV clock: GX2's swap count after this core's last swap, its flip count at the last flip
volatile uint32_t g_vis_lead_dropped; // CPU core: VIs not given to the graphics core because the TV was too far behind
volatile uint32_t g_vi_on_tv;       // flip callback: the game VI the TV shows (0: none yet)
uint32_t g_flip_marks_done, g_flips_polled;

volatile unsigned g_fc_flips, g_fc_skipped, g_fc_repeated, g_fc_never, g_fc_frames, g_fc_short, g_fc_long;
volatile double g_fc_latency_us, g_fc_latency_max_us; // VI to TV: sum, and the most since video_stats
volatile unsigned g_fc_latency_n;
bool g_fc_have_prev, g_fc_have_frame, g_fc_off;
unsigned g_fc_excused;  // swaps left out on still screens since the last flip: no skip to count
uint32_t g_fc_prev_vi, g_fc_prev_slot, g_fc_frame_start;
OSTime g_fc_prev_t, g_fc_frame_t;
// GPU flip queue (GamePad: hold L and R, press minus; dev build): each VI's
// picture is copied and swapped as soon as GLideN64 presents it, repeats too,
// so the swap sits in the GPU's command stream right after that frame's own
// drawing and the GPU flips one swap per vsync. A swap shown later, from the
// software queue, waited behind the next frame's drawing: 24% of Conker's
// flips came a vsync late (pace.bin, 2026-11-02). At most kAhead swaps wait:
// past that a repeat is skipped, or a new picture waits for a flip.
constexpr uint32_t kAhead = 2;
volatile bool g_flipq;
bool g_flipq_was;

// TV clock (sd:/wiiu/our64/tv-clock, a test, 2026-11-05): the TV's vsync is
// the clock. The CPU core runs one VI per vsync (main.c new_vi; audio no
// longer sets the pace), every VI reaches the graphics core (up to
// kScreensInFlight waiting), and every VI's picture is swapped, repeats too,
// so the GPU flips one per vsync in order: each VI stays on the TV one
// vsync, as on the N64.
volatile bool g_tv_clock;
// Each screen gets three scan buffers (triple_buffers): with WHB's two, a
// copy into the next one waited on the GPU until a swap already waiting had
// flipped, and the GPU work behind it waited too: 700-800 ms of each second
// in the first TV clock runs, and Banjo-Kazooie drew 17 frames a second
// instead of 29 (2026-11-05). With three, one swap may wait for free.
constexpr uint32_t kTvAhead = 2;          // a present waits (on this core) while this many swaps wait
constexpr uint32_t kScreensInFlight = 3;
constexpr uint32_t kTvLeadMax = 4; // TV clock: pictures the TV may be behind before a VI is dropped (video_update_screen)
// The depth: swaps waiting at each present. Something late leaves it one
// deeper (a vsync more delay). When it stayed at 2 or more for kDepthWindow
// presents, the CPU core waits one vsync more once (new_vi): the TV takes the
// waiting swaps meanwhile, so nothing shows, and the delay is a vsync shorter.
constexpr unsigned kDepthWindow = 60;
unsigned g_depth_min = ~0u, g_depth_n;
volatile uint32_t g_tv_slips_asked, g_tv_slips_done;
bool g_triple;
// A present that finds kTvAhead swaps waiting is kept (a GPU copy of its
// picture) and swapped once one has flipped (show_tv_kept), so this core
// decodes on instead of waiting: waiting there took 550 ms of each second and
// Banjo-Kazooie drew 20 frames a second instead of 29 (2026-11-05).
constexpr int kKeptMax = 4;
struct Kept { int picture; unsigned vi; uint32_t origin; unsigned rep; };
Kept g_kept[kKeptMax];
int g_kept_n;
GX2ColorBuffer g_kept_pictures[kKeptMax + 1];
volatile unsigned g_tv_trimmed, g_tv_waited, g_tv_pending_sum, g_tv_pending_n;
volatile uint32_t g_vs_seq, g_vs_t_hi, g_vs_t_lo; // the last vsync's time, for any core (video_tv_vsync)
volatile uint32_t g_vs_period_ns; // the TV's measured refresh period, 0 until measured
// The TV's refresh period: 59.94 Hz or 50 Hz, from the measured one (video_tv_is_60hz).
extern "C" int video_tv_is_60hz(void);
OSTime tv_period() { return OSNanosecondsToTicks(video_tv_is_60hz() ? 16683333 : 20000000); }
volatile unsigned g_flipq_skipped, g_flipq_waited;
Queued g_queue[kQueue];
int g_queued;
int g_queued_picture = -1;  // the picture the newest queued VI shows
int g_shown_picture = -1;   // the picture on the TV, -1: something else is
OSTime g_used_vsync;        // the vsync whose interval took the last queued VI
uint32_t g_tasks_at_present = ~0u, g_origin_at_present;
int g_spare_min = kQueue;   // fewest VIs left queued after taking one, this window
int g_window;               // vsyncs counted in this window
extern volatile uint32_t g_tasks_done;

bool picture_queued(int picture)
{
	for (int i = 0; i < g_queued; i++)
		if (g_queue[i].picture == picture)
			return true;
	return false;
}

void drop_queued(int i)
{
	if (g_queue[i].fresh && (i + 1 >= g_queued || g_queue[i + 1].fresh))
		g_frames_dropped++; // a picture never shown
	else if (g_queue[i].fresh)
		g_queue[i + 1].fresh = true; // the repeat after it shows that picture first
	for (int k = i; k + 1 < g_queued; k++)
		g_queue[k] = g_queue[k + 1];
	g_queued--;
}

// To the screens the screen mode uses (ui_wiiu.cpp).
void pace_note(char kind, OSTime t, OSTime x, uint32_t a, uint32_t b, uint32_t c);
uint32_t mark_slot(uint32_t vi);
void show(GX2ColorBuffer * screen, unsigned vi, uint32_t origin, OSTime vsync, unsigned rep = 0)
{
	ui_gfx_scan(screen);
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	g_swap_info[swaps & 15] = {swaps, vi, origin, mark_slot(vi) + rep};
	g_tv_swaps = swaps;
	pace_note('S', OSGetSystemTime(), vsync, vi, origin, swaps);
}

// The record (sd:/wiiu/our64/pace, or the flip queue switched on in the dev
// build), for tools/pace-report.py, written to sd:/wiiu/our64/pace-<game>.bin at
// HOME and when the game stops (video_pace_write). Entries are big-endian
// >QQIIII: t, x, kind, a, b, c. M: an emulated VI (t its time on the CPU
// core, a its number, b its origin). P: a present (t now, x the VI's time,
// a the VI, b its origin, c fresh). S: a swap (t now, x the vsync it was
// meant for, a the VI, b its origin, c swaps after). F: a TV flip (t its
// time, a flips, b the VI it showed, 0 unknown). T: a repeat's swap left
// out on a still screen (a the VI). R: the frame check started over. This core writes; the file
// is written while a pause holds it or after the game.
struct PaceEntry { uint64_t t, x; uint32_t kind, a, b, c; };
constexpr unsigned kPaceN = 65536;
PaceEntry * g_pace_log;
volatile unsigned g_pace_n;
uint32_t g_marks_noted; // VIs recorded as M
volatile bool g_pace_restart; // the CPU core asks for a fresh record (video_pace_start)

void pace_note(char kind, OSTime t, OSTime x, uint32_t a, uint32_t b, uint32_t c)
{
	if (g_pace_log == nullptr || g_pace_n >= kPaceN)
		return;
	g_pace_log[g_pace_n] = {uint64_t(t), uint64_t(x), uint32_t(kind), a, b, c};
	g_pace_n = g_pace_n + 1;
}

// GX2's callbacks, on this core: each flip with its time, and each vsync.
void on_flip(GX2EventType, void *)
{
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip = 0, last_vsync = 0;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	const uint32_t n = g_flip_marks_n;
	g_flip_marks[n % kFlipMarks] = {flips, last_flip ? last_flip : OSGetSystemTime()};
	const SwapInfo & info = g_swap_info[flips & 15];
	if (info.swap == flips && info.vi != 0)
		g_vi_on_tv = info.vi;
	g_tv_flips = flips;
	__sync_synchronize();
	g_flip_marks_n = n + 1;
}

void on_vsync(GX2EventType, void *)
{
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip = 0, last_vsync = 0;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	if (last_vsync == 0)
		last_vsync = OSGetSystemTime();
	g_vs_seq = g_vs_seq + 1; // odd: being written
	__sync_synchronize();
	g_vs_t_hi = uint32_t(uint64_t(last_vsync) >> 32);
	g_vs_t_lo = uint32_t(last_vsync);
	g_vsyncs = g_vsyncs + 1;
	__sync_synchronize();
	g_vs_seq = g_vs_seq + 1;
	// The measured refresh, over 64 vsyncs (video_tv_is_60hz).
	static OSTime span_start;
	static uint32_t span_n;
	if (span_n == 0)
		span_start = last_vsync;
	if (++span_n == 65) {
		g_vs_period_ns = uint32_t(OSTicksToNanoseconds(last_vsync - span_start) / 64);
		span_n = 0;
	}
}

uint32_t mark_origin(uint32_t vi) { return g_vi_marks[vi % kViMarks].origin; }
uint32_t mark_slot(uint32_t vi) { return g_vi_marks[vi % kViMarks].slot; }
uint32_t mark_hold(uint32_t vi) { return g_vi_marks[vi % kViMarks].hold; }
// VI vi's mark is still there (the CPU core has not numbered kViMarks VIs since).
bool marked(uint32_t vi) { return vi != 0 && int32_t(g_vi_marked - vi) >= 0 && g_vi_marked - vi < kViMarks - 16; }

// The first VI of the game frame VI vi belongs to.
uint32_t run_start(uint32_t vi)
{
	uint32_t v = vi;
	while (vi - v < 512 && marked(v - 1) && mark_origin(v - 1) == mark_origin(v))
		v--;
	return v;
}

void frame_check_reset()
{
	if (g_fc_have_prev)
		pace_note('R', OSGetSystemTime(), 0, 0, 0, 0); // pace-report.py measures nothing across it either
	g_fc_have_prev = g_fc_have_frame = false;
	g_fc_excused = 0;
}

// One TV flip, in order: which VI it showed, against which it should have.
void check_flip(uint32_t flips, OSTime t)
{
	const SwapInfo info = g_swap_info[flips & 15];
	const bool known = info.swap == flips && marked(info.vi) && !g_hold && !g_fc_off;
	pace_note('F', t, 0, flips, known ? info.vi : 0, known ? info.slot : 0);
	if (!known) { // one of Our64's own screens, a VI too long ago, or a PAL game
		frame_check_reset();
		return;
	}
	const OSTime period = tv_period();
	const uint32_t vi = info.vi;
	g_fc_flips++;
	const double us = double(OSTicksToMicroseconds(t - g_vi_marks[vi % kViMarks].t));
	g_fc_latency_us += us;
	g_fc_latency_n++;
	if (us > g_fc_latency_max_us)
		g_fc_latency_max_us = us;
	// The first VI of a game frame that began since the last flip's VI, 0: none did.
	uint32_t start = 0;
	if (g_fc_have_prev) {
		const int32_t vsyncs = int32_t((t - g_fc_prev_t + period / 2) / period);
		int32_t d = int32_t(info.slot - g_fc_prev_slot) - vsyncs;
		while (d > 0 && g_fc_excused > 0) { // a still screen's left-out swap: not seen
			d--;
			g_fc_excused--;
		}
		if (d > 0)
			g_fc_skipped += d;
		else
			g_fc_repeated += -d;
		// Game frames that began after the last flip's VI, and ended before this one's.
		const uint32_t span = vi - g_fc_prev_vi;
		if (span >= 1 && span < kViMarks / 2 && marked(g_fc_prev_vi)) {
			unsigned starts = 0;
			for (uint32_t v = g_fc_prev_vi + 1; v != vi + 1; v++)
				if (mark_origin(v) != mark_origin(v - 1)) {
					starts++;
					start = v;
				}
			if (starts > 1)
				g_fc_never += starts - 1;
		}
	}
	g_fc_excused = 0;
	// A new game frame on the TV: the one before stayed for as many vsyncs as the N64 kept it?
	if (start != 0) {
		if (g_fc_have_frame && marked(g_fc_frame_start)) {
			uint32_t end = g_fc_frame_start + 1;
			while (end != start && mark_origin(end) == mark_origin(g_fc_frame_start))
				end++;
			const uint32_t held = uint32_t((t - g_fc_frame_t + period / 2) / period),
			               vis = mark_slot(end) - mark_slot(g_fc_frame_start); // vsyncs it was due for
			g_fc_frames++;
			if (held < vis)
				g_fc_short++;
			else if (held > vis)
				g_fc_long++;
		}
		g_fc_frame_start = start;
		g_fc_frame_t = t;
		g_fc_have_frame = true;
	}
	g_fc_prev_vi = vi;
	g_fc_prev_slot = info.slot;
	g_fc_prev_t = t;
	g_fc_have_prev = true;
}

// Every flip since the last look: from the flip callback, or polled (then a
// look that finds two flips cannot tell when the first came).
void frame_check()
{
	if (g_pace_restart) {
		g_pace_n = 0;
		g_marks_noted = g_vi_marked;
		g_pace_restart = false;
		frame_check_reset();
	}
	const uint32_t marked_now = g_vi_marked;
	__sync_synchronize();
	if (g_pace_log != nullptr) {
		if (marked_now - g_marks_noted > kViMarks / 2)
			g_marks_noted = marked_now - kViMarks / 2;
		while (g_marks_noted != marked_now) {
			const uint32_t v = ++g_marks_noted;
			pace_note('M', g_vi_marks[v % kViMarks].t, 0, v, mark_origin(v), mark_slot(v));
		}
	}
	const uint32_t n = g_flip_marks_n;
	__sync_synchronize();
	if (n != 0) {
		if (n - g_flip_marks_done > kFlipMarks) {
			g_flip_marks_done = n - kFlipMarks;
			frame_check_reset();
		}
		while (g_flip_marks_done != n) {
			const FlipMark m = g_flip_marks[g_flip_marks_done % kFlipMarks];
			g_flip_marks_done++;
			check_flip(m.flips, m.t);
		}
		return;
	}
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	if (flips == g_flips_polled)
		return;
	if (flips - g_flips_polled != 1)
		frame_check_reset();
	g_flips_polled = flips;
	check_flip(flips, last_flip);
}

// Shows the next queued VI once the TV has flipped to the one before and a
// vsync has passed since. outside_frame: not inside GLideN64's own present,
// so its GPU state needs giving back.
void show_tv_kept(bool outside_frame);
void show_queued(bool outside_frame)
{
	if (!g_hold)
		show_tv_kept(outside_frame);
	frame_check();
	if (g_queued == 0 || g_hold)
		return;
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	if (swaps > flips)
		return;
	// The vsync that began this interval, counted on from the last one known.
	const OSTime period = tv_period(), now = OSGetSystemTime();
	const OSTime vsync = now > last_vsync ? last_vsync + (now - last_vsync) / period * period : last_vsync;
	if (vsync < g_used_vsync + period / 2)
		return;
	const Queued q = g_queue[0];
	for (int k = 0; k + 1 < g_queued; k++)
		g_queue[k] = g_queue[k + 1];
	g_queued--;
	g_used_vsync = vsync;
	g_queue_wait_us = g_queue_wait_us + OSTicksToMicroseconds(now - q.at);
	g_vis_taken++;
	if (g_queued < g_spare_min)
		g_spare_min = g_queued;
	if (++g_window == kTrimWindow) {
		if (g_spare_min > 0 && g_queued > 0) {
			int drop = 0; // the oldest repeat, else the oldest
			for (int i = 0; i < g_queued; i++)
				if (!g_queue[i].fresh) {
					drop = i;
					break;
				}
			drop_queued(drop);
			g_frames_trimmed++;
		}
		g_window = 0;
		g_spare_min = kQueue;
	}
	if (q.picture == g_shown_picture)
		return; // the TV already shows it: this vsync is the repeat
	if (vsync - q.at > period)
		g_frames_late++; // waited past a vsync
	show(&g_pictures[q.picture], q.vi, q.origin, vsync);
	g_shown_picture = q.picture;
	g_frames_shown++;
	if (outside_frame)
		gx2::restoreState();
}

// Until fewer than ahead swaps wait for the TV, or 100 ms: flips stop
// while the app is in the background, and a wait for them never ended.
void wait_for_room(uint32_t ahead)
{
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	const OSTime until = OSGetSystemTime() + OSMillisecondsToTicks(100);
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	while (swaps - flips >= ahead && OSGetSystemTime() < until) {
		OSSleepTicks(OSMicrosecondsToTicks(500));
		GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	}
}

// TV clock: kept presents swapped in order while fewer than kTvAhead swaps
// wait. outside_frame: GLideN64's GPU state needs giving back after.
void show_tv_kept(bool outside_frame)
{
	if (!g_tv_clock || g_kept_n == 0)
		return;
	bool shown = false;
	while (g_kept_n > 0) {
		uint32_t swaps = 0, flips = 0;
		OSTime last_flip, last_vsync;
		GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
		if (swaps - flips >= kTvAhead)
			break;
		const Kept k = g_kept[0];
		for (int i = 0; i + 1 < g_kept_n; i++)
			g_kept[i] = g_kept[i + 1];
		g_kept_n--;
		show(&g_kept_pictures[k.picture], k.vi, k.origin, 0, k.rep);
		g_frames_shown++;
		shown = true;
	}
	if (shown && outside_frame)
		gx2::restoreState();
}

// TV clock: one swap of the VI GLideN64 presents (rep: its second, for a VI
// due two vsyncs).
void present_tv(GX2ColorBuffer * frame, bool same, unsigned rep)
{
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	show_tv_kept(false);
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	const uint32_t depth = swaps - flips + uint32_t(g_kept_n);
	g_tv_pending_sum = g_tv_pending_sum + depth;
	g_tv_pending_n = g_tv_pending_n + 1;
	if (depth < g_depth_min)
		g_depth_min = depth;
	if (++g_depth_n == kDepthWindow) {
		if (g_depth_min >= 2 && g_tv_slips_asked == g_tv_slips_done) {
			g_tv_slips_asked = g_tv_slips_asked + 1;
			g_tv_trimmed = g_tv_trimmed + 1;
			pace_note('D', OSGetSystemTime(), 0, g_screen_vi, g_depth_min, 0);
		}
		g_depth_n = 0;
		g_depth_min = ~0u;
	}
	if (g_kept_n == 0 && swaps - flips < kTvAhead) {
		show(frame, g_screen_vi, g_regs.vi[1], 0, rep);
		g_frames_shown++;
		frame_check();
		return;
	}
	if (g_kept_n == kKeptMax) { // four kept already: this core waits after all
		g_tv_waited = g_tv_waited + 1;
		wait_for_room(kTvAhead);
		show_tv_kept(false);
	}
	// A repeat of the last kept picture shares it; anything else is copied
	// into a picture no kept present uses.
	int picture = -1;
	if (same && g_kept_n > 0)
		picture = g_kept[g_kept_n - 1].picture;
	for (int p = 0; p <= kKeptMax && picture < 0; p++) {
		bool used = false;
		for (int i = 0; i < g_kept_n; i++)
			used = used || g_kept[i].picture == p;
		if (!used)
			picture = p;
	}
	if (g_kept_n == kKeptMax || picture < 0 ||
	    (!(same && g_kept_n > 0 && picture == g_kept[g_kept_n - 1].picture) &&
	     !ui_gfx_copy_frame(frame, &g_kept_pictures[picture]))) {
		// no room or no memory: the kept ones first, then this one, waiting here
		for (int tries = 0; g_kept_n > 0 && tries < 8; tries++) {
			wait_for_room(kTvAhead);
			show_tv_kept(false);
		}
		wait_for_room(kTvAhead);
		show(frame, g_screen_vi, g_regs.vi[1], 0, rep);
		g_frames_shown++;
		frame_check();
		return;
	}
	our64_gx2_prof_mark("kept copy");
	g_kept[g_kept_n++] = {picture, g_screen_vi, g_regs.vi[1], rep};
	frame_check();
	return;
}

// Each VI GLideN64 presents: queued (see above). Controller notices go on a
// copy of the frame (ui_gfx_overlay).
void present(GX2ColorBuffer * screen)
{
	OUR64_AT();
	g_last_frame = screen;
	g_presents++;
	if (g_hold) // a pause or the closing screen is up: it keeps the screens
		return;
	OUR64_AT();
	GX2ColorBuffer * frame = ui_gfx_overlay(screen);
	OUR64_AT();
	if (g_present_waits) {
		g_frames_shown++;
		g_shown_picture = -1;
		show(frame, g_screen_vi, g_regs.vi[1], 0);
		return;
	}
	// A repeat: no task decoded since the last present, the same frame
	// buffer at the VI origin, no notices drawn on.
	const bool same = g_tasks_done == g_tasks_at_present && g_regs.vi[1] == g_origin_at_present && frame == screen;
	const bool fresh = !same || g_queued_picture < 0;
	g_tasks_at_present = g_tasks_done;
	g_origin_at_present = g_regs.vi[1];
	pace_note('P', OSGetSystemTime(), marked(g_screen_vi) ? g_vi_marks[g_screen_vi % kViMarks].t : 0, g_screen_vi,
	          g_regs.vi[1], fresh);
	if (g_flipq != g_flipq_was) { // switched: the software queue empties, its pictures unshown
		g_flipq_was = g_flipq;
		g_queued = 0;
		g_queued_picture = -1;
		g_shown_picture = -1;
		report_line("VIDEO: GPU flip queue %s at VI %u", g_flipq_was ? "on" : "off", g_presents);
	}
	if (g_tv_clock) {
		const uint32_t hold = marked(g_screen_vi) && mark_hold(g_screen_vi) == 2 ? 2 : 1;
		for (uint32_t rep = 0; rep < hold; rep++)
			present_tv(frame, same || rep > 0, rep);
		return;
	}
	if (g_flipq) {
		uint32_t swaps = 0, flips = 0;
		OSTime last_flip, last_vsync;
		GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
		if (swaps - flips >= kAhead) {
			if (!fresh) { // a repeat: the TV holds the picture anyway, one vsync fewer
				g_flipq_skipped++;
				frame_check();
				return;
			}
			g_flipq_waited++;
			wait_for_room(kAhead);
		}
		show(frame, g_screen_vi, g_regs.vi[1], 0);
		g_frames_shown++;
		frame_check();
		return;
	}
	if (!fresh && g_queued == kQueue) { // repeats the waiting picture: the TV holds it anyway
		show_queued(false);
		return;
	}
	if (g_queued == kQueue) {
		int drop = 0; // the oldest repeat, else the oldest
		for (int i = 0; i < g_queued; i++)
			if (!g_queue[i].fresh) {
				drop = i;
				break;
			}
		drop_queued(drop);
	}
	int picture = g_queued_picture;
	if (fresh) {
		picture = -1;
		for (int p = 0; p < kPictures && picture < 0; p++)
			if (p != g_queued_picture && !picture_queued(p))
				picture = p;
		// The GPU copies in order: a picture whose copy to the scan buffers
		// is still to run is copied out before this copy overwrites it.
		if (picture < 0 || !ui_gfx_copy_frame(frame, &g_pictures[picture])) {
			g_frames_shown++; // no memory for the queue: shown as before
			g_shown_picture = -1;
			show(frame, g_screen_vi, g_regs.vi[1], 0);
			return;
		}
		if (picture == g_shown_picture)
			g_shown_picture = -1;
		g_queued_picture = picture;
	}
	g_queue[g_queued++] = {picture, fresh, OSGetSystemTime(), g_screen_vi, g_regs.vi[1]};
	OUR64_AT();
	show_queued(false);
	OUR64_AT();
}

void debug(void *, int level, const char * message)
{
	if (level <= M64MSG_WARNING)
		report_line("GLideN64: %s", message);
	// With sd:/wiiu/our64/capture-unknown-gbi, the first unknown display list
	// command starts a capture, so the frames can be replayed on the PC to see
	// whether its GLideN64 reads them the same. Only then: the capture makes
	// the CPU wait on every graphics task, and Smash Remix's character select
	// crawled at 0.7 frames a second (2026-10-17).
	static bool captured = false;
	if (!captured && strstr(message, "UNKNOWN GBI") != nullptr) {
		captured = true;
		char path[256];
		struct stat st;
		const char * root = report_sd_root();
		snprintf(path, sizeof(path), "%s/capture-unknown-gbi", root ? root : "");
		if (root != nullptr && stat(path, &st) == 0)
			player_capture_start();
	}
}

void check_interrupts() {}

// sd:/wiiu/our64/profile-gfx: the graphics core's PC and LR, sampled every
// 250 us while it decodes a task (an alarm on its own core), for
// tools/profile-report.py. The last 64K samples (about the last 20 s of
// decoding) go to profile/player-gfx.prof when the game ends (Destruction
// Derby's races, 2026-10-20).
constexpr uint32_t kProfMax = 64 * 1024;
uint32_t g_prof[kProfMax][2];
volatile uint32_t g_prof_n;
volatile bool g_prof_in_task;
bool g_prof_on, g_prof_saved;
OSAlarm g_prof_alarm;

void prof_sample(OSAlarm *, OSContext * c)
{
	const uint32_t n = g_prof_n;
	if (g_prof_in_task && c) {
		g_prof[n % kProfMax][0] = c->srr0;
		g_prof[n % kProfMax][1] = c->lr;
		g_prof_n = n + 1;
	}
}

void prof_save()
{
	if (!g_prof_on || g_prof_saved || g_prof_n == 0)
		return;
	g_prof_saved = true;
	char path[256];
	const char * root = report_sd_root();
	snprintf(path, sizeof(path), "%s/profile", root ? root : "");
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/profile/player-gfx.prof", root ? root : "");
	FILE * f = fopen(path, "wb");
	if (!f)
		return;
	const uint32_t n = g_prof_n, kept = n < kProfMax ? n : kProfMax, first = n - kept;
	const uint32_t header[4] = {0x4F363450, kept, (uint32_t)(uintptr_t)&ProcessDList, 0}; // "O64P"
	fwrite(header, sizeof(header), 1, f);
	for (uint32_t i = first; i < n; i++)
		fwrite(g_prof[i % kProfMax], sizeof(g_prof[0]), 1, f);
	fclose(f);
	report_line("PROFILE: the last %u of %u samples of the graphics core's tasks written to %s", (unsigned)kept,
	            (unsigned)n, path);
}

// Test switches for Stunt Racer's live-only misplaced triangles (2026-10-28):
// sd:/wiiu/our64/gx2-drain waits for the GPU after every graphics task, as
// the GX2 Replay does (it reads back every image), so the GPU never runs
// behind the decoder. sd:/wiiu/our64/frame-dump makes GamePad minus save the
// next FRAME_DUMP screens as shown, to sd:/wiiu/our64/framedump/ (each read
// back waits for the GPU too).
#define FRAME_DUMP 60
static bool g_gx2_drain, g_frame_dump;
static volatile int g_dump_left;
static unsigned g_dump_index;

// The read-back buffer for GLideN64's window (ReadScreen2: 3 bytes a pixel),
// kept between calls and grown when the window is larger (720 lines).
static uint8_t * screen_rgb(int w, int h)
{
	static uint8_t * rgb;
	static size_t size;
	const size_t need = size_t(w) * size_t(h) * 3;
	if (need > size) {
		free(rgb);
		rgb = static_cast<uint8_t *>(malloc(need));
		size = rgb != nullptr ? need : 0;
	}
	return rgb;
}

// GLideN64's window: 640x480, or 960x720 while the dev build's test switch
// sd:/wiiu/our64/res-720 is on the card (the 480 against 720 measurement for
// 1.2.0, #22 and #35). Read at each game start.
static int g_res_h = 480;
static int g_res_wanted = 480; // the game's own (Our ROMs > Options), set before it starts

// For the speed display (main.c): game frames that reached the TV, those of
// them held the wrong number of vsyncs, and VIs the TV skipped, since the app
// started. No side effects.
extern "C" void video_hold_counts(unsigned * frames, unsigned * wrong, unsigned * skipped)
{
	*frames = g_fc_frames;
	*wrong = g_fc_short + g_fc_long;
	*skipped = g_fc_skipped;
}

extern "C" unsigned video_game_frames(void) { return g_game_frames; }

// GAMEOPT_NATIVE (240): the window stays 640x480 and GLideN64 draws at the
// game's own resolution (UseNativeResolutionFactor=1), enlarged after.
extern "C" void video_set_lines(int lines) { g_res_wanted = lines == 720 || lines == 240 ? lines : 480; }

static int res_height_from_card()
{
	if (g_res_wanted == 720)
		return 720;
#ifndef OUR64_RELEASE
	if (const char * root = report_sd_root()) {
		char path[256];
		struct stat st;
		snprintf(path, sizeof(path), "%s/res-720", root);
		if (stat(path, &st) == 0)
			return 720;
	}
#endif
	return 480;
}

extern "C" int video_res_height(void) { return res_height_from_card(); }

static void dump_screen()
{
	const char * root = report_sd_root();
	if (root == nullptr)
		return;
	int w = 0, h = 0;
	ReadScreen2(nullptr, &w, &h, 0);
	if (w <= 0 || h <= 0)
		return;
	uint8_t * rgb = screen_rgb(w, h);
	if (rgb == nullptr)
		return;
	ReadScreen2(rgb, &w, &h, 0);
	char path[256];
	snprintf(path, sizeof(path), "%s/framedump/%05u.ppm", root, g_dump_index++);
	if (FILE * f = fopen(path, "wb")) {
		fprintf(f, "P6\n%d %d\n255\n", w, h);
		for (int y = h - 1; y >= 0; y--) // rows come bottom-up, as from glReadPixels
			fwrite(rgb + size_t(y) * w * 3, 3, w, f);
		fclose(f);
	}
	if (--g_dump_left == 0)
		report_line("FRAME DUMP: done, %u screens in framedump/", g_dump_index);
}

// ---- edge measurement (the sweep): the black border at each edge of the
// picture, against where GLideN64 placed the game's picture (#36: "black
// borders, thicker at the bottom", 2026-11-11). Graphics core; every second. ----
extern "C" volatile int our64_pic_rect[6]; // FrameBuffer.cpp renderBuffer: x0, y0, x1, y1, window w, h
static volatile bool g_edges_on;
static OSTime g_edges_last;
static unsigned g_edges_n, g_edges_skipped;     // samples kept, mostly black ones left out
constexpr unsigned kEdgeSamples = 64;
static int16_t g_edge_s[kEdgeSamples][4];       // kept samples' black rows/columns: left, right, top, bottom
static int g_rect_min[4], g_rect_max[4];        // our64_pic_rect x0, y0, x1, y1 over every sample
static unsigned g_rect_n;
static int g_rect_w, g_rect_h, g_edge_w, g_edge_h;

static bool dark(const uint8_t * p) { return p[0] <= 16 && p[1] <= 16 && p[2] <= 16; }

// Once a second (the sweep): a frame mostly black (a fade, a logo on black)
// says nothing about the border and is left out; the rest are kept.
static void edge_scan()
{
	int w = 0, h = 0;
	ReadScreen2(nullptr, &w, &h, 0);
	if (w <= 0 || h <= 0)
		return;
	uint8_t * rgb = screen_rgb(w, h);
	if (rgb == nullptr)
		return;
	ReadScreen2(rgb, &w, &h, 0);
	// Rows come bottom-up: screen row r is buffer row h - 1 - r.
	auto row_dark = [&](int r) {
		const uint8_t * p = rgb + size_t(h - 1 - r) * w * 3;
		for (int x = 0; x < w; x++)
			if (!dark(p + x * 3))
				return false;
		return true;
	};
	auto col_dark = [&](int c) {
		for (int r = 0; r < h; r++)
			if (!dark(rgb + (size_t(h - 1 - r) * w + c) * 3))
				return false;
		return true;
	};
	int e[4] = {0, 0, 0, 0};
	while (e[2] < h && row_dark(e[2])) e[2]++;
	while (e[3] < h - e[2] && row_dark(h - 1 - e[3])) e[3]++;
	while (e[0] < w && col_dark(e[0])) e[0]++;
	while (e[1] < w - e[0] && col_dark(w - 1 - e[1])) e[1]++;
	int rc[4];
	for (int i = 0; i < 4; i++)
		rc[i] = our64_pic_rect[i];
	for (int i = 0; i < 4; i++) {
		if (g_rect_n == 0 || rc[i] < g_rect_min[i]) g_rect_min[i] = rc[i];
		if (g_rect_n == 0 || rc[i] > g_rect_max[i]) g_rect_max[i] = rc[i];
	}
	g_rect_n++;
	g_rect_w = our64_pic_rect[4];
	g_rect_h = our64_pic_rect[5];
	g_edge_w = w;
	g_edge_h = h;
	if ((e[0] + e[1]) * 2 >= w || (e[2] + e[3]) * 2 >= h) {
		g_edges_skipped++; // under half the width or height shows anything
		return;
	}
	if (g_edges_n < kEdgeSamples) {
		for (int i = 0; i < 4; i++)
			g_edge_s[g_edges_n][i] = int16_t(e[i]);
		g_edges_n++;
	}
}

extern "C" void video_edges_start(void)
{
	g_edges_n = g_edges_skipped = g_rect_n = 0;
	g_edges_last = OSGetSystemTime();
	g_edges_on = true;
}

// One side's most common value over the kept samples (the border that stays),
// with how many samples had it, and the smallest.
static void edge_mode(int side, int * mode, unsigned * count, int * least)
{
	*mode = -1;
	*count = 0;
	*least = 1 << 30;
	for (unsigned i = 0; i < g_edges_n; i++) {
		const int v = g_edge_s[i][side];
		if (v < *least)
			*least = v;
		unsigned c = 0;
		for (unsigned j = 0; j < g_edges_n; j++)
			c += g_edge_s[j][side] == v;
		if (c > *count || (c == *count && v < *mode)) {
			*count = c;
			*mode = v;
		}
	}
}

// One line for the sweep: the black edges (most common and smallest over the
// kept samples) and GLideN64's picture rectangle over every sample.
extern "C" void video_edges_line(char * out, unsigned size)
{
	g_edges_on = false;
	int n = snprintf(out, size, "edges of %dx%d: %u frames kept, %u mostly black left out", g_edge_w, g_edge_h, g_edges_n,
	                 g_edges_skipped);
	static const char * const side[4] = {"left", "right", "top", "bottom"};
	for (int i = 0; i < 4 && g_edges_n > 0 && n < int(size); i++) {
		int mode, least;
		unsigned count;
		edge_mode(i, &mode, &count, &least);
		n += snprintf(out + n, size - n, "%s %s %d (%u, least %d)", i ? "," : ";", side[i], mode, count, least);
	}
	if (g_rect_n > 0 && n < int(size))
		snprintf(out + n, size - n, "; picture rect x0 %d-%d y0 %d-%d x1 %d-%d y1 %d-%d in %dx%d", g_rect_min[0], g_rect_max[0],
		         g_rect_min[1], g_rect_max[1], g_rect_min[2], g_rect_max[2], g_rect_min[3], g_rect_max[3], g_rect_w,
		         g_rect_h);
}

extern "C" int video_frame_dump_on(void) { return g_frame_dump; }
extern "C" void video_frame_dump_start(void)
{
	if (g_dump_left)
		return;
	if (const char * root = report_sd_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/framedump", root);
		mkdir(path, 0777);
	}
	report_line("FRAME DUMP: the next %d screens from VI %u", FRAME_DUMP, player_vis());
	g_dump_left = FRAME_DUMP;
}

void snapshot_vi(Slot & s)
{
	for (int k = 0; k < 14; k++) s.vi[k] = *g_mem.vi[k];
}

void run(Slot & s)
{
	switch (s.cmd) {
	case CMD_CALL:
		s.fn(s.arg);
		break;
	case CMD_TASK: {
		const double t = now_us();
		memcpy(g_dmem, s.dmem, sizeof(g_dmem));
		memcpy(g_imem, s.imem, sizeof(g_imem));
		memcpy(g_regs.vi, s.vi, sizeof(g_regs.vi));
		memcpy(g_regs.dpc, s.dpc, sizeof(g_regs.dpc));
		g_regs.sp_status = s.sp_status;
		g_regs.mi_intr = 0;
		g_prof_in_task = g_prof_on;
		/* The task's GPU copies into RDRAM land before its end is told: a copy
		 * landing later wrote into memory the game had already given to
		 * something else. Without the CPU's wait Perfect Dark hung as its intro
		 * changed scenes (2026-10-30). The settle before covers copies made
		 * outside a task (screens). */
		if (g_started) {
			our64_settle_rdram_copies();
			ProcessDList();
			our64_settle_rdram_copies();
		}
		if (g_gx2_drain)
			GX2DrawDone();
		g_prof_in_task = false;
		Result & r = g_results[s.task % RESULTS];
		r.sp_set = g_regs.sp_status & ~s.sp_status;
		r.sp_clear = s.sp_status & ~g_regs.sp_status;
		r.dp = (g_regs.mi_intr & 0x20) != 0;
		r.running = RSP.infloop;
		g_signal_ucode = GBI.getMicrocodeType() == ZSortBOSS;
		if (g_trace_tasks > 0) {
			g_trace_tasks--;
			report_line("GRAPHICS: task %u after a reset or load: %s, display list 0x%08X, microcode 0x%08X", s.task,
			            r.dp ? "frame done (DP)" : r.running ? "waits on a branch to itself" : "no DP interrupt",
			            *(uint32_t *)(g_dmem + 0xFF0),
			            *(uint32_t *)(g_dmem + 0xFD0));
		}
		const double d = now_us() - t;
		g_task_us += d;
		if (d > g_worst_task_us)
			g_worst_task_us = d;
		break;
	}
	case CMD_RDP_LIST:
		/* RDP commands the CPU sent itself (DPC_END written), no RSP task: Rat
		 * Attack!, NASCAR 2000 and Toy Story 2 draw their screens this way and
		 * showed black (2026-10-29). */
		memcpy(g_regs.dpc, s.dpc, sizeof(g_regs.dpc));
		if (s.dpc[3] & 0x1) // xbus: the commands are in DMEM
			memcpy(g_dmem, s.dmem, sizeof(g_dmem));
		if (g_started) {
			const double t = now_us();
			our64_settle_rdram_copies();
			ProcessRDPList();
			our64_settle_rdram_copies();
			g_task_us += now_us() - t;
		}
		break;
		case CMD_SCREEN: {
		const double t = now_us();
		memcpy(g_regs.vi, s.vi, sizeof(g_regs.vi));
		g_screens_run = g_screens_run + 1;
		{
			/* A screen width change, then what GLideN64 found at the origin for the
			 * next 10 screens, and screens shown from RDRAM, for Resident Evil 2's
			 * green and blue wall on a room's first frames (2026-10-26). Menus
			 * (width 0) are left out. */
			static u32 lastWidth, logged, follow;
			static unsigned lastFromRdram;
			const unsigned fromRdram = our64_vi_from_rdram;
			if (g_regs.vi[2] != 0 && g_regs.vi[2] != lastWidth)
				follow = 10;
			if (g_regs.vi[2] != 0 && (follow || fromRdram != lastFromRdram) && logged < 400) {
				logged++;
				report_line("VI: origin 0x%06X width %u status 0x%X; last screen found buffer 0x%06X w %u valid %u; "
				            "%u kept the last frame, %u from RDRAM", (unsigned)(g_regs.vi[1] & 0xFFFFFF), (unsigned)g_regs.vi[2],
				            (unsigned)g_regs.vi[0], our64_vi_found_start, our64_vi_found_width, our64_vi_found_valid,
				            our64_vi_holds, fromRdram);
			}
			/* Screens the CPU drew (Rat Attack: black on the Wii U, 2026-10-29): every 600 screens
			 * while there are any. */
			static unsigned screens, lastCfb;
			if (++screens % 600 == 0 && our64_vi_cfb != lastCfb && (lastCfb = our64_vi_cfb, true))
				report_line("SCREEN: %u CPU-written of %u swaps; copies from RDRAM %u: no buffer %u, no height %u, "
				            "all zero %u, drawn %u (%u with alpha), last %ux%u", our64_vi_cfb, our64_vi_swaps, our64_cfb_calls,
				            our64_cfb_nobuffer, our64_cfb_noheight, our64_cfb_zero, our64_cfb_drawn, our64_cfb_alpha,
				            our64_cfb_last_width, our64_cfb_last_height);
			if (follow)
				follow--;
			if (g_regs.vi[2] != 0)
				lastWidth = g_regs.vi[2];
			lastFromRdram = fromRdram;
		}
		g_screen_vi = s.task; // the VI's number (video_update_screen)
		if (g_started)
			UpdateScreen();
		g_screen_vi = 0; // presents outside a VI show no VI
		if (g_started && g_dump_left > 0)
			dump_screen();
		if (g_started && g_edges_on && OSGetSystemTime() - g_edges_last >= OSSecondsToTicks(1)) {
			g_edges_last = OSGetSystemTime();
			edge_scan();
		}
		g_screen_us += now_us() - t;
		break;
	}
	case CMD_VI_STATUS:
	case CMD_VI_WIDTH:
		memcpy(g_regs.vi, s.vi, sizeof(g_regs.vi));
		if (g_started)
			s.cmd == CMD_VI_STATUS ? ViStatusChanged() : ViWidthChanged();
		break;
	}
}

int graphics_main(int, const char **)
{
	g_gfx_ok = WHBGfxInit();
	{
		struct stat st;
		char path[256];
		const char * root = report_sd_root();
		snprintf(path, sizeof(path), "%s/profile-gfx", root ? root : "");
		g_prof_on = root != nullptr && stat(path, &st) == 0;
		if (g_prof_on) { // the alarm fires on the core that set it: this one
			OSCreateAlarm(&g_prof_alarm);
			const OSTime period = OSMicrosecondsToTicks(250);
			OSSetPeriodicAlarm(&g_prof_alarm, OSGetTime() + period, period, prof_sample);
			report_line("PROFILE: sampling the graphics core's tasks every 250 us (profile-gfx)");
		}
	}
	if (g_gfx_ok) { // the frame check's flip times (frame_check)
		GX2SetEventCallback(GX2_EVENT_TYPE_FLIP, on_flip, nullptr);
		GX2SetEventCallback(GX2_EVENT_TYPE_VSYNC, on_vsync, nullptr);
	}
	g_ready = true;
	OSSignalEvent(&g_done_event);
	if (!g_gfx_ok)
		return 0;
	while (!g_quit) {
		while (g_done == g_posted) {
			// Every millisecond while a game runs: queued VIs go up, and each
			// flip is seen for the hold counts.
			if (!g_started) {
				OSWaitEvent(&g_cmd_event);
				continue;
			}
			OSWaitEventWithTimeout(&g_cmd_event, 1000000); // nanoseconds
			show_queued(true);
		}
		Slot & s = g_slots[g_done % SLOTS];
		g_busy_cmd = s.cmd;
		g_busy_since = OSGetSystemTime();
		run(s);
		if (s.cmd == CMD_TASK)
			g_tasks_done = s.task + 1;
		g_busy_since = 0;
		__sync_synchronize();
		g_done = g_done + 1;
		OSSignalEvent(&g_done_event);
		show_queued(true); // between commands too: a busy ring kept the TV waiting
	}
	if (g_prof_on)
		OSCancelAlarm(&g_prof_alarm); // a periodic alarm left behind hung the next launches (replay app)
	GX2SetEventCallback(GX2_EVENT_TYPE_FLIP, nullptr, nullptr);
	GX2SetEventCallback(GX2_EVENT_TYPE_VSYNC, nullptr, nullptr);
	WHBGfxShutdown();
	return 0;
}

// The next free slot. Waits while the ring is full.
Slot & claim()
{
	if (g_posted - g_done >= SLOTS) {
		const double t = now_us();
		while (g_posted - g_done >= SLOTS)
			OSWaitEvent(&g_done_event);
		g_cpu_wait_us += now_us() - t;
		g_cpu_wait_part_us[0] += now_us() - t;
	}
	return g_slots[g_posted % SLOTS];
}

void post()
{
	__sync_synchronize();
	g_posted = g_posted + 1;
	OSSignalEvent(&g_cmd_event);
}

// Waits until the graphics core has run every command posted so far.
void drain()
{
	if (g_done == g_posted)
		return;
	const double t = now_us();
	const uint32_t want = g_posted;
	while (int32_t(g_done - want) < 0)
		OSWaitEvent(&g_done_event);
	g_cpu_wait_us += now_us() - t;
	g_cpu_wait_part_us[1] += now_us() - t;
}

void call(void (*fn)(void *), void * arg)
{
	Slot & s = claim();
	s.cmd = CMD_CALL;
	s.fn = fn;
	s.arg = arg;
	post();
	drain();
}

void start_on_graphics_core(void * result)
{
	static char log_path[256], shader_dir[256];
	if (const char * root = report_sd_root()) {
		// The previous run's log stays as gx2-log-prev.txt (once a session: later games add to this one).
		char prev[256];
		snprintf(log_path, sizeof(log_path), "%s/gx2-log.txt", root);
		snprintf(prev, sizeof(prev), "%s/gx2-log-prev.txt", root);
		if (!g_plugin_up) {
			remove(prev);
			rename(log_path, prev);
		}
		our64_gx2_log_path = log_path;
		// sd:/wiiu/our64/player-gx2-debug.txt: GLideN64's GX2 debug switches
		// ("trace": each GPU operation logged, then waited for, so a GPU hang
		// shows as the last line without "ok"). Lines starting with # are notes.
		// Not the GX2 Replay's gx2-debug.txt, whose variants (one a line) ran
		// all at once in live play and changed every game (2026-10-26).
		static char debug[128];
		snprintf(prev, sizeof(prev), "%s/player-gx2-debug.txt", root);
		if (FILE * f = fopen(prev, "r")) {
			char line[128];
			debug[0] = 0;
			while (fgets(line, sizeof(line), f))
				if (line[0] != '#')
					strncat(debug, line, sizeof(debug) - strlen(debug) - 1);
			fclose(f);
			for (char * c = debug; *c; c++)
				if (*c == '\n' || *c == '\r')
					*c = ' ';
			if (strspn(debug, " ") != strlen(debug)) {
				our64_gx2_debug = debug;
				report_line("GX2 debug: %s (sd:/wiiu/our64/player-gx2-debug.txt)", debug);
			}
		}
		// Compiled shaders, kept between runs: compiling takes up to a second each.
		snprintf(shader_dir, sizeof(shader_dir), "%s/shaders", root);
		mkdir(shader_dir, 0777);
		our64_gx2_shader_dir = shader_dir;
		// GLideN64's per-game settings: sd:/wiiu/our64/GLideN64.custom.ini.
		our64_config_shared_dir(root);
		// .frag files in sd:/wiiu/our64/shader-bench: time a compile on each
		// core before the game starts (gx2-log.txt).
		char bench[256];
		struct stat st;
		snprintf(bench, sizeof(bench), "%s/shader-bench", root);
		if (stat(bench, &st) == 0 && S_ISDIR(st.st_mode))
			our64_gx2_shader_bench(bench);
	}
	our64_gx2_present = present;
	// The color copy to RDRAM costs a GPU readback each frame, so it is off
	// unless sd:/wiiu/our64/copy-color holds a mode (1 sync, 2 double buffer).
	// GLideN64.custom.ini entries (frameBufferEmulation\copyToRDRAM) win over both.
	// New shaders compile on a worker thread while GLideN64 draws on with the
	// previous combiner, unless sd:/wiiu/our64/sync-shaders exists.
	// Texture coordinates use GLideN64's simpler shaders: the exact texel
	// ones take CafeGLSL 3-7 s to compile instead of 0.2 s, and drop pixels
	// on the Wii U GPU. sd:/wiiu/our64/exact-texcoords switches them back.
	int copy = 0, async = 1, inexact = 1;
	if (const char * root = report_sd_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/copy-color", root);
		if (FILE * f = fopen(path, "r")) {
			int mode = 0;
			if (fscanf(f, "%d", &mode) == 1 && mode >= 0 && mode <= 3)
				copy = mode;
			fclose(f);
		}
		struct stat st;
		snprintf(path, sizeof(path), "%s/sync-shaders", root);
		async = stat(path, &st) != 0;
		snprintf(path, sizeof(path), "%s/gx2-drain", root);
		g_gx2_drain = stat(path, &st) == 0;
		snprintf(path, sizeof(path), "%s/frame-dump", root);
		g_frame_dump = stat(path, &st) == 0;
		if (g_gx2_drain || g_frame_dump)
			report_line("TEST:%s%s", g_gx2_drain ? " the graphics core waits for the GPU after every task (gx2-drain)" : "",
			            g_frame_dump ? " GamePad minus saves screens (frame-dump)" : "");
		snprintf(path, sizeof(path), "%s/exact-texcoords", root);
		inexact = stat(path, &st) != 0;
		snprintf(path, sizeof(path), "%s/present-waits", root);
		g_present_waits = stat(path, &st) == 0;
		// sd:/wiiu/our64/pace: this game's presents, swaps and flips recorded
		// from its start (pace.bin, tools/pace-report.py).
		snprintf(path, sizeof(path), "%s/pace", root);
		if (stat(path, &st) == 0) {
			if (g_pace_log == nullptr)
				g_pace_log = static_cast<PaceEntry *>(calloc(kPaceN, sizeof(PaceEntry)));
			g_pace_n = 0;
			g_marks_noted = g_vi_marked;
			report_line("PACE: recording presents, swaps and flips (sd:/wiiu/our64/pace)%s",
			            g_pace_log ? "" : ": no memory for the record");
		}
	}
	g_res_h = res_height_from_card();
	static char settings[512];
	int used = snprintf(settings, sizeof(settings),
	         "EnableCopyColorToRDRAM=%d,AsyncShaderCompilation=%d,EnableInaccurateTextureCoordinates=%d,AspectRatio=%d,"
	         "ScreenWidth=%d,ScreenHeight=%d,UseNativeResolutionFactor=%d",
	         copy, async, inexact, g_aspect, g_res_h * 4 / 3, g_res_h, g_res_wanted == 240 ? 1 : 0);
#ifndef OUR64_RELEASE
	// Dev test switch sd:/wiiu/our64/gfx-override: its first line, "Name=value,Name=value",
	// is added to GLideN64's settings (2026-11-11: what the GPU's per-pixel cost is made of).
	if (const char * root = report_sd_root()) {
		char path[256], line[200] = "";
		snprintf(path, sizeof(path), "%s/gfx-override", root);
		if (FILE * f = fopen(path, "r")) {
			if (fgets(line, sizeof(line), f) != nullptr) {
				line[strcspn(line, "\r\n")] = 0;
				if (line[0] != 0 && used > 0 && size_t(used) + strlen(line) + 2 < sizeof(settings)) {
					snprintf(settings + used, sizeof(settings) - used, ",%s", line);
					report_line("TEST: GLideN64 settings added from gfx-override: %s", line);
				}
			}
			fclose(f);
		}
	}
#endif
	our64_config_override(settings);
	report_line("Color copy to RDRAM: %d (per-game ini entries win). Shaders compile %s. Drawn at %s%dx%d%s.", copy,
	            async ? "in the background" : "before drawing", g_res_wanted == 240 ? "the game's own resolution, window " : "",
	            g_res_h * 4 / 3, g_res_h, g_res_h != 480 && g_res_wanted != 720 ? " (TEST: res-720)" : "");

	GFX_INFO gi = {};
	gi.HEADER = g_mem.header;
	gi.RDRAM = g_mem.rdram;
	gi.DMEM = g_dmem;
	gi.IMEM = g_imem;
	gi.MI_INTR_REG = &g_regs.mi_intr;
	unsigned ** dpc[8] = {&gi.DPC_START_REG, &gi.DPC_END_REG, &gi.DPC_CURRENT_REG, &gi.DPC_STATUS_REG,
	                      &gi.DPC_CLOCK_REG, &gi.DPC_BUFBUSY_REG, &gi.DPC_PIPEBUSY_REG, &gi.DPC_TMEM_REG};
	for (int k = 0; k < 8; k++) *dpc[k] = &g_regs.dpc[k];
	unsigned ** vi[14] = {&gi.VI_STATUS_REG, &gi.VI_ORIGIN_REG, &gi.VI_WIDTH_REG, &gi.VI_INTR_REG,
	                      &gi.VI_V_CURRENT_LINE_REG, &gi.VI_TIMING_REG, &gi.VI_V_SYNC_REG, &gi.VI_H_SYNC_REG,
	                      &gi.VI_LEAP_REG, &gi.VI_H_START_REG, &gi.VI_V_START_REG, &gi.VI_V_BURST_REG,
	                      &gi.VI_X_SCALE_REG, &gi.VI_Y_SCALE_REG};
	for (int k = 0; k < 14; k++) *vi[k] = &g_regs.vi[k];
	gi.CheckInterrupts = check_interrupts;
	gi.version = 2;
	gi.SP_STATUS_REG = &g_regs.sp_status;
	gi.RDRAM_SIZE = &g_rdram_size;

	int & ok = *static_cast<int *>(result);
	ok = -1;
	if (!g_plugin_up && PluginStartup(nullptr, nullptr, debug) != M64ERR_SUCCESS) {
		report_line("GLideN64: PluginStartup failed");
		return;
	}
	g_plugin_up = true;
	our64_gx2_shader_progress = ui_gfx_loading_progress;
	if (!InitiateGFX(gi)) {
		report_line("GLideN64: InitiateGFX failed");
		return;
	}
	if (!RomOpen()) {
		report_line("GLideN64: RomOpen failed");
		return;
	}
	ViStatusChanged();
	ViWidthChanged();
	g_started = true;
	ok = 0;
}

void stop_on_graphics_core(void *)
{
	prof_save(); // this game's samples, then a fresh start for the next
	g_prof_n = 0;
	g_prof_saved = false;
	if (!g_started)
		return;
	RomClosed(); // the plugin stays up for the next game
	tv_pass_game_stopped();
	ui_gfx_game_stopped();
	our64_gx2_present = nullptr;
	g_started = false;
	g_last_frame = nullptr;
	g_queued = 0; // nothing of this game reaches the screens after it
	g_queued_picture = g_shown_picture = -1;
	g_tasks_at_present = ~0u;
	g_kept_n = 0;
	frame_check_reset();
}

void quit_on_graphics_core(void *) { g_quit = true; }

// The screen after the game stops: dark red, see player-log.txt.
void idle_frame_on_graphics_core(void *)
{
	WHBGfxBeginRender();
	WHBGfxBeginRenderTV();
	WHBGfxClearColor(0.3f, 0.0f, 0.0f, 1.0f);
	WHBGfxFinishRenderTV();
	WHBGfxBeginRenderDRC();
	WHBGfxClearColor(0.3f, 0.0f, 0.0f, 1.0f);
	WHBGfxFinishRenderDRC();
	WHBGfxFinishRender();
}

void post_vi(Command cmd)
{
	Slot & s = claim();
	s.cmd = cmd;
	snapshot_vi(s);
	post();
}

} // namespace

void video_call(void (*fn)(void *), void * arg)
{
	if (g_gfx_ok)
		call(fn, arg);
}

GX2ColorBuffer * video_last_frame() { return g_last_frame; }
bool video_game_started() { return g_started; }

void video_set_hold(bool hold)
{
	g_hold = hold;
	frame_check_reset();
	if (hold) {
		g_kept_n = 0;
		g_queued = 0;
		g_queued_picture = g_shown_picture = -1;
	}
}

// Where the graphics core is: its thread stopped a moment, its PC, LR and
// the return addresses up its stack (addr2line -e player/build/our64.elf).
extern "C" void video_backtrace(char * out, unsigned size)
{
	int n = snprintf(out, size, "graphics core:");
	if (!g_ready) {
		snprintf(out + n, size - n, " not running");
		return;
	}
	OSSuspendThread(&g_thread);
	for (int i = 0; i < 1000 && !OSIsThreadSuspended(&g_thread); i++)
		OSSleepTicks(OSMicrosecondsToTicks(100));
	if (!OSIsThreadSuspended(&g_thread)) {
		OSResumeThread(&g_thread);
		snprintf(out + n, size - n, " would not stop");
		return;
	}
	const OSContext & c = g_thread.context;
	n += snprintf(out + n, n < (int)size ? size - n : 0, " pc %08X lr %08X stack", (unsigned)c.srr0, (unsigned)c.lr);
	const uintptr_t lo = (uintptr_t)g_stack, hi = lo + sizeof(g_stack);
	uintptr_t sp = c.gpr[1];
	for (int depth = 0; depth < 24 && sp >= lo && sp + 8 <= hi && n < (int)size; depth++) {
		const uintptr_t up = *(const uint32_t *)sp; // the back chain
		if (up <= sp || up + 8 > hi)
			break;
		n += snprintf(out + n, size - n, " %08X", (unsigned)*(const uint32_t *)(up + 4));
		sp = up;
	}
	OSResumeThread(&g_thread);
}

extern "C" int video_thread_start(unsigned core)
{
	player_get_memory(&g_mem);
	OSInitEvent(&g_cmd_event, FALSE, OS_EVENT_MODE_AUTO);
	OSInitEvent(&g_done_event, FALSE, OS_EVENT_MODE_AUTO);
	if (!OSCreateThread(&g_thread, graphics_main, 0, nullptr, g_stack + sizeof(g_stack), sizeof(g_stack), 16,
	                    OSThreadAttributes(1u << core)))
		return -1;
	OSSetThreadName(&g_thread, "Our64 graphics");
	OSResumeThread(&g_thread);
	while (!g_ready)
		OSWaitEvent(&g_done_event);
	return g_gfx_ok ? 0 : -1;
}

extern "C" void video_thread_stop(void)
{
	if (!g_gfx_ok)
		return;
	call(quit_on_graphics_core, nullptr);
	int ret;
	OSJoinThread(&g_thread, &ret);
	g_gfx_ok = false;
}

extern "C" void video_idle_frame(void) { if (g_gfx_ok) call(idle_frame_on_graphics_core, nullptr); }

extern "C" int video_start(void)
{
	int ok = -1;
	call(start_on_graphics_core, &ok);
	return ok;
}

extern "C" void video_stop(void) { call(stop_on_graphics_core, nullptr); }

namespace {
void picture_43_on_graphics_core(void *) { ui_gfx_set_picture_43(true); }
void picture_wide_on_graphics_core(void *) { ui_gfx_set_picture_43(false); }
} // namespace

// The shape is set where frames go to the screens (ui_gfx_scan): GLideN64's
// own aspect setting only resized its 4:3 window to the same 640x480, after
// rebuilding its frame buffers (0.8 s).
extern "C" void video_set_picture(int wide)
{
	if (g_gfx_ok)
		call(wide ? picture_wide_on_graphics_core : picture_43_on_graphics_core, nullptr);
	else
		ui_gfx_set_picture_43(!wide);
}

namespace {
// The filters are drawn in ui_gfx_scan, on the graphics core.
void shader_on_graphics_core(void * arg) { ui_gfx_set_shader(*static_cast<int *>(arg)); }
} // namespace

// Settings > Picture's Shader. Before the graphics core is up only the value is kept.
extern "C" void video_set_shader(int shader)
{
	if (g_gfx_ok)
		call(shader_on_graphics_core, &shader);
	else
		ui_gfx_set_shader(shader);
	report_line("SHADER: %s", shader_name(shader));
}

extern "C" unsigned video_submit_task(void)
{
	const uint32_t task = g_tasks_posted;
	Slot & s = claim();
	s.cmd = CMD_TASK;
	s.task = task;
	snapshot_vi(s);
	for (int k = 0; k < 8; k++) s.dpc[k] = *g_mem.dpc[k];
	s.sp_status = *g_mem.sp_status;
	memcpy(s.dmem, g_mem.dmem, sizeof(s.dmem));
	memcpy(s.imem, g_mem.imem, sizeof(s.imem));
	g_tasks_posted = task + 1;
	post();
	return task;
}

extern "C" void video_submit_rdp_list(void)
{
	Slot & s = claim();
	s.cmd = CMD_RDP_LIST;
	for (int k = 0; k < 8; k++) s.dpc[k] = *g_mem.dpc[k];
	if (s.dpc[3] & 0x1)
		memcpy(s.dmem, g_mem.dmem, sizeof(s.dmem));
	post();
}

extern "C" unsigned video_tasks_done(void) { return g_tasks_done; }

extern "C" int video_signal_ucode(void) { return g_signal_ucode; }
extern "C" void video_set_signal_ucode(int on) { g_signal_ucode = on != 0; }

extern "C" void video_task_result(unsigned task, uint32_t * sp_set, uint32_t * sp_clear, int * dp, int * running)
{
	const Result & r = g_results[task % RESULTS];
	*sp_set = r.sp_set;
	*sp_clear = r.sp_clear;
	*dp = r.dp;
	*running = r.running;
}

extern "C" int video_wait_task(unsigned task, double timeout_us)
{
	if (int32_t(g_tasks_done - task) > 0)
		return 1;
	const double t = now_us();
	if (timeout_us < 0) {
		while (int32_t(g_tasks_done - task) <= 0)
			OSWaitEvent(&g_done_event);
	} else {
		const double until = t + timeout_us;
		for (double n = t; int32_t(g_tasks_done - task) <= 0 && n < until; n = now_us())
			OSWaitEventWithTimeout(&g_done_event, OSTime((until - n) * 1000)); // nanoseconds
	}
	g_cpu_wait_us += now_us() - t;
	g_cpu_wait_part_us[timeout_us < 0 ? 3 : 2] += now_us() - t;
	return int32_t(g_tasks_done - task) > 0;
}

extern "C" void video_sync(void) { drain(); }

namespace {
// GLideN64 keeps the display list it was in across tasks when one branched to
// itself (RSP.infloop: the next task carries on from there), and skips
// microcode detection while the microcode addresses stay the same. After a
// reset or a load neither holds: the next task starts its own display list.
void reset_rsp_on_graphics_core(void *)
{
	if (RSP.infloop || RSP.busy)
		report_line("GRAPHICS: reset with GLideN64 still in a display list (%s), at 0x%08X",
		            RSP.infloop ? "a branch to itself" : "busy", RSP.PC[RSP.PCi]);
	RSP.infloop = false;
	RSP.busy = false;
	RSP.halt = false;
	RSP.uc_start = RSP.uc_dstart = 0;
	g_trace_tasks = 3;
}
} // namespace

extern "C" void video_reset_rsp(void) { if (g_gfx_ok) call(reset_rsp_on_graphics_core, nullptr); }

extern "C" unsigned video_tv_lead(void);

// A VI. While the graphics core still has one to show, this one is dropped:
// a graphics core that falls behind skips VIs instead of stalling the CPU.
extern "C" void video_update_screen(void)
{
	// Numbered first, shown or not (the frame check, see g_vi_marks).
	const uint32_t vi = g_vi_marked + 1, origin = *g_mem.vi[1];
	const ViMark & last = g_vi_marks[(vi - 1) % kViMarks];
	const uint32_t hold = g_pal_cadence && ++g_cadence_n % 5 == 0 ? 2 : 1;
	g_vi_marks[vi % kViMarks] = {origin, OSGetSystemTime(), last.slot + (last.hold ? last.hold : 1), hold};
	if (origin != g_last_marked_origin) {
		g_last_marked_origin = origin;
		g_game_frames = g_game_frames + 1;
	}
	__sync_synchronize();
	g_vi_marked = vi;
	if (g_screens_posted - g_screens_run >= (g_tv_clock ? kScreensInFlight : 1u)) {
		g_vis_unpresented = g_vis_unpresented + 1;
		return;
	}
	/* TV clock: the TV more than kTvLeadMax pictures behind (the graphics core or the GPU slower than the
	 * VIs: NASCAR 2000, Lode Runner 3D): this VI is not shown, so the game keeps its speed and the delay
	 * stays bounded. The CPU core waiting instead slowed those games to 30-44 VI/s (sweep, 2026-11-06). */
	if (g_tv_clock && video_tv_lead() > kTvLeadMax) {
		g_vis_unpresented = g_vis_unpresented + 1;
		g_vis_lead_dropped = g_vis_lead_dropped + 1;
		return;
	}
	g_screens_posted = g_screens_posted + 1;
	Slot & s = claim();
	s.cmd = CMD_SCREEN;
	s.task = vi;
	snapshot_vi(s);
	post();
}

extern "C" void video_vi_status_changed(void) { post_vi(CMD_VI_STATUS); }
extern "C" void video_vi_width_changed(void) { post_vi(CMD_VI_WIDTH); }

extern "C" void video_stats(video_stats_t * s)
{
	s->task_us = g_task_us;
	s->screen_us = g_screen_us;
	s->cpu_wait_us = g_cpu_wait_us;
	for (int i = 0; i < 4; i++)
		s->cpu_wait_part_us[i] = g_cpu_wait_part_us[i];
	s->worst_task_us = g_worst_task_us;
	g_worst_task_us = 0;
	extern volatile double our64_gx2_frame_wait_us; // gx2_ContextImpl.cpp
	s->gpu_wait_us = our64_gx2_frame_wait_us;
	s->frames_shown = g_frames_shown;
	s->frames_dropped = g_frames_dropped;
	s->frames_late = g_frames_late;
	s->frames_trimmed = g_frames_trimmed;
	s->tv_trimmed = g_tv_trimmed;
	s->tv_waited = g_tv_waited;
	s->tv_pending_sum = g_tv_pending_sum;
	s->tv_pending_n = g_tv_pending_n;
	s->vis_marked = g_vi_marked;
	s->vis_unpresented = g_vis_unpresented;
	s->game_frames = g_game_frames;
	s->tv_vsyncs = g_vsyncs;
	s->fc_flips = g_fc_flips;
	s->fc_skipped = g_fc_skipped;
	s->fc_repeated = g_fc_repeated;
	s->fc_never = g_fc_never;
	s->frames_new = g_fc_frames;
	s->frames_short = g_fc_short;
	s->frames_long = g_fc_long;
	s->latency_us = g_fc_latency_us;
	s->latency_n = g_fc_latency_n;
	s->latency_max_us = g_fc_latency_max_us;
	g_fc_latency_max_us = 0;
	s->queue_wait_us = g_queue_wait_us;
	s->vis_taken = g_vis_taken;
}

// A PAL game's 50 VIs a second cannot each hold one vsync of a 59.94 Hz TV: no frame check.
extern "C" void video_set_frame_check(int on) { g_fc_off = !on; }

// Three scan buffers for the TV and the GamePad in place of WHB's two
// (MEM2, kept for the session). Graphics core.
void triple_buffers(void *)
{
	if (g_triple)
		return;
	GX2TVRenderMode mode = GX2_TV_RENDER_MODE_WIDE_720P; // as WHBGfxInit picks it
	switch (GX2GetSystemTVScanMode()) {
	case GX2_TV_SCAN_MODE_480I:
	case GX2_TV_SCAN_MODE_480P:
		mode = GX2_TV_RENDER_MODE_WIDE_480P;
		break;
	case GX2_TV_SCAN_MODE_1080I:
	case GX2_TV_SCAN_MODE_1080P:
		mode = GX2_TV_RENDER_MODE_WIDE_1080P;
		break;
	default:
		break;
	}
	const GX2SurfaceFormat format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	uint32_t tv_size = 0, drc_size = 0, unk = 0;
	GX2CalcTVSize(mode, format, GX2_BUFFERING_MODE_TRIPLE, &tv_size, &unk);
	GX2CalcDRCSize(GX2_DRC_RENDER_MODE_SINGLE, format, GX2_BUFFERING_MODE_TRIPLE, &drc_size, &unk);
	void * tv = MEMAllocFromDefaultHeapEx(tv_size, GX2_SCAN_BUFFER_ALIGNMENT);
	void * drc = MEMAllocFromDefaultHeapEx(drc_size, GX2_SCAN_BUFFER_ALIGNMENT);
	if (tv == nullptr || drc == nullptr) {
		if (tv)
			MEMFreeToDefaultHeap(tv);
		if (drc)
			MEMFreeToDefaultHeap(drc);
		report_line("VIDEO: no memory for three scan buffers (%u + %u KB): two stay", unsigned(tv_size / 1024),
		            unsigned(drc_size / 1024));
		return;
	}
	memset(tv, 0, tv_size);
	memset(drc, 0, drc_size);
	DCFlushRange(tv, tv_size);
	DCFlushRange(drc, drc_size);
	GX2DrawDone();
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, tv, tv_size);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, drc, drc_size);
	GX2SetTVBuffer(tv, tv_size, mode, format, GX2_BUFFERING_MODE_TRIPLE);
	GX2SetDRCBuffer(drc, drc_size, GX2_DRC_RENDER_MODE_SINGLE, format, GX2_BUFFERING_MODE_TRIPLE);
	g_triple = true;
	ui_gfx_screens_changed();
	report_line("VIDEO: three scan buffers: TV mode %d, %u KB; GamePad %u KB", int(mode), unsigned(tv_size / 1024),
	            unsigned(drc_size / 1024));
}

// The frame test's GPU profile (gx2_ContextImpl.cpp: every 10th frame, no
// wait for the GPU): on for the measured VIs, then its line, read on the
// graphics core, which writes the sums.
extern "C" void our64_gx2_prof_run(int on);
extern "C" void our64_gx2_prof_report(char * out, unsigned size);
extern "C" void video_gpu_profile(int on) { our64_gx2_prof_run(on); }
namespace {
struct ProfArgs { char * out; unsigned size; };
void prof_report_on_graphics_core(void * arg)
{
	ProfArgs & a = *static_cast<ProfArgs *>(arg);
	our64_gx2_prof_report(a.out, a.size);
}
} // namespace
extern "C" void video_gpu_profile_report(char * out, unsigned size)
{
	if (size == 0)
		return;
	out[0] = 0;
	ProfArgs a = {out, size};
	if (g_gfx_ok)
		call(prof_report_on_graphics_core, &a);
}

// The TV runs at 59.94 Hz: measured from the vsyncs (16.0-17.4 ms), else by
// its mode (576i is 50 Hz). The mode alone is not enough: the 50 Hz HD modes
// homebrew can set have no GX2 value of their own (tv-clock-other-regions plan,
// 2026-11-06).
// A PAL game on a 59.94 Hz TV: every fifth VI is due two vsyncs (see ViMark).
extern "C" void video_set_pal_cadence(int on) { g_pal_cadence = on != 0; g_cadence_n = 0; }
// The vsyncs the VI just numbered is due on the TV: main.c's schedule steps by it.
extern "C" unsigned video_last_vi_hold(void) { return g_vi_marks[g_vi_marked % kViMarks].hold; }
extern "C" unsigned video_tv_period_ns(void) { return video_tv_is_60hz() ? 16683333u : 20000000u; }

extern "C" int video_tv_is_60hz(void)
{
	const uint32_t p = g_vs_period_ns;
	if (p != 0)
		return p >= 16000000 && p <= 17400000;
	return GX2GetSystemTVScanMode() != GX2_TV_SCAN_MODE_576I;
}

// The TV clock on or off for the game about to start (main.c: NTSC games on a 59.94 Hz TV).
extern "C" void video_set_tv_clock(int on)
{
	g_tv_clock = on != 0;
	g_depth_n = 0;
	g_depth_min = ~0u;
	g_tv_slips_done = g_tv_slips_asked;
	if (g_tv_clock && g_gfx_ok)
		call(triple_buffers, nullptr);
}

// How many pictures the TV is behind: screens the graphics core has still to
// run, swaps the GPU has still to flip, and kept presents. Counted in pictures,
// not VI numbers: VIs never shown (no new picture, or dropped here) must not
// count, or the count never falls. 0 when not known (none shown yet).
extern "C" unsigned video_tv_lead(void)
{
	if (g_vi_on_tv == 0)
		return 0;
	const uint32_t lead = (g_screens_posted - g_screens_run) + (g_tv_swaps - g_tv_flips) +
	                      uint32_t(*(volatile int *)&g_kept_n);
	return lead > 60 ? 0 : lead;
}
extern "C" unsigned video_tv_lead_dropped(void) { return g_vis_lead_dropped; }

// The CPU core's one extra vsync, when the graphics core asked for it (see kDepthWindow).
extern "C" int video_tv_slip(void)
{
	if (g_tv_slips_done == g_tv_slips_asked)
		return 0;
	g_tv_slips_done = g_tv_slips_asked;
	return 1;
}

// The TV's last vsync: its count and time. 0 before the first one (or while
// the vsync callback writes it, 100 tries).
extern "C" int video_tv_vsync(uint32_t * count, OSTime * t)
{
	for (int i = 0; i < 100; i++) {
		const uint32_t seq = g_vs_seq;
		if (seq & 1)
			continue;
		__sync_synchronize();
		const uint32_t c = g_vsyncs, hi = g_vs_t_hi, lo = g_vs_t_lo;
		__sync_synchronize();
		if (g_vs_seq != seq)
			continue;
		if (c == 0)
			return 0;
		*count = c;
		*t = OSTime((uint64_t(hi) << 32) | lo);
		return 1;
	}
	return 0;
}

// The GPU flip queue on or off (dev build: GamePad L + R + minus). The
// recorder starts with it, so pace.bin covers both ways.
extern "C" int video_flipq_toggle(void)
{
	if (g_pace_log == nullptr)
		g_pace_log = static_cast<PaceEntry *>(calloc(kPaceN, sizeof(PaceEntry)));
	g_flipq = !g_flipq;
	return g_flipq;
}

// A fresh record from now (the frame test's measured part, main.c): the
// graphics core starts it over at its next look (frame_check).
extern "C" void video_pace_start(void)
{
	if (g_pace_log == nullptr)
		g_pace_log = static_cast<PaceEntry *>(calloc(kPaceN, sizeof(PaceEntry)));
	g_pace_restart = true;
}

// The record so far to a file of its own; 1 when written.
extern "C" int video_pace_write_to(const char * path)
{
	if (g_pace_log == nullptr || g_pace_restart)
		return 0;
	FILE * f = fopen(path, "wb");
	if (f == nullptr)
		return 0;
	const unsigned n = g_pace_n;
	const bool ok = fwrite(g_pace_log, sizeof(PaceEntry), n, f) == n;
	return fclose(f) == 0 && ok && n > 0;
}

// The record so far to sd:/wiiu/our64/pace.bin (at HOME; see pace_note).
extern "C" void video_pace_write(const char * id)
{
	if (g_pace_log == nullptr || g_pace_n == 0)
		return;
	char path[320];
	snprintf(path, sizeof(path), "%s/pace-%s.bin", report_sd_root(), id);
	if (FILE * f = fopen(path, "wb")) {
		const unsigned n = g_pace_n;
		fwrite(g_pace_log, sizeof(PaceEntry), n, f);
		fclose(f);
		report_line("PACE: %u events written to %s", n, path);
	}
}

extern "C" double video_busy_s(unsigned * cmd)
{
	const long long since = g_busy_since;
	*cmd = g_busy_cmd;
	return since ? double(OSTicksToMicroseconds(OSGetSystemTime() - OSTime(since))) / 1e6 : 0.0;
}
