// Our64 player: Our64's own screens (ui.h), drawn on the graphics core with
// libgui's GX2 renderers and the console's system font.
//
// Screens outside play are whole frames on WHBGfx's TV and GamePad buffers
// (libgui's frame cycle). Over a running game only the controller notices
// are drawn: onto a copy of the game's frame, so GLideN64's own buffer stays
// clean for the frozen frame behind a pause. The frozen frame is copied out
// of GLideN64 when the game freezes, so it outlives GLideN64 for the
// closing screen.
#include <cstdio>
#include <cstring>
#include <string>

#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/mutex.h>
#include <gx2/context.h>
#include <gx2/display.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/state.h>
#include <gx2/swap.h>
#include <gx2/utils.h>
#include <proc_ui/procui.h>
#include <sys/stat.h>
#include <png.h>
#include <turbojpeg.h>
#include <vpad/input.h>
#include <whb/gfx.h>

#include "libgui/Gui.h"
#include "drivers/wut/WutThreadDriver.h"
#include "drivers/wut/WutVideoDriver.h"
#include "drivers/wut/shaders/Shader.h" // GX2InitTexture

#include "ui.h"
#include "ui_draw.h"
#include "video_ui.h"
extern "C" {
#include "player.h"
#include "report.h"
}

// cat1.jpg (225x225), the portrait in the headers.
__asm__(".section .rodata\n.balign 64\n.global our64_cat1_jpg\nour64_cat1_jpg:\n.incbin \"" OUR64_DATA
        "/cat1.jpg\"\n.global our64_cat1_jpg_end\nour64_cat1_jpg_end:\n.previous\n");
extern "C" const unsigned char our64_cat1_jpg[], our64_cat1_jpg_end[];
// cat2.jpg (194x259) on the loading screen; the N64 controller (Evan-Amos, public domain) on Edit buttons.
__asm__(".section .rodata\n.balign 64\n.global our64_cat2_jpg\nour64_cat2_jpg:\n.incbin \"" OUR64_DATA
        "/cat2.jpg\"\n.global our64_cat2_jpg_end\nour64_cat2_jpg_end:\n"
        ".balign 64\n.global our64_pad_png\nour64_pad_png:\n.incbin \"" OUR64_DATA
        "/n64-controller.png\"\n.global our64_pad_png_end\nour64_pad_png_end:\n.previous\n");
extern "C" const unsigned char our64_cat2_jpg[], our64_cat2_jpg_end[], our64_pad_png[], our64_pad_png_end[];

Platform * platform;
namespace gx2 { // GLideN64's GX2 backend (gx2_ContextImpl.h; u32 is unsigned int there)
void restoreState();
GX2ColorBuffer * crtColorBuffer(GX2ColorBuffer * _src, unsigned int _width, unsigned int _height);
}

namespace {

// ---- libgui's platform: video and threads; no sound, files or input (ours feed the screens) ----

class OurPlatform : public Platform
{
public:
	void init(const PlatformConfig & c) override
	{
		config = c;
		thread.init();
		video.init(c.canvasWidth, c.canvasHeight);
	}
	void requestExit() override {}
	AudioDriver * getAudio() override { return nullptr; }
	VideoDriver * getVideo() override { return &video; }
	InputDriver * getInput() override { return nullptr; }
	FileSystemDriver * getFileSystem() override { return nullptr; }
	ThreadDriver * getThread() override { return &thread; }
	Logger * getLogger() override { return nullptr; }
	SystemEvent getSystemEvent() override { return SystemEvent::None; }
	Status getStatus() const override { return ProcUIInForeground() ? Status::Running : Status::Paused; }
	void triggerExit() override {}

