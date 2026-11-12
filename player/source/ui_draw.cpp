// Our64 player: Our64's screens as drawing, apart from the GX2 that shows
// them (ui_wiiu.cpp) so tests/ui_shots.cpp draws the same screens on a PC.
// Everything is in the 854x480 canvas, top left at 0,0.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "ui_draw.h"
#include "theme.h"
#include "version.h"

namespace our64ui {

ImageRenderer * g_images;
void * g_cat, * g_cat2, * g_pad;
int g_cat2_w, g_cat2_h, g_pad_w, g_pad_h;
draw_checks * g_checks;

// ---- drawing ----

// The theme in use (theme.c, Settings > Theme), and a theme color as libgui's.
const our_theme & T = g_theme;
PixelColor rgb(our_rgb c, uint8_t alpha = 255) { return PixelColor{c.r, c.g, c.b, alpha}; }
// Overlays and edges, not surfaces: the same for every app.
constexpr PixelColor DIM = {0, 0, 0, 150};
constexpr PixelColor DIM_PAUSE = {0, 0, 0, 190}; // text over a white frozen frame keeps 4.5:1
constexpr PixelColor NEAR_BLACK = {24, 24, 32, 255}; // dark fills: screens that are off, warnings, picture edges

// The theme's gradients on its own fills (the console's sheen).
bool same(PixelColor a, our_rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == 255; }

void box(float x, float y, float w, float h, PixelColor c, float radius = 0, float stroke = 0,
         PixelColor sc = rgb(T.panel.fill), float shadow = 0)
{
	GuiMesh m;
	GuiPaint p;
	if (T.style.grad && same(c, T.screen.fill))
		p.gradient(c, rgb(T.style.screen_to));
	else if (T.style.grad && same(c, T.bar.fill))
		p.gradient(c, rgb(T.style.bar_to));
	else if (T.style.grad && same(c, T.select.fill))
		p.gradient(c, rgb(T.style.select_to));
	else
		p.color(c);
	if (stroke > 0)
		p.stroke(stroke, sc);
	if (shadow > 0 && T.style.glow) // a glow in the theme's color around raised things (OurMAME's neon)
		p.glow(shadow * 1.3f, rgb(T.style.glow_color, 210));
	else if (shadow > 0)
		p.shadow(shadow, PixelColor{0, 0, 0, 150}, 0, shadow * 0.4f);
	GuiAddPaintedShape(m, SHAPE::RECTANGLE, GuiRect{x, y, w, h}, GuiCorners(radius), p);
	if (g_checks != nullptr)
		g_checks->box(x, y, w, h, c, stroke);
	if (!m.empty())
		g_images->drawMesh(m.vertices().data(), m.size());
}

// The surface a button or card is drawn on (selectable): the bar's grey, the
// selection, or sunk when it does nothing now. Its words take their colors
// from it.
const our_surface & surface_of(bool sel, bool off)
{
	return off ? T.bar : sel ? T.select : T.bar;
}

// Dimmed words on the grey (the legend's dead keys, empty letters), and a
// button that does nothing now: faded into the screen, its words faded with
// it, both worked out from the theme (Zach, 2026-10-23; theme.h).
PixelColor off_ink() { return rgb(theme_off(&T.bar)); }
const our_surface & faded()
{
	static our_surface f;
	f = theme_faded(&T); // worked out each time: the theme can change
	return f;
}

PixelColor card_ink(bool sel, bool off) { return off ? rgb(faded().ink) : rgb(surface_of(sel, off).ink); }
PixelColor card_muted(bool sel) { return rgb(surface_of(sel, false).muted); }
PixelColor card_accent(bool sel) { return rgb(surface_of(sel, false).accent); }

void arcs(float x, float y, PixelColor c)
{
	GuiMesh m;
	m.strokeArc(GuiPoint{x, y}, 7, -45, 90, 3, c);
	m.strokeArc(GuiPoint{x, y}, 14, -45, 90, 3, c);
	if (!m.empty())
		g_images->drawMesh(m.vertices().data(), m.size());
}

int text_width(const char * s, int px)
{
	if (fontSystem == nullptr)
		return 0;
	fontSystem->setPixelSize(px);
	return fontSystem->getWidth(s);
}

void text(float x, float y, const char * s, int px, PixelColor c, uint32_t justify = GUI_TEXT_JUSTIFY_LEFT)
{
	if (fontSystem == nullptr || s == nullptr || !*s)
		return;
	fontSystem->setPixelSize(px);
	fontSystem->drawText(int16_t(x), int16_t(y), s, c, justify | GUI_TEXT_ALIGN_TOP);
	if (g_checks != nullptr)
		g_checks->text(x, y, s, px, fontSystem->getWidth(s), justify, c);
}

// How much of a word too wide for one line goes on it: up to its last "/"
// that fits (a folder path); 0 when no "/" fits (the word stays whole).
static size_t word_part(const std::string & word, int px, int max_w)
{
	size_t slash = 0;
	for (size_t i = 1; i < word.size(); i++)
		if (word[i - 1] == '/') {
			if (text_width(word.substr(0, i).c_str(), px) > max_w)
				break;
			slash = i;
		}
	return slash;
}

// Splits text into lines no wider than max_w, at spaces; a word wider than a
// line (a long folder path) is split after a "/".
// Returns the count.
int wrap(const char * s, int px, int max_w, std::string * lines, int max_lines)
{
	int n = 0;
	std::string line;
	while (*s && n < max_lines) {
		const char * sp = strchr(s, ' ');
		std::string word = sp ? std::string(s, sp - s) : std::string(s);
		std::string tried = line.empty() ? word : line + " " + word;
		if (!line.empty() && text_width(tried.c_str(), px) > max_w) {
			lines[n++] = line;
			line = word;
		} else {
			line = tried;
		}
		// the line is one word that is too wide: its start fills the line
		while (n < max_lines && line == word && text_width(line.c_str(), px) > max_w) {
			const size_t k = word_part(word, px, max_w);
			if (k == 0)
				break;
			lines[n++] = word.substr(0, k);
			line = word = word.substr(k);
		}
		s = sp ? sp + 1 : s + strlen(s);
	}
	if (!line.empty() && n < max_lines)
		lines[n++] = line;
	return n;
}

// s at px, its middle cut ("SD card: /Games/N…/USA") until it fits max_w:
// for one line of a folder path.
static std::string cut_to_fit(const char * s, int px, int max_w)
{
	if (text_width(s, px) <= max_w)
		return s;
	char out[512];
	int chars = 0;
	for (const char * c = s; *c; c++)
		chars += (*c & 0xC0) != 0x80;
	for (int keep = chars - 1; keep >= 3; keep--) {
		library_cut_middle(s, keep, out, sizeof(out));
		if (text_width(out, px) <= max_w)
			return out;
	}
	return out;
}

// The largest size from px down to min_px at which s fits max_w (min_px if none).
int fit(const char * s, int px, int min_px, int max_w)
{
	while (px > min_px && text_width(s, px) > max_w)
		px--;
	if (g_checks != nullptr && text_width(s, px) > max_w)
		g_checks->too_long(s, px, text_width(s, px), max_w);
	return px;
}

// Wrapped lines at the first size from px down that fits max_lines; returns the count.
int wrap_fit(const char * s, int & px, int min_px, int max_w, std::string * lines, int max_lines)
{
	for (;; px--) {
		// one line more than the caller's array holds, to see whether it all fits
		std::string tried[9];
		const int room = max_lines < 8 ? max_lines : 8;
		int n = wrap(s, px, max_w, tried, room + 1);
		if (n <= room || px <= min_px) {
			if (g_checks != nullptr && n > room)
				g_checks->too_long(s, px, n, room);
			n = n < room ? n : room;
			for (int i = 0; i < n; i++)
				lines[i] = tried[i];
			return n;
		}
	}
}

void portrait(float x, float y, float size)
{
	box(x - 5, y - 5, size + 10, size + 10, rgb(T.panel.fill), 6, 0, rgb(T.panel.fill), 6);
	if (g_cat != nullptr)
		g_images->drawTexture(g_cat, x, y, uint16_t(size), uint16_t(size), 0, 1, 1, 255);
}

void bottom_bar() { box(0, 412, UI_W, UI_H - 412, rgb(T.bar.fill)); }

void legend(const char * s)
{
	bottom_bar();
	text(UI_W / 2, 426, s, 26, rgb(T.bar.ink), GUI_TEXT_JUSTIFY_CENTER);
}

// A TV and a GamePad, lit or dark, with sound waves beside the lit ones. A lit
// screen is purple, a dark one is off: nearly black.
void tv(float cx, float cy, bool lit, PixelColor body)
{
	box(cx - 52, cy - 34, 104, 64, body, 6);
	box(cx - 46, cy - 28, 92, 52, lit ? rgb(T.lit) : NEAR_BLACK, 3);
	box(cx - 16, cy + 30, 32, 6, body, 2);
	if (lit)
		arcs(cx + 60, cy - 4, body);
}

void gamepad(float cx, float cy, bool lit, PixelColor body)
{
	box(cx - 38, cy - 22, 76, 44, body, 14);
	box(cx - 20, cy - 15, 40, 30, lit ? rgb(T.lit) : NEAR_BLACK, 2);
	box(cx - 32, cy - 12, 8, 8, lit ? rgb(T.panel.fill) : off_ink(), 4);
	box(cx + 24, cy - 12, 8, 8, lit ? rgb(T.panel.fill) : off_ink(), 4);
	if (lit)
		arcs(cx + 46, cy, body);
}

void card(const ui_view & v, int mode)
{
	int r[4];
	prompt_card_rect(mode, r);
	const bool sel = v.prompt.selected == mode;
	const bool on = prompt_enabled(&v.prompt, mode);
	float x = r[0], y = r[1], w = r[2], h = r[3];
	if (sel) { // bigger, with a thick light border and a shadow
		x -= 8; y -= 8; w += 16; h += 16;
		const float frame = on ? 6 : 2; // thin for a mode that needs the missing GamePad
		box(x, y, w, h, rgb(T.panel.fill), 14, 0, rgb(T.panel.fill), on ? 14 : 0);
		box(x + frame, y + frame, w - 2 * frame, h - 2 * frame, on ? rgb(T.select.fill) : rgb(faded().fill), 10);
	} else { // grey; a mode that needs the missing GamePad is faded
		box(x, y, w, h, on ? rgb(T.bar.fill) : rgb(faded().fill), 12);
	}
	float cx = x + w / 2, py = y + h * 0.36f;
	tv(cx - 30, py, on && mode != MODE_GAMEPAD, on ? rgb(T.screen.ink) : rgb(faded().ink));
	gamepad(cx + 58, py + 26, on && mode != MODE_TV, on ? rgb(T.screen.ink) : rgb(faded().ink));
	const char * name = mode_name(mode);
	text(cx, y + h - 70, name, fit(name, 30, 20, int(w) - 32), card_ink(sel, !on), GUI_TEXT_JUSTIFY_CENTER);
	if (!on)
		text(cx, y + h - 34, "GamePad not connected", fit("GamePad not connected", 22, 14, int(w) - 32), rgb(faded().ink),
		     GUI_TEXT_JUSTIFY_CENTER);
}

// The portrait and the headline, light on the blue, on red and over the
// dimmed game: the ink of the surface it sits on.
// Standard has no portrait: the headline starts at the margin the rows below use.
float header_x() { return theme_comrade() ? 160 : 40; }
void draw_header(const char * headline, const our_surface & on = T.screen)
{
	if (theme_comrade())
		portrait(52, 24, 84);
	text(header_x(), 40, headline, 44, rgb(on.ink));
}

void draw_prompt(const ui_view & v)
{
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	draw_header("HOW ARE YOU PLAYING?");
	if (v.warning[0])
		text(160, 88, v.warning, 24, rgb(T.screen.accent));
	for (int m = 0; m < MODES; m++)
		if (v.prompt.selected != m)
			card(v, m);
	card(v, v.prompt.selected); // on top, since it is bigger
	std::string lines[2];
	int n = wrap(v.line, 24, UI_W - 2 * 52, lines, 2);
	for (int i = 0; i < n; i++)
		text(UI_W / 2, 344 + i * 30, lines[i].c_str(), 24, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
	legend(FONT_DPAD_LR " Choose      " FONT_A " Play      " FONT_HOME " Wii U Menu");
}

void draw_loading(const ui_view & v);

void draw_message(const ui_view & v)
{
	const our_surface & on = v.failure ? T.fail : T.screen;
	box(0, 0, UI_W, UI_H, rgb(on.fill));
	draw_header(v.headline, on);
	std::string lines[6];
	int n = wrap(v.body, 26, UI_W - 2 * 60, lines, 6);
	for (int i = 0; i < n; i++)
		text(60, 150 + i * 36, lines[i].c_str(), 26, rgb(on.ink));
	if (v.code[0])
		text(60, 150 + n * 36 + 14, v.code, 24, rgb(on.muted));
	if (v.legend[0])
		legend(v.legend);
}

// Words straight on the dimmed game: the screen's when they are light, else
// light ones of their own (OurNES's, OurSNES's and OurPS1's grey screens take
// dark words, 2026-10-25).
const our_surface & over()
{
	static our_surface light = { {24, 24, 32}, {246, 246, 250}, {208, 208, 218}, {255, 255, 255} };
	const our_rgb i = T.screen.ink;
	if (i.r + i.g + i.b > 3 * 128)
		return T.screen;
	light.accent = T.select.accent;
	return light;
}

void draw_frozen(PixelColor dim = DIM)
{
	if (void * frozen = frozen_texture())
		g_images->drawTexture(frozen, 0, 0, UI_W, UI_H, 0, 1, 1, 255);
	else
		box(0, 0, UI_W, UI_H, NEAR_BLACK);
	box(0, 0, UI_W, UI_H, dim);
}

void draw_notices(bool paused);

// Saving before Our ROMs or the Wii U Menu: one word (Zach, 2026-10-17).
// CLOSING... when the pause menu already wrote it all (Zach, 2026-10-20: it
// read as a second autosave).
void draw_closing(const ui_view & v)
{
	draw_frozen();
	const float w = 340, h = 110;
	// The dialogs' blue with a light edge (Zach, 2026-10-20: a dialog's colors, not white)
	box((UI_W - w) / 2, (UI_H - h) / 2, w, h, rgb(T.screen.fill), 14, 3, rgb(T.panel.fill), 12);
	text(UI_W / 2, UI_H / 2 - 27, v.nothing_to_save ? "CLOSING..." : "SAVING...", 44, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
}

void draw_paused(const ui_view & v)
{
	(void)v;
	draw_frozen();
	draw_notices(true);
	legend(FONT_HOME " Pause menu");
}

// The TV in GamePad only mode: black. A card that named the game and told
// how to switch screens was dropped (Zach, 2026-11-06): it cost a full-size
// copy each VI and said little.
void draw_tv_black() { box(0, 0, UI_W, UI_H, {0, 0, 0, 255}); }

// Notices at the top, centred, oldest first: grey, a warning dark with a
// lavender edge, so both stand out on the blue screens and over the game.
void draw_notices(bool paused)
{
	const notice * shown[NOTICES_SHOWN];
	lock_notices();
	int n = notices_visible(&g_notices, now_us(), shown);
	float y = 24;
	for (int i = 0; i < n; i++) {
		const notice & q = *shown[i];
		int w = text_width(q.line1, 28);
		if (q.line2[0]) {
			int w2 = text_width(q.line2, 24);
			w = w2 > w ? w2 : w;
		}
		w += 56;
		if (w < 360) w = 360;
		if (w > UI_W - 2 * 44) w = UI_W - 2 * 44;
		float h = q.line2[0] ? 84 : 52;
		float x = (UI_W - w) / 2.0f;
		const bool warn = q.kind == NOTICE_WARN;
		box(x, y, w, h, warn ? NEAR_BLACK : rgb(T.bar.fill), 12, warn ? 4 : 0, rgb(over().accent), paused ? 0 : 10);
		text(x + 28, y + 10, q.line1, 28, warn ? rgb(over().ink) : rgb(T.bar.ink));
		if (q.line2[0])
			text(x + 28, y + 48, q.line2, 24, warn ? rgb(over().accent) : rgb(T.bar.ink));
		y += h + 10;
	}
	unlock_notices();
}

// ---- the pause screens (pause.h) ----

// The pictures on the cards and in the dialogs, as textures; remade when one changes.
void * g_thumb_tex[CARDS + 1];
const uint16_t * g_thumb_src[CARDS + 1];
unsigned g_thumb_gen[CARDS + 1];

void * thumb_texture(int i, const uint16_t * src, unsigned gen)
{
	if (src == nullptr)
		return nullptr;
	if (g_thumb_tex[i] != nullptr && g_thumb_src[i] == src && g_thumb_gen[i] == gen)
		return g_thumb_tex[i];
	if (g_thumb_tex[i] == nullptr)
		g_thumb_tex[i] = (g_images->createTexture(THUMB_W, THUMB_H));
	if (g_thumb_tex[i] == nullptr)
		return nullptr;
	static uint8_t rgba[THUMB_W * THUMB_H * 4];
	for (int k = 0; k < THUMB_W * THUMB_H; k++) {
		const uint16_t c = src[k];
		uint8_t * o = rgba + k * 4;
		o[0] = uint8_t((c >> 11) << 3 | (c >> 13));
		o[1] = uint8_t(((c >> 5) & 63) << 2 | ((c >> 9) & 3));
		o[2] = uint8_t((c & 31) << 3 | ((c >> 2) & 7));
		o[3] = 255;
	}
	g_images->loadTextureData(g_thumb_tex[i], rgba, THUMB_W, THUMB_H);
	g_thumb_src[i] = src;
	g_thumb_gen[i] = gen;
	return g_thumb_tex[i];
}

// A selected thing grows, turns dark grey and gets a thick light border, never
// color alone; the others are grey. A thing that does nothing now is the
// normal grey with its words dimmed (off_ink); selected, it grows inside a
// thin light frame, staying grey, and the screen says why (Zach,
// 2026-10-23). A failure (a file that can't be read, a save state that can't
// be loaded) stays red, selected or not, so it never looks like a normal
// item; its words are light. card_ink() picks the words' color for the rest.
// A disabled button's reason in place of its name: where the name goes, at
// its size (px, from 18 when it must shrink to fit), else wrapped onto up to
// three smaller lines, centred.
void button_words(const int r[4], const char * s, int px, PixelColor c)
{
	if (text_width(s, 18) <= r[2] - 20) {
		text(r[0] + r[2] / 2, r[1] + 18, s, fit(s, px, 18, r[2] - 20), c, GUI_TEXT_JUSTIFY_CENTER);
		return;
	}
	std::string lines[3];
	int size = 18;
	const int n = wrap_fit(s, size, 13, r[2] - 20, lines, 3);
	const float top = r[1] + (r[3] - n * (size + 2)) / 2.0f;
	for (int i = 0; i < n; i++)
		text(r[0] + r[2] / 2, top + i * (size + 2), lines[i].c_str(), size, c, GUI_TEXT_JUSTIFY_CENTER);
}

// A on a button that does nothing now: its edge flashes in the accent for
// half a second, so the press is never a dead end (Zach, 2026-10-23, after
// Nielsen). One per screen kind: a count that changed since the last frame
// starts the flash.
enum { FLASH_PAUSE, FLASH_CONTROLLERS, FLASH_MAPPING, FLASHES };
bool refused_flash(int which, int refused)
{
	static int seen[FLASHES];
	static double until[FLASHES];
	const double now = now_us() / 1e6;
	if (refused != seen[which]) {
		seen[which] = refused;
		until[which] = now + 0.5;
	}
	return now < until[which];
}

// The flash: a thick accent edge round a selected disabled button (r: its rect, grown by grow).
void flash_edge(float x, float y, float w, float h, float grow, float radius)
{
	box(x - grow - 2, y - grow - 2, w + 2 * grow + 4, h + 2 * grow + 4, PixelColor{0, 0, 0, 0}, radius + 4, 5,
	    rgb(T.screen.accent));
}

void selectable(float x, float y, float w, float h, bool sel, bool dim, float grow, float radius,
                bool failed = false)
{
	const bool off = dim && !failed;
	const PixelColor fill = failed ? rgb(T.fail.fill) : off ? rgb(faded().fill) : sel ? rgb(T.select.fill) : rgb(T.bar.fill);
	if (sel) {
		const float frame = off ? 2 : 5; // thin for a thing that does nothing now
		box(x - grow, y - grow, w + 2 * grow, h + 2 * grow, rgb(T.panel.fill), radius + 2, 0, rgb(T.panel.fill), off ? 0 : 12);
		box(x - grow + frame, y - grow + frame, w + 2 * grow - 2 * frame, h + 2 * grow - 2 * frame, fill, radius - 2);
	} else {
		box(x, y, w, h, fill, radius);
	}
}


// A picture in a dark frame, or words in its place on a screen that is off:
// near black with a light edge, seen on the grey cards, on red and on the
// blue dialogs.
void thumb_or_note(void * t, float x, float y, float w, float h, const char * note, int px)
{
	if (t != nullptr) {
		box(x - 2, y - 2, w + 4, h + 4, NEAR_BLACK, 3);
		g_images->drawTexture(t, x, y, uint16_t(w), uint16_t(h), 0, 1, 1, 255);
		return;
	}
	box(x, y, w, h, NEAR_BLACK, 4, 2, rgb(T.panel.fill));
	std::string lines[4];
	int n = wrap_fit(note, px, 13, int(w) - 12, lines, 4);
	float ty = y + (h - n * (px + 4)) / 2;
	for (int i = 0; i < n; i++)
		text(x + w / 2, ty + i * (px + 4), lines[i].c_str(), px, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
}

// A button glyph: the system font's where it has one, a drawn badge otherwise.
// A button's glyph at px (the legend's are 30), its top left at x, y; returns its width.
float key_glyph(float x, float y, int glyph, PixelColor c, int px = 30)
{
	const float k = px / 30.0f;
	const char * font_glyph = nullptr;
	switch (glyph) {
	case GLYPH_A: font_glyph = ""; break;
	case GLYPH_B: font_glyph = ""; break;
	case GLYPH_Y: font_glyph = ""; break;
	case GLYPH_X: font_glyph = ""; break;
	case GLYPH_HOME: font_glyph = ""; break;
	case GLYPH_PLUS: font_glyph = ""; break;
	case GLYPH_MINUS: font_glyph = "\xee\x81\x86"; break;
	case GLYPH_1: font_glyph = "\xee\x81\x87"; break;
	case GLYPH_2: font_glyph = "\xee\x81\x88"; break;
	case GLYPH_L: font_glyph = "\xee\x80\x84"; break;
	case GLYPH_R: font_glyph = "\xee\x80\x85"; break;
	case GLYPH_DPAD: // a D-pad: a cross
		box(x + 8 * k, y + 2 * k, 10 * k, 26 * k, c, 2);
		box(x, y + 10 * k, 26 * k, 10 * k, c, 2);
		return 28 * k;
	default: {
		const char * label = glyph == GLYPH_MINUS ? "-" : glyph == GLYPH_1 ? "1" : "2";
		box(x, y + k, 28 * k, 28 * k, c, 14 * k);
		text(x + 14 * k, y + k, label, int(24 * k), rgb(T.bar.fill), GUI_TEXT_JUSTIFY_CENTER);
		return 30 * k;
	}
	}
	text(x, y - 2 * k, font_glyph, px, c);
	return float(text_width(font_glyph, px));
}

void draw_keys(const pause_key * keys, int n, const char * idle);

void pause_legend_bar(const pause_view & pv)
{
	pause_key keys[8];
	int n = pause_legend(&pv.ui, pv.glyphs, keys, 8);
	// busy: nothing to press, the work in progress instead, unless the dialog
	// or the slots already say it (Zach, 2026-10-19: Reset showed it twice)
	const bool said = pv.ui.screen == PAUSE_ASK || pv.ui.screen == PAUSE_SLOTS;
	draw_keys(keys, n, said ? "" : pv.busy);
}

void draw_pause_menu(const pause_view & pv)
{
	const pause_ui & p = pv.ui;
	draw_header("PAUSED", over());
	// The game, and the autosave beside it when all is well (Zach, 2026-10-17: no description area).
	char sub[200];
	if (pv.autosave_lines > 0 && !pv.autosave_red)
		snprintf(sub, sizeof(sub), "%s  ·  %s", pv.title, pv.autosave[0]);
	else
		snprintf(sub, sizeof(sub), "%s", pv.title);
	text(160, 92, sub, fit(sub, 22, 16, UI_W - 160 - 30), rgb(over().muted));
	for (int i = 0; i < ITEMS; i++) {
		int r[4];
		pause_item_rect(&p, i, r);
		if (r[2] < 0)
			continue; // not this game's (Change disc for one disc)
		const bool off = !pause_item_enabled(&p, i);
		const bool sel = p.item == i && p.screen == PAUSE_MENU;
		selectable(r[0], r[1], r[2], r[3], sel, off, 6, 12);
		if (sel && off && refused_flash(FLASH_PAUSE, p.refused))
			flash_edge(r[0], r[1], r[2], r[3], 6, 12);
		const char * name = pause_item_name(&p, i);
		if (sel && off) // selected while it does nothing now: the item says why instead of its name
			button_words(r, pause_item_reason(&p, i), 26, card_ink(sel, off));
		else
			text(r[0] + r[2] / 2, r[1] + 18, name, fit(name, 26, 18, r[2] - 20), card_ink(sel, off), GUI_TEXT_JUSTIFY_CENTER);
	}
	if (!pv.autosave_red)
		return;
	// What failed and what is safe, under the items: a line each, on a light
	// panel, what failed in red.
	int fr[4];
	pause_failure_rect(fr);
	const float x0 = fr[0], y0 = fr[1], w = fr[2], h = fr[3], pad = 8;
	box(x0, y0, w, h, rgb(T.panel.fill), 8);
	const int lines = pv.autosave_lines < 4 ? pv.autosave_lines : 4;
	int size = 18;
	for (int i = 0; i < lines; i++) {
		int f = fit(pv.autosave[i], 18, 12, int(w - 2 * pad));
		size = f < size ? f : size;
	}
	while (size > 12 && lines * (size + 3) > h - 2 * pad + 3)
		size--;
	float y = y0 + (h - lines * (size + 3) + 3) / 2;
	for (int i = 0; i < lines; i++, y += size + 3)
		text(x0 + pad, y, pv.autosave[i], size, i == 0 ? rgb(T.fail.fill) : rgb(T.panel.ink));
}

void draw_slot_card(const pause_view & pv, int c)
{
	const pause_ui & p = pv.ui;
	int r[4];
	pause_card_rect(pause_cards_shown(&p), c, r);
	const pause_card & card = p.cards[c];
	const bool sel = p.card == c && p.screen == PAUSE_SLOTS;
	const bool unusable = card.status == CARD_DAMAGED || card.status == CARD_OTHER_VERSION ||
	                      card.status == CARD_OTHER_GAME;
	selectable(r[0], r[1], r[2], r[3], sel, false, 7, 10, unusable); // the Undo card too: its name sets it apart
	const PixelColor ink = unusable ? rgb(T.fail.ink) : card_ink(sel, false);
	char detail[96];
	pause_card_detail(detail, sizeof(detail), &card, pv.now, pv.us);
	const bool saving = p.busy && pv.busy_card == c;
	void * t = card.status == CARD_FILLED && !saving ? thumb_texture(c, pv.thumb[c], pv.thumb_gen[c]) : nullptr;
	const float tw = 112, th = 84, tx = r[0] + (r[2] - tw) / 2, ty = r[1] + 42;
	const char * name = pause_card_name(&p, c);
	text(r[0] + r[2] / 2, r[1] + 10, name, fit(name, 22, 16, r[2] - 20), ink,
	     GUI_TEXT_JUSTIFY_CENTER);
	thumb_or_note(t, tx, ty, tw, th, saving ? "Saving..." : detail, 18);
	if (t != nullptr)
		text(r[0] + r[2] / 2, r[1] + 133, detail, fit(detail, 18, 13, r[2] - 20), ink, GUI_TEXT_JUSTIFY_CENTER);
}

void draw_slots(const pause_view & pv)
{
	const pause_ui & p = pv.ui;
	draw_header(p.mode == SLOTS_SAVE ? "SAVE STATE" : "LOAD STATE", over());
	if (p.busy && pv.busy[0])
		text(160, 92, pv.busy, fit(pv.busy, 24, 18, UI_W - 200), rgb(over().accent));
	else if (p.line[0])
		text(160, 92, p.line, fit(p.line, 24, 18, UI_W - 200), rgb(over().ink));
	for (int c = 0; c < pause_cards_shown(&p); c++)
		if (!(p.card == c && p.screen == PAUSE_SLOTS))
			draw_slot_card(pv, c);
	if (p.screen == PAUSE_SLOTS)
		draw_slot_card(pv, p.card); // on top, since it is bigger
	char about[128];
	pause_card_description(about, sizeof(about), &p, p.card);
	if (about[0] && p.screen == PAUSE_SLOTS)
		text(UI_W / 2, 320, about, fit(about, 20, 15, UI_W - 80), rgb(over().muted), GUI_TEXT_JUSTIFY_CENTER);
}

void arrow(float x, float y, float len, PixelColor c)
{
	box(x, y - 4, len - 14, 8, c, 2);
	for (int i = 0; i < 16; i++) // the head, a triangle of thin boxes
		box(x + len - 16 + i, y - (16 - i), 1.5f, 2 * (16 - i), c);
}

// A dialog's frame: the same width on every screen, its buttons
// (pause_button_rect) 20 above its bottom. With pictures it is tall; with
// words alone it is shorter, so the words do not float in empty space.
const float DIALOG_X = 47, DIALOG_W = UI_W - 2 * 47, DIALOG_BOTTOM = 404;
const float DIALOG_TALL = 36, DIALOG_SHORT = 120;

// What failed is red: red stands for failure alone (every Our64
// dialog was red).
void dialog_frame(float top, bool failed = false)
{
	box(DIALOG_X, top, DIALOG_W, DIALOG_BOTTOM - top, failed ? rgb(T.fail.fill) : rgb(T.screen.fill), 16, 3, rgb(T.panel.fill), 14);
}

// Body lines centred between top and bottom: each whole on one line at the
// largest size from 22 down to 20 that allows it, else wrapped, smaller as
// needed.
void dialog_body(const char * const * body, int count, float top, float bottom, PixelColor c)
{
	const int room = int(DIALOG_W) - 80;
	std::string lines[8];
	int n = 0, size = 22;
	bool done = false;
	for (; size >= 20 && !done; size--) { // each line whole
		bool whole = top + count * (size + 8) - 8 <= bottom;
		for (int i = 0; i < count && whole; i++)
			whole = text_width(body[i], size) <= room;
		if (whole) {
			for (n = 0; n < count && n < 8; n++)
				lines[n] = body[n];
			done = true;
			break;
		}
	}
	for (size = done ? size : 22; !done && size >= 15; size--) { // wrapped
		n = 0;
		for (int i = 0; i < count && n < 8; i++)
			n += wrap(body[i], size, room, lines + n, 8 - n);
		if (top + n * (size + 8) - 8 <= bottom || size == 15)
			break;
	}
	const float h = n * (size + 8) - 8;
	float y = top + (bottom - top - h) / 2;
	for (int i = 0; i < n; i++, y += size + 8)
		if (y + size <= bottom + 4)
			text(UI_W / 2, y, lines[i].c_str(), size, c, GUI_TEXT_JUSTIFY_CENTER);
}

// The focused button is dark grey in a light frame; the others are grey with
// dark words. The action looks like every other button: a purple one read as
// a different kind of thing, and hid which was selected (Zach, 2026-10-23).
void dialog_buttons(int focus, bool one, const char * action)
{
	for (int b = 0; b < (one ? 1 : 2); b++) {
		int r[4];
		pause_button_rect(one ? 2 : b, r);
		if (focus == b) {
			box(r[0] - 6, r[1] - 6, r[2] + 12, r[3] + 12, rgb(T.panel.fill), 14, 0, rgb(T.panel.fill), 12);
			box(r[0] - 1, r[1] - 1, r[2] + 2, r[3] + 2, rgb(T.select.fill), 9);
		} else {
			box(r[0], r[1], r[2], r[3], rgb(T.bar.fill), 10);
		}
		const char * label = one ? "OK" : b == 1 ? action : "Cancel";
		const PixelColor c = card_ink(focus == b, false);
		text(r[0] + r[2] / 2, r[1] + 15, label, fit(label, 28, 18, r[2] - 24), c, GUI_TEXT_JUSTIFY_CENTER);
	}
}

void draw_ask(const pause_view & pv)
{
	const pause_ui & p = pv.ui;
	pause_ask_text t;
	pause_ask_words(&p, pv.now, pv.us, &t);
	const float bw = DIALOG_W;
	const bool pictures = p.ask != ASK_FAILED && t.thumbs > 0;
	const float top = pictures ? DIALOG_TALL : DIALOG_SHORT;
	dialog_frame(top, p.ask == ASK_FAILED);
	const our_surface & on = p.ask == ASK_FAILED ? T.fail : T.screen; // dialog_frame's
	text(UI_W / 2, top + 20, t.question, fit(t.question, 32, 22, int(bw) - 48), rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
	int button[4];
	pause_button_rect(0, button);
	float y = top + 72;
	if (pictures) {
		float tw = 128, th = 96; /* 4:3, small enough to leave the body three lines */
		if (t.lines == 0) /* no words under them: the pictures and labels sit midway to the buttons */
			y += (button[1] - 14 - y - (th + 8 + 18)) / 2;
		void * now = thumb_texture(CARDS, pv.now_thumb, pv.now_gen);
		void * card = p.ask_card >= 0 ? thumb_texture(p.ask_card, pv.thumb[p.ask_card], pv.thumb_gen[p.ask_card])
		                              : nullptr;
		if (p.ask_card >= 0 && p.cards[p.ask_card].status != CARD_FILLED)
			card = nullptr;
		if (p.ask == ASK_OVERWRITE) {
			const float x1 = UI_W / 2 - 60 - tw, x2 = UI_W / 2 + 60;
			thumb_or_note(now, x1, y, tw, th, "Now", 20);
			arrow(UI_W / 2 - 44, y + th / 2, 88, rgb(T.screen.accent));
			char old[96];
			pause_card_detail(old, sizeof(old), &p.cards[p.ask_card], pv.now, pv.us);
			thumb_or_note(card, x2, y, tw, th, old, 18);
			text(x1 + tw / 2, y + th + 8, t.thumb_label[0], 18, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
			text(x2 + tw / 2, y + th + 8, t.thumb_label[1], fit(t.thumb_label[1], 18, 13, 300), rgb(T.screen.ink),
			     GUI_TEXT_JUSTIFY_CENTER);
		} else {
			void * one = p.ask == ASK_RESET ? now : card;
			thumb_or_note(one, UI_W / 2 - tw / 2, y, tw, th, t.thumb_label[0], 18);
			text(UI_W / 2, y + th + 8, t.thumb_label[0], fit(t.thumb_label[0], 18, 13, 400), rgb(T.screen.ink),
			     GUI_TEXT_JUSTIFY_CENTER);
		}
		y += th + 40;
	} else {
		y = top + 64;
	}
	const char * body[3];
	for (int i = 0; i < t.lines && i < 3; i++)
		body[i] = t.body[i];
	dialog_body(body, t.lines < 3 ? t.lines : 3, y, button[1] - 14, rgb(on.ink));
	if (p.busy) {
		text(UI_W / 2, button[1] + 16, pv.busy, fit(pv.busy, 26, 18, int(bw) - 48), rgb(on.accent), GUI_TEXT_JUSTIFY_CENTER);
		return;
	}
	dialog_buttons(p.focus, p.ask == ASK_FAILED, t.button);
}

void draw_pause(const ui_view & v)
{
	if (v.pause == nullptr)
		return;
	const pause_view & pv = *v.pause;
	const pause_ui & p = pv.ui;
	draw_frozen(DIM_PAUSE);
	const int under = p.screen == PAUSE_ASK ? p.ask_from : p.screen;
	if (under == PAUSE_MENU)
		draw_pause_menu(pv);
	else
		draw_slots(pv);
	if (p.screen == PAUSE_ASK) {
		box(0, 0, UI_W, UI_H, DIM); // the screen it came from stays in view, dimmed
		draw_ask(pv);
	}
	draw_notices(true);
	pause_legend_bar(pv);
}


// ---- the legend, for every screen with keys ----

void draw_keys(const pause_key * keys, int n, const char * idle)
{
	bottom_bar();
	if (n == 0) {
		if (idle && *idle)
			text(UI_W / 2, 426, idle, 24, rgb(T.bar.muted), GUI_TEXT_JUSTIFY_CENTER);
		return;
	}
	float widths[8], total = 0;
	int size = 26, gap = 34;
	for (;;) {
		total = 0;
		for (int i = 0; i < n && i < 8; i++) {
			widths[i] = 30 + 8 + text_width(keys[i].label, size);
			total += widths[i] + (i ? gap : 0);
		}
		if (total <= UI_W - 48 || size <= 18)
			break;
		if (gap > 20)
			gap -= 2;
		else
			size--;
	}
	float x = (UI_W - total) / 2;
	for (int i = 0; i < n && i < 8; i++) {
		PixelColor c = keys[i].dim ? off_ink() : rgb(T.bar.ink);
		float g = key_glyph(x, 432, keys[i].glyph, c);
		text(x + g + 8, 428 + (26 - size) / 2, keys[i].label, size, c);
		x += widths[i] + gap;
	}
}

// ---- Our ROMs ----

// Covers: one texture per art slot, remade when the slot's gen changes.
void * g_art_tex[ROMS_ART];
unsigned g_art_gen[ROMS_ART];
int g_art_w[ROMS_ART], g_art_h[ROMS_ART];

void * art_texture(const roms_view & rv, int game)
{
	for (int i = 0; i < ROMS_ART; i++) {
		if (rv.art[i].game != game || rv.art[i].rgba == nullptr)
			continue;
		if (g_art_tex[i] == nullptr || g_art_gen[i] != rv.art[i].gen || g_art_w[i] != rv.art[i].w ||
		    g_art_h[i] != rv.art[i].h) {
			if (g_art_tex[i] != nullptr && (g_art_w[i] != rv.art[i].w || g_art_h[i] != rv.art[i].h)) {
				g_images->destroyTexture(g_art_tex[i]);
				g_art_tex[i] = nullptr;
			}
			if (g_art_tex[i] == nullptr)
				g_art_tex[i] = (g_images->createTexture(rv.art[i].w, rv.art[i].h));
			if (g_art_tex[i] == nullptr)
				return nullptr;
			g_images->loadTextureData(g_art_tex[i], rv.art[i].rgba, rv.art[i].w, rv.art[i].h);
			g_art_gen[i] = rv.art[i].gen;
			g_art_w[i] = rv.art[i].w;
			g_art_h[i] = rv.art[i].h;
		}
		return g_art_tex[i];
	}
	return nullptr;
}

// A game with no box art: its title light on blue, framed light; a file that
// cannot be read: light on red, a failure.
void poster(float x, float y, float w, float h, const char * title, bool failed)
{
	const our_surface & on = failed ? T.fail : T.screen;
	box(x, y, w, h, rgb(T.panel.fill));
	box(x + 4, y + 4, w - 8, h - 8, rgb(on.fill));
	std::string upper(title);
	for (char & c : upper)
		c = char(c >= 'a' && c <= 'z' ? c - 32 : c);
	int size = h > 100 ? 22 : 12;
	std::string lines[3];
	int n = wrap_fit(upper.c_str(), size, h > 100 ? 13 : 9, int(w) - 32, lines, 3);
	float ty = y + (h - n * (size + 3)) / 2;
	for (int i = 0; i < n; i++)
		text(x + w / 2, ty + i * (size + 3), lines[i].c_str(), size, rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
}

// The art slot that holds a game's cover, -1.
int art_slot(const roms_view & rv, int game)
{
	for (int i = 0; i < ROMS_ART; i++)
		if (rv.art[i].game == game && rv.art[i].rgba != nullptr)
			return i;
	return -1;
}

// The covers, left to right (Zach, 2026-10-30): the selected one large in
// the middle, the ones before it behind it on the left, the ones after it on
// the right, smaller and darker further out. pos slides, so the covers glide
// between selections.
const float STAGE_CX = UI_W / 2.0f; // the selected cover's middle
const float STAGE_CY = 220; // between the letter bar and the bottom bar
const float SEL_W = ROMS_COVER_RIGHT - ROMS_COVER_LEFT; // the widest the selected cover is
const float SEL_H = 270;                                // the tallest (Zach, 2026-10-30: 320 was a tad big)
const float SEL_AREA = 387 * SEL_H;                     // an N64 box at full height: every cover's area

// A cover's box takes the art's own shape: the area of an N64 box, no wider
// than SEL_W, no taller than SEL_H.
void cover_box(float aspect, float & w, float & h)
{
	w = sqrtf(SEL_AREA * aspect);
	h = w / aspect;
	if (h > SEL_H) {
		h = SEL_H;
		w = h * aspect;
	}
	if (w > SEL_W) {
		w = SEL_W;
		h = w / aspect;
	}
}

// The shape of the covers last shown: a cover still on its way from the SD
// card, and a poster, stand in it, so the row keeps the app's shape.
float g_cover_aspect = 387 / SEL_H;

float cover_aspect(const roms_view & rv, int game)
{
	for (int i = 0; i < ROMS_ART; i++)
		if (rv.art[i].game == game && rv.art[i].rgba != nullptr && rv.art[i].w > 0 && rv.art[i].h > 0)
			return float(rv.art[i].w) / float(rv.art[i].h);
	return g_cover_aspect;
}

// By distance from the selection: a cover's scale, and how far its middle
// sits from the selected one's. Five show (Zach, 2026-10-30): about 135 of
// each next one past the selected cover's edge, about 90 of each second one
// out past that, to the screen's edge.
float by_distance(const float * at, float a)
{
	if (a >= 3)
		return at[3];
	const int i = int(a);
	return at[i] + (at[i + 1] - at[i]) * (a - i);
}
const float COVER_SCALE[4] = { 1, 0.72f, 0.55f, 0.45f };
const float COVER_OFF[4] = { 0, 189, 313, 380 };

void stage_cover(const roms_view & rv, int at, float d)
{
	const int game = rv.shown[at];
	const library_game & g = rv.games[game];
	const float a = fabsf(d);
	const float scale = by_distance(COVER_SCALE, a);
	float w, h;
	cover_box(cover_aspect(rv, game), w, h);
	w *= scale;
	h *= scale;
	const float cx = STAGE_CX + (d < 0 ? -1 : 1) * by_distance(COVER_OFF, a);
	const float x = cx - w / 2, top = STAGE_CY - h / 2;
	// the third ones out only show while the covers slide in or out
	const uint8_t fade = uint8_t(a > 2.2f ? fmaxf(0, 255 * (3.0f - a) / 0.8f) : 255);
	if (fade == 0)
		return;
	const int slot = g.readable ? art_slot(rv, game) : -1;
	void * t = slot >= 0 ? art_texture(rv, game) : nullptr;
	// One thing has the focus (Zach, 2026-10-30): the selected cover, framed
	// light and glowing, while left/right move the covers; on the bar, it
	// stands framed dark like the rest.
	const bool focus = a < 0.5f && !rv.bar_focus;
	const PixelColor edge = focus ? rgb(T.panel.fill, fade) : rgb(T.bar.fill, fade);
	if (focus) // the selected one throws a shadow on the rest
		box(x - 3, top - 3, w + 6, h + 6, PixelColor{0, 0, 0, 0}, 0, 0, rgb(T.panel.fill), 18);
	if (t != nullptr) {
		box(x - 4, top - 4, w + 8, h + 8, edge);
		// the box is the art's shape: the art fills it
		g_cover_aspect = float(rv.art[slot].w) / float(rv.art[slot].h);
		box(x, top, w, h, PixelColor{NEAR_BLACK.r, NEAR_BLACK.g, NEAR_BLACK.b, fade});
		g_images->drawTexture(t, x, top, uint16_t(lroundf(w)), uint16_t(lroundf(h)), 0, 1, 1, fade);
	} else if (!g.known || (g.readable && g.art)) { // its cover is on the way from the SD card
		box(x - 4, top - 4, w + 8, h + 8, edge);
		box(x, top, w, h, rgb(T.dim.fill, fade));
	} else {
		poster(x, top, w, h, g.title, !g.readable);
	}
	if (a > 0.05f) // further out is darker
		box(x - 4, top - 4, w + 8, h + 8, PixelColor{0, 0, 0, uint8_t(fminf(a, 3) * 50 * fade / 255)});
	else if (rv.bar_focus) // the bar has the focus: the covers wait, dimmed
		box(x - 4, top - 4, w + 8, h + 8, PixelColor{0, 0, 0, 60});
	// The badge stars a favorite, else counts save states (an autosave alone
	// gets none): Zach, 2026-10-25, after the star had replaced the count.
	if (a < 0.5f && (g.favorite || g.states > 0)) {
		char count[12];
		snprintf(count, sizeof(count), "%d", g.states);
		box(x + w - 24, top - 12, 36, 36, rgb(T.lit), 18);
		// a light badge (Comrade's gold, Standard's pale blue) takes the dark panel words
		const PixelColor on_lit = theme_contrast(T.screen.ink, T.lit) >= 3 ? rgb(T.screen.ink) : rgb(T.panel.ink);
		// the star a little smaller and higher than a digit: a star's middle sits above the middle of its box
		text(x + w - 6, top - (g.favorite ? 10 : 9), g.favorite ? "★" : count, g.favorite ? 22 : 24, on_lit, GUI_TEXT_JUSTIFY_CENTER);
	}
}

// The letter bar: the Favorites star, Recent, All, #, A to Z (library_bar_rect). Letters with no games are dim.
void draw_bar(const roms_view & rv)
{
	int first[4], last[4];
	library_bar_rect(0, first);
	library_bar_rect(LIBRARY_CATEGORIES - 1, last);
	box(first[0] - 6, first[1] - 3, last[0] + last[2] - first[0] + 12, first[3] + 6, rgb(T.bar.fill)); // square, as the box art (Zach, 2026-10-25)
	// The selected tab first, so the letters beside it draw on top. Its pill
	// grows past the label so the white frame clears the glyph, and stays inside
	// the bar (which reaches 3 past the tabs), 2 taller each way. Recent and All
	// have room: a 3 frame and 6 inside, at least the tab and 2. A letter, #
	// or the star sits 24 from its neighbours: a 2 frame and 2 inside (the
	// glyph and 8), so the frame stays clear of the letters beside it too. Corners rounded less
	// than a capsule's, so they do not cut into the label's. The other tabs stay
	// where they are.
	int t[4];
	library_bar_rect(rv.category, t);
	char sel_label[8];
	library_category_label(rv.category, sel_label, sizeof(sel_label));
	const bool one = strlen(sel_label) == 1 || rv.category == LIBRARY_FAVORITES; // the star is one glyph too
	const float frame = one ? 2 : 3, room = one ? 2 : 6;
	float pw = text_width(sel_label, 20) + 2 * (frame + room);
	if (!one && pw < t[2] + 4)
		pw = t[2] + 4;
	const float ph = t[3] + 4;
	// The focus look only while left/right move along the bar (Zach,
	// 2026-10-30); else the tab in use is underlined in the accent.
	const bool focus = rv.bar_focus || theme_roms_list(); // the list's left/right always move along the bar
	if (focus)
		box(t[0] + t[2] / 2.0f - pw / 2, t[1] - 2, pw, ph, rgb(T.select.fill), 0, frame, rgb(T.panel.fill), 6);
	else
		box(t[0] + t[2] / 2.0f - (pw - 2 * frame) / 2, t[1] + t[3] - 3, pw - 2 * frame, 3, rgb(T.bar.accent));
	for (int c = 0; c < LIBRARY_CATEGORIES; c++) {
		int r[4];
		library_bar_rect(c, r);
		char label[8];
		library_category_label(c, label, sizeof(label));
		const bool sel = rv.category == c;
		const PixelColor ink = sel ? (focus ? rgb(T.select.ink) : rgb(T.bar.accent)) : rv.has[c] ? rgb(T.bar.ink) : off_ink();
		text(r[0] + r[2] / 2, r[1] + 6, label, 20, ink, GUI_TEXT_JUSTIFY_CENTER);
	}
}

// The bottom row, one dark bar across the screen (Zach, 2026-10-30): the
// game's buttons on the left, its title in the middle, OUR ROMS and the count
// on the right.
const float ROW_TOP = 394;           // the bar's top; it runs to the screen's bottom
const float ROW_BOX_W = 176;         // the buttons' and the count's room at either end
const float PILL_X = ROW_BOX_W + 12, PILL_W = UI_W - 2 * PILL_X, PILL_Y = 402, PILL_H = 70;

// The buttons one under another, small: only the game's own (Zach,
// 2026-10-30: the d-pad and HOME work alike in every Wii U app).
// room: the keys' width, the box at the left unless nothing is drawn beside it.
void draw_side_keys(const pause_key * keys, int n, float room = ROW_BOX_W)
{
	box(0, ROW_TOP, UI_W, UI_H - ROW_TOP, rgb(T.bar.fill));
	const int size = 18, step = 25;
	const float top = ROW_TOP + (UI_H - ROW_TOP - n * step) / 2.0f;
	for (int i = 0; i < n; i++) {
		const PixelColor c = keys[i].dim ? off_ink() : rgb(T.bar.ink);
		const float y = top + i * step;
		const float g = key_glyph(16, y, keys[i].glyph, c, 20);
		text(16 + g + 7, y + 1, keys[i].label, fit(keys[i].label, size, 15, int(room - 16 - g - 7 - 10)), c);
	}
}

void draw_count_box(const roms_view & rv)
{
	const float cx = UI_W - ROW_BOX_W / 2;
	text(cx, ROW_TOP + 14, "OUR ROMS", 24, rgb(T.bar.ink), GUI_TEXT_JUSTIFY_CENTER);
	char count[40];
	snprintf(count, sizeof(count), "%d %s", rv.total, rv.total == 1 ? "Game" : "Games");
	text(cx, ROW_TOP + 46, count, fit(count, 19, 14, int(ROW_BOX_W - 20)), rgb(T.bar.muted), GUI_TEXT_JUSTIFY_CENTER);
}

// The selected game's title: one line, two when it is long; a file that
// cannot be read says so under it. A notice takes the title's place.
void draw_title_pill(const roms_view & rv, const library_game * g)
{
	const bool failed = rv.notice[0] || (g != nullptr && g->known && !g->readable);
	const bool focus = !rv.bar_focus; // the title is the selected cover's: focused with it
	selectable(PILL_X, PILL_Y, PILL_W, PILL_H, focus, false, 0, 12, failed);
	const our_surface & on = failed ? T.fail : focus ? T.select : T.bar; // selectable's
	const int room = int(PILL_W - 36);
	const char * second = rv.notice[0] || g == nullptr || !g->known || g->readable ? nullptr
	                    : "Can't read this file. Copy it again with a computer.";
	const char * s = rv.notice[0] ? rv.notice : g != nullptr ? g->title : "";
	if (second != nullptr) {
		text(PILL_X + PILL_W / 2, PILL_Y + 9, s, fit(s, 22, 15, room), rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
		text(PILL_X + PILL_W / 2, PILL_Y + 40, second, fit(second, 18, 13, room), rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
		return;
	}
	if (!rv.notice[0] && text_width(s, 22) <= room) {
		const int px = fit(s, 26, 22, room);
		text(PILL_X + PILL_W / 2, PILL_Y + (PILL_H - px) / 2 - 2, s, px, rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
		return;
	}
	std::string two[2];
	int ts = rv.notice[0] ? 18 : 21;
	const int k = wrap_fit(s, ts, 13, room, two, 2);
	const float top = PILL_Y + (PILL_H - k * (ts + 4)) / 2.0f - 1;
	for (int i = 0; i < k; i++)
		text(PILL_X + PILL_W / 2, top + i * (ts + 4), two[i].c_str(), ts, rgb(on.ink), GUI_TEXT_JUSTIFY_CENTER);
}

// Our ROMs as a list (Settings > Theme > Our ROMs: List), as it was before the
// row of covers (2026-10-30). The drawer. Covers stand in a column like records in a crate seen from the
// front: the selected one pulled up and out to the right, whole; the ones
// before it lean back behind it, their tops showing; the ones after it stand
// in front, their bottoms showing. Further from the selection is smaller and
// darker. pos slides, so the covers glide between selections.
const float DRAWER_X = 196;   // the column's middle
const float DRAWER_Y = 284;   // the selected cover's middle
const float COVER_H = 132;    // the box of a landscape cover (N64, SNES): its area is every cover's
const float COVER_W = 182;    // the widest a cover is
const float COVER_H_MAX = 136; // the tallest: the covers behind and in front stay clear of the letter bar
const float PULL = 30;        // how far the selected cover comes out to the right

// A cover's box takes the art's own shape (Zach, 2026-10-23: NES and Genesis
// boxes had black bars, GBA's came out small): the area of the landscape box,
// no wider than COVER_W, no taller than COVER_H_MAX.
void drawer_box(float aspect, float & w, float & h)
{
	w = sqrtf(COVER_W * COVER_H * aspect);
	h = w / aspect;
	if (h > COVER_H_MAX) {
		h = COVER_H_MAX;
		w = h * aspect;
	}
	if (w > COVER_W) {
		w = COVER_W;
		h = w / aspect;
	}
}

// Distance from the selection to how much of a cover shows past its neighbour's edge.
float drawer_reach(float d)
{
	const float a = fabsf(d);
	const float step[4] = { 0, 26, 44, 54 };
	if (a >= 3)
		return step[3];
	const int i = int(a);
	return step[i] + (step[i + 1] - step[i]) * (a - i);
}

void drawer_cover(const roms_view & rv, int at, float d)
{
	const int game = rv.shown[at];
	const library_game & g = rv.games[game];
	const float a = fabsf(d);
	const float scale = 1 - 0.07f * (a < 3 ? a : 3);
	float w, h, sel_w, sel_h;
	drawer_box(cover_aspect(rv, game), w, h);
	w *= scale;
	h *= scale;
	drawer_box(cover_aspect(rv, rv.shown[rv.sel]), sel_w, sel_h);
	const float reach = drawer_reach(d);
	// before the selection: the top edge rises; after it: the bottom edge drops
	const float top = d < 0 ? DRAWER_Y - sel_h / 2 - reach : DRAWER_Y + sel_h / 2 + reach - h;
	const float out = a < 1 ? PULL * (1 - a) : 0;
	const float x = DRAWER_X - w / 2 + out;
	const uint8_t fade = uint8_t(a > 2.4f ? fmaxf(0, 255 * (3.2f - a) / 0.8f) : 255);
	if (fade == 0)
		return;
	const int slot = g.readable ? art_slot(rv, game) : -1;
	void * t = slot >= 0 ? art_texture(rv, game) : nullptr;
	if (a < 0.5f) // the selected one throws a shadow on the rest
		box(x - 2, top - 2, w + 4, h + 4, PixelColor{0, 0, 0, 0}, 0, 0, rgb(T.panel.fill), 14);
	if (t != nullptr) {
		box(x - 3, top - 3, w + 6, h + 6, rgb(T.panel.fill, fade));
		// the box is the art's shape: the art fills it
		g_cover_aspect = float(rv.art[slot].w) / float(rv.art[slot].h);
		box(x, top, w, h, PixelColor{NEAR_BLACK.r, NEAR_BLACK.g, NEAR_BLACK.b, fade});
		g_images->drawTexture(t, x, top, uint16_t(lroundf(w)), uint16_t(lroundf(h)), 0, 1, 1, fade);
	} else if (!g.known || (g.readable && g.art)) { // its cover is on the way from the SD card
		box(x - 3, top - 3, w + 6, h + 6, rgb(T.panel.fill, fade));
		box(x, top, w, h, rgb(T.dim.fill, fade));
	} else {
		poster(x, top, w, h, g.title, !g.readable);
	}
	if (a > 0.05f) // further back is darker
		box(x - 3, top - 3, w + 6, h + 6, PixelColor{0, 0, 0, uint8_t(fminf(a, 3) * 45 * fade / 255)});
	// The badge stars a favorite, else counts save states (an autosave alone
	// gets none): Zach, 2026-10-25, after the star had replaced the count.
	if (a < 0.5f && (g.favorite || g.states > 0)) {
		char count[12];
		snprintf(count, sizeof(count), "%d", g.states);
		box(x + w - 20, top - 10, 30, 30, rgb(T.lit), 15);
		// Our64's gold is light: the badge takes the dark panel words there
		const PixelColor on_lit = theme_contrast(T.screen.ink, T.lit) >= 3 ? rgb(T.screen.ink) : rgb(T.panel.ink);
		text(x + w - 5, top - 8, g.favorite ? "\u2605" : count, 20, on_lit, GUI_TEXT_JUSTIFY_CENTER);
	}
}

// No games yet, or the ROM folder not found, where the covers go: a headline,
// and the sentence with the folder's full path in up to four lines. x is the left edge, or the middle when
// centred.
void draw_roms_none(const roms_view & rv, float x, float head_y, float body_y, int w, bool centred)
{
	const char * folder = rv.folder[0] ? rv.folder : "SD card: /wiiu/our64/roms";
	const char * head = "No games yet.";
	char body[sizeof(rv.folder) + 100];
	if (rv.missing) { // a USB drive not plugged in, or a folder deleted: one message (Zach, 2026-11-04)
		head = "No ROMs found.";
		snprintf(body, sizeof(body), "Could not locate %s. Press A to select a new folder.", folder);
	} else {
		snprintf(body, sizeof(body), "Copy your .z64, .n64 or .v64 files to %s with a computer, then start Our64 again.", folder);
	}
	const uint32_t align = centred ? GUI_TEXT_JUSTIFY_CENTER : GUI_TEXT_JUSTIFY_LEFT;
	text(x, head_y, head, fit(head, 30, 22, w), rgb(T.screen.ink), align);
	// the folder whole, never cut: up to four lines, smaller type for a long path
	std::string lines[4];
	int size = 24;
	int k = wrap_fit(body, size, 16, w, lines, 4);
	for (int i = 0; i < k; i++)
		text(x, body_y + i * (size + 8), lines[i].c_str(), size, rgb(T.screen.ink), align);
}

// A on a folder not found: the ROM folder picker, as in Settings > Games.
pause_key select_folder_key(const roms_view & rv)
{
	return pause_key{ rv.glyphs == INPUT_WIIMOTE ? GLYPH_2 : GLYPH_A, "Select a new folder", 0 };
}

void draw_roms_list(const ui_view & v)
{
	const roms_view & rv = *v.roms;
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	draw_header("OUR ROMS");
	char sub[200];
	if (rv.notice[0]) {
		text(header_x(), 92, rv.notice, fit(rv.notice, 22, 15, int(UI_W - 40 - header_x())), rgb(T.screen.accent));
	} else {
		snprintf(sub, sizeof(sub), "%d %s", rv.total, rv.total == 1 ? "Game" : "Games");
		text(header_x(), 92, sub, 22, rgb(T.screen.muted));
	}
	pause_key keys[7];
	int n = 0;
	if (rv.missing)
		keys[n++] = select_folder_key(rv);
	else
		keys[n++] = pause_key{ GLYPH_DPAD, "Choose", rv.total == 0 };
	keys[n++] = pause_key{ GLYPH_PLUS, "Settings", 0 };
	keys[n++] = pause_key{ GLYPH_HOME, "Wii U Menu", 0 };
	if (rv.total == 0) {
		draw_roms_none(rv, 60, 190, 240, UI_W - 120, false);
		draw_keys(keys, n, nullptr);
		return;
	}
	draw_bar(rv);
	// The crate the covers stand in.
	box(DRAWER_X - COVER_W / 2 - 22, 166, COVER_W + 44 + PULL, 240, rgb(T.bar.fill)); // square (Zach, 2026-10-25)
	const int sel = rv.sel;
	const float pos = rv.pos;
	int order[8], k = 0;
	for (int at = int(floorf(pos)) - 3; at <= int(ceilf(pos)) + 3; at++)
		if (at >= 0 && at < rv.count && fabsf(at - pos) < 3.2f && k < 8)
			order[k++] = at;
	// furthest first, so the nearer covers stand in front
	for (int i = 0; i < k; i++)
		for (int j = i + 1; j < k; j++)
			if (fabsf(order[j] - pos) > fabsf(order[i] - pos)) {
				int t = order[i];
				order[i] = order[j];
				order[j] = t;
			}
	for (int i = 0; i < k; i++)
		drawer_cover(rv, order[i], order[i] - pos);

	// The names as a list beside the drawer: the selected game's row framed,
	// with its autosave and save states inside, and the games before and after
	// it in rows of their own, evenly spaced (Zach, 2026-10-17).
	const library_game & g = rv.games[rv.shown[sel]];
	const float lx = 392, lw = UI_W - 40 - lx, pad = 18, gap = 12, row_h = 44, sel_h = 100;
	const float sel_y = DRAWER_Y - sel_h / 2;
	for (int d = -1; d <= 1; d += 2) {
		const int at = sel + d;
		if (at < 0 || at >= rv.count)
			continue;
		const float y = d < 0 ? sel_y - gap - row_h : sel_y + sel_h + gap;
		const library_game & n = rv.games[rv.shown[at]];
		const bool unreadable = n.known && !n.readable; // a failure: red
		const our_surface & row = unreadable ? T.fail : T.bar;
		box(lx, y, lw, row_h, rgb(row.fill), 10);
		// down to 13: Ocarina of Time - Master Quest, Our64's longest title, is 2 too wide at 14
		text(lx + pad, y + 11, n.title, fit(n.title, 20, 13, int(lw - 2 * pad)), rgb(row.muted));
	}
	selectable(lx, sel_y, lw, sel_h, true, false, 0, 12, g.known && !g.readable);
	const our_surface & on = g.known && !g.readable ? T.fail : T.select; // selectable's
	const int room = int(lw - 2 * pad);
	float dy = sel_y + 56;
	if (text_width(g.title, 22) <= room) {
		text(lx + pad, sel_y + 14, g.title, fit(g.title, 28, 22, room), rgb(on.ink));
	} else { // a long name on two lines, the details closer under it
		std::string two[2];
		int ts = 20;
		int k = wrap_fit(g.title, ts, 15, room, two, 2);
		for (int i = 0; i < k; i++)
			text(lx + pad, sel_y + 10 + i * (ts + 3), two[i].c_str(), ts, rgb(on.ink));
		dy = sel_y + 66;
	}
	if (!g.known) {
		// its saves are still being read: nothing yet rather than a guess
	} else if (!g.readable) {
		const char * no = "Can't read this file. Copy it again with a computer.";
		text(lx + pad, dy, no, fit(no, 20, 14, room), rgb(on.ink));
	} else {
		char save[64] = "";
		if (g.autosave && g.autosave_ok) {
			char when[48];
			ui_when(when, sizeof(when), rv.now, g.autosave, rv.us);
			snprintf(save, sizeof(save), "Autosave: %s", when);
		} else if (g.autosave) {
			snprintf(save, sizeof(save), "Autosave from another Our64 version");
		} else {
			snprintf(save, sizeof(save), "Not played yet");
		}
		text(lx + pad, dy, save, fit(save, 20, 14, room), g.autosave && g.autosave_ok ? rgb(on.ink) : rgb(on.muted));
	}
	keys[n++] = pause_key{ rv.glyphs == INPUT_WIIMOTE ? GLYPH_2 : GLYPH_A,
	                        g.autosave && g.autosave_ok ? "Resume" : "Start", g.known && !g.readable };
	keys[n++] = pause_key{ rv.glyphs == INPUT_WIIMOTE || rv.glyphs == INPUT_NUNCHUK ? GLYPH_MINUS : GLYPH_Y,
	                        g.favorite ? "Unfavorite" : "Favorite", 0 };
#ifndef OUR64_RELEASE // Delete is the developer's own: the public build has none
	if (rv.glyphs != INPUT_WIIMOTE && rv.glyphs != INPUT_NUNCHUK) // X: a Wii Remote has none to spare
		keys[n++] = pause_key{ GLYPH_X, "Delete", rv.usb }; // greyed on a USB drive: X says why
#endif
	draw_keys(keys, n, nullptr);
}

void draw_roms(const ui_view & v)
{
	if (theme_roms_list()) {
		draw_roms_list(v);
		return;
	}
	const roms_view & rv = *v.roms;
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	pause_key keys[4];
	int n = 0;
	if (rv.total == 0) {
		draw_roms_none(rv, UI_W / 2, 150, 200, UI_W - 160, true);
		if (rv.missing)
			keys[n++] = select_folder_key(rv);
		keys[n++] = pause_key{ GLYPH_PLUS, "Settings", 0 };
		draw_side_keys(keys, n, rv.notice[0] ? ROW_BOX_W : UI_W - ROW_BOX_W); // no title: up to the count
		if (rv.notice[0])
			draw_title_pill(rv, nullptr);
		draw_count_box(rv);
		return;
	}
	draw_bar(rv);
	const int sel = rv.sel;
	const float pos = rv.pos;
	int order[8], k = 0;
	for (int at = int(floorf(pos)) - 3; at <= int(ceilf(pos)) + 3; at++)
		if (at >= 0 && at < rv.count && fabsf(at - pos) < 3.0f && k < 8)
			order[k++] = at;
	// furthest first, so the nearer covers stand in front
	for (int i = 0; i < k; i++)
		for (int j = i + 1; j < k; j++)
			if (fabsf(order[j] - pos) > fabsf(order[i] - pos)) {
				int t = order[i];
				order[i] = order[j];
				order[j] = t;
			}
	for (int i = 0; i < k; i++)
		stage_cover(rv, order[i], order[i] - pos);

	const library_game & g = rv.games[rv.shown[sel]];
	keys[n++] = pause_key{ rv.glyphs == INPUT_WIIMOTE ? GLYPH_2 : GLYPH_A,
	                        rv.bar_focus ? "Games" : g.autosave && g.autosave_ok ? "Resume" : "Start",
	                        !rv.bar_focus && g.known && !g.readable };
	keys[n++] = pause_key{ rv.glyphs == INPUT_WIIMOTE || rv.glyphs == INPUT_NUNCHUK ? GLYPH_MINUS : GLYPH_Y,
	                        g.favorite ? "Unfavorite" : "Favorite", 0 };
	keys[n++] = pause_key{ GLYPH_PLUS, "Settings", 0 };
	// three shown (Zach, 2026-10-30): X still deletes in the developer's own build, unlisted
	draw_side_keys(keys, n);
	draw_title_pill(rv, &g);
	draw_count_box(rv);
}

// ---- Loading ----

// The loading screen's bar, and the step under it (Fetch covers' too).
static void progress_bar(float progress, const char * step)
{
	const float bx = 177, bw = 500;
	box(bx, 278, bw, 22, rgb(T.bar.fill), 11);
	const float p = progress < 0 ? 0 : progress > 1 ? 1 : progress;
	if (p > 0.02f)
		box(bx, 278, bw * p, 22, rgb(T.lit), 11);
	text(UI_W / 2, 316, step, 22, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
}

void draw_loading(const ui_view & v)
{
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	const loading_view & l = v.loading;
	// The portrait and the game's cover, the same height, side by side in one
	// light frame, centred (Zach, 2026-10-20); the portrait alone without a cover.
	// Standard has no portrait: the cover alone, or nothing.
	const float h = 170, w = !theme_comrade() ? 0 : g_cat2_h > 0 ? h * g_cat2_w / g_cat2_h : 127;
	void * cover = v.roms != nullptr ? art_texture(*v.roms, l.game) : nullptr;
	float cw = 0;
	if (cover != nullptr)
		for (int i = 0; i < ROMS_ART; i++)
			if (v.roms->art[i].game == l.game && v.roms->art[i].h > 0)
				cw = h * v.roms->art[i].w / v.roms->art[i].h;
	const float gap = cw > 0 && w > 0 ? 5 : 0, total = w + gap + cw, x0 = UI_W / 2 - total / 2;
	if (total > 0)
		box(x0 - 5, 30 - 5, total + 10, h + 10, rgb(T.panel.fill), 6, 0, rgb(T.panel.fill), 6);
	if (g_cat2 != nullptr && w > 0)
		g_images->drawTexture(g_cat2, x0, 30, uint16_t(w), uint16_t(h), 0, 1, 1, 255);
	if (cw > 0)
		g_images->drawTexture(cover, x0 + w + gap, 30, uint16_t(cw), uint16_t(h), 0, 1, 1, 255);
	text(UI_W / 2, 218, v.title, fit(v.title, 32, 20, UI_W - 80), rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
	pause_key keys[2];
	int n = 0;
	if (l.stopped[0]) { // the autosave cannot be used: why, and what A and B do
		std::string lines[4];
		int size = 22;
		int k = wrap_fit(l.stopped, size, 16, UI_W - 120, lines, 4);
		box(40, 264, UI_W - 80, k * (size + 6) + 18, rgb(T.fail.fill), 12); // a failure: red
		for (int i = 0; i < k; i++)
			text(UI_W / 2, 276 + i * (size + 6), lines[i].c_str(), size, rgb(T.fail.ink), GUI_TEXT_JUSTIFY_CENTER);
		keys[n++] = pause_key{ GLYPH_B, "Back to Our ROMs", 0 };
		keys[n++] = pause_key{ GLYPH_A, "Start from the title screen", 0 };
		draw_keys(keys, n, nullptr);
		return;
	}
	// One bar for the whole load; the words below say which step it is on.
	progress_bar(l.progress, l.step);
	if (v.legend[0]) {
		keys[n++] = pause_key{ GLYPH_HOME, v.legend, 0 };
		draw_keys(keys, n, nullptr);
	} else {
		bottom_bar();
	}
}

// ---- Setup screens ----

void setup_header(const char * headline, const char * sub)
{
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	draw_header(headline);
	if (sub && *sub) { // a long folder path loses its middle
		const int room = int(UI_W - 40 - header_x());
		int px = 22;
		while (px > 15 && text_width(sub, px) > room)
			px--;
		text(header_x(), 92, cut_to_fit(sub, px, room).c_str(), px, rgb(T.screen.muted));
	}
}

// Rows of named values, Settings', Picture's and Theme's: a row that opens a
// page has ">", a value row its values with the one in use ticked and the one
// left/right is on in a thick light border.
void value_rows(int rows, int sel, const char * (*name)(int), int (*values)(int), const char * (*value)(int, int),
                const int * pick, const int * in_use, const bool * page, bool (*off)(int, int))
{
	for (int row = 0; row < rows; row++) {
		int r[4];
		rows_rect(rows, row, r);
		selectable(r[0], r[1], r[2], r[3], sel == row, false, 5, 10);
		const int inset = (r[3] - 26) / 2 - 2; // 12 in a row of 54
		text(r[0] + 20, r[1] + inset, name(row), 26, card_ink(sel == row, false));
		if (page[row]) {
			text(r[0] + r[2] - 24, r[1] + inset - 1, ">", 28, card_muted(sel == row), GUI_TEXT_JUSTIFY_RIGHT);
			continue;
		}
		for (int i = 0; i < values(row); i++) {
			int vr[4];
			rows_value_rect(rows, row, i, vr);
			const bool picked = pick[row] == i, dead = off != nullptr && off(row, i);
			if (picked)
				box(vr[0] - 3, vr[1] - 3, vr[2] + 6, vr[3] + 6, rgb(T.panel.fill), 24);
			box(vr[0], vr[1], vr[2], vr[3], i == in_use[row] ? rgb(T.panel.fill) : rgb(T.bar.fill), 21);
			char label[48];
			snprintf(label, sizeof(label), "%s%s", i == in_use[row] ? "✓ " : "", value(row, i));
			const our_surface & on = i == in_use[row] ? T.panel : T.bar;
			text(vr[0] + vr[2] / 2, vr[1] + (vr[3] - 20) / 2 - 1, label, fit(label, 20, 14, vr[2] - 24), dead ? rgb(theme_off(&on)) : rgb(on.ink),
			     GUI_TEXT_JUSTIFY_CENTER);
		}
	}
}

// The line under the last of a page's rows.
void row_about(int rows, const char * about)
{
	int under[4];
	rows_rect(rows, rows, under);
	text(40, under[1] + 4, about, fit(about, 19, 14, UI_W - 80), rgb(T.screen.muted));
}

void draw_settings(const setup_view & sv)
{
	const settings_ui & s = sv.settings;
	setup_header("OUR SETTINGS", sv.line[0] ? sv.line : nullptr);
	text(UI_W - 40, 52, "Version " OUR64_VERSION, 18, rgb(T.screen.muted), GUI_TEXT_JUSTIFY_RIGHT);
	bool page[SET_ROWS];
	for (int row = 0; row < SET_ROWS; row++)
		page[row] = true; // every row opens a page: no values to draw
	value_rows(SET_ROWS, s.row, settings_row_name, nullptr, nullptr, nullptr, nullptr, page, nullptr);
	const char * about = settings_row_about(&s);
	text(40, 384, about, fit(about, 19, 14, UI_W - 80), rgb(T.screen.muted));
	pause_key keys[6];
	int n = settings_legend(&s, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

// Settings > Picture: Screen mode and Shape.
const picture_ui * g_rows_picture; // for picture_greyed
bool picture_greyed(int row, int i) { return picture_off(g_rows_picture, row, i) != 0; }

void draw_picture(const setup_view & sv)
{
	const picture_ui & p = sv.picture;
	setup_header("PICTURE", sv.line[0] ? sv.line : nullptr);
	int in_use[PIC_ROWS];
	bool page[PIC_ROWS] = {};
	for (int row = 0; row < PIC_ROWS; row++)
		in_use[row] = picture_in_use(&p, row);
	g_rows_picture = &p;
	value_rows(PIC_ROWS, p.row, picture_row_name, picture_values, picture_value_name, p.pick, in_use, page, picture_greyed);
	row_about(PIC_ROWS, picture_row_about(&p));
	pause_key keys[6];
	int n = picture_legend(&p, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

// Settings > Theme: the colors and Our ROMs' layout.
void draw_look(const setup_view & sv)
{
	const look_ui & l = sv.look;
	setup_header("THEME", sv.line[0] ? sv.line : nullptr);
	int in_use[LOOK_ROWS];
	bool page[LOOK_ROWS] = {};
	for (int row = 0; row < LOOK_ROWS; row++)
		in_use[row] = look_in_use(&l, row);
	value_rows(LOOK_ROWS, l.row, look_row_name, look_values, look_value_name, l.pick, in_use, page, nullptr);
	row_about(LOOK_ROWS, look_row_about(&l)); // under the last row, as Settings' sits under its own
	pause_key keys[6];
	int n = look_legend(&l, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

// A drawing of each controller type for its card.
void pad_picture(float cx, float cy, int type, PixelColor c)
{
	switch (type) {
	case INPUT_GAMEPAD:
		box(cx - 40, cy - 24, 80, 48, c, 16);
		box(cx - 21, cy - 16, 42, 32, off_ink(), 3);
		break;
	case INPUT_PRO:
	case INPUT_CLASSIC:
		box(cx - 42, cy - 18, 84, 36, c, 18);
		box(cx - 30, cy - 6, 12, 12, off_ink(), 6);
		box(cx + 18, cy - 6, 12, 12, off_ink(), 6);
		break;
	case INPUT_NUNCHUK:
		box(cx - 30, cy - 30, 18, 60, c, 8);
		box(cx + 8, cy - 14, 28, 44, c, 14);
		break;
	default: // a Wii Remote held sideways
		box(cx - 40, cy - 10, 80, 20, c, 8);
		break;
	}
}

void draw_controllers(const setup_view & sv)
{
	const controllers_ui & c = sv.controllers;
	setup_header("OUR CONTROLLERS", sv.line[0] ? sv.line : nullptr);
	for (int i = 0; i < INPUT_PORTS; i++) {
		int r[4];
		controllers_card_rect(i, r);
		const setup_port & p = c.ports[i];
		const bool sel = c.card == i;
		if (sel && c.in_actions) // the player the actions act on stays marked: a light edge
			box(r[0] - 3, r[1] - 3, r[2] + 6, r[3] + 6, rgb(T.panel.fill), 12);
		const bool on = sel && !c.in_actions; // drawn selected
		selectable(r[0], r[1], r[2], r[3], on, false, 7, 10);
		char name[16];
		snprintf(name, sizeof(name), "Player %d", i + 1);
		text(r[0] + r[2] / 2, r[1] + 12, name, 22, card_ink(on, false), GUI_TEXT_JUSTIFY_CENTER);
		if (p.type < 0) {
			text(r[0] + r[2] / 2, r[1] + 66, "Press A on a", 18, card_muted(on), GUI_TEXT_JUSTIFY_CENTER);
			text(r[0] + r[2] / 2, r[1] + 88, "controller to join", 18, card_muted(on), GUI_TEXT_JUSTIFY_CENTER);
		} else {
			pad_picture(r[0] + r[2] / 2, r[1] + 66, p.type, p.gone ? card_muted(on) : card_ink(on, false));
			const char * tn = input_type_name(p.type);
			text(r[0] + r[2] / 2, r[1] + 98, p.gone ? "Disconnected" : tn, fit(tn, 18, 13, r[2] - 10),
			     p.gone ? card_accent(on) : card_ink(on, false), GUI_TEXT_JUSTIFY_CENTER);
			if (p.charging)
				text(r[0] + r[2] / 2, r[1] + 124, "Charging", 16, card_muted(on), GUI_TEXT_JUSTIFY_CENTER);
			else if (p.battery >= 0)
				for (int b = 0; b < 4; b++)
					box(r[0] + r[2] / 2 - 31 + b * 16, r[1] + 128, 12, 12, b < p.battery ? card_ink(on, false) : off_ink(), 2);
		}
		if (p.type >= 0) // an empty player has no rumble to switch (Zach, 2026-10-29)
			text(r[0] + r[2] / 2, r[1] + 148, c.rumble[i] ? "Rumble: On" : "Rumble: Off", 17,
			     c.rumble[i] ? card_accent(on) : card_muted(on), GUI_TEXT_JUSTIFY_CENTER);
	}
	// the system's actions, left to right (setup.h: Rumble only with rumble)
	for (int slot = 0; slot < controllers_action_count(); slot++) {
		const int a = controllers_action_id(slot);
		int r[4];
		controllers_action_rect(slot, r);
		const bool off = !controllers_action_enabled(&c, a);
		const bool sel = c.in_actions && c.action == a;
		selectable(r[0], r[1], r[2], r[3], sel, off, 5, 10);
		if (sel && off && refused_flash(FLASH_CONTROLLERS, c.refused))
			flash_edge(r[0], r[1], r[2], r[3], 5, 10);
		const char * name = controllers_action_name(&c, a);
		if (sel && off) // selected while it does nothing now: the button says why instead of its name
			button_words(r, controllers_action_reason(&c, a), 22, card_ink(sel, off));
		else
			text(r[0] + r[2] / 2, r[1] + 18, name, fit(name, 22, 15, r[2] - 12), card_ink(sel, off), GUI_TEXT_JUSTIFY_CENTER);
	}
	pause_key keys[6];
	int n = controllers_legend(&c, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

void draw_order(const setup_view & sv)
{
	const order_ui & o = sv.order;
	char sub[96];
	if (o.total > 1)
		snprintf(sub, sizeof(sub), "Press A on the controller for Player %d of %d", o.count + 1, o.total);
	else // one player: the controller that plays
		snprintf(sub, sizeof(sub), "Press A on the controller for Player %d", o.count + 1);
	setup_header("CHANGE ORDER", sub);
	for (int i = 0; i < INPUT_PORTS; i++) {
		int r[4];
		controllers_card_rect(i, r);
		char name[24];
		snprintf(name, sizeof(name), "Player %d", i + 1);
		if (i < o.count) {
			box(r[0], r[1], r[2], r[3], rgb(T.bar.fill), 10);
			const int type = sv.controllers.ports[0].type; // filled in per id below
			(void)type;
			text(r[0] + r[2] / 2, r[1] + 6, name, 22, rgb(T.bar.ink), GUI_TEXT_JUSTIFY_CENTER);
			text(r[0] + r[2] / 2, r[1] + 140, "Done", 18, rgb(T.bar.accent), GUI_TEXT_JUSTIFY_CENTER);
		} else { // still to come: an outline on the blue
			box(r[0], r[1], r[2], r[3], PixelColor{0, 0, 0, 0}, 10, 2, i == o.count ? rgb(T.panel.fill) : rgb(T.screen.muted));
			text(r[0] + r[2] / 2, r[1] + 6, name, 22, i == o.count ? rgb(T.screen.ink) : rgb(T.screen.muted), GUI_TEXT_JUSTIFY_CENTER);
			if (i == o.count && i < o.total)
				text(r[0] + r[2] / 2, r[1] + 80, "Next: press A", 20, rgb(T.screen.accent), GUI_TEXT_JUSTIFY_CENTER);
		}
	}
	// B: Cancel until someone is Player 1, then Done with only those (Zach, 2026-10-20)
	pause_key keys[2] = { { GLYPH_HOME, "Wii U Menu", 0 },
	                      { sv.glyphs == INPUT_WIIMOTE ? GLYPH_1 : GLYPH_B, o.count ? "Done" : "Cancel", 0 } };
	draw_keys(keys, 2, nullptr);
}

// Where each N64 input sits on the controller photo, in the photo's own
// pixels (960x765 before it was cut out at 25,8 and scaled by 320/934).
// Inputs a ring can't show get an arrow instead (r is 0), its tip at x, y
// travelling dir degrees (0 right, 90 down) (Zach, 2026-10-29): a curved one
// for Z under the controller (curl: which way it bends), straight ones down
// onto L and R, and straight ones out of the stick for its four directions.
struct PadSpot { float x, y, r; float dir; int curl; };
const PadSpot g_pad_spot[N64_INPUTS] = {
	{ 705, 315, 16 }, { 670, 253, 16 }, { 520, 510, 0, -120, -1 }, { 517, 226, 13 },
	{ 288, 132, 11 }, { 288, 200, 11 }, { 252, 168, 11 }, { 322, 165, 11 },
	{ 304, 84, 0, 90 }, { 836, 150, 0, 90 },
	{ 830, 220, 12 }, { 793, 290, 12 }, { 762, 236, 12 }, { 862, 272, 12 },
	{ 448, 215, 0, -90 }, { 448, 445, 0, 90 }, { 333, 330, 0, 180 }, { 563, 330, 0, 0 },
};

// The mark on the controller photo: bright green (Zach, 2026-10-29), on a dark edge.
static const PixelColor PAD_MARK = { 64, 230, 96, 255 }, PAD_MARK_EDGE = { 20, 40, 24, 255 };

// A curved arrow whose head ends at tip, travelling dir degrees there: a
// quarter-and-a-bit of a circle behind the head, bending clockwise (curl 1)
// or anticlockwise (-1) on the screen.
static void pad_arrow(GuiMesh & m, float tx, float ty, float dir, int curl, PixelColor color)
{
	const float pi = 3.14159265f, d = dir * pi / 180, head = 16, half = 10, rad = 30, sweep = 120;
	const GuiPoint b = { tx - head * cosf(d), ty - head * sinf(d) }; // where the arc meets the head
	// the arc's end angle, so its tangent there is dir
	const float e = d - curl * pi / 2;
	const GuiPoint c = { b.x - rad * cosf(e), b.y - rad * sinf(e) };
	const float edeg = e * 180 / pi, start = curl > 0 ? edeg - sweep : edeg;
	const GuiPoint n = { -sinf(d), cosf(d) };
	for (int pass = 0; pass < 2; pass++) {
		const PixelColor col = pass ? color : PAD_MARK_EDGE;
		const float w = pass ? 6 : 10, grow = pass ? 0 : 3;
		m.strokeArc(c, rad, start, sweep, w, col);
		std::vector<GuiPoint> tri = { { tx + grow * cosf(d), ty + grow * sinf(d) },
		                              { b.x + (half + grow) * n.x - grow * cosf(d), b.y + (half + grow) * n.y - grow * sinf(d) },
		                              { b.x - (half + grow) * n.x - grow * cosf(d), b.y - (half + grow) * n.y - grow * sinf(d) } };
		m.addPolygon(tri, GuiFill::solid(col));
	}
}

// A short straight arrow (a stub of a stem behind the head) ending at tip, pointing dir degrees.
static void pad_straight_arrow(GuiMesh & m, float tx, float ty, float dir, PixelColor color)
{
	const float d = dir * 3.14159265f / 180, len = 26, head = 16, half = 10;
	const float ux = cosf(d), uy = sinf(d), nx = -uy, ny = ux;
	const GuiPoint b = { tx - head * ux, ty - head * uy }, tail = { tx - len * ux, ty - len * uy };
	for (int pass = 0; pass < 2; pass++) {
		const PixelColor col = pass ? color : PAD_MARK_EDGE;
		const float w = pass ? 3 : 5, grow = pass ? 0 : 3;
		std::vector<GuiPoint> shaft = { { tail.x + w * nx - grow * ux, tail.y + w * ny - grow * uy },
		                                { b.x + w * nx, b.y + w * ny }, { b.x - w * nx, b.y - w * ny },
		                                { tail.x - w * nx - grow * ux, tail.y - w * ny - grow * uy } };
		m.addPolygon(shaft, GuiFill::solid(col));
		std::vector<GuiPoint> tri = { { tx + grow * ux, ty + grow * uy },
		                              { b.x + (half + grow) * nx - grow * ux, b.y + (half + grow) * ny - grow * uy },
		                              { b.x - (half + grow) * nx - grow * ux, b.y - (half + grow) * ny - grow * uy } };
		m.addPolygon(tri, GuiFill::solid(col));
	}
}

// One input's mark on the controller photo, in color (its edge stays dark).
static void pad_mark(GuiMesh & m, float x0, float y0, int input, PixelColor color)
{
	const float k = 320.0f / 934.0f;
	const PadSpot & s = g_pad_spot[input];
	const float x = x0 + (s.x - 25) * k, y = y0 + (s.y - 8) * k;
	if (s.r > 0) { // a dark edge under the color keeps it clear on the grey plastic
		const float rr = s.r * k * 2.2f + 4;
		m.strokeArc(GuiPoint{x, y}, rr, 0, 360, 7, PAD_MARK_EDGE);
		m.strokeArc(GuiPoint{x, y}, rr, 0, 360, 4, color);
	} else if (s.curl) {
		pad_arrow(m, x, y, s.dir, s.curl, color);
	} else {
		pad_straight_arrow(m, x, y, s.dir, color);
	}
}

// The controller photo with the selected row's N64 input marked in green and,
// blinking white for a moment, the input a capture just took a button from
// (Zach, 2026-10-29: the photo shows every input, the list only four).
// Inputs do not light while pressed (Zach, 2026-10-18: the light flickered).
const double PAD_BLINK_S = 3.2; // Zach, 2026-10-29: twice the first 1.6 s

// Seconds since the capture counted by seq took a button from another input.
static double pad_blink_age(unsigned seq)
{
	static unsigned seen_seq;
	static double blink_from = -100;
	const double now = now_us() / 1e6;
	if (seq != seen_seq) {
		seen_seq = seq;
		blink_from = now;
	}
	return now - blink_from;
}

void draw_pad(float x0, float y0, int selected, int other, unsigned other_seq)
{
	if (g_pad != nullptr)
		g_images->drawTexture(g_pad, x0, y0, 320, 253, 0, 1, 1, 255);
	GuiMesh m;
	if (selected >= 0 && selected < N64_INPUTS)
		pad_mark(m, x0, y0, selected, PAD_MARK);
	const double t = pad_blink_age(other_seq);
	if (other >= 0 && other < N64_INPUTS && other != selected && t < PAD_BLINK_S && int(t / 0.2) % 2 == 0)
		pad_mark(m, x0, y0, other, PixelColor{ 255, 255, 255, 255 });
	if (m.size())
		g_images->drawMesh(m.vertices().data(), m.size());
}

// s cut at the end with "..." to fit room at px (a long game name in a tab).
static std::string clipped(const char * s, int px, int room)
{
	std::string t = s;
	if (text_width(t.c_str(), px) <= room)
		return t;
	while (!t.empty() && (text_width((t + "...").c_str(), px) > room || t.back() == ' ' || t.back() == ':'))
		t.pop_back();
	return t + "...";
}

void draw_mapping(const setup_view & sv)
{
	const mapping_ui & m = sv.mapping;
	char head[64], sub[160];
	mapping_headline(&m, head, sizeof(head), sub, sizeof(sub));
	const float pad = 20;
	if (m.has_game) { // the tabs, L and R either side; the line goes right of them
		setup_header(head, nullptr);
		const PixelColor glyph = rgb(T.screen.muted);
		int r[4];
		mapping_tab_rect(MAP_TAB_GAME, r);
		const char * game = mapping_tab_name(&m, MAP_TAB_GAME);
		// both tabs at one size: the game's name's, clipped when too long even at 15
		const int inner = r[2] - 40; // 20 px of tab each side of the words
		const int px = text_width(game, 15) > inner ? 15 : fit(game, 18, 15, inner);
		mapping_tab_rect(MAP_TAB_ALL, r);
		const float gap = 10; // the same space between L and its tab as between the other tab and R
		key_glyph(r[0] - gap - text_width("\xee\x80\x84", 30), r[1] - 1, GLYPH_L, glyph);
		for (int t = 0; t < 2; t++) {
			mapping_tab_rect(t, r);
			const our_surface & s = m.tab == t ? T.panel : T.bar;
			box(r[0], r[1], r[2], r[3], rgb(s.fill), 14);
			text(r[0] + r[2] / 2, r[1] + (r[3] - px) / 2 - 1, clipped(mapping_tab_name(&m, t), px, r[2] - 40).c_str(), px,
			     rgb(s.ink), GUI_TEXT_JUSTIFY_CENTER);
		}
		key_glyph(r[0] + r[2] + gap, r[1] - 1, GLYPH_R, glyph);
		if (m.tab == MAP_TAB_GAME) { // what the tab means, plainly (Zach, 2026-10-29)
			int all[4];
			mapping_tab_rect(MAP_TAB_ALL, all);
			const std::string only = std::string("Only for ") + game;
			const int room = r[0] + r[2] - all[0];
			text(all[0] + 4, r[1] + r[3] + 4, clipped(only.c_str(), 17, room).c_str(), 17, rgb(T.screen.muted));
		}
	} else {
		setup_header(head, sub);
	}
	for (int i = 0; i < MAP_SHOWN && m.top + i < MAP_ROWS; i++) {
		const int row = m.top + i;
		int r[4];
		mapping_row_rect(i, r);
		const bool sel = m.row == row;
		selectable(r[0], r[1], r[2], r[3], sel, false, 4, 8);
		if (sel && refused_flash(FLASH_MAPPING, m.refused))
			flash_edge(r[0], r[1], r[2], r[3], 4, 8);
		char label[96], value[96];
		mapping_row_text(&m, row, label, sizeof(label), value, sizeof(value));
		// The label column fits "N64 C-Right" and "Dead zone".
		const int label_w = 150;
		const int ls = fit(label, 22, 15, label_w);
		text(r[0] + pad, r[1] + 14, label, ls, card_ink(sel, false));
		const bool none = row < N64_INPUTS && !m.edit.sources[row]; // no button: "None" in gold
		text(r[0] + r[2] - pad, r[1] + 14, value, fit(value, 22, 14, r[2] - 2 * pad - label_w - 12),
		     none ? card_accent(sel) : card_ink(sel, false), GUI_TEXT_JUSTIFY_RIGHT);
	}
	if (m.line[0] && m.other_row >= 0 && pad_blink_age(m.other_seq) < PAD_BLINK_S) { // what the blink means
		int tr[4];
		mapping_tab_rect(MAP_TAB_GAME, tr);
		const int left = tr[0] + tr[2] + 46;
		text(UI_W - 40, tr[1] + 3, m.line, fit(m.line, 18, 13, UI_W - 40 - left), rgb(T.screen.muted), GUI_TEXT_JUSTIFY_RIGHT);
	}
	draw_pad(500, 140, m.row < N64_INPUTS ? m.row : -1, m.other_row, m.other_seq);
	pause_key keys[6];
	int n = mapping_legend(&m, sv.glyphs, keys, 6);
	if (m.screen == MAP_CAPTURE) {
		box(0, 0, UI_W, UI_H, DIM);
		box(177, 130, 500, 220, rgb(T.screen.fill), 16, 3, rgb(T.panel.fill), 12);
		char q[96];
		snprintf(q, sizeof(q), m.capture_add ? "Press a button to add to %s" : "Press the button for %s",
		         input_n64_label(m.row));
		text(UI_W / 2, 150, q, fit(q, 28, 18, 460), rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
		double left = m.capture_until - now_us() / 1e6;
		char count[12];
		snprintf(count, sizeof(count), "%d", left > 0 ? int(left) + 1 : 0);
		GuiMesh ring;
		ring.strokeArc(GuiPoint{UI_W / 2.0f, 282}, 34, 0, 360, 6, rgb(T.screen.accent));
		g_images->drawMesh(ring.vertices().data(), ring.size());
		text(UI_W / 2, 262, count, 34, rgb(T.screen.accent), GUI_TEXT_JUSTIFY_CENTER);
	} else if (m.screen == MAP_FAILED) {
		box(0, 0, UI_W, UI_H, DIM);
		dialog_frame(DIALOG_SHORT, true);
		const our_surface & on = T.fail; // dialog_frame's
		text(UI_W / 2, DIALOG_SHORT + 20, m.failed_title, fit(m.failed_title, 32, 20, int(DIALOG_W) - 48), rgb(on.ink),
		     GUI_TEXT_JUSTIFY_CENTER);
		int button[4];
		pause_button_rect(0, button);
		const char * body[1] = { m.failed_body };
		dialog_body(body, 1, DIALOG_SHORT + 64, button[1] - 14, rgb(on.muted));
		dialog_buttons(0, true, "OK");
	}
	draw_keys(keys, n, nullptr);
}

// Settings > Games: the ROM folder, with the folder as words at the right in
// the room the row's name leaves, and Fetch covers.
void draw_games(const setup_view & sv)
{
	const games_ui & g = sv.games;
	setup_header("GAMES", sv.line[0] ? sv.line : nullptr);
	for (int row = 0; row < GAMES_ROWS; row++) {
		int r[4];
		settings_row_rect(row, r);
		const bool sel = g.row == row;
		selectable(r[0], r[1], r[2], r[3], sel, false, 5, 10);
		const char * name = games_row_name(row);
		text(r[0] + 20, r[1] + 12, name, 26, card_ink(sel, false));
		char value[sizeof(g.folder)];
		games_value(&g, row, value, sizeof(value));
		if (!value[0])
			continue;
		const int room = r[2] - 20 - text_width(name, 26) - 32 - 24;
		int px = 22;
		while (px > 15 && text_width(value, px) > room)
			px--;
		// one line: a long path loses its middle, so the drive and the last folder stay
		text(r[0] + r[2] - 24, r[1] + 15, cut_to_fit(value, px, room).c_str(), px, card_muted(sel), GUI_TEXT_JUSTIFY_RIGHT);
	}
	row_about(GAMES_ROWS, games_row_about(&g));
	pause_key keys[6];
	int n = games_legend(&g, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

// Settings > Games > Start on: All, Recent and Favorites as rows, each with a
// round mark at the right, filled on the one in use (one choice of three, as
// a radio button shows it); A picks the selected one.
void draw_start(const setup_view & sv)
{
	const start_ui & st = sv.start;
	setup_header("START-UP ROM CATEGORY", sv.line[0] ? sv.line : nullptr);
	for (int row = 0; row < STARTS; row++) {
		int r[4];
		settings_row_rect(row, r);
		const bool sel = st.row == row;
		selectable(r[0], r[1], r[2], r[3], sel, false, 5, 10);
		text(r[0] + 20, r[1] + 12, start_name(row), 26, card_ink(sel, false));
		const PixelColor ink = card_ink(sel, false);
		const float cx = r[0] + r[2] - 44, cy = r[1] + r[3] / 2.0f;
		box(cx - 14, cy - 14, 28, 28, PixelColor{ 0, 0, 0, 0 }, 14, 3, ink);
		if (row == st.in_use)
			box(cx - 8, cy - 8, 16, 16, ink, 8);
	}
	pause_key keys[6];
	int n = start_legend(&st, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

// Settings > Games > Fetch covers: the loading screen's layout, with the
// title, one bar, "Reading your games: 80 of 200" then "12 of 40" under it,
// and HOME or B to stop.
void draw_fetch(const setup_view & sv)
{
	const games_ui & g = sv.games;
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	text(UI_W / 2, 218, "Fetching covers", 32, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
	char step[64];
	games_fetch_step(&g, step, sizeof(step));
	progress_bar(games_fetch_progress(&g), step);
	pause_key keys[4];
	draw_keys(keys, games_legend(&g, sv.glyphs, keys, 4), nullptr);
}

// Auto-Resume turned off: every game's Auto-Saves deleted, on a screen laid
// out like Fetch covers, so a long delete never looks like a freeze (NH 1).
// Nothing to press: it ends by itself.
void draw_deleting(const setup_view & sv)
{
	box(0, 0, UI_W, UI_H, rgb(T.screen.fill));
	text(UI_W / 2, 218, "Deleting Auto-Saves", 32, rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
	char step[64];
	snprintf(step, sizeof(step), "%d of %d", sv.deleting_done, sv.deleting_total);
	progress_bar(sv.deleting_total > 0 ? float(sv.deleting_done) / sv.deleting_total : 0.0f, step);
	draw_keys(nullptr, 0, nullptr);
}

// Settings > Games > ROM folder: the drives, then a drive's folders, with
// "Use this folder" first; arrows in the margin when rows are out of view.
void draw_browse(const setup_view & sv)
{
	const browse_ui & b = sv.browse;
	setup_header("ROM FOLDER", sv.line[0] ? sv.line : browse_title(&b));
	const int rows = browse_rows(&b);
	for (int i = 0; i < BROWSE_SHOWN && b.top + i < rows; i++) {
		const int row = b.top + i;
		int r[4];
		browse_row_rect(i, r);
		const bool sel = b.row == row;
		selectable(r[0], r[1], r[2], r[3], sel, false, 4, 8);
		char value[32];
		browse_row_value(&b, row, value, sizeof(value));
		const int vw = value[0] ? text_width(value, 20) + 24 : 0;
		const char * name = browse_row_name(&b, row);
		const int room = r[2] - 40 - vw;
		const int px = fit(name, 22, 16, room);
		// a drive without a partition Our64 reads is shown greyed: A does nothing on it
		const PixelColor ink = browse_row_off(&b, row) ? rgb(theme_off(&surface_of(sel, false))) : card_ink(sel, false);
		text(r[0] + 20, r[1] + 8, clipped(name, px, room).c_str(), px, ink);
		if (value[0])
			text(r[0] + r[2] - 24, r[1] + 9, value, 20, card_muted(sel), GUI_TEXT_JUSTIFY_RIGHT);
	}
	int r[4];
	if (b.top > 0) {
		browse_row_rect(0, r);
		text(UI_W - 20, r[1] + 10, "▲", 18, rgb(T.screen.muted), GUI_TEXT_JUSTIFY_CENTER);
	}
	if (b.top + BROWSE_SHOWN < rows) {
		browse_row_rect(BROWSE_SHOWN - 1, r);
		text(UI_W - 20, r[1] + 10, "▼", 18, rgb(T.screen.muted), GUI_TEXT_JUSTIFY_CENTER);
	}
	pause_key keys[6];
	int n = browse_legend(&b, sv.glyphs, keys, 6);
	draw_keys(keys, n, nullptr);
}

void draw_setup(const ui_view & v)
{
	const setup_view & sv = *v.setup;
	switch (sv.screen) {
	case SETUP_SETTINGS: draw_settings(sv); break;
	case SETUP_GAMES: draw_games(sv); break;
	case SETUP_BROWSE: draw_browse(sv); break;
	case SETUP_FETCH: draw_fetch(sv); break;
	case SETUP_START: draw_start(sv); break;
	case SETUP_DELETING: draw_deleting(sv); break;
	case SETUP_THEME: draw_look(sv); break;
	case SETUP_PICTURE: draw_picture(sv); break;
	case SETUP_CONTROLLERS: draw_controllers(sv); break;
	case SETUP_ORDER: draw_order(sv); break;
	default: draw_mapping(sv); break;
	}
	draw_notices(true);
}

// HOME outside a game: the screen stays in view, dimmed, under the question.
void draw_exit_ask(const ui_view & v)
{
	box(0, 0, UI_W, UI_H, DIM);
	dialog_frame(DIALOG_SHORT);
	// "Exit to the Wii U Menu?", or the question the caller gave (Delete, 2026-10-25)
	const char * q = v.ask_title[0] ? v.ask_title : "Exit to the Wii U Menu?";
	text(UI_W / 2, DIALOG_SHORT + 20, q, fit(q, 32, 22, int(DIALOG_W) - 48), rgb(T.screen.ink), GUI_TEXT_JUSTIFY_CENTER);
	int button[4];
	pause_button_rect(0, button);
	const char * body[1] = { v.ask_title[0] ? v.ask_body : theme_comrade() ? "Goodbye, Comrade." : "Goodbye." };
	dialog_body(body, 1, DIALOG_SHORT + 64, button[1] - 14, rgb(T.screen.ink));
	dialog_buttons(v.exit_focus, false, v.ask_title[0] ? v.ask_action : "Exit");
	pause_key keys[4];
	draw_keys(keys, exit_ask_legend(v.exit_glyphs, keys, 4), "");
}

void draw_screen(const ui_view & v)
{
	switch (v.screen) {
	case UI_LOADING: draw_loading(v); break;
	case UI_PROMPT: draw_prompt(v); break;
	case UI_PAUSED: draw_paused(v); break;
	case UI_CLOSING: draw_closing(v); break;
	case UI_PAUSE: draw_pause(v); break;
	case UI_ROMS: draw_roms(v); break;
	case UI_SETUP: draw_setup(v); break;
	default: draw_message(v); break;
	}
	if (v.exit_ask)
		draw_exit_ask(v);
}

} // namespace our64ui
