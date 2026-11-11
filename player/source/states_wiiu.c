/* Our64 player: save states and resume points (the autosave). The whole N64
 * (CPU, RSP and RDP registers, RDRAM, TLB, pending events), captured at the
 * top of Not64's interrupt handler, called between instructions (from the
 * dispatcher, or as an interpreted branch's last act: our64_at_boundary).
 * In-game save memory is not part of a state: in-game saves, save states and
 * the autosave never read, write or change one another (Zach, 2026-10-16).
 *
 * Resume points go to sd:/wiiu/our64/states/<game id>/resume-a.o64s or
 * resume-b.o64s in turn. Capturing is a copy of about 8.2 MB, done at the
 * start of a VI only when the wait ahead of that VI covers it (states.c:
 * resume_fits), so it costs no speed; compressing and writing run on the
 * writer's core. A capture also waits for a moment with no graphics task in
 * flight, so the graphics core holds nothing the state does not. When an
 * in-game save is due, it waits for the capture and goes to the SD after it
 * (writer.h), so the autosave is never older than the newest in-game save.
 *
 * HOME captures the game the same way into a buffer of its own and runs the
 * pause menu right there (pause_wiiu.c), the game frozen. Save states and
 * the Undo card (slot1-3 and undo in the same folder) are written from that
 * buffer; a card to load is read into a third. Loading and resetting stop go(), and main.c starts it again. */
#include <gctypes.h>
#include "main/rom.h"
#include "main/plugin.h"
#include "main/savestates.h"
#include "r4300/r4300.h"
#include "r4300/macros.h"
#include "r4300/interupt.h"
#include "r4300/Invalid_Code.h"
#include "gc_memory/memory.h"
#include "gc_memory/tlb.h"
#include "gc_memory/MEM2.h"

#include "states.h"
#include "pause_wiiu.h"
#include "saves.h"
#include "writer.h"
#include "player.h"
#include "prompt.h"
#include "report.h"

#include <coreinit/cache.h>
#include <coreinit/event.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

extern unsigned long interp_addr;
extern int llbit;
extern long long int reg_cop1_fgr_64[32];
void save_flashram_infos(char *buf);
void load_flashram_infos(char *buf);
void tlb_mem2_init(void);
void aiDacrateChanged(int SystemType);

int savestates_job;

/* ---- copying ---- */

/* memcpy for large copies of memory that is not in the cache, as GLideN64's
 * copyBytes (FastMemory.h): on the Espresso a store to an uncached line first
 * reads the line, and each read waits for the one before. dcbz claims each
 * 32-byte destination line without reading it; dcbt starts reading source
 * lines ahead. lines_ahead is how far ahead (the startup test tries several). */
static void copy_cold(void *dst_, const void *src_, unsigned size, unsigned lines_ahead)
{
	uint8_t *dst = dst_;
	const uint8_t *src = src_;
	unsigned head = (0 - (uintptr_t)dst) & 31;
	if (head > size)
		head = size;
	memcpy(dst, src, head);
	dst += head;
	src += head;
	size -= head;
	const unsigned ahead = lines_ahead * 32; /* dcbt never faults, so reading past the end is safe */
	while (size >= 32) {
		__asm__ volatile("dcbt 0,%0" : : "r"(src + ahead));
		__asm__ volatile("dcbz 0,%0" : : "r"(dst) : "memory");
		memcpy(dst, src, 32);
		dst += 32;
		src += 32;
		size -= 32;
	}
	memcpy(dst, src, size);
}

static unsigned g_lines_ahead = 4;

/* The Espresso's locked cache DMA (coreinit, no header in wut): 4 KB at a
 * time from memory into this core's locked cache and back out, at bus speed
 * and without the CPU waiting on each line. Memory only, so the source is
 * written back from the cache first and the destination dropped from it. */
void *LCAlloc(uint32_t bytes);
void LCDealloc(void *addr);
BOOL LCHardwareIsAvailable(void);
BOOL LCEnableDMA(void);
void LCDisableDMA(void);
uint32_t LCGetDMAQueueLength(void);
void LCLoadDMABlocks(void *lc, const void *src, uint32_t blocks);
void LCStoreDMABlocks(void *dst, const void *lc, uint32_t blocks);
void LCWaitDMAQueue(uint32_t length);

#define LC_CHUNK 4096 /* 128 blocks of 32 bytes, the most one call moves */
static uint8_t *g_lc;  /* 4 chunks of locked cache on the CPU core, or NULL */

/* On the calling thread's core; NULL when there is none to have. */
static uint8_t *lc_start(void)
{
	uint8_t *lc;
	if (!LCHardwareIsAvailable() || !(lc = LCAlloc(4 * LC_CHUNK)))
		return NULL;
	if (!LCEnableDMA()) {
		LCDealloc(lc);
		return NULL;
	}
	return lc;
}

/* dst and src 32-byte aligned, size a multiple of LC_CHUNK. The source is
 * written back from the caches first (dcbst reaches every core's copy). The
 * destination is not dropped from the caches: only the writer reads it, and
 * it does so first (write_job). */
