/* Our64 player: plugin entry points. Not64's RSP HLE plugin runs every RSP
 * task but the GB Tower's (cxd4's interpreter runs that, rsp_lle.c);
 * graphics tasks go to GLideN64 (video.cpp); the controller is the
 * GamePad (main.c reads it once per VI); each AI DMA goes to AX (audio.c). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <coreinit/time.h>
#include <gctypes.h>
#include "main/winlnxdefs.h"
#include "main/plugin.h"
#include "gc_memory/memory.h"
#include "gc_memory/tlb.h"
#include "r4300/r4300.h"
#include "r4300/interupt.h"
#include "main/rom.h"
#include "player.h"
#include "bench.h"
#include "report.h"
#include "tpak.h"
#include "rsp_lle.h"
#include "gb_tower_hle.h"
#include <stdio.h>
#include <sys/stat.h>
#include <coreinit/event.h>
#include <coreinit/thread.h>
#include <coreinit/cache.h>

static void none(void) {}

/* Graphics tasks run on the graphics core (video.cpp) while the CPU goes on.
 * The task's interrupts wait until it is decoded: SP (task done) and, when
 * the display list ended with a full sync, DP. Until then the game sees the
 * RSP busy, so it keeps its hands off the task's display list, vertices and
 * textures, which GLideN64 reads from live RDRAM. Not every task ends with a
 * DP interrupt: Blast Corps splits a frame into two display lists, and its
 * scheduler faults on a DP interrupt with no RDP task.
 *
 * A GFX_INT event checks for decoded tasks every 1/32 VI while any are in
 * flight. When emulated time runs ahead of real time, it waits for the
 * decoder for that long instead of running on, so a game idling until its
 * task is done gets the interrupt when decoding ends, not at the next VI.
 *
 * A yield request (signal 0, which libultra's scheduler sets to run an audio
 * task) is answered at the next check, as a yielding microcode would: halted,
 * signals 1 (yielded) and 2. Decoding goes on. The game resumes the task with
 * OS_TASK_YIELDED, which starts nothing new, and gets its interrupts once the
 * task is decoded. Until then the task is not done, so its data stays put. */
static uint32_t g_gfx_tasks;     /* posted */
static uint32_t g_gfx_finished;  /* interrupts delivered */
static int g_gfx_event;          /* a GFX_INT event is queued */
static double g_gfx_event_wait_us;
enum { GFX_RUNNING, GFX_YIELDED, GFX_RESUMED };
static int g_gfx_yield;          /* the state of task g_gfx_finished */
static uint32_t g_gfx_yields;
/* The last task ended on a display list branching to itself: the RSP spins
 * there (GLideN64's RSP.infloop) until the game restarts it, or yields it
 * for an audio task and resumes it after (Gauntlet Legends). */
static int g_rsp_spinning, g_spin_yielded;
/* ZSortBOSS (Stunt Racer 64, World Driver Championship) talks with the game
 * mid-task: at the end of its main display list it waits for signal 0 (then
 * draws the sub list), and between lists it sets signal 3 and waits for the
 * game to clear it. GLideN64 spins there (RSP.infloop) and carries on when
 * called again, so the task is posted again once the game sets signal 0 or
 * clears signal 3 (our64_sp_status_written), or at once if signal 0 was set
 * before the decoder got there. Signal 0 is no yield request here. Without
 * this the game waited on its signals forever: a black screen after two
 * tasks (Zach, 2026-10-25). */
static void gfx_task(int resume);
/* VIs since the last graphics task, counted once one came after a load or
 * reset (gfx_reset); the stall report's (gfx_watch_vi). */
static unsigned g_vis_without_gfx;
static int g_gfx_seen, g_gfx_stall_told;
static void note(char kind, unsigned long a, unsigned long b, unsigned long c, unsigned long d); /* stall report */
static int g_gb_busy;     /* a GB Tower task runs on core 2 (see gb_task) */
static int g_gb_held;     /* one done in C waits for its end (see gb_task) */
static int gb_poll(void);
static void gb_quiet(void);
static int g_au_busy;     /* an audio list runs on core 2 (see audio_task) */
static int audio_poll(void);
static void audio_drop(void);
static unsigned long rd32(unsigned long a);
extern int our64_events(int *types, unsigned long *ahead, int max); /* Not64's interupt.c */
extern void remove_interupt_event(void);                             /* Not64's interupt.c */

enum { ZSORT_INT_DELAY = 1000 }; /* Count; mupen64plus's SP interrupt after a graphics task */
/* Count; the latest a ZSortBOSS task's first piece gets its interrupts. On
 * the PC Stunt Racer drew clean with them 200000 late, a little garbage at
 * 400000 (2026-10-28). */
enum { ZSORT_START_MAX = 150000 };
static unsigned long g_zsort_start; /* Count when the ZSortBOSS task in flight was posted */

static unsigned gfx_poll_delay(void)
{
	unsigned d = vi_register.vi_delay / 32;
	return d ? d : 1000;
}

/* Delivers the interrupts of every decoded task, in order; non-zero when an MI interrupt was raised. */
static int gfx_finish_done(void)
{
	const uint32_t done = video_tasks_done();
	int raised = 0;
	/* A yielded task's interrupts wait until the game resumes it. */
	for (; g_gfx_finished != done && g_gfx_yield != GFX_YIELDED; g_gfx_finished++) {
		uint32_t set, clear;
		g_gfx_yield = GFX_RUNNING;
		int dp, running;
		video_task_result(g_gfx_finished, &set, &clear, &dp, &running);
		note('F', g_gfx_finished, set, clear, (unsigned long)dp << 16 | (sp_register.sp_status_reg & 0xFFFF));
		if (running) {
			/* The display list branched to itself: on the N64 the RSP spins there
			 * while the CPU writes on, and GLideN64 carries on from it when the
			 * game restarts the RSP. Not halted, no task done, no interrupt
			 * (Gauntlet Legends streams its display lists: told its task ended,
			 * it went black). */
			sp_register.sp_status_reg = (sp_register.sp_status_reg & ~clear & ~0x203u) | set;
			g_rsp_spinning = 1;
			/* ZSortBOSS can end a frame (full sync) and then wait on a signal:
			 * the RDP is done, so is the DP interrupt. Dropped, Stunt Racer
			 * waited for its third frame forever (Zach, 2026-10-26). Its wait
			 * also raises the SP interrupt, not halted, as mupen64plus does for
			 * a task that leaves the RSP running (rsp_core.c rsp_task_locked):
			 * without it the game never saw signal 3 and stopped after 94
			 * frames. */
			if (video_signal_ucode()) {
				if (dp) {
					MI_register.mi_intr_reg |= 0x20;
					dpc_register.dpc_status &= ~(0x20 | 0x40 | 0x08);
					raised = 1;
				}
				if (sp_register.sp_status_reg & 0x40) {
					MI_register.mi_intr_reg |= 0x01;
					raised = 1;
				}
			}
			continue;
		}
		/* What the HLE plugin does after a graphics task: halt, broke, task done. */
		sp_register.sp_status_reg = (sp_register.sp_status_reg & ~clear) | set | 0x203;
		if (sp_register.sp_status_reg & 0x40) {
			MI_register.mi_intr_reg |= 0x01;
			raised = 1;
		}
		if (dp) { /* gDPFullSync: DP interrupt, pipe, command and clock no longer busy */
			MI_register.mi_intr_reg |= 0x20;
			dpc_register.dpc_status &= ~(0x20 | 0x40 | 0x08);
			raised = 1;
		}
	}
	if (g_rsp_spinning && g_gfx_finished == g_gfx_tasks && video_signal_ucode() && (sp_register.sp_status_reg & 0x80) && !g_au_busy)
		gfx_task(1); /* signal 0 came before the decoder waited for it */
	return raised;
}

/* Answers a yield request for the task in flight: what the microcode does when it yields. */
static int gfx_yield(void)
{
	if (g_gfx_finished == g_gfx_tasks || g_gfx_yield == GFX_YIELDED || video_signal_ucode() ||
	    (sp_register.sp_status_reg & 0x81) != 0x80)
		return 0;
	g_gfx_yield = GFX_YIELDED;
	g_gfx_yields++;
	note('Y', g_gfx_finished, g_gfx_tasks, sp_register.sp_status_reg, dpc_register.dpc_status);
	sp_register.sp_status_reg |= 0x303; /* halt, broke, yielded, task done */
	if (sp_register.sp_status_reg & 0x40) {
		MI_register.mi_intr_reg |= 0x01;
		return 1;
	}
	return 0;
}

/* A yield request while the RSP spins on a branch to itself: what the
 * microcode does there, as gfx_yield; the resume carries on (gfx_task). */
static int spin_yield(void)
{
	if (!g_rsp_spinning || video_signal_ucode() || (sp_register.sp_status_reg & 0x81) != 0x80)
		return 0;
	g_rsp_spinning = 0;
	g_spin_yielded = 1;
	g_gfx_yields++;
	sp_register.sp_status_reg |= 0x303; /* halt, broke, yielded, task done */
	if (sp_register.sp_status_reg & 0x40) {
		MI_register.mi_intr_reg |= 0x01;
		return 1;
	}
	return 0;
}

uint32_t gfx_yields(void) { return g_gfx_yields; }
uint32_t gfx_tasks_posted(void) { return g_gfx_tasks; }

int gfx_int_event(void)
{
	g_gfx_event = 0;
	int raised = gfx_finish_done();
	raised |= gfx_yield();
	raised |= spin_yield();
	raised |= gb_poll();
	raised |= audio_poll();
	/* A ZSortBOSS piece gfx_finish_done resumed is decoded and keeps to its
	 * own event (gfx_task): delivered here, it would get its interrupt with
	 * the last one's. */
	if (g_gfx_finished != g_gfx_tasks && g_gfx_yield != GFX_YIELDED && !(video_signal_ucode() && g_gfx_event)) {
		double ahead = player_ahead_us();
		if (ahead > 20) {
			double t = report_now_us();
			video_wait_task(g_gfx_finished, ahead);
			g_gfx_event_wait_us += report_now_us() - t;
			raised |= gfx_finish_done();
		}
	}
	/* A ZSortBOSS task's first piece runs beside the CPU; past ZSORT_START_MAX
	 * by the next poll, the CPU waits for it now. */
	if (video_signal_ucode() && !g_gfx_event && g_gfx_finished != g_gfx_tasks && g_gfx_yield != GFX_YIELDED &&
	    reg_cop0[9] - g_zsort_start + gfx_poll_delay() > ZSORT_START_MAX) {
		double t = report_now_us();
		video_wait_task(g_gfx_finished, -1.0);
		g_gfx_event_wait_us += report_now_us() - t;
		raised |= gfx_finish_done();
	}
	/* gfx_finish_done may have posted a task (gfx_task) that queued its own event. */
	if (!g_gfx_event && (g_gfx_finished != g_gfx_tasks || g_rsp_spinning || g_gb_busy || g_gb_held || g_au_busy)) { /* spinning: watch for a yield request */
		add_interupt_event(GFX_INT, gfx_poll_delay());
		g_gfx_event = 1;
	}
	return raised;
}

double gfx_event_wait_us(void) { return g_gfx_event_wait_us; }

/* Before a save state loads or the game resets outside go(): every task
 * posted is decoded and forgotten. The new state has none in flight (states
 * are captured only when gfx_idle) and no GFX_INT event queued. GLideN64
 * forgets the display list it was in (video.cpp). */