	WutVideoDriver video;
	WutThreadDriver thread;

protected:
	void shutdown() override {}
};

OurPlatform * g_platform;
WutVideoDriver * g_video;
GX2Texture g_frozen;         // the game's last frame, behind pauses and the closing screen
bool g_frozen_ok;            // g_frozen holds a frame
bool g_frozen_now;           // ... of the current freeze (cleared when the game runs again)
GX2ColorBuffer g_overlay;    // a game frame with the notices on
GX2ColorBuffer g_tv_card;    // GamePad only: what the TV shows (black, small: the copy scales it)
bool g_tv_card_ok;
// GamePad only: the TV's black is copied to the TV's scan buffers until each
// holds it, then no more. A full-size copy every VI (8 MB at 1080p) was GPU
// time the game's frames waited behind (frame test, 2026-11-06). Any other
// swap in between (a menu, Our ROMs) or a new card or new scan buffers starts
// the count over. More copies than buffers (two or three), so each has one.
constexpr unsigned kCardCopies = 4;
unsigned g_card_copies;
uint32_t g_card_swap = ~0u; // the swap count after the last scan with the card
GX2ContextState * g_copy_ctx;

volatile int g_mode = MODE_BOTH; // before the prompt's choice both screens are on
ui_view g_last;
bool g_have_last;

OSMutex g_notice_lock;
bool g_notice_lock_ok;

} // namespace

// ---- ui_draw.h's platform side ----

namespace our64ui {

notices g_notices;

double now_us() { return double(OSTicksToMicroseconds(OSGetSystemTime())); }

void lock_notices()
{
	if (!g_notice_lock_ok) {
		OSInitMutex(&g_notice_lock);
		notices_init(&g_notices);
		g_notice_lock_ok = true;
	}
	OSLockMutex(&g_notice_lock);
}

void unlock_notices() { OSUnlockMutex(&g_notice_lock); }

void * frozen_texture() { return g_frozen_ok ? &g_frozen : nullptr; }

} // namespace our64ui

using namespace our64ui;

namespace {

bool alloc_texture(GX2Texture & t, uint32_t w, uint32_t h, GX2TileMode tile)
{
	memset(&t, 0, sizeof(t));
	GX2InitTexture(&t, w, h, 1, 0, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_DIM_TEXTURE_2D, tile);
	GX2CalcSurfaceSizeAndAlignment(&t.surface);
	GX2InitTextureRegs(&t);
	t.surface.image = MEMAllocFromDefaultHeapEx(t.surface.imageSize, t.surface.alignment);
	return t.surface.image != nullptr;
}

bool alloc_color_buffer(GX2ColorBuffer & cb, uint32_t w, uint32_t h)
{
	memset(&cb, 0, sizeof(cb));
	cb.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	cb.surface.width = w;
	cb.surface.height = h;
	cb.surface.depth = 1;
	cb.surface.mipLevels = 1;
	cb.surface.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	cb.surface.aa = GX2_AA_MODE1X;
	cb.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
	cb.surface.tileMode = GX2_TILE_MODE_DEFAULT;
	cb.viewNumSlices = 1;
	GX2CalcSurfaceSizeAndAlignment(&cb.surface);
	GX2InitColorBufferRegs(&cb);
	cb.surface.image = MEMAllocFromDefaultHeapEx(cb.surface.imageSize, cb.surface.alignment);
	return cb.surface.image != nullptr;
}

// A surface copy on the GPU, in a state of our own.
void copy_surface(GX2Surface * from, GX2Surface * to)
{
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx == nullptr)
			return;
		GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	GX2SetContextState(g_copy_ctx);
	GX2CopySurface(from, 0, 0, to, 0, 0);
	GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, to->image, to->imageSize);
	GX2Flush();
}