static void copy_dma(uint8_t *lc_base, void *dst, const void *src, unsigned size)
{
	DCStoreRange((void *)src, size);
	for (unsigned i = 0, n = size / LC_CHUNK; i < n; i++) {
		uint8_t *lc = lc_base + (i & 3) * LC_CHUNK;
		if (LCGetDMAQueueLength() > 6)
			LCWaitDMAQueue(6);
		LCLoadDMABlocks(lc, (const uint8_t *)src + i * LC_CHUNK, LC_CHUNK / 32);
		LCStoreDMABlocks((uint8_t *)dst + i * LC_CHUNK, lc, LC_CHUNK / 32);
	}
	LCWaitDMAQueue(0);
}

/* The copy split across all three cores: helpers on cores 0 and 2 take a
 * third each, at a priority above everything else there for those few ms. */
static struct {
	OSThread thread __attribute__((aligned(16)));
	OSEvent go, done;
	uint8_t *dst;
	const uint8_t *src;
	unsigned size;
	int dma;     /* this job by DMA */
	uint8_t *lc; /* the helper core's locked cache, or NULL */
} g_helper[2];
static uint8_t g_helper_stack[2][16 * 1024] __attribute__((aligned(16)));
static int g_helpers_on;

static int helper(int argc, const char **argv)
{
	(void)argv;
	int i = argc;
	g_helper[i].lc = lc_start();
	OSSignalEvent(&g_helper[i].done);
	for (;;) {
		OSWaitEvent(&g_helper[i].go);
		if (g_helper[i].dma)
			copy_dma(g_helper[i].lc, g_helper[i].dst, g_helper[i].src, g_helper[i].size);
		else
			copy_cold(g_helper[i].dst, g_helper[i].src, g_helper[i].size, g_lines_ahead);
		OSSignalEvent(&g_helper[i].done);
	}
	return 0;
}

static void helpers_start(void)
{
	static const OSThreadAttributes core[2] = { OS_THREAD_ATTRIB_AFFINITY_CPU0, OS_THREAD_ATTRIB_AFFINITY_CPU2 };
	for (int i = 0; i < 2; i++) {
		OSInitEvent(&g_helper[i].go, FALSE, OS_EVENT_MODE_AUTO);
		OSInitEvent(&g_helper[i].done, FALSE, OS_EVENT_MODE_AUTO);
		if (!OSCreateThread(&g_helper[i].thread, (OSThreadEntryPointFn)helper, i, NULL, g_helper_stack[i] + sizeof(g_helper_stack[i]),
		                    sizeof(g_helper_stack[i]), 1, core[i]))
			return;
		OSSetThreadName(&g_helper[i].thread, "Our64 state copy");
		OSResumeThread(&g_helper[i].thread);
		OSWaitEvent(&g_helper[i].done); /* its locked cache is set up */
	}
	g_helpers_on = 1;
}

/* dma: each core moves its third with its own locked cache DMA. */
static void copy_cores(void *dst, const void *src, unsigned size, int dma)
{
	unsigned third = (size / 3) & ~(LC_CHUNK - 1);
	for (int i = 0; i < 2; i++) {
		g_helper[i].dst = (uint8_t *)dst + (i + 1) * third;
		g_helper[i].src = (const uint8_t *)src + (i + 1) * third;
		g_helper[i].size = i ? size - 2 * third : third;
		g_helper[i].dma = dma;
		OSSignalEvent(&g_helper[i].go);
	}
	if (dma)
		copy_dma(g_lc, dst, src, third);
	else
		copy_cold(dst, src, third, g_lines_ahead);
	OSWaitEvent(&g_helper[0].done);
	OSWaitEvent(&g_helper[1].done);
}

/* How RDRAM is copied into a state: the fastest the startup test found. */
enum { COPY_MEMCPY, COPY_COLD, COPY_DMA, COPY_CORES, COPY_CORES_DMA, COPY_METHODS };
static int g_copy = COPY_COLD;
static const char *const g_copy_name[] = { "memcpy", "dcbt/dcbz", "locked cache DMA", "3 cores dcbt/dcbz",
                                           "3 cores DMA" };

static double g_copy_ms = 10; /* the chosen copy's time in the startup test */

static int uses_dma(int m) { return m == COPY_DMA || m == COPY_CORES_DMA; }

static void copy_rdram(void *dst)
{
	switch (g_copy) {
	case COPY_MEMCPY: memcpy(dst, rdram, 0x800000); break;
	case COPY_DMA: copy_dma(g_lc, dst, rdram, 0x800000); break;
	case COPY_CORES: copy_cores(dst, rdram, 0x800000, 0); break;
	case COPY_CORES_DMA: copy_cores(dst, rdram, 0x800000, 1); break;
	default: copy_cold(dst, rdram, 0x800000, g_lines_ahead); break;
	}
}

/* Once at start: what each way of copying RDRAM costs, checked against the
 * source. The cheapest correct one is used for every capture. */
