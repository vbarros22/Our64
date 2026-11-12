/* Our64 player: what the C side (core glue) and the C++ side (GLideN64) share. */
#ifndef OUR64_PLAYER_H
#define OUR64_PLAYER_H
#include <stdint.h>
#include <time.h>
#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Not64's memory and registers, as GLideN64's GFX_INFO wants them. */
typedef struct {
	unsigned char *rdram, *dmem, *imem, *header;
	unsigned int *mi_intr, *sp_status;
	unsigned int *dpc[8];  /* start end current status clock bufbusy pipebusy tmem */
	unsigned int *vi[14];  /* status origin width intr current burst v_sync h_sync leap
	                          h_start v_start v_burst x_scale y_scale */
} player_memory;
void player_get_memory(player_memory *m);  /* plugins.c */

/* video.cpp: GLideN64 on the GX2 backend, on the graphics core (where GX2
 * is set up). Everything here is called from the CPU core. */
int  video_thread_start(unsigned core); /* sets up GX2 on that core; 0 on success */
void video_thread_stop(void);
void video_idle_frame(void);      /* one dark red frame, after the game stopped */
int  video_start(void);           /* after the ROM is loaded; 0 on success */
void video_stop(void);
void video_set_picture(int wide); /* 4:3 or stretched to 16:9; a running game changes at its next frame */
void video_set_crt(int look);     /* TV look (LOOK_): CRT scanlines on the TV (game frames), from the next frame */
void video_set_filter(int n64);   /* textures: standard bilinear or the N64's 3-point; read at video_start */
unsigned video_submit_task(void); /* posts the graphics task in DMEM; returns its number */
void video_submit_rdp_list(void); /* posts the RDP commands from DPC_CURRENT to DPC_END */
unsigned video_tasks_done(void);  /* graphics tasks decoded so far */
/* What task did to SP status, and whether it raised the DP interrupt. Kept
 * for VIDEO_RESULTS tasks after it is decoded. */
#define VIDEO_RESULTS 64
void video_task_result(unsigned task, uint32_t *sp_set, uint32_t *sp_clear, int *dp, int *running);
int video_signal_ucode(void);     /* the last task's microcode waits on SP signals mid-task (ZSortBOSS) */
void video_set_signal_ucode(int on); /* the game is known to use one, before its first task is decoded */
void cpuprof_start(const char *root, volatile int *paused); /* sd:/wiiu/our64/profile-cpu (cpuprof.c) */
void cpuprof_save(const char *name); /* before cpu_deinit: profile/cpu-<name>-<k>.prof */
void cpuprof_restart(void);       /* a reset: the samples so far are dropped */
int  video_wait_task(unsigned task, double timeout_us); /* < 0: no timeout; 1 when decoded */
void video_sync(void);            /* waits until the graphics core is idle */
int  video_frame_dump_on(void);   /* sd:/wiiu/our64/frame-dump: GamePad minus saves screens */
void video_frame_dump_start(void);
void video_reset_rsp(void);       /* after a reset or load: GLideN64 starts the next task afresh */
void video_update_screen(void);   /* each VI */
void video_vi_status_changed(void);
void video_vi_width_changed(void);
typedef struct {
	double task_us, screen_us;    /* graphics core: decoding, VIs (totals) */
	double cpu_wait_us;           /* CPU core waiting for the graphics core (total) */
	double cpu_wait_part_us[4];   /* of which: the ring full, calls, a task while ahead, a task needed */
	double worst_task_us;         /* longest task since the last call */
	double gpu_wait_us;           /* graphics core waiting for the GPU at frame ends (total) */
	unsigned frames_shown, frames_late, frames_dropped; /* shown (late: held for a flip first), replaced unshown */
	unsigned frames_trimmed; /* queued VIs dropped because the queue never emptied for a second (video.cpp) */
	unsigned frames_new, frames_short, frames_long; /* game frames on the TV; of them, held fewer or more vsyncs than the game's VIs */
	/* The frame check (video.cpp g_vi_marks): totals since the app started. */
	unsigned tv_trimmed;      /* TV clock: repeats left unswapped on a still screen (latency back) */
	unsigned tv_waited;       /* TV clock: presents that waited for a flip (the queue full) */
	unsigned tv_pending_sum, tv_pending_n; /* TV clock: swaps waiting at each present, summed */
	unsigned vis_marked;      /* emulated VIs */
	unsigned vis_unpresented; /* of them, never given to the graphics core (it still had one) */
	unsigned game_frames;     /* VIs with a new origin */
	unsigned tv_vsyncs;       /* the TV's vsyncs */
	unsigned fc_flips;        /* TV flips that showed a game VI */
	unsigned fc_skipped;      /* VIs the TV passed over: shown fewer vsyncs than the N64 would */
	unsigned fc_repeated;     /* vsyncs a VI stayed on the TV past its time */
	unsigned fc_never;        /* game frames none of whose VIs reached the TV */
	double latency_us;        /* VI to TV flip, summed over latency_n flips */
	unsigned latency_n;
	double latency_max_us;    /* the most since the last call */
	double queue_wait_us;    /* time VIs waited in the queue before the TV took them, summed */
	unsigned vis_taken;      /* queued VIs the TV took */
} video_stats_t;
void video_stats(video_stats_t *s);
void video_pace_write(const char *id);
void video_pace_start(void);      /* the flip record starts over now (frame test, main.c) */
int  video_pace_write_to(const char *path); /* the record so far; 1 when written */
void video_set_frame_check(int on); /* off for PAL games (video.cpp check_flip) */
void video_gpu_profile(int on); /* the frame test: a GPU profile of every 10th frame, no wait for the GPU */
void video_gpu_profile_report(char *out, unsigned size); /* its line (blocks until the graphics core reads it) */
void video_set_tv_clock(int on); /* the VI clock is the TV's vsync (video.cpp, main.c new_vi) */
int video_tv_is_60hz(void);
unsigned video_tv_period_ns(void); /* the TV's refresh: 16683333 or 20000000 */
void video_set_pal_cadence(int on); /* a PAL game on a 59.94 Hz TV: every fifth VI due two vsyncs */
unsigned video_last_vi_hold(void); /* vsyncs the VI just numbered is due on the TV */      /* the TV runs at 59.94 Hz (not 576i's 50) */ /* the flip record (sd:/wiiu/our64/pace or the flip queue): pace-<game>.bin (video.cpp) */
double video_busy_s(unsigned *cmd); /* how long the current command has run; 0 when idle */
void video_backtrace(char *out, unsigned size); /* the graphics core's PC and return addresses, for the log */