GX2Texture * load_jpeg(const unsigned char * data, const unsigned char * end, int * pw, int * ph)
{
	GX2Texture * t = nullptr;
	tjhandle j = tjInitDecompress();
	if (j == nullptr)
		return nullptr;
	int w = 0, h = 0, sub = 0, cs = 0;
	unsigned long size = (unsigned long)(end - data);
	if (tjDecompressHeader3(j, data, size, &w, &h, &sub, &cs) == 0 && w > 0 && h > 0) {
		std::string rgba(size_t(w) * h * 4, '\0');
		if (tjDecompress2(j, data, size, (unsigned char *)&rgba[0], w, 0, h, TJPF_RGBA, 0) == 0) {
			t = static_cast<GX2Texture *>(g_images->createTexture(w, h));
			if (t != nullptr)
				g_images->loadTextureData(t, (const uint8_t *)rgba.data(), w, h);
		}
	}
	tjDestroy(j);
	if (pw) *pw = w;
	if (ph) *ph = h;
	return t;
}

GX2Texture * load_png(const unsigned char * data, const unsigned char * end)
{
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&im, data, size_t(end - data)))
		return nullptr;
	im.format = PNG_FORMAT_RGBA;
	std::string rgba(PNG_IMAGE_SIZE(im), '\0');
	GX2Texture * t = nullptr;
	if (png_image_finish_read(&im, nullptr, &rgba[0], 0, nullptr)) {
		t = static_cast<GX2Texture *>(g_images->createTexture(int(im.width), int(im.height)));
		if (t != nullptr)
			g_images->loadTextureData(t, (const uint8_t *)rgba.data(), int(im.width), int(im.height));
	}
	png_image_free(&im);
	return t;
}

void load_portrait()
{
	g_cat = load_jpeg(our64_cat1_jpg, our64_cat1_jpg_end, nullptr, nullptr);
	g_cat2 = load_jpeg(our64_cat2_jpg, our64_cat2_jpg_end, &g_cat2_w, &g_cat2_h);
	g_pad = load_png(our64_pad_png, our64_pad_png_end);
}

// The system font. A copy goes to sd:/wiiu/our64/debug once, so the
// button glyphs can be looked at on a computer (it never leaves the SD).
void load_font()
{
	void * font = nullptr;
	uint32_t size = 0;
	if (!OSGetSharedData(OS_SHAREDDATATYPE_FONT_STANDARD, 0, &font, &size) || font == nullptr) {
		report_line("UI: no system font");
		return;
	}
	report_line("UI: system font, %u bytes", size);
	report_save();
	fontSystem = new GuiTextRenderer(static_cast<const uint8_t *>(font), size, g_video->getGlyphRenderer(),
	                                 g_video->getUIScale());
	if (const char * root = report_sd_root()) {
		char path[256];
		// A mkdir on the SD took 15 s once (console, 2026-10-17): only when the copy is missing.
		snprintf(path, sizeof(path), "%s/debug/system-font.ttf", root);
		struct stat st;
		if (stat(path, &st) != 0) {
			char dir[256];
			snprintf(dir, sizeof(dir), "%s/debug", root);
			mkdir(dir, 0777);
		}
		// Through a buffer of our own: an SD write straight from the shared
		// font's memory never finished (console, 2026-10-16).
		static uint8_t chunk[64 * 1024] __attribute__((aligned(64)));
		if (stat(path, &st) != 0 || st.st_size != (off_t)size)
			if (FILE * f = fopen(path, "wb")) {
				for (uint32_t at = 0; at < size; at += sizeof(chunk)) {
					uint32_t n = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
					memcpy(chunk, static_cast<const uint8_t *>(font) + at, n);
					if (fwrite(chunk, 1, n, f) != n)
						break;
				}
				fclose(f);
			}
	}
}

bool ready()
{
	if (g_platform != nullptr)
		return true;
	if (!ProcUIInForeground())
		return false;
	g_platform = new OurPlatform();
	platform = g_platform;
	PlatformConfig c = {};
	c.canvasWidth = UI_W;
	c.canvasHeight = UI_H;
	g_platform->init(c);
	g_video = &g_platform->video;
	g_images = g_video->getImageRenderer();
	load_font();
	load_portrait();
	report_line("UI: ready (portrait %s)", g_cat ? "loaded" : "missing");
	return true;
}