static void copy_test(void *dst)
{
	static const unsigned ahead[] = { 2, 4, 8, 16 };
	double best_ahead = 1e9;
	for (unsigned i = 0; i < sizeof(ahead) / sizeof(ahead[0]); i++) {
		OSTime t0 = OSGetSystemTime();
		copy_cold(dst, rdram, 0x800000, ahead[i]);
		double ms = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3;
		if (ms < best_ahead) {
			best_ahead = ms;
			g_lines_ahead = ahead[i];
		}
	}
	g_lc = lc_start();
	helpers_start();
	OSTime t0 = OSGetSystemTime();
	DCStoreRange(rdram, 0x800000);
	double store_ms = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3;
	/* Something to tell a good copy from a stale one. */
	for (unsigned i = 0; i < 0x800000 / 4; i += 997)
		rdram[i] ^= 0x5A5A0000 + i;
	char line[448];
	int n = snprintf(line, sizeof(line), "RESUME: copying 8 MB of RDRAM (best of 2; dcbt/dcbz %u lines ahead; "
	                 "writing back the caches alone %.1f ms):", g_lines_ahead, store_ms);
	double best = 1e9;
	for (int m = COPY_MEMCPY; m < COPY_METHODS; m++) {
		int helpers_ok = g_helpers_on && (m != COPY_CORES_DMA || (g_helper[0].lc && g_helper[1].lc));
		if ((uses_dma(m) && !g_lc) || (m >= COPY_CORES && !helpers_ok)) {
			n += snprintf(line + n, sizeof(line) - n, " %s unavailable;", g_copy_name[m]);
			continue;
		}
		int keep = g_copy;
		g_copy = m;
		double ms = 1e9;
		for (int k = 0; k < 2; k++) {
			memset(dst, 0xEE, 0x800000);
			DCStoreRange(dst, 0x800000);
			OSTime t0 = OSGetSystemTime();
			copy_rdram(dst);
			double t = OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3;
			if (t < ms)
				ms = t;
		}
		g_copy = keep;
		if (uses_dma(m))
			DCInvalidateRange(dst, 0x800000); /* read what the DMA wrote, not stale lines */
		int ok = !memcmp(dst, rdram, 0x800000);
		n += snprintf(line + n, sizeof(line) - n, " %s %.1f ms%s;", g_copy_name[m], ms, ok ? "" : " (WRONG)");
		if (ok && ms < best) {
			best = ms;
			g_copy = m;
		}
	}
	for (unsigned i = 0; i < 0x800000 / 4; i += 997)
		rdram[i] ^= 0x5A5A0000 + i;
	report_line("%s using %s", line, g_copy_name[g_copy]);
	g_copy_ms = best;
}


/* ---- what a state holds ---- */

typedef struct {
	void *at;
	unsigned size;
} part;

static char g_flash_infos[24], g_events[1024];
static unsigned g_events_len;
static uint32_t g_misc[4]; /* interp_addr, next_interupt, next_vi, vi_field */

/* A state is RDRAM (first, so it sits 32-byte aligned in the buffer, as the
 * DMA copy needs), these fixed-size parts in order, then the event queue
 * with its length. Version 3 had the in-game save memory after RDRAM: a
 * version 3 autosave still loads, without it. */
static unsigned parts(part *p)
{
	unsigned n = 0;
#define PART(a, s) (p[n].at = (void *)(a), p[n].size = (s), n++)
	PART(&rdram_register, sizeof(rdram_register));
	PART(&MI_register, sizeof(MI_register));
	PART(&pi_register, sizeof(pi_register));
	PART(&sp_register, sizeof(sp_register));
	PART(&rsp_register, sizeof(rsp_register));
	PART(&si_register, sizeof(si_register));
	PART(&vi_register, sizeof(vi_register));
	PART(&ri_register, sizeof(ri_register));
	PART(&ai_register, sizeof(ai_register));
	PART(&dpc_register, sizeof(dpc_register));
	PART(&dps_register, sizeof(dps_register));
	PART(SP_DMEM, 0x1000);
	PART(SP_IMEM, 0x1000);
	PART(PIF_RAM, 0x40);
	PART(g_flash_infos, sizeof(g_flash_infos));
	PART(tlb_e, sizeof(tlb_e));
	PART(&llbit, sizeof(llbit));
	PART(reg, sizeof(reg));
	PART(reg_cop0, sizeof(reg_cop0));
	PART(reg_cop1_fgr_64, sizeof(reg_cop1_fgr_64));
	PART(&FCR0, sizeof(FCR0));
	PART(&FCR31, sizeof(FCR31));
	PART(g_misc, sizeof(g_misc));
#undef PART
	return n;
}

#define MAX_PARTS 32
#define V3_SAVE_MEMORY (0x800 + FLASHRAM_SIZE + SRAM_SIZE + MEMPACK_SIZE) /* EEPROM, Flash, SRAM, Controller Pak */
#define RAW_MAX (0x800000 + V3_SAVE_MEMORY + 0x10000 + sizeof(g_events))

static unsigned char *g_raw, *g_now, *g_load, *g_work; /* autosaves; the paused game; a card to load; compressing */
static unsigned g_work_size, g_now_size, g_load_size, g_load_version;

static unsigned capture(unsigned char *buf)
{
	part p[MAX_PARTS];
	save_flashram_infos(g_flash_infos);
	g_misc[0] = interp_addr;
	g_misc[1] = next_interupt;
	g_misc[2] = next_vi;
	g_misc[3] = vi_field;
	copy_rdram(buf);
	unsigned at = 0x800000, n = parts(p);
	for (unsigned i = 0; i < n; i++) {
		memcpy(buf + at, p[i].at, p[i].size);
		at += p[i].size;
	}
	/* bounded: a queue past g_events once ran into the in-game save's failure
	 * time beside it (Racer's 266 VIs, 2026-10-30: "save from Aug 29, -3689800") */
	g_events_len = save_eventqueue_infos_max(g_events, sizeof(g_events));
	memcpy(buf + at, &g_events_len, 4);
	memcpy(buf + at + 4, g_events, g_events_len);
	return at + 4 + g_events_len;
}