void gfx_reset(void)
{
	video_sync();
	video_reset_rsp();
	gb_quiet();
	audio_drop();
	ai_stream_reset();
	g_gfx_finished = g_gfx_tasks;
	g_gfx_event = 0;
	g_gfx_yield = GFX_RUNNING;
	g_rsp_spinning = g_spin_yielded = 0;
	g_gfx_seen = 0;
}

static void gfx_task(int resume)
{
	if (player_capturing() || player_sync_before())
		video_sync(); /* the capture holds RDRAM as the task finds it, without the decoder writing to it */
	player_capture_task(g_gfx_tasks);
	/* video.cpp keeps VIDEO_RESULTS results; deliver old ones before they go. */
	if (g_gfx_tasks - g_gfx_finished >= VIDEO_RESULTS / 2) {
		video_wait_task(g_gfx_finished, -1.0);
		if (gfx_finish_done())
			check_interupt();
	}
	video_submit_task();
	g_gfx_tasks++;
	if (!resume)
		g_zsort_start = reg_cop0[9];
	g_rsp_spinning = g_spin_yielded = 0; /* a restart or a resume carries on from the branch */
	/* What the HLE plugin does at the end of a graphics task: unfreeze the RDP. */
	dpc_register.dpc_status &= ~0x0002;
	/* ZSortBOSS: a piece the game resumes (to the next signal wait) gets its
	 * interrupts ZSORT_INT_DELAY after it is posted, as in mupen64plus. At
	 * the next GFX_INT poll (1/32 VI, ~24600 Count) Stunt Racer reused its
	 * frame's memory while still clipping triangles from it: misplaced
	 * triangles, reproduced on the PC with mupen64plus's SP interrupt 20000+
	 * late after a resume that cleared signal 3; a task's first piece drew
	 * clean up to 200000 late (ZSORT_START_MAX, 2026-10-28). The CPU waits
	 * for the graphics core here, only for resumes: each follows a decoded
	 * piece, so the wait is this piece's alone. */
	if (resume && video_signal_ucode()) {
		video_wait_task(g_gfx_tasks - 1, -1.0);
		if (g_gfx_event) {
			/* Not64's remove_event leaves next_interupt at a removed head:
			 * the next event would then fire that early. */
			int type;
			unsigned long ahead;
			if (our64_events(&type, &ahead, 1) == 1 && type == GFX_INT)
				remove_interupt_event();
			else
				remove_event(GFX_INT);
		}
		add_interupt_event(GFX_INT, ZSORT_INT_DELAY);
		g_gfx_event = 1;
		return;
	}
	if (player_sync_gfx())
		video_wait_task(g_gfx_tasks - 1, -1.0);
	if (!g_gfx_event) {
		add_interupt_event(GFX_INT, player_sync_gfx() ? 100 : gfx_poll_delay());
		g_gfx_event = 1;
	}
}

static void read_controller(int Control, BYTE *Command) { (void)Control; (void)Command; }
static void get_keys(int Control, BUTTONS *Keys) { Keys->Value = input_keys(Control); }
static void controller_command(int Control, BYTE *Command) { (void)Control; (void)Command; }

/* Not64 links its plugins statically, so these are plain functions. */
void processDList(void) {} /* graphics tasks are taken in __wrap_doRspCycles */
void processAList(void) {}
/* The game wrote DPC_END: RDP commands it sent without an RSP task. GLideN64
 * draws them on the graphics core in order with the tasks; the game sees
 * them taken at once (Not64 raises the DP interrupt next). */
void processRDPList(void)
{
	video_submit_rdp_list();
	dpc_register.dpc_current = dpc_register.dpc_end;
}
void showCFB(void) {}
void updateScreen(void)
{
	start_section(SCREEN_SECTION);
	video_update_screen();
	end_section(SCREEN_SECTION);
}
void viStatusChanged(void) { video_vi_status_changed(); }
void viWidthChanged(void) { video_vi_width_changed(); }
void aiDacrateChanged(int SystemType)
{
	unsigned clock = SystemType == SYSTEM_PAL ? 49656530 : SystemType == SYSTEM_MPAL ? 48628316 : 48681812;
	audio_set_rate(clock / (ai_register.ai_dacrate + 1));
}

/* AI_LEN is in bytes, 4 per stereo frame. */
/* ---- audio dump (sd:/wiiu/our64/audio-dump): the last AUDIO_DUMP_BYTES of
 * what the game gave the AI, each piece with Count, VI, time, its address
 * and length, and a sum of the previous piece's bytes read again now (it
 * differs from the samples kept when the game or the RSP wrote them after
 * the AI took them). Written to sd:/wiiu/our64/audio-dump.bin when the game
 * stops; tools/audio-dump.py reads it. Stunt Racer stutters (2026-10-28). */
#define AUDIO_DUMP_BYTES (6 << 20)
#define AUDIO_DUMP_PIECES 8192
typedef struct { uint32_t count, vi, addr, len, prev_sum, at, t_hi, t_lo; } audio_dump_piece;
int g_audio_dump_on;
static uint8_t *g_ad_bytes;
static audio_dump_piece *g_ad_pieces;
static uint32_t g_ad_wr, g_ad_n;       /* bytes and pieces written, ever */
static uint32_t g_ad_prev_addr, g_ad_prev_len;

static uint32_t audio_dump_sum(uint32_t addr, uint32_t len)
{
	uint32_t s = 0;
	for (uint32_t i = 0; i < len; i += 4)
		s = s * 31 + *(const uint32_t *)((char *)rdram + addr + i);
	return s;
}

static void audio_dump_piece_add(uint32_t addr, uint32_t len)
{
	if (!g_ad_bytes) {
		g_ad_bytes = malloc(AUDIO_DUMP_BYTES);
		g_ad_pieces = malloc(AUDIO_DUMP_PIECES * sizeof(audio_dump_piece));
		if (!g_ad_bytes || !g_ad_pieces) {
			free(g_ad_bytes), free(g_ad_pieces);
			g_ad_bytes = NULL, g_ad_pieces = NULL;
			g_audio_dump_on = 0;
			return;
		}
	}
	const uint64_t t = OSGetSystemTime();
	audio_dump_piece *p = &g_ad_pieces[g_ad_n++ % AUDIO_DUMP_PIECES];
	*p = (audio_dump_piece){reg_cop0[9], player_vis(), addr, len,
	                        g_ad_prev_len ? audio_dump_sum(g_ad_prev_addr, g_ad_prev_len) : 0,
	                        g_ad_wr, (uint32_t)(t >> 32), (uint32_t)t};
	for (uint32_t i = 0; i < len; i++)
		g_ad_bytes[(g_ad_wr + i) % AUDIO_DUMP_BYTES] = ((const uint8_t *)rdram)[addr + i];
	g_ad_wr += len;
	g_ad_prev_addr = addr, g_ad_prev_len = len;
}

void audio_dump_write(void)
{
	if (!g_ad_bytes || !g_ad_n)
		return;
	char path[256];
	snprintf(path, sizeof(path), "%s/audio-dump.bin", report_sd_root());
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	/* The pieces whose bytes are all still in the ring, oldest first. */
	uint32_t first = g_ad_n > AUDIO_DUMP_PIECES ? g_ad_n - AUDIO_DUMP_PIECES : 0;
	while (first < g_ad_n && g_ad_wr - g_ad_pieces[first % AUDIO_DUMP_PIECES].at > AUDIO_DUMP_BYTES)
		first++;
	unsigned kept = 0;
	for (uint32_t k = first; k < g_ad_n; k++, kept++) {
		const audio_dump_piece *p = &g_ad_pieces[k % AUDIO_DUMP_PIECES];
		fwrite(p, sizeof(*p), 1, f);
		const uint32_t start = p->at % AUDIO_DUMP_BYTES;
		const uint32_t first_part = p->len < AUDIO_DUMP_BYTES - start ? p->len : AUDIO_DUMP_BYTES - start;
		fwrite(g_ad_bytes + start, 1, first_part, f);
		fwrite(g_ad_bytes, 1, p->len - first_part, f); /* wrapped around the ring */
	}
	fclose(f);
	report_line("AUDIO DUMP: %u pieces written to audio-dump.bin", kept);
	free(g_ad_bytes), free(g_ad_pieces);
	g_ad_bytes = NULL, g_ad_pieces = NULL;
	g_ad_n = g_ad_wr = g_ad_prev_len = 0;
}

static void ai_push(uint32_t addr, uint32_t len)
{
	if (!len)
		return;
	if (g_audio_dump_on)
		audio_dump_piece_add(addr, len);
	audio_push((const uint32_t *)((char *)rdram + addr), len / 4);
}

/* ---- AI DMA as it plays (g_ai_stream): Stunt Racer gives the AI its whole
 * 16 KB ring again and again (osAiSetNextBuffer at 0x80055B78) and mixes into
 * it just ahead of where osAiGetLength says the DMA is. Taken whole when the
 * buffer is queued, most of it was up to 186 ms old: crackle and lagging
 * chunks; 306 of 332 buffers changed after they were taken (audio-dump,
 * 2026-10-28). Here each buffer's bytes are taken as the DMA would have read
 * them by then (every VI and at each AI_LEN write), as mupen64plus does on
 * AI_LEN reads. Timed in Count as Not64 times its AI interrupt (write_ai:
 * the second buffer starts when the first ends). */
int g_ai_stream;
typedef struct { uint32_t addr, len, delay, start, taken; int on; } ai_dma;
static ai_dma g_ai_cur, g_ai_next;
static uint32_t g_ai_written_addr;

void ai_stream_tick(void)
{
	while (g_ai_cur.on) {
		const uint32_t elapsed = reg_cop0[9] - g_ai_cur.start;
		const int done = elapsed >= g_ai_cur.delay;
		const uint32_t upto = done ? g_ai_cur.len :
		                      (uint32_t)((uint64_t)elapsed * g_ai_cur.len / g_ai_cur.delay) & ~3u;
		if (upto > g_ai_cur.taken) {
			ai_push(g_ai_cur.addr + g_ai_cur.taken, upto - g_ai_cur.taken);
			g_ai_cur.taken = upto;
		}
		if (!done)
			return;
		const uint32_t end = g_ai_cur.start + g_ai_cur.delay;
		g_ai_cur = g_ai_next;
		g_ai_cur.start = end;
		g_ai_next.on = 0;
	}
}

static void ai_stream_queue(uint32_t len, uint32_t delay, int busy)
{
	ai_stream_tick();
	const uint32_t addr = g_ai_written_addr;
	if (!len || addr + len > 0x800000)
		return;
	const ai_dma d = {addr, len, delay, reg_cop0[9], 0, 1};
	/* A DMA in flight we never saw (a loaded state): rebuilt from Not64's
	 * registers so the new one starts when it ends, as Not64 starts it. Its
	 * address is not kept, so its bytes are skipped. */
	const uint32_t ai_end = get_event(AI_INT);
	if (busy && !g_ai_cur.on && ai_end && ai_register.current_delay &&
	    ai_end - reg_cop0[9] <= ai_register.current_delay) {
		g_ai_cur = (ai_dma){0, ai_register.current_len & 0x3FFF8, ai_register.current_delay,
		                    ai_end - ai_register.current_delay, 0, 1};
		g_ai_cur.taken = g_ai_cur.len;
	}
	if (busy && g_ai_cur.on)
		g_ai_next = d; /* starts when the current one ends (ai_stream_tick) */
	else
		g_ai_cur = d;
	ai_stream_tick(); /* a buffer with no duration (no_audio_delay) goes at once */
}

