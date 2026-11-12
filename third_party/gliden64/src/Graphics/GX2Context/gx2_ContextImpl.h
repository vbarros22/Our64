#pragma once
// Our64 GX2 backend for the Wii U (switched on with OUR64_GX2_GFX). See
// docs/feasibility.md, stage 3, in the Our64 repository.
#include <gx2/surface.h>
#include <Graphics/ContextImpl.h>

graphics::ContextImpl * our64_createGX2Context();

namespace gx2 {

// The "window": an offscreen color buffer the size of the display window.
GX2ColorBuffer * screenColorBuffer();

// The screen turned the right way up for the TV (rows top first).
GX2ColorBuffer * presentColorBuffer();

// Settings > Picture > TV look / GamePad look = CRT (or Soft CRT, _soft), for
// the TV (_slot 0) or the GamePad (_slot 1), each with its own frame: _src (a finished frame, rows top first)
// drawn at _width x _height with scanlines and a light aperture grille. Null
// without a context or memory; GLideN64's state is marked to be set again.
GX2ColorBuffer * crtColorBuffer(GX2ColorBuffer * _src, u32 _width, u32 _height, int _soft, int _slot);

// Ends a frame: waits for the GPU and recycles per-frame memory.
void endFrame();

// Puts GLideN64's GPU state back after the app presented a frame between frames.
void restoreState();

// Copies the screen into _dest as RGB rows, bottom row first (as glReadPixels).
void readScreen(u8 * _dest, u32 _width, u32 _height);

}

// Set by the app to show each finished frame (e.g. copy it to the TV).
extern void (*our64_gx2_present)(GX2ColorBuffer * _screen);
// Our64: called before each saved shader is built while a game loads (the
// loading screen's "Preparing graphics 12/17"). On the graphics core.
extern void (*our64_gx2_shader_progress)(unsigned _done, unsigned _total);
extern const char * our64_gx2_debug; // debug variants, see gx2_ContextImpl.cpp