/* audio.c: the game's AI DMAs through two AX voices. */
int  audio_start(void);           /* 0 on success */
void audio_stop(void);
void audio_set_rate(unsigned rate);                    /* N64 output rate, Hz */
void audio_push(const uint32_t *frames, unsigned count); /* stereo frames, left in the high half */
int  audio_paces(void);           /* the game sent audio lately: audio_push sets the pace */
void audio_set_unpaced(int on);   /* the TV clock paces the game: audio_push never waits */
int  audio_ring_full(void);       /* the audio ring holds FULL_MS or more */
unsigned audio_underruns(void);   /* times the ring ran dry and the voices stopped */
void audio_smoothing(int *lowest_pm, int *highest_pm, unsigned *dips); /* playback rate range since last call, volume dips */
double audio_wait_us(void);       /* total time audio_push waited for room */
void audio_route(int tv, int drc); /* which speakers play */
void audio_debug(unsigned *pushed, unsigned *dropped, unsigned *rate, int *playing); /* frame totals */

/* main.c: the GamePad as N64 controller 1, read once per VI. */
/* input_wiiu.c: controllers, read once per VI on the CPU core */
void input_init(void);            /* at launch: controllers are read from here on */
int  input_start(const char *root, const char *game_id); /* the game's mappings */
void input_read(void);
uint32_t input_keys(int port);    /* BUTTONS.Value for N64 port 0-3 */
void input_age_reset(void);       /* input delay: the reading's age at the game's polls (issue #19) */
unsigned input_age_stats(double *median, double *p95, double *p99, double *most);
int  input_waiting_port(void);    /* a port whose controller is gone, or -1 */
int  input_take_home(void);       /* HOME was pressed on any controller since the last call */
void input_set_joining(int on);   /* off while the prompt runs: presses put no controller in a port */
void input_withhold(void);        /* a menu closes: buttons held now reach the game only once released */
void input_rumble_allow(int port, int on); /* the player's Rumble setting for an N64 port */
void input_rumble_hold(int held); /* a menu is up: every motor stops until released */
void input_rumble_reset(void);    /* a game ended, loaded or reset: every motor off */
void input_claim(int id);         /* the controller that answered the prompt is player 1; its held
                                     buttons reach the game once released */
/* Our Controllers and Edit buttons (input_wiiu.c). */
void input_map_get(int type, input_map *m, int *profile); /* the buttons in use for a type, and whether from the game's profile */
void input_map_all(int type, input_map *m); /* the all-games buttons for a type, profile or not */
int  input_map_save(int type, const input_map *m, int profile); /* 0 when written */
int  input_profile_remove(int type);
void input_port_info(int port, int *type, int *battery, int *charging, int *gone);
int  input_port_owner(int port);  /* controller id, -1 */
int  input_controller_type(int id);
int  input_connected_count(void);
void input_reorder(const int *ids, int n);
void input_set_players(const int *ids, int n); /* only these, in order; the rest leave their ports */
int  player_ask_exit(void);      /* pause_wiiu.c: HOME outside a game asks "Exit to the Wii U Menu?"; 1 to leave */
int  player_ask(const char *title, const char *body, const char *action); /* pause_wiiu.c: another question there; 1 for action */
void input_set_notices(int on); /* player notices (joined, gone, back) only while a game runs */
void input_game_start(void);     /* a game starting: controllers that are gone leave their ports; each port with one is present */
void player_set_paks(const int *pak); /* plugins.c: PAK_ (gamedb.h) for ports 1-4 */
void player_set_pak(int port, int pak);
void player_pak_rumble_only(void); /* plugins.c: a Rumble Pak game whose Pak checks read the data area */
/* For menus, after input_read: per controller (0 the GamePad, 1-4 Wii Remote
 * channels), its INPUT_ type or -1, and SRC_ bits pressed by that read (the
 * left stick counts as SRC_LSTICK_ directions). Touch is on the UI canvas. */