/* Where the parts start in a state of this version, or 0 when its size
 * does not fit the layout (then nothing may be restored from it). */
static unsigned layout(const unsigned char *buf, unsigned size, unsigned version)
{
	part p[MAX_PARTS];
	unsigned n = parts(p), first = 0x800000 + (version == 3 ? V3_SAVE_MEMORY : 0), at = first;
	for (unsigned i = 0; i < n; i++)
		at += p[i].size;
	unsigned events_len;
	if (size < at + 4)
		return 0;
	memcpy(&events_len, buf + at, 4);
	/* a queue longer than g_events is damage: it ran into the memory beside
	 * g_events when that state was made (Racer, 2026-10-30; loaded, it froze) */
	if (events_len > sizeof(g_events) || size != at + 4 + events_len)
		return 0;
	return first;
}

/* The TLB lookup tables, rebuilt from the TLB entries as TLBWI fills them. */
static void tlb_rebuild(void)
{
	tlb_mem2_init();
	for (int e = 0; e < 32; e++) {
		const tlb *t = &tlb_e[e];
		unsigned long start[2] = { t->start_even, t->start_odd }, end[2] = { t->end_even, t->end_odd };
		unsigned long phys[2] = { t->phys_even, t->phys_odd };
		int v[2] = { t->v_even, t->v_odd }, d[2] = { t->d_even, t->d_odd };
		for (int h = 0; h < 2; h++) {
			if (!v[h] || start[h] >= end[h] || (start[h] >= 0x80000000 && end[h] < 0xC0000000) ||
			    phys[h] >= 0x20000000)
				continue;
			for (unsigned long i = start[h]; i < end[h]; i++) {
				tlb_LUT_r[i >> 12] = 0x80000000 | (phys[h] + (i - start[h]));
				if (d[h])
					tlb_LUT_w[i >> 12] = 0x80000000 | (phys[h] + (i - start[h]));
			}
		}
	}
}

/* The in-game save memory is left as it is: the files' (Zach, 2026-10-16). */
static int restore(const unsigned char *buf, unsigned size, unsigned version)
{
	part p[MAX_PARTS];
	unsigned at = layout(buf, size, version);
	if (!at)
		return -1;
	memcpy(rdram, buf, 0x800000);
	unsigned n = parts(p);
	for (unsigned i = 0; i < n; i++) {
		memcpy(p[i].at, buf + at, p[i].size);
		at += p[i].size;
	}
	load_flashram_infos(g_flash_infos);
	set_fpr_pointers(Status);
	static const int rounding[4] = { 0x33F, 0xF3F, 0xB3F, 0x73F }; /* as CTC1 sets it */
	rounding_mode = rounding[FCR31 & 3];
	tlb_rebuild();
	for (int i = 0; i < 0x100000; i++)
		invalid_code_set(i, 1);
	interp_addr = g_misc[0];
	next_interupt = g_misc[1];
	next_vi = g_misc[2];
	vi_field = g_misc[3];
	load_eventqueue_infos((char *)buf + at + 4); /* after the parts: the queue's length, then the queue */
	last_addr = interp_addr;
	skip_jump = 0;
	delay_slot = 0;

	video_vi_status_changed();
	video_vi_width_changed();
	aiDacrateChanged(player_is_pal() ? SYSTEM_PAL : SYSTEM_NTSC);
	return 0;
}

/* ---- what reached the SD card ---- */

static volatile time_t g_last_ok;       /* the newest autosave on the SD */
static volatile int g_last_ok_launch;   /* ... is the one this launch started from */
static volatile int g_autosave_failed;  /* the newest autosave did not reach the SD */
static volatile time_t g_ingame_failed; /* an in-game save that did not, 0 none */

void states_write_done(int what, int ok)
{
	if (!ok)
		player_write_failed();
	if (what == WRITE_INGAME)
		g_ingame_failed = ok ? 0 : time(NULL);
	else
		g_autosave_failed = !ok;
}

/* ---- resume points ---- */

static char g_dir[256];
static char g_path[2][384];
static resume_plan g_plan;
static resume_timer g_timer;
static int g_on, g_exit, g_exit_done;
static int g_pause;  /* HOME: capture into g_now and run the pause menu */
static int g_soon;   /* after a load or reset: an autosave at the first boundary, whatever the idle time */
static int g_action; /* ACTION_ the menu chose; go() stops for it */
static double g_idle_min = 1e9, g_idle_sum, g_cost_max; /* since the last states_stats */
static unsigned g_idle_vis, g_captures, g_misses, g_gfx_busy;
static double g_due_since = -1; /* when the capture asked for became due */
/* The game is still the resume point it launched from: set when that state is
 * restored, cleared by the first resume point written or the first pause. */
static int g_as_loaded;
static unsigned g_loaded_vi;

/* The buffer the next resume point job writes, and the one being written
 * (under the writer lock), so a buffer is never captured into while the
 * writer still reads it. */