// The frozen frame's 4:3 middle, drawn small and read back for a save state's card.
void thumb_on_graphics_core(void * arg)
{
	uint16_t * out = static_cast<uint16_t *>(arg);
	memset(out, 0, THUMB_BYTES);
	if (!ready() || !g_frozen_ok)
		return;
	static GX2ColorBuffer cb;
	static bool cb_ok;
	if (!cb_ok)
		cb_ok = alloc_color_buffer(cb, THUMB_W, THUMB_H);
	GX2Surface lin;
	memset(&lin, 0, sizeof(lin));
	lin.dim = GX2_SURFACE_DIM_TEXTURE_2D;
	lin.width = THUMB_W;
	lin.height = THUMB_H;
	lin.depth = 1;
	lin.mipLevels = 1;
	lin.format = GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
	lin.aa = GX2_AA_MODE1X;
	lin.use = GX2_SURFACE_USE_TEXTURE;
	lin.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
	GX2CalcSurfaceSizeAndAlignment(&lin);
	lin.image = MEMAllocFromDefaultHeapEx(lin.imageSize, lin.alignment);
	if (!cb_ok || lin.image == nullptr) {
		if (lin.image != nullptr)
			MEMFreeToDefaultHeap(lin.image);
		report_line("UI: no memory for a thumbnail");
		return;
	}
	const float fw = g_frozen.surface.width, fh = g_frozen.surface.height;
	const float cw = fh * 4 / 3 < fw ? fh * 4 / 3 : fw, ch = fw * 3 / 4 < fh ? fw * 3 / 4 : fh;
	const float dw = UI_W * fw / cw, dh = UI_H * fh / ch;
	g_images->drawTexture(&g_frozen, (UI_W - dw) / 2, (UI_H - dh) / 2, uint16_t(dw), uint16_t(dh), 0, 1, 1, 255);
	g_video->drawOverlay(&cb);
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx != nullptr)
			GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	if (g_copy_ctx != nullptr) {
		GX2SetContextState(g_copy_ctx);
		GX2CopySurface(&cb.surface, 0, 0, &lin, 0, 0);
		GX2DrawDone();
		DCInvalidateRange(lin.image, lin.imageSize);
		const uint8_t * px = static_cast<const uint8_t *>(lin.image);
		for (int y = 0; y < THUMB_H; y++)
			for (int x = 0; x < THUMB_W; x++) {
				const uint8_t * q = px + (size_t(y) * lin.pitch + x) * 4;
				out[y * THUMB_W + x] = uint16_t((q[0] >> 3) << 11 | (q[1] >> 2) << 5 | (q[2] >> 3));
			}
	}
	MEMFreeToDefaultHeap(lin.image);
	if (video_game_started())
		gx2::restoreState();
}

void apply_screens()
{
	GX2SetTVEnable(TRUE);
	GX2SetDRCEnable(g_mode == MODE_TV ? FALSE : TRUE);
}

// One frame of a screen on WHBGfx's buffers, presented.
void frame(const ui_view & v)
{
	g_video->prepareFrame();
	const bool chosen = v.screen != UI_PROMPT;
	if (chosen && g_mode == MODE_GAMEPAD) {
		g_video->setUiTargets(1u << unsigned(OutputTarget::TV));
		draw_tv_black();
		g_video->flushDrawQueue();
		g_video->setUiTargets(1u << unsigned(OutputTarget::DRC));
	} else if (chosen && g_mode == MODE_TV) {
		g_video->setUiTargets(1u << unsigned(OutputTarget::TV));
	}
	draw_screen(v);
	g_video->presentBuffer();
	g_video->setUiTargets(WutVideoDriver::kAllTargets);
	apply_screens();
}