void ai_stream_reset(void)
{
	g_ai_cur.on = g_ai_next.on = 0;
}

void aiLenChanged(void)
{
	uint32_t addr = ai_register.ai_dram_addr & 0x7FFFF8, len = ai_register.ai_len & 0x3FFF8;
	if (g_ai_stream) {
		g_ai_written_addr = addr; /* queued in our64_note_ai, which knows the duration */
		return;
	}
	if (addr + len <= 0x800000)
		ai_push(addr, len);
}
DWORD aiReadLength(void) { return 0; }
void controllerCommand(int Control, BYTE *Command) { controller_command(Control, Command); }
void getKeys(int Control, BUTTONS *Keys) { get_keys(Control, Keys); }
void readController(int Control, BYTE *Command) { read_controller(Control, Command); }
void (*fBRead)(DWORD addr) = 0;
void (*fBWrite)(DWORD addr, DWORD size) = 0;
void (*fBGetFrameBufferInfo)(void *p) = 0;

CONTROL Controls[4];
extern char padType[4]; /* defined below */

static void dummy_check(void) {}

void player_plugins_init(void)
{
	static RSP_INFO rsp_info;
	static DWORD cycle_count;
	rsp_info.MemoryBswaped = TRUE;
	rsp_info.RDRAM = (BYTE *)rdram;
	rsp_info.DMEM = (BYTE *)SP_DMEM;
	rsp_info.IMEM = (BYTE *)SP_IMEM;
	rsp_info.MI_INTR_REG = &MI_register.mi_intr_reg;
	rsp_info.SP_MEM_ADDR_REG = &sp_register.sp_mem_addr_reg;
	rsp_info.SP_DRAM_ADDR_REG = &sp_register.sp_dram_addr_reg;
	rsp_info.SP_RD_LEN_REG = &sp_register.sp_rd_len_reg;
	rsp_info.SP_WR_LEN_REG = &sp_register.sp_wr_len_reg;
	rsp_info.SP_STATUS_REG = &sp_register.sp_status_reg;
	rsp_info.SP_DMA_FULL_REG = &sp_register.sp_dma_full_reg;
	rsp_info.SP_DMA_BUSY_REG = &sp_register.sp_dma_busy_reg;
	rsp_info.SP_PC_REG = &rsp_register.rsp_pc;
	rsp_info.SP_SEMAPHORE_REG = &sp_register.sp_semaphore_reg;
	rsp_info.DPC_START_REG = &dpc_register.dpc_start;
	rsp_info.DPC_END_REG = &dpc_register.dpc_end;
	rsp_info.DPC_CURRENT_REG = &dpc_register.dpc_current;
	rsp_info.DPC_STATUS_REG = &dpc_register.dpc_status;
	rsp_info.DPC_CLOCK_REG = &dpc_register.dpc_clock;
	rsp_info.DPC_BUFBUSY_REG = &dpc_register.dpc_bufbusy;
	rsp_info.DPC_PIPEBUSY_REG = &dpc_register.dpc_pipebusy;
	rsp_info.DPC_TMEM_REG = &dpc_register.dpc_tmem;
	rsp_info.CheckInterrupts = dummy_check;
	rsp_info.ProcessDlistList = none;
	rsp_info.ProcessAlistList = none;
	rsp_info.ProcessRdpList = none;
	rsp_info.ShowCFB = none;
	initiateRSP(rsp_info, &cycle_count); /* Not64's rsp_hle (mupen64plus-rsp-hle), which also provides doRspCycles */
	/* It keeps which code each microcode address runs, until a ROM closes:
	 * one session plays many games, so each starts with none known. */
	extern void romClosed_RSP(void);
	romClosed_RSP();

	for (int i = 0; i < 4; i++) {
		Controls[i].Present = i == 0; /* port 1 always; the others once a controller joins */
		Controls[i].RawData = FALSE;
		/* A Controller Pak in every port, as mupen64plus does: games that save
		 * only there (and Mario Kart's ghosts) need one. Saved as the .mpk. */
		Controls[i].Plugin = PLUGIN_MEMPAK;
		/* Non-zero: the PIF answers through internal_ControllerCommand and
		 * internal_ReadController (getKeys), which mark controllers 2-4 as
		 * not connected. Zero would send them to Not64's Wii controller code,
		 * a stub here: no answer, so libultra's 0xFF fill read as every
		 * button held (Mario Golf's menus scrolled on their own). */
		padType[i] = 1;
	}
}

/* mupen64plus.ini's boot patches ("Allows the game to boot"): 16-bit writes
 * mupen64plus makes at the game's first VI. Rat Attack! sent no graphics task
 * without its pointer (2026-10-29): the game reads it before its own code at
 * 0x800572B8 stores it. Its boot code clears 0x100000-0x1856BC, after
 * mupen64plus's first VI but not after Our64's, so a patch is made again at
 * each of the first 60 VIs while the word still reads zero. */
static const struct { uint32_t crc1, crc2, addr; uint16_t value; } g_boot_patches[] = {
	{ 0x0304C48E, 0xAC4001B8, 0x17E34C, 0x8014 }, { 0x0304C48E, 0xAC4001B8, 0x17E34E, 0xA8F0 }, /* Rat Attack! (USA) */
	{ 0x20FD0BF1, 0xF5CF1D87, 0x17DAEC, 0x8014 }, { 0x20FD0BF1, 0xF5CF1D87, 0x17DAEE, 0xA1D0 }, /* Rat Attack (Europe) */
};
static int g_boot_patched; /* VIs since the game started, up to 60 */

static void boot_patches_vi(void)
{
	if (g_boot_patched >= 60)
		return;
	g_boot_patched++;
	for (unsigned i = 0; i < sizeof(g_boot_patches) / sizeof(g_boot_patches[0]); i++)
		if (g_boot_patches[i].crc1 == ROM_HEADER.CRC1 && g_boot_patches[i].crc2 == ROM_HEADER.CRC2 &&
		    rdram[(g_boot_patches[i].addr & ~3u) / 4] == 0) {
			/* both halves of the word, so the second half's check does not see the first half's write */
			for (unsigned j = i; j < sizeof(g_boot_patches) / sizeof(g_boot_patches[0]) &&
			     (g_boot_patches[j].addr & ~3u) == (g_boot_patches[i].addr & ~3u); j++)
				((uint16_t *)rdram)[g_boot_patches[j].addr / 2] = g_boot_patches[j].value;
			report_line("PATCH: VI %d: 0x%06X = 0x%08lX (mupen64plus.ini: allows the game to boot)", g_boot_patched,
			            (unsigned)(g_boot_patches[i].addr & ~3u), rdram[(g_boot_patches[i].addr & ~3u) / 4]);
		}
}

/* Each port's Pak for this game (gamedb.h): a Rumble Pak or a Controller Pak. */
static unsigned g_pak_traced; /* Pak reads and writes logged for this game */
static unsigned g_pak_probed_vi[4]; /* osMotorInit's last probe (0xFE at 0x8000), +1; 0 never */
static unsigned g_pak_used_vi[4]; /* the game's last use of a Controller Pak */
static int g_pak_pulled[4];       /* the next status read says the Pak was pulled out */
static unsigned g_pak_keep_until[4]; /* the VI until which ID checks find the Rumble Pak */
static int g_pak_rumble_only; /* player_pak_rumble_only: the game's Pak checks never get a Controller Pak */

/* A new game: no Pak state carries over from the one before (NFL Blitz 2000
 * left a pulled-out report pending, and Destruction Derby, next, read its
 * Pak as pulled out at its first check). */
void player_set_paks(const int *pak)
{
	g_pak_traced = 0; /* a new game: its own 150 */
	memset(g_pak_probed_vi, 0, sizeof(g_pak_probed_vi));
	memset(g_pak_pulled, 0, sizeof(g_pak_pulled));
	memset(g_pak_keep_until, 0, sizeof(g_pak_keep_until));
	g_pak_rumble_only = 0;
	g_boot_patched = 0;
	for (int i = 0; i < 4; i++)
		g_pak_used_vi[i] = player_vis();
	for (int i = 0; i < 4; i++)
		player_set_pak(i, pak[i]);
}

/* With rumble on in a game that has it, the Pak follows what the game asks
 * of it (Zach, 2026-10-20: games kept asking for one or the other): the motor
 * makes it a Rumble Pak, the data area a Controller Pak, at once, before the
 * game's command is answered. osMotorInit's probe changes nothing: it asks
 * which Pak is there (NFL Blitz 2000 probes every few seconds). */
static int g_pak_auto[4];

/* Between VIs only (the pause menu's thread is the CPU's): the game sees the
 * new Pak at its next controller status read. */
/* Chameleon Twist saves to the cartridge, yet reads 0x0020 and write-tests
 * the data area as it looks for its Rumble Pak: swapped for a Controller Pak
 * there, it said the Rumble Pak was malfunctioning (2026-10-29). After
 * player_set_paks. */
void player_pak_rumble_only(void)
{
	g_pak_rumble_only = 1;
}

void player_set_pak(int port, int pak)
{
	if (port >= 0 && port < 4) {
		Controls[port].Plugin = pak == 1 ? PLUGIN_RUMBLE_PAK : PLUGIN_MEMPAK; /* 1: PAK_RUMBLE */
		g_pak_auto[port] = pak == 1;
	}
}

/* Every Pak read and write, the first 150 of each game (Gauntlet Legends never rumbled,
 * 2026-10-20: in what order does a game look for each Pak?). */
void our64_pak_trace(int port, int write, int address, int value)
{
	if (write && address == 0x8000 && value == 0xFE && port >= 0 && port < 4)
		g_pak_probed_vi[port] = player_vis() + 1;
	if (g_pak_traced++ < 150)
		report_line("PAK: VI %u port %d %s 0x%04X%s%s", player_vis(), port + 1, write ? "write" : "read", address,
		            write ? (value == 0xFE ? " 0xFE" : value == 0x80 ? " 0x80" : value == 0 ? " 0x00" : " data") : "",
		            Controls[port].Plugin == PLUGIN_RUMBLE_PAK ? " (holding a Rumble Pak)" :
		            Controls[port].Plugin == PLUGIN_MEMPAK ? " (holding a Controller Pak)" :
		            Controls[port].Plugin == PLUGIN_TANSFER_PAK ? " (holding a Transfer Pak)" : " (holding nothing)");
}

/* ---- the Transfer Pak (tpak.c): port 1, in a game that takes one, when a
 * Game Boy game's save was found (tpak_wiiu.c). Like the other Paks it is
 * there when the game asks for it: libultra's osGbpakInit powers the
 * cartridge with 0x84 at 0x8000, and the port holds the Transfer Pak from
 * that write on. osMotorInit's 0x80 there, or a motor command (0xC000) long
 * after the last cartridge access, gives the port its own Pak back. ---- */
static tpak g_tpak;
static int g_tpak_in;           /* a cartridge is in port 1's Transfer Pak */
static unsigned g_tpak_used_vi; /* the game's last Transfer Pak access */

void player_tpak_insert(gb_cart *cart)
{
	tpak_power_on(&g_tpak, cart);
	g_tpak_in = cart != NULL;
	if (!g_tpak_in && Controls[0].Plugin == PLUGIN_TANSFER_PAK)
		Controls[0].Plugin = g_pak_auto[0] ? PLUGIN_RUMBLE_PAK : PLUGIN_MEMPAK;
}