static unsigned char *g_post_src, *g_job_src;
static unsigned g_post_size;

static void write_job(void)
{
	writer_lock();
	unsigned char *src = g_job_src = g_post_src;
	unsigned size = g_post_size;
	g_post_src = NULL;
	writer_unlock();
	if (!src)
		return;
	OSTime t0 = OSGetSystemTime();
	if (uses_dma(g_copy))
		DCInvalidateRange(src, 0x800000); /* this core may hold lines from the last state */
	state_info info = { .crc1 = ROM_HEADER.CRC1, .crc2 = ROM_HEADER.CRC2, .counter = g_plan.next_counter };
	info.saved_at = (uint32_t)time(NULL);
	const char *path = g_path[g_plan.next_file];
	unsigned bytes = 0;
	int r = statefile_pack(&info, src, size, 0, g_work, g_work_size, &bytes);
	OSTime t1 = OSGetSystemTime();
	if (r == STATEFILE_OK)
		r = statefile_store_over(path, g_work, bytes);
	OSTime t2 = OSGetSystemTime();
	writer_slow(strrchr(path, '/') + 1, OSTicksToMicroseconds(t2 - t1) / 1e3);
	if (r == STATEFILE_OK) {
		report_line("RESUME: point %u written to %s: %u KB in %.0f ms (compress %.0f, write %.0f; core 2)",
		            (unsigned)info.counter, strrchr(path, '/') + 1, bytes / 1024,
		            OSTicksToMicroseconds(t2 - t0) / 1e3, OSTicksToMicroseconds(t1 - t0) / 1e3,
		            OSTicksToMicroseconds(t2 - t1) / 1e3);
		g_plan.next_file = 1 - g_plan.next_file;
		g_plan.next_counter++;
		g_last_ok = info.saved_at;
		g_last_ok_launch = 0;
	} else {
		report_line("RESUME FAILED: point not written to %s (SD full, locked or removed?)", path);
	}
	states_write_done(WRITE_AUTOSAVE, r == STATEFILE_OK);
	writer_lock();
	g_job_src = NULL;
	writer_unlock();
}

static void post_resume(unsigned char *src, unsigned size)
{
	g_as_loaded = 0;
	writer_lock();
	g_post_src = src;
	g_post_size = size;
	writer_unlock();
	writer_post(WRITER_RESUME, write_job);
}

static int buffer_busy(const unsigned char *buf)
{
	writer_lock();
	int busy = g_job_src == buf || g_post_src == buf;
	writer_unlock();
	return busy;
}

static void forget_job(void)
{
	writer_lock();
	g_post_src = NULL; /* posting this replaced any resume point job still waiting */
	writer_unlock();
	resume_forget(g_path[0], g_path[1]);
	report_line("RESUME: points deleted, so an older one cannot undo the in-game save that follows");
}

static int g_launch = LAUNCH_FIRST;
static time_t g_launch_when;

/* Settings > Games' Auto-Resume, read as it is now: turned off from a paused
 * game, no resume point is written from that pause on. Off, nothing waits for
 * a capture: in-game saves go straight to the writer (states_vi). */
static int resume_on(void) { return setup_settings()->auto_resume; }

int states_launch(time_t *when)
{
	*when = g_launch_when;
	return g_launch;
}

int states_on(void) { return g_on; }

int states_start(const char *root, int fresh)
{
	char id[64];
	saves_game_id(id, sizeof(id), ROM_HEADER.Name, ROM_HEADER.CRC1);
	snprintf(g_dir, sizeof(g_dir), "%s/states", root);
	mkdir(g_dir, 0777);
	snprintf(g_dir, sizeof(g_dir), "%s/states/%s", root, id);
	mkdir(g_dir, 0777);
	snprintf(g_path[0], sizeof(g_path[0]), "%s/resume-a.o64s", g_dir);
	snprintf(g_path[1], sizeof(g_path[1]), "%s/resume-b.o64s", g_dir);

	if (!g_work) { /* once: the buffers and the copy test serve every game this session */
		g_raw = memalign(64, RAW_MAX);
		g_now = memalign(64, RAW_MAX);
		g_load = memalign(64, RAW_MAX);
		g_work_size = statefile_work_size(RAW_MAX);
		g_work = memalign(64, g_work_size);
		if (!g_raw || !g_now || !g_load || !g_work) {
			report_line("RESUME: no memory for resume points and save states; off this session");
			free(g_raw); free(g_now); free(g_load); free(g_work);
			g_raw = g_now = g_load = g_work = NULL;
			return -1;
		}
		copy_test(g_work);
	}
	resume_timer_init(&g_timer, report_now_us());
	g_timer.cost_us = (g_copy_ms + 2) * 1e3; /* the rest of a capture is about 2 ms; measured from the first one on */

	static const char *const why[] = { "", "none yet", "damaged", "from another Our64 version", "for another game",
	                                   "too big", "", "" };
	unsigned size = 0;
	if (!resume_on()) { /* no resume point read: the game starts fresh and running (main.c pauses only LAUNCH_RESUME) */
		resume_plan none = { 0, 1, -1 };
		g_plan = none;
		g_launch = LAUNCH_FIRST;
		report_line("RESUME: Auto-Resume is off: the game starts fresh, no resume point read or written");
		g_on = 1;
		return 0;
	}
	int r = resume_load(g_path[0], g_path[1], ROM_HEADER.CRC1, ROM_HEADER.CRC2, &g_plan, g_load, RAW_MAX, &size);
	if (r == STATEFILE_OK) {
		state_info info;
		struct stat st;
		if (statefile_peek(g_path[g_plan.loaded_file], ROM_HEADER.CRC1, ROM_HEADER.CRC2, &info, g_work) == STATEFILE_OK) {
			g_load_version = info.version;
			g_launch_when = info.saved_at;
		}
		if (!g_launch_when && !stat(g_path[g_plan.loaded_file], &st))
			g_launch_when = st.st_mtime;
		g_load_size = size;
		g_last_ok = g_launch_when;
		g_last_ok_launch = 1;
	}
	g_launch = r == STATEFILE_OK ? (fresh ? LAUNCH_FRESH : LAUNCH_RESUME)
	         : r == STATEFILE_OTHER_VERSION ? LAUNCH_OTHER_VERSION
	         : r == STATEFILE_OTHER_GAME ? LAUNCH_OTHER_GAME
	         : r == STATEFILE_MISSING ? LAUNCH_FIRST : LAUNCH_DAMAGED;
	if (r == STATEFILE_OK && fresh) {
		report_line("RESUME: B held at start: the game starts fresh (resume point %s kept until the next one)",
		            strrchr(g_path[g_plan.loaded_file], '/') + 1);
	} else if (r == STATEFILE_OK) {
		savestates_job |= LOADSTATE;
		report_line("RESUME: resuming from %s (version %u)", strrchr(g_path[g_plan.loaded_file], '/') + 1,
		            g_load_version);
	} else if (r != STATEFILE_MISSING) {
		report_line("RESUME: resume point not used (%s); the game starts fresh", why[r]);
	}
	g_on = 1;
	return 0;
}