void freeze()
{
	GX2ColorBuffer * last = video_game_started() ? video_last_frame() : nullptr;
	if (last == nullptr)
		return;
	if (g_frozen_ok && (g_frozen.surface.width != last->surface.width || g_frozen.surface.height != last->surface.height)) {
		MEMFreeToDefaultHeap(g_frozen.surface.image);
		g_frozen_ok = false;
	}
	if (!g_frozen_ok)
		g_frozen_ok = alloc_texture(g_frozen, last->surface.width, last->surface.height, GX2_TILE_MODE_DEFAULT);
	if (g_frozen_ok)
		copy_surface(&last->surface, &g_frozen.surface);
}

void resume_on_graphics_core(void *)
{
	g_frozen_now = false;
	video_set_hold(false);
}

void show_on_graphics_core(void * arg)
{
	const ui_view & v = *static_cast<const ui_view *>(arg);
	if (!ready() || !ProcUIInForeground()) // another app (the Wii U Menu) has the screen
		return;
	// The first frozen screen takes the game's last frame; later ones keep it.
	if ((v.screen == UI_PAUSED || v.screen == UI_CLOSING || v.screen == UI_PAUSE) && !g_frozen_now) {
		freeze();
		g_frozen_now = true;
	}
	video_set_hold(true);
	g_last = v;
	g_have_last = true;
	frame(v);
	if (video_game_started())
		gx2::restoreState();
}

void tv_card_on_graphics_core(void *)
{
	if (!ready())
		return;
	g_card_copies = 0; // each scan buffer gets the black again
	if (g_tv_card_ok)
		return;
	g_tv_card_ok = alloc_color_buffer(g_tv_card, 64, 64);
	if (!g_tv_card_ok)
		return;
	memset(g_tv_card.surface.image, 0, g_tv_card.surface.imageSize); // black (the scan copy leaves alpha out)
	DCFlushRange(g_tv_card.surface.image, g_tv_card.surface.imageSize);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU, g_tv_card.surface.image, g_tv_card.surface.imageSize);
}

void restore_on_graphics_core(void *)
{
	g_mode = MODE_BOTH;
	apply_screens();
}

} // namespace

namespace gx2 { void restoreState(); }

// ---- the graphics core's side, for video.cpp ----

// While GLideN64 builds a game's saved shaders (inside RomOpen): the loading
// screen's bar and "Preparing graphics 12/17", at most every 80 ms.
void ui_gfx_loading_progress(unsigned done, unsigned total)
{
	static double last;
	const double now = now_us();
	if (!g_have_last || g_last.screen != UI_LOADING || total == 0 || (now - last < 80000 && done + 1 < total))
		return;
	last = now;
	g_last.loading.progress = 0.7f + 0.3f * float(done + 1) / float(total);
	snprintf(g_last.loading.step, sizeof(g_last.loading.step), "Preparing graphics %u/%u", done + 1, total);
	frame(g_last);
	gx2::restoreState(); // GLideN64 is mid-RomOpen: its GPU state back before it draws on
}

GX2ColorBuffer * ui_gfx_overlay(GX2ColorBuffer * frame)
{
	lock_notices();
	const notice * shown[NOTICES_SHOWN];
	int n = notices_visible(&g_notices, now_us(), shown);
	unlock_notices();
	if (n == 0 || !ready())
		return frame;
	if (g_overlay.surface.image != nullptr &&
	    (g_overlay.surface.width != frame->surface.width || g_overlay.surface.height != frame->surface.height)) {
		MEMFreeToDefaultHeap(g_overlay.surface.image);
		g_overlay.surface.image = nullptr;
	}
	if (g_overlay.surface.image == nullptr && !alloc_color_buffer(g_overlay, frame->surface.width, frame->surface.height))
		return frame;
	copy_surface(&frame->surface, &g_overlay.surface);
	draw_notices(false);
	g_video->drawOverlay(&g_overlay);
	return &g_overlay;
}

bool ui_gfx_copy_frame(GX2ColorBuffer * frame, GX2ColorBuffer * to)
{
	if (to->surface.image != nullptr &&
	    (to->surface.width != frame->surface.width || to->surface.height != frame->surface.height)) {
		MEMFreeToDefaultHeap(to->surface.image);
		to->surface.image = nullptr;
	}
	if (to->surface.image == nullptr && !alloc_color_buffer(*to, frame->surface.width, frame->surface.height))
		return false;
	copy_surface(&frame->surface, &to->surface);
	return true;
}