static void tpak_give_back(const char *why)
{
	Controls[0].Plugin = g_pak_auto[0] ? PLUGIN_RUMBLE_PAK : PLUGIN_MEMPAK;
	static unsigned told;
	if (told++ < 20)
		report_line("PAK: VI %u port 1 has its %s back (%s)", player_vis(),
		            g_pak_auto[0] ? "Rumble Pak" : "Controller Pak", why);
}

void our64_tpak_wanted(int port, int address, const unsigned char *data)
{
	if (port != 0 || !g_tpak_in)
		return;
	const unsigned char v = data[31];
	if (Controls[0].Plugin != PLUGIN_TANSFER_PAK) {
		if (address == 0x8000 && v == 0x84) {
			Controls[0].Plugin = PLUGIN_TANSFER_PAK;
			g_tpak_used_vi = player_vis();
			static unsigned told;
			if (told++ < 20)
				report_line("PAK: VI %u port 1 now has the Transfer Pak (the game powered its cartridge)", player_vis());
		}
		return;
	}
	if (address == 0x8000 && v == 0x80) {
		tpak_give_back("the game looked for a Rumble Pak");
		return;
	}
	/* A motor command is 32 bytes of 0x00 or 0x01 at 0xC000. The cartridge
	 * gets writes there too, but only right after the game picked its bank. */
	if (address == 0xC000 && (v == 0x00 || v == 0x01) && player_vis() - g_tpak_used_vi > 30) {
		int same = 1;
		for (int i = 0; i < 32; i++)
			same &= data[i] == v;
		if (same) {
			tpak_give_back("a motor command");
			return;
		}
	}
	g_tpak_used_vi = player_vis();
}

void our64_tpak_read(int port, int address, unsigned char *data)
{
	if (port != 0 || !g_tpak_in) {
		memset(data, 0, 32);
		return;
	}
	g_tpak_used_vi = player_vis();
	tpak_read(&g_tpak, (uint16_t)address, data);
}

void our64_tpak_write(int port, int address, const unsigned char *data)
{
	if (port != 0 || !g_tpak_in)
		return;
	g_tpak_used_vi = player_vis();
	tpak_write(&g_tpak, (uint16_t)address, data);
}

void our64_pak_wanted(int port, int rumble, int address)
{
	if (port < 0 || port >= 4 || !g_pak_auto[port])
		return;
	if (rumble && Controls[port].Plugin == PLUGIN_TANSFER_PAK)
		return; /* the cartridge's, or our64_tpak_wanted saw a motor command */
	if (rumble && address == 0xC000) {
		static unsigned motor_told;
		if (motor_told++ < 6)
			report_line("PAK: port %d motor %s", port + 1, Controls[port].Plugin == PLUGIN_RUMBLE_PAK ? "command" : "command (now a Rumble Pak)");
	}
	/* The game's check right after the swap back (it reads the Controller
	 * Pak's ID, 0x0020, before it ever probes for a motor: Gauntlet Legends)
	 * finds the Rumble Pak, so the game goes on to look for the motor. A
	 * check after that, as the game saves, gets the Controller Pak. */
	if (!rumble && g_pak_rumble_only)
		return;
	if (!rumble && address < 0x100 && Controls[port].Plugin == PLUGIN_RUMBLE_PAK &&
	    (int)(g_pak_keep_until[port] - player_vis()) > 0) {
		static unsigned kept;
		if (kept++ < 20)
			report_line("PAK: port %d keeps its Rumble Pak for the game's check after the swap back (0x%04X)", port + 1, address);
		return;
	}
	if (!rumble) {
		g_pak_used_vi[port] = player_vis();
		g_pak_pulled[port] = 0; /* the game is using its Controller Pak: a pulled-out report now would fail it */
	}
	const int want = rumble ? PLUGIN_RUMBLE_PAK : PLUGIN_MEMPAK;
	if (Controls[port].Plugin == want)
		return;
	Controls[port].Plugin = want;
	static unsigned told;
	if (told++ < 40)
		report_line("PAK: port %d now has a %s (the game %s 0x%04X)", port + 1, rumble ? "Rumble Pak" : "Controller Pak",
		            rumble ? "wrote" : "used", address);
}

void player_port_present(int port, int present)
{
	if (port >= 0 && port < 4)
		Controls[port].Present = port == 0 || present;
}

void player_get_memory(player_memory *m)
{
	m->rdram = (unsigned char *)rdram;
	m->dmem = (unsigned char *)SP_DMEM;
	m->imem = (unsigned char *)SP_IMEM;
	m->header = (unsigned char *)&ROM_HEADER;
	m->mi_intr = (unsigned int *)&MI_register.mi_intr_reg;
	m->sp_status = (unsigned int *)&sp_register.sp_status_reg;
	unsigned long *dpc[8] = {&dpc_register.dpc_start, &dpc_register.dpc_end, &dpc_register.dpc_current,
	                         &dpc_register.dpc_status, &dpc_register.dpc_clock, &dpc_register.dpc_bufbusy,
	                         &dpc_register.dpc_pipebusy, &dpc_register.dpc_tmem};
	for (int i = 0; i < 8; i++) m->dpc[i] = (unsigned int *)dpc[i];
	unsigned long *vi = &vi_register.vi_status; /* 14 consecutive words, in GFX_INFO's order */
	for (int i = 0; i < 14; i++) m->vi[i] = (unsigned int *)(vi + i);
}

/* ---- RSP task log: the HLE plugin skips tasks it does not know, and a
 * game waiting on one of those hangs. Linked with --wrap=doRspCycles. ---- */

DWORD __real_doRspCycles(DWORD Cycles);
static unsigned g_tasks[4]; /* graphics, audio, other, GB Tower */

/* ---- Pokemon Stadium's and Stadium 2's GB Tower: the Game Boy CPU runs on
 * the N64 CPU, and an RSP program of its own (an M_AUDTASK, no HLE code for
 * it) turns each picture into framebuffer pixels. It runs on cxd4's
 * interpreter (rsp_lle.c), to its end, inside the CPU's write that starts it.
 *
 * Early on the program sets SIG3 ("input read"); at its end it waits until
 * SIG3 is clear, sets SIG2 (task done) and breaks. The CPU waits for SIG3,
 * then clears it. Run to its end at once, the program would wait forever for
 * a CPU that has not run yet, so inside it MFC0 reads SIG3 as clear (as the
 * Stadium recompilation does) while the register keeps SIG3 set for the CPU.
 *
 * The task's end (halt, broke, SIG2 and the SP interrupt) is held until the
 * CPU clears SIG3, as on the console, where the RSP is still waiting then.
 * Ended at once, the SP interrupt came first, and libultra's handler for it
 * clears SIG3 (it writes 0x8008): the game never saw "input read" and waited
 * at 0x80154D18 forever (Stadium 2, 2026-10-21). The hold needs no state of
 * its own: the RSP running with this program in IMEM is the held task, so a
 * resume point taken meanwhile ends it the same way. ---- */
static unsigned g_gb_reports;
static unsigned g_gb_window_vi, g_gb_window_tasks;
static double g_gb_window_start_us, g_gb_window_us, g_gb_window_worker_us, g_gb_window_blocked_us;
static unsigned long long g_gb_window_instructions;
static unsigned g_gb_window_async;

/* After SIG3 (234 instructions in; a picture takes 320 000-380 000), the rest
 * of the task runs on core 2 against copies of the RSP registers, while the
 * CPU runs on: on the CPU core the whole task took 15 ms, 57-80% of the CPU's
 * time in the GB Tower (2026-10-21). Its end is held until the RSP of a
 * console would have finished (kGbRspCounts after the start: about 420 000
 * RSP cycles at 62.5 MHz, in Count's 46.875 MHz) and core 2 has; the CPU waits
 * for core 2 only past that point. The game's scheduler drops a GB task
 * submitted while the last has not ended, so its end must not come later than
 * a console's by much. */
enum { kGbRspCounts = 330000 };
static OSThread g_gb_thread;
static uint8_t g_gb_stack[16 * 1024] __attribute__((aligned(16)));
static OSEvent g_gb_go, g_gb_done;
static int g_gb_thread_ok;
static rsp_lle_regs g_gb_real, g_gb_copy;
static uint32_t g_gb_copy_sp[8], g_gb_copy_dpc[8], g_gb_copy_mi, g_gb_copy_pc;
static unsigned long g_gb_due;        /* Count when a console's RSP would be done */
static unsigned long g_gb_first;      /* instructions before SIG3 */
static volatile unsigned long g_gb_rest_n;
static volatile int g_gb_rest_result;
static volatile double g_gb_rest_us;
static volatile int g_gb_rest_done;
static uint32_t g_gb_boot_size;

static int gb_worker(int argc, const char **argv)
{
	(void)argc, (void)argv;
	for (;;) {
		OSWaitEvent(&g_gb_go);
		const double t = report_now_us();
		int result;
		g_gb_rest_n = rsp_lle_run(0x400, 50000000, 0, &result);
		g_gb_rest_result = result;
		g_gb_rest_us = report_now_us() - t;
		OSMemoryBarrier();
		g_gb_rest_done = 1;
		OSSignalEvent(&g_gb_done);
	}
	return 0;
}

static void gb_ready(void)
{
	static int ready;
	if (ready)
		return;
	ready = 1;
	_Static_assert(sizeof(unsigned long) == sizeof(uint32_t), "Not64's registers are 32-bit words");
	g_gb_real = (rsp_lle_regs){
		(unsigned char *)rdram, (unsigned char *)SP_DMEM, (unsigned char *)SP_IMEM,
		(uint32_t *)&MI_register.mi_intr_reg,
		{(uint32_t *)&sp_register.sp_mem_addr_reg, (uint32_t *)&sp_register.sp_dram_addr_reg,
		 (uint32_t *)&sp_register.sp_rd_len_reg, (uint32_t *)&sp_register.sp_wr_len_reg,
		 (uint32_t *)&sp_register.sp_status_reg, (uint32_t *)&sp_register.sp_dma_full_reg,
		 (uint32_t *)&sp_register.sp_dma_busy_reg, (uint32_t *)&sp_register.sp_semaphore_reg},
		(uint32_t *)&rsp_register.rsp_pc,
		{(uint32_t *)&dpc_register.dpc_start, (uint32_t *)&dpc_register.dpc_end,
		 (uint32_t *)&dpc_register.dpc_current, (uint32_t *)&dpc_register.dpc_status,
		 (uint32_t *)&dpc_register.dpc_clock, (uint32_t *)&dpc_register.dpc_bufbusy,
		 (uint32_t *)&dpc_register.dpc_pipebusy, (uint32_t *)&dpc_register.dpc_tmem},
		dummy_check, /* as the HLE's: the interrupt is seen at the next check */
	};
	g_gb_copy = g_gb_real;
	g_gb_copy.mi_intr = &g_gb_copy_mi;
	for (int i = 0; i < 8; i++) {
		g_gb_copy.sp[i] = &g_gb_copy_sp[i];
		g_gb_copy.dpc[i] = &g_gb_copy_dpc[i];
	}
	g_gb_copy.sp_pc = &g_gb_copy_pc;
	rsp_lle_init(&g_gb_real);
	OSInitEvent(&g_gb_go, FALSE, OS_EVENT_MODE_AUTO);
	OSInitEvent(&g_gb_done, FALSE, OS_EVENT_MODE_AUTO);
	g_gb_thread_ok = OSCreateThread(&g_gb_thread, (OSThreadEntryPointFn)gb_worker, 0, NULL, g_gb_stack + sizeof(g_gb_stack), sizeof(g_gb_stack),
	                                20, OS_THREAD_ATTRIB_AFFINITY_CPU2);
	if (g_gb_thread_ok) {
		OSSetThreadName(&g_gb_thread, "Our64 GB Tower RSP");
		OSResumeThread(&g_gb_thread);
	} else {
		report_line("GB Tower: no thread on core 2; tasks run on the CPU core");
	}
}