typedef struct {
	int type[5];
	uint32_t pressed[5];
	uint32_t held[5];             /* SRC_ bits held now (for repeats while held) */
	uint32_t sources[5];          /* SRC_ bits pressed by that read, both sticks included (Edit buttons) */
	int last_id;                  /* the controller pressed last, -1 */
	int touch, touch_x, touch_y;
} input_menu;
void input_menu_state(input_menu *m);
int  input_take_capture(void);    /* the debug capture combination was pressed */
int  input_take_flipq(void);      /* dev: GamePad L + R + minus, the GPU flip queue on or off */
int  video_flipq_toggle(void);    /* video.cpp: switches it, returns 1 when now on */
void player_port_present(int port, int present); /* plugins.c */

void player_plugins_init(void);
void gfx_watch_vi(unsigned vi);  /* plugins.c: logs a stall report once a game starts no RSP task for 180 VIs */

/* saves_wiiu.c: in-game saves in sd:/wiiu/our64/saves, on the CPU core. */
int  saves_start(const char *root); /* after the ROM and memory are set up: loads the game's saves */
void saves_poll(double now_us);     /* each VI: marks saves the game stopped changing as due */
double saves_waiting_since(void);   /* when the oldest due save started waiting for a resume point; < 0: none */
int  saves_pending(void);           /* an in-game save the game made is not with the writer yet */
void saves_release(void);           /* hands every due save to the SD writer (after a resume point) */
void saves_flush(void);             /* leaving: every unsaved change to the writer */
void saves_stop(void);              /* the game ended: flushed, and nothing of it kept */

/* states_wiiu.c: resume points in sd:/wiiu/our64/states/<game id>. */
int  states_start(const char *root, int fresh); /* after saves_start; resumes unless fresh or Auto-Resume is off */
void states_vi(double now_us, double idle_us);  /* each VI, with the time it spent waiting */
void states_exit(void);           /* leaving: ask for a last capture */
int  states_launch(time_t *when); /* LAUNCH_ (prompt.h): what this launch does, and the resume point's time */
void player_write_failed(void);   /* main.c: an in-game save or resume point did not reach the SD (count) */
int  player_write_failures(void);
int  states_exit_done(void);      /* the last capture is taken (or resume points are off) */
void states_stop(void);           /* the game ended: writes drained, nothing of it kept */
int  gfx_idle(void);              /* plugins.c: no graphics task in flight */
void gfx_reset(void);             /* plugins.c: outside go(), before a load or reset: nothing in flight */
unsigned player_vis(void);        /* main.c: VIs so far */
double player_idle_left_us(void); /* main.c: at the start of a VI, the wait the CPU core has ahead of it */
int  player_is_pal(void);         /* main.c */
void states_stats(char *buf, unsigned size); /* idle time per VI and capture cost since the last call */
/* main.c: GamePad minus saves the graphics tasks that follow to sd:/wiiu/our64/frames
 * (bench format), each before it runs, for replaying a frame that hangs. */
void player_capture_task(uint32_t task);
int  player_capturing(void);       /* a capture is running: decode synchronously */
void player_capture_start(void);  /* GamePad minus, or the first unknown display list command */
double gfx_event_wait_us(void);   /* plugins.c: time the GFX_INT event waited for decoding (total) */
uint32_t gfx_yields(void);        /* plugins.c: yield requests answered while a task was decoding (total) */
uint32_t gfx_tasks_posted(void);  /* plugins.c: graphics tasks sent to the graphics core (total) */
double player_ahead_us(void);     /* main.c: how far emulated time runs ahead of real time */
void timeline_note(char kind, unsigned long a, unsigned long b, unsigned long c, unsigned long d); /* plugins.c */
void timeline_write(void);
extern int g_timeline_on;          /* sd:/wiiu/our64/timeline exists */
void audio_dump_write(void);        /* plugins.c */
extern int g_audio_dump_on;        /* sd:/wiiu/our64/audio-dump exists */
extern int g_ai_stream;            /* plugins.c: AI buffers taken as they play (Stunt Racer) */
void ai_stream_tick(void);         /* plugins.c: every VI */
int player_sync_gfx(void);        /* main.c: sd:/wiiu/our64/sync-gfx exists: wait for each task */
int player_sync_before(void);     /* main.c: sd:/wiiu/our64/sync-before: idle graphics core before each post */
void player_core_state(char *buf, unsigned size); /* N64 PC, interrupts, RSP task counts */
void player_thread_dump(unsigned long threads_at); /* libultra threads, see plugins.c */
#define SCREEN_SECTION 10         /* profiling section: updateScreen (VI); Not64 uses 1-9 */

#ifdef __cplusplus
}
#endif
#endif