// Our Settings' Picture. GLideN64 draws into a 640x480 window, already 4:3,
// and the copy to a scan buffer fills the whole 16:9 screen, so both settings
// showed the game stretched (Zach, 2026-10-27). 4:3 now puts the frame in the
// middle of a 16:9 one, black at the sides; stretched sends it as it is.
static bool g_picture_43 = true;
static GX2ColorBuffer g_shaped;

void ui_gfx_set_picture_43(bool on) { g_picture_43 = on; }

static GX2ColorBuffer * shaped(GX2ColorBuffer * frame)
{
	const uint32_t w = frame->surface.width, h = frame->surface.height;
	const uint32_t wide = (h * 16 / 9 + 1) & ~1u; // 854 for 480
	if (!g_picture_43 || wide <= w)
		return frame;
	if (g_shaped.surface.image != nullptr && (g_shaped.surface.width != wide || g_shaped.surface.height != h)) {
		MEMFreeToDefaultHeap(g_shaped.surface.image);
		g_shaped.surface.image = nullptr;
	}
	if (g_shaped.surface.image == nullptr) {
		if (!alloc_color_buffer(g_shaped, wide, h))
			return frame;
		// Black once: only the middle is written after.
		memset(g_shaped.surface.image, 0, g_shaped.surface.imageSize);
		DCFlushRange(g_shaped.surface.image, g_shaped.surface.imageSize);
	}
	GX2Rect from = { 0, 0, int32_t(w), int32_t(h) };
	GX2Point at = { int32_t((wide - w) / 2), 0 };
	GX2SetContextState(g_copy_ctx);
	GX2CopySurfaceEx(&frame->surface, 0, 0, &g_shaped.surface, 0, 0, 1, &from, &at);
	GX2Flush();
	return &g_shaped;
}

void ui_gfx_screens_changed() { g_card_copies = 0; }

// Settings > Picture > TV look (2026-11-12): CRT draws the TV's frame again
// at the TV's size with scanlines (gx2::crtColorBuffer); the GamePad keeps the
// clean frame. Game frames only: the menus are not the game's picture.
static bool g_crt;

void ui_gfx_set_crt(bool on) { g_crt = on; }

static GX2ColorBuffer * tv_frame(GX2ColorBuffer * frame)
{
	if (!g_crt || !video_game_started())
		return frame;
	uint32_t w = 1280, h = 720; // a line of 240 is 3 rows; 1080p TVs get it stretched
	switch (GX2GetSystemTVScanMode()) {
	case GX2_TV_SCAN_MODE_480I:
	case GX2_TV_SCAN_MODE_480P:
		w = 854;
		h = 480;
		break;
	default:
		break;
	}
	GX2ColorBuffer * crt = gx2::crtColorBuffer(frame, w, h);
	return crt != nullptr ? crt : frame;
}

// GPU profile marks (the frame test's, gx2_ContextImpl.cpp): each names the GPU
// work since the one before.
extern "C" void our64_gx2_prof_mark(const char * what);