/* The end of a task: SIG2 (task done), broke, halt and the SP interrupt, or,
 * stopped before its BREAK, halt and broke as the HLE ends a task it does not
 * know (the game is not left waiting). */
static int gb_end(int broke)
{
	sp_register.sp_status_reg |= broke ? 0x203 : 0x3;
	if (sp_register.sp_status_reg & 0x40) {
		MI_register.mi_intr_reg |= 0x01;
		return 1;
	}
	return 0;
}

static void gb_window(double us, unsigned long n);

/* Takes the task back from core 2, waiting for it if need be. */
static void gb_collect(void)
{
	if (!g_gb_busy)
		return;
	double blocked = 0;
	if (!g_gb_rest_done) {
		const double t = report_now_us();
		OSWaitEvent(&g_gb_done);
		blocked = report_now_us() - t;
	} else {
		OSWaitEvent(&g_gb_done); /* set already: this only resets it */
	}
	OSMemoryBarrier();
	rsp_lle_bind(&g_gb_real);
	g_gb_busy = 0;
	for (int i = 0; i < 4; i++) /* the DMA registers; DMA_FULL, DMA_BUSY and SEMAPHORE stay */
		g_gb_real.sp[i][0] = g_gb_copy_sp[i];
	rsp_register.rsp_pc = g_gb_copy_pc & 0xFFF;
	const int broke = g_gb_rest_result == RSP_LLE_BROKE;
	/* The end, unless SIG3 is still set: then once the CPU clears it. */
	if (!broke || !(sp_register.sp_status_reg & 0x400))
		gb_end(broke);
	g_gb_window_async++;
	g_gb_window_worker_us += g_gb_rest_us;
	g_gb_window_blocked_us += blocked;
	gb_window(blocked, g_gb_first + g_gb_rest_n);
	if (!broke) {
		static unsigned told;
		if (told++ < 8)
			report_line("GB Tower: RSP task (boot size 0x%X) stopped on core 2 before its end, after %lu instructions",
			            (unsigned)g_gb_boot_size, g_gb_first + g_gb_rest_n);
	}
}

/* At each GFX_INT event and VI: core 2's task ends once a console's would have. */
static int gb_poll(void)
{
	if (g_gb_held && (long)(reg_cop0[9] - g_gb_due) >= 0) {
		/* Done in C: its end now, or once the CPU clears SIG3 (our64_sp_status_written). */
		g_gb_held = 0;
		return !(sp_register.sp_status_reg & 0x400) && gb_end(1);
	}
	if (!g_gb_busy || (long)(reg_cop0[9] - g_gb_due) < 0)
		return 0;
	const uint32_t mi = MI_register.mi_intr_reg;
	gb_collect();
	return (MI_register.mi_intr_reg & ~mi) != 0;
}

/* Before a save state loads, a reset or the game's end: nothing on core 2. */
static void gb_quiet(void)
{
	g_gb_held = 0; /* a loaded state ends a held task when SIG3 is cleared */
	if (!g_gb_busy)
		return;
	OSWaitEvent(&g_gb_done);
	rsp_lle_bind(&g_gb_real);
	g_gb_busy = 0;
}

static void gb_task(uint32_t boot_size)
{
	gb_ready();
	gb_quiet(); /* not expected: a task starts only after the last one ended */
	g_tasks[3]++;
	g_gb_boot_size = boot_size;
	const double t = report_now_us();
	if (gb_tower_hle((unsigned char *)rdram, (unsigned char *)SP_DMEM)) {
		/* In C (gb_tower_hle.c), whole: the program's SIG3 ("input read") now,
		 * its end once a console's RSP would be done and SIG3 is clear. */
		const double us = report_now_us() - t;
		sp_register.sp_status_reg |= 0x400;
		g_gb_due = reg_cop0[9] + kGbRspCounts; /* Count */
		g_gb_held = 1;
		if (!g_gfx_event) { /* the check for its end (gfx_int_event) */
			add_interupt_event(GFX_INT, gfx_poll_delay());
			g_gfx_event = 1;
		}
		note('G', boot_size, 0, (unsigned long)us, sp_register.sp_status_reg & 0xFFFF);
		if (g_tasks[3] == 1)
			report_line("GB Tower: RSP task (boot size 0x%X) done in C in %.0f us", (unsigned)boot_size, us);
		gb_window(us, 0);
		return;
	}
	const uint32_t status_before = sp_register.sp_status_reg, sp_intr_before = MI_register.mi_intr_reg & 0x01;
	int result;
	/* 50 million instructions (a picture takes about 350 000): a task waiting
	 * for something that never comes stops instead of hanging the console. */
	const unsigned long n = rsp_lle_run(0x400, 50000000, g_gb_thread_ok, &result);
	const double us = report_now_us() - t;
	rsp_register.rsp_pc &= 0xFFF; /* do_SP_task puts back the high bits */
	if (result == RSP_LLE_AT_SIG3) {
		/* The rest on core 2, against copies of the registers. */
		for (int i = 0; i < 8; i++) {
			g_gb_copy_sp[i] = g_gb_real.sp[i][0];
			g_gb_copy_dpc[i] = g_gb_real.dpc[i][0];
		}
		g_gb_copy_mi = MI_register.mi_intr_reg;
		g_gb_copy_pc = rsp_register.rsp_pc;
		rsp_lle_bind(&g_gb_copy);
		g_gb_first = n;
		g_gb_due = reg_cop0[9] + kGbRspCounts; /* Count */
		g_gb_busy = 1;
		g_gb_rest_done = 0;
		OSMemoryBarrier();
		OSSignalEvent(&g_gb_go);
		if (!g_gfx_event) { /* the check for its end (gfx_int_event) */
			add_interupt_event(GFX_INT, gfx_poll_delay());
			g_gfx_event = 1;
		}
		note('G', boot_size, n, (unsigned long)us, sp_register.sp_status_reg & 0xFFFF);
		if (g_tasks[3] == 1)
			report_line("GB Tower: RSP task (boot size 0x%X): SIG3 after %lu instructions in %.0f us, the rest on core 2",
			            (unsigned)boot_size, n, us);
		return;
	}
	const int broke = result == RSP_LLE_BROKE;
	if (broke && (sp_register.sp_status_reg & 0x400)) {
		/* Held until the CPU clears SIG3 (our64_sp_status_written). */
		sp_register.sp_status_reg &= ~(0x203 & ~status_before);
		if (!sp_intr_before)
			MI_register.mi_intr_reg &= ~0x01;
	} else if (!broke) {
		gb_end(0);
	}
	note('G', boot_size, n, (unsigned long)us, sp_register.sp_status_reg & 0xFFFF);
	if (g_tasks[3] == 1 || !broke) {
		static unsigned told;
		if (told++ < 8)
			report_line("GB Tower: RSP task (boot size 0x%X) run on cxd4: %lu instructions, %.0f us%s; SP status 0x%04lX%s",
			            (unsigned)boot_size, n, us, !broke ? ", stopped before its end" : (sp_register.sp_status_reg & 0x3) ? "" : ", its end held until SIG3 is cleared", sp_register.sp_status_reg,
			            rsp_lle_messages() ? " (cxd4 met reserved instructions)" : "");
	}
	gb_window(us, n);
}

/* The running cost, every 10 s while tasks come: the CPU core's time (the
 * whole task, or its part before SIG3 and any wait for core 2) and core 2's. */
static void gb_window(double us, unsigned long n)
{
	const unsigned vi = player_vis();
	if (!g_gb_window_tasks) {
		g_gb_window_vi = vi;
		g_gb_window_start_us = report_now_us() - us;
	}
	g_gb_window_tasks++;
	g_gb_window_us += us;
	g_gb_window_instructions += n;
	if (vi - g_gb_window_vi >= 600) {
		const double span = report_now_us() - g_gb_window_start_us;
		if (g_gb_reports++ < 60)
			report_line("GB Tower: %u RSP tasks in %u VIs, %.0f instructions each; CPU core %.2f ms each, %.1f%% of the time "
			            "(%u on core 2: %.2f ms each there, the CPU waited %.2f ms each)",
			            g_gb_window_tasks, vi - g_gb_window_vi, (double)g_gb_window_instructions / g_gb_window_tasks,
			            g_gb_window_us / g_gb_window_tasks / 1000.0, span > 0 ? 100.0 * g_gb_window_us / span : 0.0,
			            g_gb_window_async, g_gb_window_async ? g_gb_window_worker_us / g_gb_window_async / 1000.0 : 0.0,
			            g_gb_window_async ? g_gb_window_blocked_us / g_gb_window_async / 1000.0 : 0.0);
		g_gb_window_tasks = g_gb_window_async = 0;
		g_gb_window_us = g_gb_window_worker_us = g_gb_window_blocked_us = 0;
		g_gb_window_instructions = 0;
	}
}

/* Not64's update_SP, after a write to SP_STATUS: clearing SIG3 ends a held
 * GB Tower task, as its program does once it sees SIG3 clear; one still on
 * core 2 ends when it is done there (gb_collect). */
void our64_sp_status_written(unsigned long w)
{
	timeline_note('S', w, sp_register.sp_status_reg, g_rsp_spinning, g_gfx_tasks - g_gfx_finished);
	if (g_rsp_spinning && g_gfx_finished == g_gfx_tasks && video_signal_ucode() && (w & (0x400 | 0x8000)) && !g_au_busy) {
		static int told;
		if (!told++)
			report_line("GRAPHICS: the microcode's signal wait ended by the game (SP status 0x%04lX, VI %u)",
			            sp_register.sp_status_reg & 0xFFFF, player_vis());
		gfx_task(1); /* signal 0 set or signal 3 cleared: ZSortBOSS carries on */
		return;
	}
	if (!(w & 0x8000) || g_gb_busy || g_gb_held || (sp_register.sp_status_reg & 0x3) ||
	    !rsp_lle_gb_loaded((const unsigned char *)SP_IMEM))
		return;
	static int told;
	if (!told++)
		report_line("GB Tower: the first task ended when the CPU cleared SIG3 (VI %u)", player_vis());
	if (gb_end(1))
		check_interupt();
}

/* ---- audio lists on core 2: the CPU core spent 4.5-8% of its time in the
 * audio HLE in Battle for Naboo (2026-10-26) while core 2 sat idle. An audio
 * task the HLE knows (our64_hle_audio_task) runs on a thread there, against
 * copies of the SP and MI registers (Not64's second HLE state), while the CPU
 * runs on, as next to a console's RSP. The RSP stays running meanwhile (halt
 * clear), so the game starts no other task and keeps its hands off the
 * task's lists and buffers. The end (halt, broke, task done and the SP
 * interrupt, what the copies gained) is held until the thread is done and
 * Count has passed g_au_counts, about what a console's RSP takes for a
 * frame's audio list; past that the CPU waits for core 2. Checked at each
 * GFX_INT event (1/32 VI) and VI, as the GB Tower's task. With
 * sd:/wiiu/our64/audio-on-cpu, audio tasks run on the CPU core as before.
 * 1 ms made the CPU wait for most lists (Mario Tennis 594 of 601, about 0.8 ms
 * each, 2026-10-27): the CPU reaches 1 ms of Count sooner than core 2 finishes
 * a list. sd:/wiiu/our64/audio-due holds another time in microseconds. ---- */