void savestates_load(void)
{
	OSTime t0 = OSGetSystemTime();
	if (restore(g_load, g_load_size, g_load_version))
		report_line("RESUME: the resume point does not match this build's layout; the game starts fresh");
	else {
		report_line("RESUME: state restored in %.0f ms", OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
		g_as_loaded = g_launch == LAUNCH_RESUME && !g_soon;
		g_loaded_vi = player_vis();
	}
}

/* HOME: the game captured into g_now, its autosave and in-game saves to the
 * SD, and the menu, with the game frozen here until it closes. */
static void pause_here(void)
{
	g_pause = 0;
	g_soon = 0; /* this capture is the autosave a load or reset asked for */
	savestates_job &= ~SAVESTATE;
	if (buffer_busy(g_now)) { /* paused again before the last pause's autosave was written */
		OSTime t0 = OSGetSystemTime();
		while (buffer_busy(g_now))
			OSSleepTicks(OSMillisecondsToTicks(2));
		report_line("PAUSE: waited %.0f ms for the last pause's autosave", OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	}
	OSTime t0 = OSGetSystemTime();
	g_now_size = capture(g_now);
	double cost = OSTicksToMicroseconds(OSGetSystemTime() - t0);
	/* The pause a resume launch opens with comes a few VIs after the state it
	 * loaded, which is on the SD already: writing it again only kept "Saving to
	 * SD card..." up over the menu. */
	int as_loaded = g_as_loaded && player_vis() - g_loaded_vi < 60 && !saves_pending();
	g_as_loaded = 0;
	const int resume = resume_on(); /* off: g_now is still captured, for Save state and the menu's picture */
	if (!as_loaded && resume)
		post_resume(g_now, g_now_size);
	saves_flush(); /* in-game saves the game made go after it */
	resume_captured(&g_timer, report_now_us(), cost);
	g_due_since = -1;
	report_line("PAUSE: game captured at VI %u in %.1f ms%s", player_vis(), cost / 1e3,
	            !resume ? " (Auto-Resume off: not written)"
	            : as_loaded ? " (not written: it is the resume point just loaded)" : "");
	switch (pause_run(1)) {
	case PAUSE_LOAD:
		g_action = ACTION_LOAD;
		stop = 1;
		break;
	case PAUSE_RESET:
		g_action = ACTION_RESET;
		stop = 1;
		break;
	case PAUSE_EXIT:
	case PAUSE_ROMS:
		g_exit = g_exit_done = 1; /* the autosave from this pause is the last one */
		stop = 1;
		break;
	}
}

void savestates_save(void)
{
	/* Unless leaving, pausing or just after a load: at the start of a VI, when the wait ahead of it covers the copy. */
	int vi_start = next_interupt == next_vi;
	if (!g_on || g_action || (!g_exit && !g_pause && !g_soon && !vi_start))
		return;
	/* Not mid-jump, and nothing on the graphics core the state would miss. */
	if (skip_jump || delay_slot || !gfx_idle()) {
		g_gfx_busy += vi_start;
		return;
	}
	if (g_pause) {
		pause_here();
		return;
	}
	if (!resume_on()) { /* turned off while a capture was asked for: none is taken */
		savestates_job &= ~SAVESTATE;
		g_soon = 0;
		if (g_exit)
			g_exit_done = 1;
		return;
	}
	if (!g_exit && !g_soon && !resume_fits(&g_timer, player_idle_left_us())) {
		g_misses++;
		return;
	}
	if (buffer_busy(g_raw))
		writer_drain(); /* only on exit or right after a load: otherwise nothing is asked while the writer has one */
	OSTime t0 = OSGetSystemTime();
	unsigned size = capture(g_raw);
	double cost = OSTicksToMicroseconds(OSGetSystemTime() - t0);
	savestates_job &= ~SAVESTATE;
	post_resume(g_raw, size);
	saves_release(); /* in-game saves waiting for this capture go after it */
	resume_captured(&g_timer, report_now_us(), cost);
	g_captures++;
	if (cost > g_cost_max)
		g_cost_max = cost;
	report_line("RESUME: captured at VI %u in %.1f ms, %.1f s after it was due%s", player_vis(), cost / 1e3,
	            g_due_since >= 0 ? (report_now_us() - g_due_since) / 1e6 : 0,
	            g_exit ? " (leaving)" : g_soon ? " (after a load or reset)" : "");
	g_due_since = -1;
	g_soon = 0;
	if (g_exit) {
		g_exit_done = 1;
		stop = 1; /* nothing runs after the last capture, so the saves written next match it */
	}
}

void states_stats(char *buf, unsigned size)
{
	snprintf(buf, size, "resume: idle per VI min %.1f avg %.1f ms, %u captures (longest %.1f ms); a due one passed "
	         "%u VIs with too little idle time, %u with a graphics task in flight", g_idle_vis ? g_idle_min / 1e3 : 0,
	         g_idle_vis ? g_idle_sum / g_idle_vis / 1e3 : 0, g_captures, g_cost_max / 1e3, g_misses, g_gfx_busy);
	g_idle_min = 1e9;
	g_idle_sum = g_cost_max = 0;
	g_idle_vis = g_captures = g_misses = g_gfx_busy = 0;
}

/* Each VI, with the time this VI spent waiting (for the VI or for audio). */
void states_vi(double now_us, double idle_us)
{
	if (idle_us < g_idle_min)
		g_idle_min = idle_us;
	g_idle_sum += idle_us;
	g_idle_vis++;
	if (!g_on || !resume_on()) { /* no resume point to wait for: in-game saves go now */
		saves_release();
		return;
	}
	if (g_exit || g_pause || g_soon || g_action)
		return;
	switch (resume_decide(&g_timer, now_us, saves_waiting_since(), writer_pending(WRITER_RESUME))) {
	case RESUME_DUE: /* savestates_save takes it at the first VI with room */
		if (!(savestates_job & SAVESTATE))
			g_due_since = now_us;
		savestates_job |= SAVESTATE;
		break;
	case RESUME_GIVE_UP:
		savestates_job &= ~SAVESTATE;
		g_due_since = -1;
		report_line("RESUME: no room for a capture in 5 s, so the in-game save goes without a resume point");
		writer_post(WRITER_RESUME, forget_job);
		saves_release();
		break;
	}
}

/* Leaving: one last capture, whatever the idle time. */
void states_exit(void)
{
	g_exit = 1;
	if (!resume_on()) { /* nothing to capture: main.c stops at once, saves_stop flushes the in-game saves */
		g_exit_done = 1;
		return;
	}
	savestates_job |= SAVESTATE;
}

int states_exit_done(void) { return !g_on || g_exit_done; }

/* The game ended (its writes are drained): the next game starts with none of its state. */
void states_stop(void)
{
	writer_drain();
	g_on = g_exit = g_exit_done = g_pause = g_soon = g_as_loaded = 0;
	g_action = ACTION_NONE;
	g_due_since = -1;
	g_launch = LAUNCH_FIRST;
	g_launch_when = g_last_ok = 0;
	g_last_ok_launch = g_autosave_failed = 0;
	g_ingame_failed = 0;
	g_load_size = g_load_version = 0;
	savestates_job = 0;
}

void states_pause(void)
{
	if (g_exit || g_action)
		return;
	if (!g_on) { /* no state to keep: the menu runs here, with Save, Load and Reset off */
		int done = pause_run(0);
		if (done == PAUSE_EXIT || done == PAUSE_ROMS)
			stop = 1;
		return;
	}
	g_pause = 1;
	savestates_job |= SAVESTATE;
}

/* ---- save state cards ---- */

static uint16_t g_thumb[THUMB_W * THUMB_H] __attribute__((aligned(64)));
static struct {
	int card, keep_old, undo;
} g_card_job;
static volatile int g_job_running, g_job_result;

uint16_t *states_now_thumb(void) { return g_thumb; }

static void card_job(void)
{
	OSTime t0 = OSGetSystemTime();
	const int card = g_card_job.card;
	if (uses_dma(g_copy))
		DCInvalidateRange(g_now, 0x800000);
	state_info info = { .crc1 = ROM_HEADER.CRC1, .crc2 = ROM_HEADER.CRC2 };
	info.saved_at = (uint32_t)time(NULL);
	info.slot = card < CARD_SLOTS ? card + 1 : 0;
	info.before = (uint8_t)g_card_job.undo;
	int r = card_save(g_dir, card, &info, g_card_job.keep_old, g_now, g_now_size, g_thumb, g_work, g_work_size);
	writer_slow(card_file(card), OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	report_line("STATES: %s %s in %.0f ms%s", card_file(card), r == STATEFILE_OK ? "written" : "NOT written",
	            OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3,
	            r == STATEFILE_BACKUP_FAILED ? " (the old copy could not become the Undo card)" : "");
	g_job_result = r;
	__sync_synchronize();
	g_job_running = 0;
}

void states_card_save(int card, int keep_old, int undo)
{
	g_card_job.card = card;
	g_card_job.keep_old = keep_old;
	g_card_job.undo = undo;
	g_job_result = STATEFILE_WRITE_FAILED;
	g_job_running = 1;
	__sync_synchronize();
	writer_post(WRITER_STATE, card_job);
}

int states_job_running(void) { return g_job_running; }
int states_job_result(void) { return g_job_result; }

int states_card_scan(int card, pause_card *c, void *thumb)
{
	state_info info;
	int r = card_peek(g_dir, card, ROM_HEADER.CRC1, ROM_HEADER.CRC2, &info, thumb);
	memset(c, 0, sizeof(*c));
	c->status = r == STATEFILE_OK ? CARD_FILLED
	          : r == STATEFILE_MISSING ? CARD_EMPTY
	          : r == STATEFILE_OTHER_VERSION ? CARD_OTHER_VERSION
	          : r == STATEFILE_OTHER_GAME ? CARD_OTHER_GAME : CARD_DAMAGED;
	if (r == STATEFILE_OK) {
		c->when = info.saved_at;
		c->slot = info.slot;
		c->undo = card == CARD_UNDO ? card_undo_kind(&info) : 0;
	}
	if (r != STATEFILE_OK || !info.has_thumb)
		memset(thumb, 0, THUMB_BYTES);
	return r == STATEFILE_OK && info.has_thumb;
}

int states_card_read(int card)
{
	OSTime t0 = OSGetSystemTime();
	state_info info;
	unsigned size = 0;
	int r = card_read(g_dir, card, ROM_HEADER.CRC1, ROM_HEADER.CRC2, &info, g_load, RAW_MAX, &size);
	if (r == STATEFILE_OK && !layout(g_load, size, info.version))
		r = STATEFILE_DAMAGED;
	if (r == STATEFILE_OK) {
		g_load_size = size;
		g_load_version = info.version;
	}
	report_line("STATES: %s read in %.0f ms: %s", card_file(card), OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3,
	            r == STATEFILE_OK ? "good" : "can't be loaded");
	return r;
}

int states_card_delete(int card)
{
	int r = card_delete(g_dir, card);
	report_line("STATES: %s %s", card_file(card), r ? "NOT deleted" : "deleted");
	return r;
}

int states_writing(void)
{
	for (int s = WRITER_RESUME; s < WRITER_STATE; s++)
		if (writer_pending(s))
			return 1;
	return 0;
}

void states_autosave(pause_autosave *a)
{
	memset(a, 0, sizeof(*a));
	a->last_ok = g_last_ok;
	a->from_launch = g_last_ok_launch;
	a->writing = states_writing();
	a->autosave_failed = g_autosave_failed;
	a->ingame_failed = g_ingame_failed;
	a->off = !resume_on();
}

/* Auto-Resume turned off (Settings > Games, confirmed): the writer drained
 * first, so a resume point still being written cannot land after the
 * deletion; then every game's resume points go (resume_forget_all). A game
 * paused now starts its next resume point afresh if Auto-Resume comes back on. */
int states_autosaves(const char *root)
{
	writer_drain(); /* one being written counts */
	return resume_games_with_points(root);
}

int states_forget_all(const char *root, void (*each)(int done, int total, void *ctx), void *ctx)
{
	writer_drain();
	const int n = resume_forget_all(root, each, ctx);
	resume_plan none = { 0, 1, -1 };
	g_plan = none;
	g_last_ok = 0;
	g_last_ok_launch = g_autosave_failed = g_as_loaded = g_soon = 0;
	report_line("RESUME: Auto-Resume off: %d resume point files deleted", n);
	return n;
}

/* An autosave that cannot be used is kept, renamed, so the new game's
 * autosaves never replace it (another Our64 version may still load it). */
void states_set_aside(void)
{
	char to[400];
	for (int i = 0; i < 2; i++) {
		snprintf(to, sizeof(to), "%s/resume-%c.unused.o64s", g_dir, 'a' + i);
		remove(to);
		rename(g_path[i], to);
	}
	report_line("RESUME: the unusable autosave is kept as resume-a/b.unused.o64s");
	resume_plan fresh = { 0, 1, -1 };
	g_plan = fresh;
}

int states_take_action(void)
{
	int a = g_action;
	g_action = ACTION_NONE;
	return a;
}

/* Outside go(): the card read last replaces the game. Then an autosave of
 * it at the first boundary, so the next start is not the game left behind. */
void states_load_now(void)
{
	OSTime t0 = OSGetSystemTime();
	if (restore(g_load, g_load_size, g_load_version))
		report_line("STATES: the card no longer matches (checked when read); the game runs on unchanged");
	else
		report_line("STATES: loaded in %.0f ms", OSTicksToMicroseconds(OSGetSystemTime() - t0) / 1e3);
	states_after_reset();
}

void states_after_reset(void)
{
	if (!resume_on())
		return;
	g_soon = 1;
	savestates_job |= SAVESTATE;
}