void ui_gfx_scan(GX2ColorBuffer * frame)
{
	our64_gx2_prof_mark("player: notices, kept copies");
	if (g_copy_ctx == nullptr) {
		g_copy_ctx = static_cast<GX2ContextState *>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
		if (g_copy_ctx != nullptr)
			GX2SetupContextStateEx(g_copy_ctx, TRUE);
	}
	if (g_copy_ctx != nullptr) {
		frame = shaped(frame);
		our64_gx2_prof_mark("4:3 copy");
	}
	uint32_t swaps = 0, flips = 0;
	OSTime last_flip, last_vsync;
	GX2GetSwapStatus(&swaps, &flips, &last_flip, &last_vsync);
	if (g_mode == MODE_GAMEPAD && g_tv_card_ok) {
		if (swaps != g_card_swap)
			g_card_copies = 0;
		if (g_card_copies < kCardCopies) {
			GX2CopyColorBufferToScanBuffer(&g_tv_card, GX2_SCAN_TARGET_TV);
			g_card_copies++;
			our64_gx2_prof_mark("TV copy (GamePad only black)");
		}
		g_card_swap = swaps + 1;
	} else {
		GX2CopyColorBufferToScanBuffer(tv_frame(frame), GX2_SCAN_TARGET_TV);
		our64_gx2_prof_mark("TV copy");
	}
	if (g_mode != MODE_TV) {
		GX2CopyColorBufferToScanBuffer(frame, GX2_SCAN_TARGET_DRC);
		our64_gx2_prof_mark("GamePad copy");
	}
	GX2SwapScanBuffers();
	our64_gx2_prof_mark("swap");
	GX2Flush();
	apply_screens();
}

// ---- ui.h ----

extern "C" void ui_show(const ui_view * v) { video_call(show_on_graphics_core, const_cast<ui_view *>(v)); }

namespace {
struct exit_ask_args { int focus, glyphs; const char * title, * body, * action; };
void exit_ask_on_graphics_core(void * arg)
{
	const exit_ask_args & a = *static_cast<const exit_ask_args *>(arg);
	if (!g_have_last || !ready() || !ProcUIInForeground())
		return;
	ui_view v = g_last; // the screen under the question is kept as it was
	v.exit_ask = 1;
	v.exit_focus = a.focus;
	v.exit_glyphs = a.glyphs;
	snprintf(v.ask_title, sizeof(v.ask_title), "%s", a.title ? a.title : "");
	snprintf(v.ask_body, sizeof(v.ask_body), "%s", a.body ? a.body : "");
	snprintf(v.ask_action, sizeof(v.ask_action), "%s", a.action ? a.action : "");
	frame(v);
	if (video_game_started())
		gx2::restoreState();
}
} // namespace

extern "C" void ui_show_exit_ask(int focus, int glyphs) { ui_show_ask(nullptr, nullptr, nullptr, focus, glyphs); }

extern "C" void ui_show_ask(const char * title, const char * body, const char * action, int focus, int glyphs)
{
	exit_ask_args a = { focus, glyphs, title, body, action };
	video_call(exit_ask_on_graphics_core, &a);
}

extern "C" void ui_redraw(void)
{
	if (g_have_last) {
		ui_view v = g_last;
		video_call(show_on_graphics_core, &v);
	}
}

extern "C" void ui_thumbnail(uint16_t * out) { video_call(thumb_on_graphics_core, out); }

extern "C" void ui_set_mode(int mode)
{
	g_mode = mode;
	audio_route(mode != MODE_GAMEPAD, mode != MODE_TV);
	VPADSetLcdMode(VPAD_CHAN_0, mode == MODE_TV ? VPAD_LCD_OFF : VPAD_LCD_ON);
	if (mode == MODE_GAMEPAD)
		video_call(tv_card_on_graphics_core, nullptr);
	report_line("UI: screen mode %s", mode_name(mode));
}

extern "C" int ui_mode(void) { return g_mode; }

extern "C" void ui_resume(void) { video_call(resume_on_graphics_core, nullptr); }

extern "C" void ui_restore_screens(void)
{
	if (g_mode == MODE_TV)
		VPADSetLcdMode(VPAD_CHAN_0, VPAD_LCD_ON);
	video_call(restore_on_graphics_core, nullptr);
}

extern "C" void ui_notice(int key, int kind, double secs, const char * line1, const char * line2)
{
	lock_notices();
	notices_add(&g_notices, key, kind, line1, line2, secs);
	unlock_notices();
}

extern "C" void ui_notice_clear(int key)
{
	lock_notices();
	notices_remove(&g_notices, key);
	unlock_notices();
}