enum { kAudioRspUs = 2000, kCountsPerMs = 46875 }; /* Count runs at 46.875 MHz */
static unsigned long g_au_counts = kAudioRspUs * kCountsPerMs / 1000;
static OSThread g_au_thread;
static uint8_t g_au_stack[64 * 1024] __attribute__((aligned(16)));
static OSEvent g_au_go, g_au_done;
static int g_au_on = -1;              /* the thread runs; 0: audio tasks on the CPU core */
static uint32_t g_au_sp[8], g_au_dpc[8], g_au_mi, g_au_pc;
static unsigned long g_au_due;        /* Count when a console's RSP would be done */
static volatile double g_au_us;
static volatile int g_au_finished;
static unsigned g_au_window_vi, g_au_window_tasks, g_au_window_waits, g_au_reports;
static double g_au_window_worker_us, g_au_window_blocked_us;

extern void our64_hle_audio_bind(unsigned int *mi_intr, unsigned int *const sp[8], unsigned int *sp_pc,
                                 unsigned int *const dpc[8]);           /* Not64's rsp_hle/plugin.c */
extern int our64_hle_audio_task(void);
extern void our64_hle_audio_run(void);

static int audio_worker(int argc, const char **argv)
{
	(void)argc, (void)argv;
	for (;;) {
		OSWaitEvent(&g_au_go);
		OSMemoryBarrier();
		const double t = report_now_us();
		our64_hle_audio_run();
		g_au_us = report_now_us() - t;
		OSMemoryBarrier();
		g_au_finished = 1;
		OSSignalEvent(&g_au_done);
	}
	return 0;
}

static int audio_ready(void)
{
	if (g_au_on >= 0)
		return g_au_on;
	struct stat st;
	char path[256];
	const char *root = report_sd_root();
	snprintf(path, sizeof(path), "%s/audio-on-cpu", root ? root : "");
	if (root && !stat(path, &st)) {
		report_line("AUDIO: audio tasks run on the CPU core (sd:/wiiu/our64/audio-on-cpu)");
		return g_au_on = 0;
	}
	snprintf(path, sizeof(path), "%s/audio-due", root ? root : "");
	FILE *f = root ? fopen(path, "r") : NULL;
	unsigned us = kAudioRspUs;
	if (f) {
		if (fscanf(f, "%u", &us) != 1 || us < 100 || us > 16000)
			us = kAudioRspUs;
		fclose(f);
	}
	g_au_counts = (unsigned long)us * kCountsPerMs / 1000;
	unsigned int *sp[8], *dpc[8];
	for (int i = 0; i < 8; i++) {
		sp[i] = (unsigned int *)&g_au_sp[i];
		dpc[i] = (unsigned int *)&g_au_dpc[i];
	}
	our64_hle_audio_bind((unsigned int *)&g_au_mi, sp, (unsigned int *)&g_au_pc, dpc);
	OSInitEvent(&g_au_go, FALSE, OS_EVENT_MODE_AUTO);
	OSInitEvent(&g_au_done, FALSE, OS_EVENT_MODE_AUTO);
	g_au_on = OSCreateThread(&g_au_thread, (OSThreadEntryPointFn)audio_worker, 0, NULL, g_au_stack + sizeof(g_au_stack), sizeof(g_au_stack),
	                         18, OS_THREAD_ATTRIB_AFFINITY_CPU2) != 0;
	if (g_au_on) {
		OSSetThreadName(&g_au_thread, "Our64 audio RSP");
		OSResumeThread(&g_au_thread);
		report_line("AUDIO: audio tasks run on core 2, each held at least %u us of N64 time", us);
	} else {
		report_line("AUDIO: no thread on core 2; audio tasks run on the CPU core");
	}
	return g_au_on;
}

/* An audio task the HLE knows: posted to core 2. Non-zero when it was. */
static int audio_task(void)
{
	if (!audio_ready() || !our64_hle_audio_task())
		return 0;
	g_au_sp[0] = sp_register.sp_mem_addr_reg, g_au_sp[1] = sp_register.sp_dram_addr_reg;
	g_au_sp[2] = sp_register.sp_rd_len_reg, g_au_sp[3] = sp_register.sp_wr_len_reg;
	g_au_sp[4] = sp_register.sp_status_reg, g_au_sp[5] = sp_register.sp_dma_full_reg;
	g_au_sp[6] = sp_register.sp_dma_busy_reg, g_au_sp[7] = sp_register.sp_semaphore_reg;
	g_au_dpc[0] = dpc_register.dpc_start, g_au_dpc[1] = dpc_register.dpc_end;
	g_au_dpc[2] = dpc_register.dpc_current, g_au_dpc[3] = dpc_register.dpc_status;
	g_au_dpc[4] = dpc_register.dpc_clock, g_au_dpc[5] = dpc_register.dpc_bufbusy;
	g_au_dpc[6] = dpc_register.dpc_pipebusy, g_au_dpc[7] = dpc_register.dpc_tmem;
	g_au_mi = MI_register.mi_intr_reg;
	g_au_pc = rsp_register.rsp_pc;
	g_au_due = reg_cop0[9] + g_au_counts; /* Count */
	g_au_finished = 0;
	g_au_busy = 1;
	OSMemoryBarrier();
	OSSignalEvent(&g_au_go);
	if (!g_gfx_event) { /* the check for its end (gfx_int_event) */
		add_interupt_event(GFX_INT, gfx_poll_delay());
		g_gfx_event = 1;
	}
	return 1;
}

/* Takes the task back from core 2, waiting for it if need be, and applies its end. */
static void audio_collect(void)
{
	double blocked = 0;
	if (!g_au_finished) {
		const double t = report_now_us();
		OSWaitEvent(&g_au_done);
		blocked = report_now_us() - t;
		g_au_window_waits++;
	} else {
		OSWaitEvent(&g_au_done); /* set already: this only resets it */
	}
	OSMemoryBarrier();
	g_au_busy = 0;
	/* The HLE's rsp_break on the copies: halt, broke and task done, and the SP
	 * interrupt if the game asked for one. Set outright, not as what the copies
	 * gained, so a bit the game cleared meanwhile is still set. */
	if ((g_au_sp[4] & 0x3) == 0x3) {
		sp_register.sp_status_reg |= g_au_sp[4] & 0x203;
		if (sp_register.sp_status_reg & 0x40)
			MI_register.mi_intr_reg |= 0x01;
	}
	note('E', g_au_sp[4], g_au_mi, (unsigned long)g_au_us, sp_register.sp_status_reg & 0xFFFF);
	const unsigned vi = player_vis();
	if (!g_au_window_tasks)
		g_au_window_vi = vi;
	g_au_window_tasks++;
	g_au_window_worker_us += g_au_us;
	g_au_window_blocked_us += blocked;
	if (vi - g_au_window_vi >= 600) {
		if (g_au_reports++ < 60)
			report_line("AUDIO: %u lists on core 2 in %u VIs, %.2f ms each there; the CPU waited for %u, %.2f ms each",
			            g_au_window_tasks, vi - g_au_window_vi, g_au_window_worker_us / g_au_window_tasks / 1000.0,
			            g_au_window_waits, g_au_window_waits ? g_au_window_blocked_us / g_au_window_waits / 1000.0 : 0.0);
		g_au_window_tasks = g_au_window_waits = 0;
		g_au_window_worker_us = g_au_window_blocked_us = 0;
	}
}

/* At each GFX_INT event and VI: the task ends once a console's would have. Non-zero when an MI interrupt was raised. */
static int audio_poll(void)
{
	if (!g_au_busy || (long)(reg_cop0[9] - g_au_due) < 0)
		return 0;
	const uint32_t mi = MI_register.mi_intr_reg;
	audio_collect();
	return (MI_register.mi_intr_reg & ~mi) != 0;
}

/* Outside go(), before a save state loads, a reset or the game's end: nothing
 * on core 2, and the task forgotten (the new state has none in flight). */
static void audio_drop(void)
{
	if (!g_au_busy)
		return;
	OSWaitEvent(&g_au_done);
	g_au_busy = 0;
}

/* sd:/wiiu/our64/jpeg-dump: the input of the first JPEG tasks goes to
 * frames/jpeg-NN.bin, for decoding on the PC (tools/jpeg-pc). Each file holds
 * records of (address, length, bytes), big-endian; address 0xFFFFFFC0 is the
 * task header from DMEM. */
static int g_jpeg_dump_left = -1;

static void jpeg_record(FILE *f, uint32_t addr, const void *p, uint32_t len)
{
	uint32_t h[2] = {addr, len};
	fwrite(h, 4, 2, f);
	fwrite(p, 1, len, f);
}

static void jpeg_dump(const unsigned long *task)
{
	char path[256];
	const char *root = report_sd_root();
	if (g_jpeg_dump_left < 0) {
		struct stat st;
		snprintf(path, sizeof(path), "%s/jpeg-dump", root ? root : "");
		g_jpeg_dump_left = root && !stat(path, &st) ? 24 : 0;
	}
	if (!g_jpeg_dump_left)
		return;
	const unsigned char *ram = (const unsigned char *)rdram;
	uint32_t data = task[12] & 0x7FFFFC;
	const unsigned long *d = (const unsigned long *)(ram + data);
	uint32_t mbs = d[1], mode = d[2];
	if (mbs > 4096 || (mode != 0 && mode != 2))
		mbs = 0;
	snprintf(path, sizeof(path), "%s/frames", root);
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/frames/jpeg-%02d.bin", root, 24 - g_jpeg_dump_left);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	g_jpeg_dump_left--;
	jpeg_record(f, 0xFFFFFFC0, task, 64);
	jpeg_record(f, data, ram + data, 24);
	for (int i = 0; i < 3; i++)
		jpeg_record(f, d[3 + i] & 0x7FFFFE, ram + (d[3 + i] & 0x7FFFFE), 128);
	uint32_t ud = task[6] & 0x7FFFFC, uds = task[7] < 0x1000 ? task[7] : 0x1000;
	jpeg_record(f, ud, ram + ud, uds);
	jpeg_record(f, d[0] & 0x7FFFFC, ram + (d[0] & 0x7FFFFC), mbs * (mode + 4) * 128);
	fclose(f);
	report_line("JPEG task dumped: %u macroblocks, mode %u, to %s", mbs, mode, path);
}

/* ---- stall report: a game that stops sending tasks while VIs run (Conker,
 * Perfect Dark, 2026-10-19). The last 32 RSP starts, yields, task ends,
 * interrupts and AI length writes, and the pending events, logged once a
 * game has started no task for 180 VIs. ---- */
typedef struct { unsigned vi; char kind; unsigned long a, b, c, d; } stall_note;
static stall_note g_notes[32];
static unsigned g_note_n, g_last_task_vi, g_stall_reports;
static int g_stall_told;
/* The graphics notes alone (graphics task starts, ends, yields): when only
 * graphics stops, audio's notes fill the ring above long before the report
 * (Mario Tennis froze at a serve that way, 2026-10-21). */
static stall_note g_gfx_notes[16];
static unsigned g_gfx_note_n;

/* ---- timeline (sd:/wiiu/our64/timeline): every note below, plus VIs ('V'),
 * the game's SP status writes ('S') and audio waits ('W'), with the system
 * time and Count, into RAM; written to sd:/wiiu/our64/timeline.bin when full
 * or when the game stops. Stunt Racer runs at 45-48 VI/s with both cores
 * mostly idle and flickers (2026-10-28): this shows when its signal waits,
 * task ends and VIs happen. CPU core only. ---- */
typedef struct { uint64_t t; uint32_t count, vi; uint32_t kind, a, b, c, d; uint32_t pad; } timeline_entry;
#define TIMELINE_N 65536
int g_timeline_on;
static timeline_entry *g_timeline;
static unsigned g_timeline_n;

void timeline_note(char kind, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
	if (!g_timeline_on)
		return;
	if (!g_timeline && !(g_timeline = calloc(TIMELINE_N, sizeof(timeline_entry)))) {
		g_timeline_on = 0;
		return;
	}
	if (g_timeline_n >= TIMELINE_N)
		return;
	timeline_entry *e = &g_timeline[g_timeline_n++];
	e->t = OSGetSystemTime();
	e->count = reg_cop0[9]; /* Count */
	e->vi = player_vis();
	e->kind = (unsigned char)kind;
	e->a = a, e->b = b, e->c = c, e->d = d;
	if (g_timeline_n == TIMELINE_N)
		timeline_write();
}

/* Each call writes everything so far again: a stall, HOME or the end. */
void timeline_write(void)
{
	static unsigned written;
	if (!g_timeline || g_timeline_n == written)
		return;
	char p[256];
	snprintf(p, sizeof(p), "%s/timeline.bin", report_sd_root());
	FILE *f = fopen(p, "wb");
	if (f) {
		fwrite(g_timeline, sizeof(timeline_entry), g_timeline_n, f);
		fclose(f);
		written = g_timeline_n;
		report_line("TIMELINE: %u events written to timeline.bin (%u ticks a second)", g_timeline_n, (unsigned)OSSecondsToTicks(1));
	}
}

static void note(char kind, unsigned long a, unsigned long b, unsigned long c, unsigned long d)
{
	timeline_note(kind, a, b, c, d);
	stall_note *n = &g_notes[g_note_n++ % 32];
	n->vi = player_vis();
	n->kind = kind;
	n->a = a, n->b = b, n->c = c, n->d = d;
	if (kind == 'F' || kind == 'Y' || kind == 'D' || kind == 'R' || kind == 'S' || (kind == 'T' && a == 1)) /* D, R: a start while audio or graphics ran, S: a yielded one dropped */
		g_gfx_notes[g_gfx_note_n++ % 16] = *n;
}

void our64_note_int(int type)
{
	if (type != 0x001 && type != 0x200 && type != 0x020 && type != 0x004) /* not VI, GFX, SPECIAL, CHECK */
		note('I', type, MI_register.mi_intr_reg, MI_register.mi_intr_mask_reg, reg_cop0[12]);
}

void our64_note_ai(unsigned long len, unsigned long delay, unsigned long status)
{
	note('A', len, delay, status, ai_register.ai_dacrate);
	if (g_ai_stream)
		ai_stream_queue(len & 0x3FFF8, delay, (status & 0x40000000) != 0);
}

int our64_pak_pulled(int port)
{
	if (port < 0 || port >= 4 || !g_pak_pulled[port])
		return 0;
	g_pak_pulled[port] = 0;
	g_pak_keep_until[port] = player_vis() + 120;
	report_line("PAK: VI %u port %d reads as pulled out", player_vis(), port + 1);
	return 1;
}

/* A game that read or wrote its Controller Pak and then left it alone for
 * 10 s has its Rumble Pak back, reported pulled out once so the game looks
 * again (a game told nothing keeps believing in the Controller Pak and never
 * starts the motor). */
static void pak_watch_vi(unsigned vi)
{
	for (int port = 0; port < 4; port++)
		if (g_pak_auto[port] && Controls[port].Plugin == PLUGIN_MEMPAK && vi - g_pak_used_vi[port] > 600) {
			Controls[port].Plugin = PLUGIN_RUMBLE_PAK;
			/* A game that probed for a Rumble Pak in the last 30 s looks again by
			 * itself; told the Pak was pulled out, a Controller Pak operation
			 * under way fails (NFL Blitz 2000 probes every few seconds and said
			 * its save failed). */
			const int probes = g_pak_probed_vi[port] && vi + 1 - g_pak_probed_vi[port] < 1800;
			g_pak_pulled[port] = !probes;
			static unsigned told;
			if (told++ < 40)
				report_line("PAK: port %d has its Rumble Pak back (no Controller Pak use for 10 s)%s", port + 1,
				            probes ? ", not reported pulled out: the game looks for it by itself" : "");
		}
}

static void stall_dump(void);
static void stall_tlb_dump(void);

/* The last graphics task starts, finishes and yields (T, F, Y), for a stall report. */
static void gfx_notes_dump(void)
{
	for (unsigned i = g_gfx_note_n > 16 ? g_gfx_note_n - 16 : 0; i < g_gfx_note_n; i++) {
		const stall_note *x = &g_gfx_notes[i % 16];
		report_line("STALL:  graphics VI %u %c %08lX %08lX %08lX %08lX", x->vi, x->kind, x->a, x->b, x->c, x->d);
	}
}

void gfx_watch_vi(unsigned vi)
{
	boot_patches_vi();
	pak_watch_vi(vi);
	if (gb_poll() | audio_poll())
		check_interupt();
	if (g_stall_reports >= 4)
		return;
	char state[400];
	if (!g_stall_told && vi - g_last_task_vi >= 180) {
		g_stall_told = 1;
		g_stall_reports++;
		player_core_state(state, sizeof(state));
		report_line("STALL: no RSP task for %u VIs (VI %u); %s; AI status 0x%08lX; gfx posted %u finished %u yield %d",
		            vi - g_last_task_vi, vi, state, ai_register.ai_status, (unsigned)g_gfx_tasks,
		            (unsigned)g_gfx_finished, g_gfx_yield);
		gfx_notes_dump(); /* Toy Story 2 stopped yielded, waiting on its RSP (2026-10-29) */
		stall_tlb_dump();
		stall_dump();
	} else if (g_gfx_seen && ++g_vis_without_gfx >= 180 && !g_gfx_stall_told && vi - g_last_task_vi < 180) {
		g_gfx_stall_told = 1;
		g_stall_reports++;
		player_core_state(state, sizeof(state));
		report_line("STALL: no graphics task for %u VIs, other tasks go on (VI %u); %s; gfx posted %u finished %u yield %d "
		            "spinning %d spin-yielded %d; DP status 0x%04lX",
		            g_vis_without_gfx, vi, state, (unsigned)g_gfx_tasks, (unsigned)g_gfx_finished, g_gfx_yield,
		            g_rsp_spinning, g_spin_yielded, dpc_register.dpc_status & 0xFFFF);
		gfx_notes_dump();
		stall_dump();
	}
}

/* The pending events, the code the CPU waits in, its registers and the last
 * notes, then the log saved. */
/* The valid TLB entries and what the lookup tables give for the bad address,
 * when the CPU stopped on a TLB miss (All-Star Baseball 2001: its refill
 * handler jumps to 0x001004F8, a page it keeps mapped; 2026-10-29). */
static void stall_tlb_dump(void)
{
	if (((reg_cop0[13] >> 2) & 0x1F) != 2 && ((reg_cop0[13] >> 2) & 0x1F) != 3)
		return;
	const unsigned long bad = reg_cop0[8];
	report_line("STALL: TLB: Wired %lu, Random %lu, Index 0x%lX, PageMask 0x%lX, EntryHi 0x%08lX; lookup for 0x%08lX: read 0x%08lX "
	            "write 0x%08lX", reg_cop0[6], reg_cop0[1], reg_cop0[0], reg_cop0[5], reg_cop0[10], bad,
	            tlb_LUT_r[bad >> 12], tlb_LUT_w[bad >> 12]);
	for (int i = 0; i < 32; i++)
		if (tlb_e[i].v_even || tlb_e[i].v_odd || tlb_e[i].vpn2)
			report_line("STALL: TLB %2d: %08lX-%08lX -> %08lX v%d d%d | %08lX-%08lX -> %08lX v%d d%d; mask 0x%X g%d asid %u", i,
			            tlb_e[i].start_even, tlb_e[i].end_even, tlb_e[i].phys_even, tlb_e[i].v_even, tlb_e[i].d_even,
			            tlb_e[i].start_odd, tlb_e[i].end_odd, tlb_e[i].phys_odd, tlb_e[i].v_odd, tlb_e[i].d_odd,
			            (unsigned)tlb_e[i].mask, tlb_e[i].g, tlb_e[i].asid);
}

static void stall_dump(void)
{
	int types[16];
	unsigned long ahead[16];
	int n = our64_events(types, ahead, 16);
	char line[400];
	int k = snprintf(line, sizeof(line), "STALL: events (type, counts ahead):");
	for (int i = 0; i < n && k < (int)sizeof(line) - 24; i++)
		k += snprintf(line + k, sizeof(line) - k, " %03X %+ld", types[i], (long)ahead[i]);
	report_line("%s", line);
	/* Where it waits: the code around the PC (through the TLB, without an
	 * exception) and the registers, low 32 bits. */
	extern unsigned long our64_tlb_peek(unsigned long addr); /* Not64's tlb.c */
	extern long long int reg[34];
	k = snprintf(line, sizeof(line), "STALL: code at 0x%08lX:", interp_addr - 16);
	for (int i = 0; i < 12; i++) {
		unsigned long p = our64_tlb_peek(interp_addr - 16 + 4 * i);
		k += snprintf(line + k, sizeof(line) - k, p && p < 0x800000 ? " %08lX" : " --------", p < 0x800000 ? rd32(p) : 0);
	}
	report_line("%s", line);
	for (int half = 0; half < 2; half++) {
		k = snprintf(line, sizeof(line), "STALL: r%d-r%d:", half * 16, half * 16 + 15);
		for (int i = half * 16; i < half * 16 + 16; i++)
			k += snprintf(line + k, sizeof(line) - k, " %08lX", (unsigned long)reg[i]);
		report_line("%s", line);
	}
	report_line("STALL: EPC 0x%08lX BadVAddr 0x%08lX EntryHi 0x%08lX", reg_cop0[14], reg_cop0[8], reg_cop0[10]);
	for (unsigned i = g_note_n > 32 ? g_note_n - 32 : 0; i < g_note_n; i++) {
		const stall_note *x = &g_notes[i % 32];
		report_line("STALL:  VI %u %c %08lX %08lX %08lX %08lX", x->vi, x->kind, x->a, x->b, x->c, x->d);
	}
	report_save();
	if (g_gfx_tasks)
		timeline_write();
}

/* The last n notes (RSP starts, yields, task ends, interrupts), for a crash report. */
void gfx_notes_report(const char *prefix, unsigned n)
{
	for (unsigned i = g_note_n > n ? g_note_n - n : 0; i < g_note_n; i++) {
		const stall_note *x = &g_notes[i % 32];
		report_line("%s VI %u %c %08lX %08lX %08lX %08lX", prefix, x->vi, x->kind, x->a, x->b, x->c, x->d);
	}
	report_line("%sgfx posted %u finished %u yield %d spinning %d", prefix, (unsigned)g_gfx_tasks,
	            (unsigned)g_gfx_finished, g_gfx_yield, g_rsp_spinning);
}

/* sd:/wiiu/our64/lle-audio, a debug switch: audio tasks run on cxd4 (the
 * game's own RSP program) instead of the HLE. Slow, but it tells an HLE gap
 * from sound data the CPU never wrote (Resident Evil 2's missing dialogue,
 * 2026-10-26). Non-zero when the task was run so. */
static int lle_audio_task(void)
{
	static int on = -1;
	if (on < 0) {
		struct stat st;
		char path[256];
		const char *root = report_sd_root();
		snprintf(path, sizeof(path), "%s/lle-audio", root ? root : "");
		on = root && !stat(path, &st);
		if (on)
			report_line("AUDIO: audio tasks run on cxd4, not the HLE (sd:/wiiu/our64/lle-audio)");
	}
	if (!on)
		return 0;
	gb_ready();
	gb_quiet();
	int result;
	const double t = report_now_us();
	const unsigned long n = rsp_lle_run(0, 20000000, 0, &result);
	rsp_register.rsp_pc &= 0xFFF; /* do_SP_task puts back the high bits */
	if (result != RSP_LLE_BROKE)
		gb_end(0);
	static unsigned told;
	if (told++ < 4)
		report_line("AUDIO: task on cxd4: %lu instructions in %.0f us, %s", n, report_now_us() - t,
		            result == RSP_LLE_BROKE ? "ended" : "stopped");
	return 1;
}

DWORD __wrap_doRspCycles(DWORD Cycles)
{
	/* An audio list still runs on core 2: the RSP is running, and a write that
	 * clears halt, broke or the DP freeze (update_SP, update_DPC) starts nothing. */
	if (g_au_busy) {
		const unsigned long *t = (const unsigned long *)((char *)SP_DMEM + 0xFC0);
		note('D', t[0], t[1], t[12], (unsigned long)g_gfx_yield << 16 | (sp_register.sp_status_reg & 0xFFFF));
		return Cycles;
	}
	const unsigned long *task = (const unsigned long *)((char *)SP_DMEM + 0xFC0);
	/* A graphics task still being decoded, not yielded: the RSP is running
	 * too, so such a write starts nothing either. Without the wait Perfect
	 * Dark posted a task while the one before was in flight, got the two
	 * ends at once and stopped at its first loading screen (2026-10-30).
	 * R: type, the last SP and DP status written, the yield state and SP status. */
	if (g_gfx_finished != g_gfx_tasks && g_gfx_yield != GFX_YIELDED) {
		note('R', task[0], sp_register.w_sp_status_reg, dpc_register.w_dpc_status,
		     (unsigned long)g_gfx_yield << 16 | (sp_register.sp_status_reg & 0xFFFF));
		static unsigned told;
		if (told++ < 3)
			report_line("GRAPHICS: a start while task %u is decoded, not taken (SP status written 0x%lX, DP 0x%lX, VI %u)",
			            (unsigned)g_gfx_finished, (unsigned long)sp_register.w_sp_status_reg,
			            (unsigned long)dpc_register.w_dpc_status, player_vis());
		return Cycles;
	}
	unsigned type = task[0];
	/* T: type, flags, the data pointer, then the yield state and SP status */
	note('T', type, task[1], task[12], (unsigned long)g_gfx_yield << 16 | (sp_register.sp_status_reg & 0xFFFF));
	g_last_task_vi = player_vis();
	g_stall_told = 0;
	if (type == 1) {
		g_gfx_seen = 1;
		g_vis_without_gfx = 0;
		g_gfx_stall_told = 0;
	}
	/* A yielded graphics task resumed: it is still being decoded. */
	if (type == 1 && (task[1] & 1) && g_gfx_yield == GFX_YIELDED && !g_spin_yielded) {
		g_gfx_yield = GFX_RESUMED;
		dpc_register.dpc_status &= ~0x0002;
		if (!g_gfx_event) {
			add_interupt_event(GFX_INT, gfx_poll_delay());
			g_gfx_event = 1;
		}
		return Cycles;
	}
	/* A yielded graphics task the game starts again from the top, without
	 * OS_TASK_YIELDED (Toy Story 2 at its warning screen): it is dropped, its
	 * interrupts never given, and the new one runs. Waiting for its resume,
	 * the game waited on its RSP forever (2026-10-29). */
	if (type == 1 && !(task[1] & 1) && g_gfx_yield == GFX_YIELDED && !g_spin_yielded) {
		video_wait_task(g_gfx_finished, -1.0);
		note('S', g_gfx_finished, g_gfx_tasks, task[12], sp_register.sp_status_reg);
		g_gfx_finished++;
		g_gfx_yield = GFX_RUNNING;
		static unsigned told;
		if (told++ < 3)
			report_line("GRAPHICS: a yielded task started again from the top: dropped (VI %u)", player_vis());
	}
	/* A graphics task with a display list: what the HLE plugin would do, on the graphics core. */
	if (type == 1 && task[12] != 0) {
		g_tasks[0]++;
		gfx_task(0);
		return Cycles;
	}
	/* The GB Tower's program: on cxd4, never the HLE (which would take it for an audio list). */
	if (type != 1) {
		int seen;
		const uint32_t gb = rsp_lle_gb_tower((const unsigned char *)rdram, (const unsigned char *)SP_DMEM,
		                                     (const unsigned char *)SP_IMEM, &seen);
		if (gb) {
			gb_task(gb);
			return Cycles;
		}
		static int told;
		if (seen && !told) {
			told = 1;
			report_line("GB Tower: its RSP code with a boot size never seen (0x%lX): left to the HLE", task[3]);
		}
	}
	if (type == 2 && lle_audio_task()) {
		g_tasks[1]++;
		return Cycles;
	}
	if (type == 2 && audio_task()) {
		g_tasks[1]++;
		return Cycles;
	}
	if (type == 4)
		jpeg_dump(task);
	g_tasks[type == 1 ? 0 : type == 2 ? 1 : 2]++;
	{
		/* Each distinct task the HLE plugin gets, once: type, ucode address and size, its
		 * checksums (the HLE's over half the size, and over 256 bytes, which tells Resident
		 * Evil 2's video tasks apart) and the ucode data's word 0x10 (which tells audio
		 * ucodes apart). Audio tasks too: Resident Evil 2's dialogue and the garbage on
		 * its screen changes (2026-10-26). */
		static unsigned long seen[32];
		static int nseen;
		unsigned long ucode = task[4] & 0x7FFFFF, size = task[5], udata = task[6] & 0x7FFFFF;
		unsigned sum256 = 0;
		for (unsigned i = 0; i < 256 && ucode + i < 0x800000; i++)
			sum256 += ((const unsigned char *)rdram)[ucode + i];
		const unsigned long id = ucode ^ (unsigned long)type << 24 ^ (unsigned long)sum256 << 8;
		int known = 0;
		for (int i = 0; i < nseen; i++)
			known |= seen[i] == id;
		if (!known && nseen < 32) {
			seen[nseen++] = id;
			unsigned sum = 0, n = (size <= 0x1000 ? size : 0x1000) / 2;
			const unsigned char *src = size <= 0x1000 ? (const unsigned char *)rdram + ucode
			                                          : (const unsigned char *)SP_IMEM;
			for (unsigned i = 0; i < n && ucode + i < 0x800000; i++)
				sum += src[i];
			report_line("RSP task type %u: ucode 0x%06lX size 0x%lX data 0x%06lX, sum 0x%X, sum256 0x%X, data word 0x10 "
			            "0x%08lX, VI %u", type, ucode, size, udata, sum, sum256,
			            udata + 0x14 <= 0x800000 ? rdram[(udata + 0x10) / 4] : 0, g_bench.vis);
		}
	}
	return __real_doRspCycles(Cycles);
}

/* One line of core state, for finding where a game waits. */
extern unsigned int our64_dyna_fallbacks, our64_dyna_fallback_first; /* Not64's Wrappers.c */
extern unsigned int our64_branch_exits;                               /* Not64's Recompile.c */

void player_core_state(char *buf, unsigned size)
{
	int n = snprintf(buf, size, "N64 PC 0x%08lX/0x%08lX, cop0 status 0x%08lX cause 0x%08lX, MI intr 0x%02lX mask 0x%02lX, "
	                 "SP status 0x%04lX, DP status 0x%04lX, tasks gfx %u audio %u other %u gb %u",
	                 last_addr, interp_addr, reg_cop0[12], reg_cop0[13], MI_register.mi_intr_reg,
	                 MI_register.mi_intr_mask_reg, sp_register.sp_status_reg, dpc_register.dpc_status,
	                 g_tasks[0], g_tasks[1], g_tasks[2], g_tasks[3]);
	if ((our64_dyna_fallbacks || our64_branch_exits) && n > 0 && n < (int)size)
		snprintf(buf + n, size - n, "; branches to code-less instructions: %u compiled as exits, %u run interpreted "
		         "(first at 0x%08X)", our64_branch_exits, our64_dyna_fallbacks, our64_dyna_fallback_first);
}

/* The game's libultra threads, for finding what a hung game waits on. Needs
 * the address of __osActiveQueue (from a decomp's symbols) in threads_at. */
static unsigned long rd32(unsigned long a) { return rdram[(a & 0x7FFFFC) / 4]; }

void player_thread_dump(unsigned long threads_at)
{
	unsigned long t = rd32(threads_at);
	for (int n = 0; n < 16 && t >= 0x80000000 && t < 0x80800000 && rd32(t + 4) != (unsigned long)-1; n++) {
		/* OSThread: priority 0x04, queue 0x08, tlnext 0x0C, state 0x10 (u16),
		 * id 0x14, context ra 0x104 (low word), sp 0xF4, pc 0x11C. */
		unsigned long q = rd32(t + 8);
		char qs[96] = "";
		if (q >= 0x80000000 && q < 0x80800000) /* OSMesgQueue: validCount 0x08, first 0x0C, msgCount 0x10 */
			snprintf(qs, sizeof(qs), ", queue 0x%08lX (%lu of %lu messages)", q, rd32(q + 8), rd32(q + 16));
		report_line("  thread %ld at 0x%08lX: priority %ld, state %lu, pc 0x%08lX, ra 0x%08lX, sp 0x%08lX%s",
		            (long)rd32(t + 0x14), t, (long)rd32(t + 4), rd32(t + 0x10) >> 16, rd32(t + 0x11C),
		            rd32(t + 0x104), rd32(t + 0xF4), qs);
		t = rd32(t + 0xC);
	}
}

/* No graphics task in flight or waiting for its interrupts: the graphics
 * core holds nothing a save state would miss. */
int gfx_idle(void)
{
	/* A spinning RSP has nothing in flight: its event only watches for a yield
	 * request, and pausing must still work. A state captured then restarts the
	 * spin from the display list's start (GLideN64's place in it is not saved).
	 * Not a microcode waiting on signals (ZSortBOSS): restarted from the start
	 * it waits for a signal the game already gave, and Stunt Racer resumed
	 * frozen at 0x8000219C (2026-10-28). Its frames end between waits. */
	const int spin_ok = g_rsp_spinning && !video_signal_ucode();
	return g_gfx_finished == g_gfx_tasks && (!g_gfx_event || spin_ok) && g_gfx_yield == GFX_RUNNING && !g_gb_busy &&
	       !g_gb_held && !g_au_busy;
}

char padType[4];
