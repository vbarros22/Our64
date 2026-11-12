// Our64 GX2 backend: GLideN64's graphics layer on the Wii U GPU.
//
// Parameters arrive as the OpenGL enum values (NullContext/null_Parameters.cpp
// defines them), and the shader programs come from GLideN64's GLSL
// generators through gx2_GL.cpp. State is shadowed here and applied before
// each draw. Conventions (docs/feasibility.md, stage 3):
//
// - Vertex shaders negate gl_Position.y, so every surface holds rows in
//   OpenGL's order: row 0 is the bottom. Viewport and scissor rectangles then
//   pass through unchanged, and CPU uploads and readback need no flips.
// - Textures are tiled and can also be render targets. Uploads go through a
//   linear staging surface in the per-frame ring and GX2CopySurfaceEx.
// - 16-bit uploads are widened to RGBA8 on the CPU for now.
// - Per-frame memory (vertices, indices, staging) comes from one ring; the
//   frame end waits for the GPU before reusing it.
// - Depth textures: GX2 draws depth only into depth buffers, and samples
//   only textures. A depth texture attached to a framebuffer gets a depth
//   buffer of its own; its texture surface is refreshed from it
//   (GX2ConvertDepthBufferToTextureSurface) when sampled after drawing.
// - Readback copies the texture into a linear surface and waits for the GPU.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/displaylist.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>

#include <Graphics/OpenGLContext/GLFunctions.h>
#include <Graphics/OpenGLContext/opengl_CachedFunctions.h>
#include <Graphics/OpenGLContext/opengl_GLInfo.h>
#include <Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderAccurate.h>
#include <Graphics/OpenGLContext/GLSL/glsl_CombinerProgramBuilderFast.h>
#include <Graphics/OpenGLContext/GLSL/glsl_SpecialShadersFactory.h>
#include <Graphics/ColorBufferReader.h>
#include <Graphics/FramebufferTextureFormats.h>
#include <Graphics/Parameters.h>
#include <gDP.h>
#include <GBI.h>
#include <RSP.h>
#include <Combiner.h>
#include <Config.h>
#include <DisplayWindow.h>
#include <gSP.h>

#include "gx2_ContextImpl.h"
#include "gx2_Internal.h"

// In gx2.rpl, not declared by wut.
extern "C" void GX2ConvertDepthBufferToTextureSurface(const GX2DepthBuffer * _src, GX2Surface * _dst, u32 _mip, u32 _slice);

void (*our64_gx2_present)(GX2ColorBuffer * _screen) = nullptr;
void (*our64_gx2_shader_progress)(unsigned _done, unsigned _total) = nullptr;
// Time spent waiting for the GPU at frame ends (microseconds), for the player's log.
extern "C" volatile double our64_gx2_frame_wait_us = 0;
// In wut's import library but not its headers: the GPU writes its clock to
// the address once all earlier work is done.
extern "C" void GX2SampleBottomGPUCycle(uint64_t * _sample);
// ... and once the command processor reaches it (top of the pipe): when the
// commands after it reached the GPU, if it was idle waiting for them.
extern "C" void GX2SampleTopGPUCycle(uint64_t * _sample);
extern "C" uint64_t GX2GPUTimeToCPUTime(uint64_t _gpuTime); // gx2.rpl: GPU counter to OSTime (tv_pass.cpp)

// Debug variants, set by the app before a run (empty or null: none). Words:
// "skip=<otherMode.l hex>" drops draws with that RDP mode word; "nodepth",
// "noblend" and "nocull" force that GPU state off; "nosampleflush" skips the
// cache flush before sampling a texture drawn to since; "latez", "killoff" and
// "ztestalways" change the depth unit (see beginDraw and applyState); "uniforms" logs each
// rectangle's vertices and pixel shader uniforms, and with "srclog=<otherMode.l
// hex>" the fragment shader of each program drawing rectangles in that mode;
// "blitlog" logs each blit's buffers and "oldblit" blits between the bound
// buffers instead of the named ones (see blitFramebuffers).
const char * our64_gx2_debug = nullptr;
const char * our64_gx2_log_path = nullptr;

namespace gx2 {

void log(const char * _format, ...)
{
	if (our64_gx2_log_path == nullptr)
		return;
	FILE * f = fopen(our64_gx2_log_path, "a");
	if (f == nullptr)
		return;
	va_list args;
	va_start(args, _format);
	vfprintf(f, _format, args);
	va_end(args);
	fclose(f);
}

namespace {

using namespace graphics;

// Debug variant "vcframe" (and "abvc", which turns it on and off every 20 s
// for the GPU profile): vertices written by the CPU are flushed from its cache
// per draw, and the GPU's vertex cache is invalidated once per frame for the
// whole ring (and when the ring wraps), instead of a GPU invalidate per draw.
// Stunt Racer's ~460 draws a frame took 20-100 us of GPU time each
// (gpuprof, 2026-10-28).
bool g_vcFrame = false;

inline void attribWritten(void * _p, u32 _size)
{
	if (g_vcFrame)
		DCFlushRange(_p, _size);
	else
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, _p, _size);
}

// Per-frame memory for vertices, indices and upload staging.
class Ring {
public:
	bool init(u32 _size) {
		m_base = static_cast<u8*>(MEMAllocFromDefaultHeapEx(_size, 0x1000));
		m_size = _size;
		m_used = 0;
		return m_base != nullptr;
	}
	// Waits for the GPU when the ring is full, then starts over.
	void * alloc(u32 _size, u32 _align) {
		if (_size > m_size)
			gx2::log("CHECK: ring request of %u bytes is larger than the ring (%u)\n", _size, m_size);
		u32 start = (m_used + _align - 1) & ~(_align - 1);
		if (start + _size > m_size) {
			GX2DrawDone();
			start = 0;
			if (g_vcFrame)
				invalidateVertexCache();
		}
		m_used = start + _size;
		return m_base + start;
	}
	// alloc for memory the CPU fills whole (align 32 or more): its lines are
	// claimed in the cache without reading them (dcbz). Each store to a line
	// not in the cache first read the line from memory, and the vertex copy
	// was a quarter of the graphics core in Destruction Derby's races
	// (2026-10-20). The rest of the last line is not anyone's yet.
	void * allocFilled(u32 _size, u32 _align) {
		u8 * p = static_cast<u8*>(alloc(_size, _align));
		for (u32 o = 0; o < _size; o += 32)
			__asm__ volatile("dcbz 0,%0" : : "r"(p + o) : "memory");
		return p;
	}
	void reset() { m_used = 0; }
	void invalidateVertexCache() { GX2Invalidate(GX2_INVALIDATE_MODE_ATTRIBUTE_BUFFER, m_base, m_size); }
private:
	u8 * m_base = nullptr;
	u32 m_size = 0;
	u32 m_used = 0;
};

struct Texture {
	GX2Texture tex = {};
	GX2Sampler sampler = {};
	GX2ColorBuffer cb = {};
	bool allocated = false;
	bool cbReady = false;
	u32 internalFormat = 0;
	bool depth = false;        // a depth texture (GL_DEPTH_COMPONENT*)
	GX2DepthBuffer db = {};    // where a depth texture is drawn, made when first attached
	bool dbAllocated = false;
	bool dbDirty = false;      // drawn since its texture surface was last refreshed
	bool cbDirty = false;      // drawn as a color buffer since its caches were last flushed
};

struct Renderbuffer {
	GX2DepthBuffer db = {};
	bool allocated = false;
};

struct Framebuffer {
	u32 color = 0;
	u32 depth = 0;   // renderbuffer or depth texture name
	bool depthTexture = false;
};

// PA_CL_CLIP_CNTL with only ZCLIP_NEAR_DISABLE set: the far plane clips, the
// near plane does not. GX2SetRasterizerClipControlEx sets both bits from one
// flag; this is the register as it writes it (decaf's GX2: rasterizer on,
// OpenGL clip space) with the near bit alone. A PM4 SET_CONTEXT_REG packet,
// padded with type-2 NOPs to 32 bytes for GX2CallDisplayList.
alignas(32) u32 g_nearClipOff[8] = {0xC0016900u, 0x204u, 1u << 26, 0x80000000u, 0x80000000u, 0x80000000u,
	0x80000000u, 0x80000000u};
bool g_nearClipOffFlushed = false;

GX2SurfaceFormat surfaceFormat(u32 _internalFormat)
{
	switch (_internalFormat) {
	case GL_R8: return GX2_SURFACE_FORMAT_UNORM_R8;
	case GL_R16F: return GX2_SURFACE_FORMAT_FLOAT_R16;
	case GL_R32F: return GX2_SURFACE_FORMAT_FLOAT_R32;
	case GL_RG32F: return GX2_SURFACE_FORMAT_FLOAT_R32_G32;
	case GL_R32UI: return GX2_SURFACE_FORMAT_UINT_R32;
	case GL_DEPTH_COMPONENT:
	case GL_DEPTH_COMPONENT24: return GX2_SURFACE_FORMAT_FLOAT_R32;  // as the depth buffers
	default: return GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;  // RGBA8, RGB8, RGBA4, RGB5_A1
	}
}

// Where a texture's levels 1 and up start in its block, and the block's size.
u32 mipOffset(const GX2Surface & _s)
{
	return (_s.imageSize + _s.alignment - 1) / _s.alignment * _s.alignment;
}

u32 textureBytes(const GX2Surface & _s)
{
	return _s.mipLevels > 1 ? mipOffset(_s) + _s.mipmapSize : _s.imageSize;
}

u32 surfaceBytes(GX2SurfaceFormat _format)
{
	switch (_format) {
	case GX2_SURFACE_FORMAT_UNORM_R8: return 1;
	case GX2_SURFACE_FORMAT_FLOAT_R16: return 2;
	case GX2_SURFACE_FORMAT_FLOAT_R32_G32: return 8;
	default: return 4;
	}
}

u32 sourceBytes(u32 _format, u32 _type)
{
	switch (_type) {
	case GL_UNSIGNED_SHORT_4_4_4_4:
	case GL_UNSIGNED_SHORT_5_5_5_1:
	case GL_UNSIGNED_SHORT_5_6_5:
	case GL_HALF_FLOAT:
		return 2;
	case GL_FLOAT:
	case GL_UNSIGNED_INT:
		return _format == GL_RG ? 8 : 4;
	default:
		if (_format == GL_RGBA) return 4;
		if (_format == GL_RGB) return 3;
		if (_format == GL_RG) return 2;
		return 1;
	}
}

// Float to half float, rounded to nearest; tiny values become half denormals.
u16 halfFloat(f32 _v)
{
	u32 b;
	memcpy(&b, &_v, 4);
	const u32 sign = (b >> 16) & 0x8000;
	const s32 exp = s32((b >> 23) & 0xFF) - 127 + 15;
	u32 mant = b & 0x7FFFFF;
	if (((b >> 23) & 0xFF) == 0xFF)
		return u16(sign | 0x7C00 | (mant != 0 ? 0x200 : 0));
	if (exp >= 31)
		return u16(sign | 0x7C00);
	if (exp <= 0) {
		if (exp < -10)
			return u16(sign);
		mant |= 0x800000;
		const u32 shift = u32(14 - exp);
		const u32 h = mant >> shift, rest = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
		return u16(sign | (h + (rest > half || (rest == half && (h & 1)) ? 1 : 0)));
	}
	const u32 h = (u32(exp) << 10) | (mant >> 13), rest = mant & 0x1FFF;
	return u16(sign | (h + (rest > 0x1000 || (rest == 0x1000 && (h & 1)) ? 1 : 0)));
}

inline u8 expand4(u32 _v) { return u8(_v * 17); }
inline u8 expand5(u32 _v) { return u8((_v << 3) | (_v >> 2)); }
inline u8 expand6(u32 _v) { return u8((_v << 2) | (_v >> 4)); }

// Converts one row of GL-format texels to the surface format. GLideN64
// builds RGBA8 texels as host u32 values 0xAABBGGRR and 16-bit ones as host
// u16 values, so they are read as numbers, not bytes.
void convertRow(u8 * _dst, const u8 * _src, u32 _width, u32 _format, u32 _type, GX2SurfaceFormat _surface)
{
	// The depth textures of RE2 and Quarterback Club 98 come as floats for an
	// R16F texture. Copied as they were, each float filled two texels: the
	// characters were drawn in stripes through the background (Zach, 2026-10-26).
	// The GPU reads the half floats byte-swapped: unswapped, the background's
	// depth hid RE2's characters, in Cemu and in the console's GX2 replay
	// (results/gx2-re2-v897 d1 and d2, 2026-10-26; uploaded FLOAT_R32 and
	// UINT_R32 textures read back byte-swapped in Cemu too). Debug variant
	// "r16noswap" leaves them unswapped.
	if (_surface == GX2_SURFACE_FORMAT_FLOAT_R16 && _type == GL_FLOAT) {
		const bool swap = our64_gx2_debug == nullptr || strstr(our64_gx2_debug, "r16noswap") == nullptr;
		for (u32 x = 0; x < _width; ++x) {
			const u16 h = halfFloat(reinterpret_cast<const f32*>(_src)[x]);
			reinterpret_cast<u16*>(_dst)[x] = swap ? __builtin_bswap16(h) : h;
		}
		return;
	}
	// RE2's N64 depth table: 16-bit values from u32s, high byte to red, low to green.
	if (_surface == GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 && _type == GL_UNSIGNED_INT && _format == GL_RED) {
		for (u32 x = 0; x < _width; ++x) {
			const u32 v = reinterpret_cast<const u32*>(_src)[x];
			u8 * d = _dst + x * 4;
			d[0] = u8(v >> 8); d[1] = u8(v); d[2] = 0; d[3] = 255;
		}
		return;
	}
	if (_surface != GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8) {
		memcpy(_dst, _src, _width * surfaceBytes(_surface));
		return;
	}
	for (u32 x = 0; x < _width; ++x) {
		u8 * d = _dst + x * 4;
		switch (_type) {
		case GL_UNSIGNED_SHORT_4_4_4_4: {
			const u32 v = reinterpret_cast<const u16*>(_src)[x];
			d[0] = expand4(v >> 12); d[1] = expand4((v >> 8) & 15); d[2] = expand4((v >> 4) & 15); d[3] = expand4(v & 15);
			break;
		}
		case GL_UNSIGNED_SHORT_5_5_5_1: {
			const u32 v = reinterpret_cast<const u16*>(_src)[x];
			d[0] = expand5(v >> 11); d[1] = expand5((v >> 6) & 31); d[2] = expand5((v >> 1) & 31); d[3] = (v & 1) ? 255 : 0;
			break;
		}
		case GL_UNSIGNED_SHORT_5_6_5: {
			const u32 v = reinterpret_cast<const u16*>(_src)[x];
			d[0] = expand5(v >> 11); d[1] = expand6((v >> 5) & 63); d[2] = expand5(v & 31); d[3] = 255;
			break;
		}
		default:
			if (_format == GL_RGB) {
				d[0] = _src[x * 3]; d[1] = _src[x * 3 + 1]; d[2] = _src[x * 3 + 2]; d[3] = 255;
			} else {
				const u32 v = reinterpret_cast<const u32*>(_src)[x];
				d[0] = u8(v); d[1] = u8(v >> 8); d[2] = u8(v >> 16); d[3] = u8(v >> 24);
			}
		}
	}
}

GX2BlendMode blendMode(u32 _gl)
{
	switch (_gl) {
	case GL_ZERO: return GX2_BLEND_MODE_ZERO;
	case GL_ONE: return GX2_BLEND_MODE_ONE;
	case GL_SRC_COLOR: return GX2_BLEND_MODE_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return GX2_BLEND_MODE_INV_SRC_COLOR;
	case GL_SRC_ALPHA: return GX2_BLEND_MODE_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return GX2_BLEND_MODE_INV_SRC_ALPHA;
	case GL_DST_ALPHA: return GX2_BLEND_MODE_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return GX2_BLEND_MODE_INV_DST_ALPHA;
	case GL_DST_COLOR: return GX2_BLEND_MODE_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return GX2_BLEND_MODE_INV_DST_COLOR;
	case GL_CONSTANT_COLOR: return GX2_BLEND_MODE_BLEND_FACTOR;
	case GL_ONE_MINUS_CONSTANT_COLOR: return GX2_BLEND_MODE_INV_BLEND_FACTOR;
	case GL_CONSTANT_ALPHA: return GX2_BLEND_MODE_CONSTANT_ALPHA;
	case GL_ONE_MINUS_CONSTANT_ALPHA: return GX2_BLEND_MODE_INV_CONSTANT_ALPHA;
	case GL_SRC1_COLOR: return GX2_BLEND_MODE_SRC1_COLOR;
	case GL_ONE_MINUS_SRC1_COLOR: return GX2_BLEND_MODE_INV_SRC1_COLOR;
	case GL_SRC1_ALPHA: return GX2_BLEND_MODE_SRC1_ALPHA;
	case GL_ONE_MINUS_SRC1_ALPHA: return GX2_BLEND_MODE_INV_SRC1_ALPHA;
	default: return GX2_BLEND_MODE_ONE;
	}
}

GX2CompareFunction compareFunction(u32 _gl)
{
	switch (_gl) {
	case GL_NEVER: return GX2_COMPARE_FUNC_NEVER;
	case GL_LESS: return GX2_COMPARE_FUNC_LESS;
	case GL_EQUAL: return GX2_COMPARE_FUNC_EQUAL;
	case GL_LEQUAL: return GX2_COMPARE_FUNC_LEQUAL;
	case GL_GREATER: return GX2_COMPARE_FUNC_GREATER;
	case GL_NOTEQUAL: return GX2_COMPARE_FUNC_NOT_EQUAL;
	case GL_GEQUAL: return GX2_COMPARE_FUNC_GEQUAL;
	default: return GX2_COMPARE_FUNC_ALWAYS;
	}
}

GX2PrimitiveMode primitiveMode(u32 _gl)
{
	switch (_gl) {
	case GL_TRIANGLE_STRIP: return GX2_PRIMITIVE_MODE_TRIANGLE_STRIP;
	case GL_TRIANGLE_FAN: return GX2_PRIMITIVE_MODE_TRIANGLE_FAN;
	case GL_LINES: return GX2_PRIMITIVE_MODE_LINES;
	default: return GX2_PRIMITIVE_MODE_TRIANGLES;
	}
}

GX2TexClampMode clampMode(u32 _gl)
{
	switch (_gl) {
	case GL_CLAMP_TO_EDGE: return GX2_TEX_CLAMP_MODE_CLAMP;
	case GL_MIRRORED_REPEAT: return GX2_TEX_CLAMP_MODE_MIRROR;
	default: return GX2_TEX_CLAMP_MODE_WRAP;
	}
}

GX2TexXYFilterMode filterMode(u32 _gl)
{
	return (_gl == GL_LINEAR || _gl == GL_LINEAR_MIPMAP_NEAREST || _gl == GL_LINEAR_MIPMAP_LINEAR)
		? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT;
}

// Formats for frame buffer textures, as the OpenGL backend picks for GL 3.3.
struct GX2Formats : FramebufferTextureFormats {
	GX2Formats() {
		colorInternalFormat = InternalColorFormatParam(GL_RGBA8); colorFormat = ColorFormatParam(GL_RGBA);
		colorType = DatatypeParam(GL_UNSIGNED_BYTE); colorFormatBytes = 4;
		monochromeInternalFormat = InternalColorFormatParam(GL_R8); monochromeFormat = ColorFormatParam(GL_RED);
		monochromeType = DatatypeParam(GL_UNSIGNED_BYTE); monochromeFormatBytes = 1;
		depthInternalFormat = InternalColorFormatParam(GL_DEPTH_COMPONENT24); depthFormat = ColorFormatParam(GL_DEPTH_COMPONENT);
		depthType = DatatypeParam(GL_FLOAT); depthFormatBytes = 4;
		depthImageInternalFormat = InternalColorFormatParam(GL_R32F); depthImageFormat = ColorFormatParam(GL_RED);
		depthImageType = DatatypeParam(GL_FLOAT); depthImageFormatBytes = 4;
		// The N64 depth table (RE2) goes in an RGBA8 texture, high byte in red,
		// low byte in green (see convertRow and zlutAsRGBA8).
		lutInternalFormat = InternalColorFormatParam(GL_RGBA8); lutFormat = ColorFormatParam(GL_RED);
		lutType = DatatypeParam(GL_UNSIGNED_INT); lutFormatBytes = 4;
		fontInternalFormat = InternalColorFormatParam(GL_R8); fontFormat = ColorFormatParam(GL_RED);
		fontType = DatatypeParam(GL_UNSIGNED_BYTE); fontFormatBytes = 1;
	}
};

// The pixel read buffer (depth copy from video memory, off in Our64) is
// not implemented: it returns black, as the null backend does.
class GX2PixelReadBuffer : public PixelReadBuffer {
public:
	explicit GX2PixelReadBuffer(size_t _size) : m_data(_size) {}
	void readPixels(s32, s32, u32 _w, u32 _h, Parameter, Parameter) override {
		static unsigned n;
		if (n++ < 4) gx2::log("CHECK: pixel read buffer used (%ux%u), a stub\n", _w, _h);
	}
	void * getDataRange(u32 _offset, u32 _range) override {
		if (_offset + _range > m_data.size()) m_data.resize(_offset + _range);
		return m_data.data() + _offset;
	}
	void closeReadBuffer() override {}
	void bind() override {}
	void unbind() override {}
private:
	std::vector<u8> m_data;
};

// The last preload's counts (loadShadersStorage), for the player's shader
// builder (sd:/wiiu/our64/build-shaders): keys, combiners made, pairs from
// the game's pack and from single files, keys not in the shader cache.
struct Our64Preload { unsigned keys = 0, loaded = 0, fromPack = 0, fromFiles = 0, missing = 0, ms = 0; };
extern "C" Our64Preload our64_gx2_preload;
Our64Preload our64_gx2_preload;

class GX2ContextImpl;
GX2ContextImpl * g_context = nullptr;

// What every context shares, made by the first (see GX2ContextImpl::init).
struct KeptState {
	GX2ContextState * ctx = nullptr;
	Ring rings[2];
	Texture screen;
	GX2DepthBuffer screenDepth = {};
	Texture dummy;
	Texture present;
	Texture crt;                 // the TV look's frame (crtBuffer), at the TV's size
	GX2DepthBuffer crtDepth = {};
	GX2Sampler crtSampler = {};
};
KeptState g_kept;

// A frame buffer texture copied by the GPU into linear memory.
struct ReadbackCopy {
	GX2Surface linear = {};
	OSTime stamp = 0;     // GPU time stamp of the copy
	bool valid = false;
};

// Reads a frame buffer texture (the N64-sized copy GLideN64 makes for the
// color copy to RDRAM) back to the CPU. 16-bit frame buffers are converted
// to N64 pixels on the GPU, so the CPU only copies them. In GLideN64's
// asynchronous mode (EnableCopyColorToRDRAM=2) a read returns the previous
// frame's copy, so it does not wait for the GPU to finish this frame. Each
// kind of read (16-bit packed or not) keeps its own two copies: when the
// 16-bit read has no earlier copy yet, GLideN64 falls back to a plain read
// of the same buffer, and with shared copies that fallback overwrote the
// other copy in the other format, so every later read failed (Mario Golf's
// camera-transition panel, cut from the previous frame, stayed black).
class GX2ColorBufferReader : public ColorBufferReader {
public:
	explicit GX2ColorBufferReader(CachedTexture * _pTexture) : ColorBufferReader(_pTexture) {}
	~GX2ColorBufferReader() override;
	void cleanUp() override {}
	const u16 * readPixels16(s32 _x0, s32 _y0, u32 _width, u32 _height, bool _sync) override;
	bool copyPixels16ToMemory(void * _dst, u32 _width, u32 _height) override;
private:
	const u8 * _readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride,
	                       size_t & _gpuDataSize) override;
	const ReadbackCopy * read(bool _pack16, bool _sync);
	ReadbackCopy m_copies[2][2];  // [packed 16-bit][slot]
	u32 m_cur[2] = {0, 0};
	std::vector<u16> m_rows16;
};

class GX2ContextImpl : public ContextImpl
{
public:
	GX2ContextImpl() { g_context = this; }
	~GX2ContextImpl() override {
		stopCompileThread();
		// Once the GPU is done: what this context still holds, the frame
		// buffers GLideN64 deleted at the end (they wait in m_pendingFree)
		// and any it did not. The kept state (g_kept) stays for the next.
		GX2DrawDone();
		for (auto & pending : m_pendingFree) {
			for (void * p : pending)
				MEMFreeToDefaultHeap(p);
			pending.clear();
		}
		for (auto & t : m_textures) {
			if (t.second.tex.surface.image != nullptr)
				MEMFreeToDefaultHeap(t.second.tex.surface.image);
			if (t.second.dbAllocated)
				MEMFreeToDefaultHeap(t.second.db.surface.image);
		}
		m_textures.clear();
		for (Texture * t : {&m_depthScratch, &m_packTarget, &m_layoutTarget})
			if (t->allocated)
				MEMFreeToDefaultHeap(t->tex.surface.image);
		if (g_context == this)
			g_context = nullptr;
	}

	void init() override {
		initCompiler();
		// What the GLSL generators see: GL 3.3 core (which has dual-source
		// blending) without extensions.
		m_glInfo.majorVersion = 3;
		m_glInfo.minorVersion = 3;
		m_glInfo.dual_source_blending = true;
		// Shaders compile on a worker thread (gx2_GL.cpp), so GLideN64 can draw
		// on while a new combiner compiles (config AsyncShaderCompilation).
		m_glInfo.parallelShaderCompile = true;
		m_glInfo.drawElementsBaseVertex = true;
		m_glInfo.depthTexture = true;
		config.frameBufferEmulation.N64DepthCompare = Config::dcDisable;
		config.video.multisampling = 0;
		config.video.fxaa = 0;

		// GLideN64 makes a new context at each RomOpen, and nothing freed the
		// last one's: 33 MB a game start, until the shader builder's 21st open
		// found no memory (2026-10-28). The GPU state, rings, screen and dummy
		// texture are made once and kept for the app's life.
		if (m_ctx == nullptr && g_kept.ctx != nullptr) {
			m_ctx = g_kept.ctx;
			m_rings[0] = g_kept.rings[0];
			m_rings[1] = g_kept.rings[1];
			m_screen = g_kept.screen;
			m_screenDepth = g_kept.screenDepth;
			m_dummy = g_kept.dummy;
			m_present = g_kept.present;
			m_crt = g_kept.crt;
			m_crtDepth = g_kept.crtDepth;
			m_crtSampler = g_kept.crtSampler;
		}
		if (m_ctx == nullptr) {
			m_ctx = static_cast<GX2ContextState*>(MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT));
			GX2SetupContextStateEx(m_ctx, TRUE);
			m_rings[0].init(16 * 1024 * 1024);
			m_rings[1].init(16 * 1024 * 1024);
			createScreen(dwnd().getScreenWidth(), dwnd().getScreenHeight());
			createDummyTexture();
			g_kept.ctx = m_ctx;
			g_kept.rings[0] = m_rings[0];
			g_kept.rings[1] = m_rings[1];
			g_kept.screen = m_screen;
			g_kept.screenDepth = m_screenDepth;
			g_kept.dummy = m_dummy;
		}
		GX2SetContextState(m_ctx);
		blendFuncHook = [](u32 _s, u32 _d) {
			if (g_context != nullptr) g_context->setBlending(BlendParam(_s), BlendParam(_d));
		};
		m_dirty = true;
		resetCombinerProgramBuilder();
	}

	void destroy() override {
		m_combinerProgramBuilder.reset();
		m_specialShadersFactory.reset();
	}

	void setClampMode(ClampMode _mode) override { m_clampMode = _mode; m_dirty = true; }
	ClampMode getClampMode() override { return m_clampMode; }

	void enable(EnableParam _parameter, bool _enable) override {
		const u32 p = u32(_parameter);
		bool * flag = nullptr;
		switch (p) {
		case GL_BLEND: flag = &m_blend; break;
		case GL_CULL_FACE: flag = &m_cull; break;
		case GL_DEPTH_TEST: flag = &m_depthTest; break;
		case GL_SCISSOR_TEST: flag = &m_scissorTest; break;
		case GL_POLYGON_OFFSET_FILL: flag = &m_polygonOffset; break;
		case GL_DEPTH_CLAMP: flag = &m_depthClamp; break;
		default: return;
		}
		if (*flag != _enable) {
			*flag = _enable;
			m_dirty = true;
		}
	}
	u32 isEnabled(EnableParam _parameter) override {
		switch (u32(_parameter)) {
		case GL_BLEND: return m_blend;
		case GL_CULL_FACE: return m_cull;
		case GL_DEPTH_TEST: return m_depthTest;
		case GL_SCISSOR_TEST: return m_scissorTest;
		case GL_POLYGON_OFFSET_FILL: return m_polygonOffset;
		case GL_DEPTH_CLAMP: return m_depthClamp;
		default: return 0;
		}
	}
	void cullFace(CullModeParam _mode) override { m_cullMode = u32(_mode); m_dirty = true; }
	void enableDepthWrite(bool _enable) override { m_depthWrite = _enable; m_dirty = true; }
	void setDepthCompare(CompareParam _mode) override { m_depthFunc = u32(_mode); m_dirty = true; }
	void setViewport(s32 _x, s32 _y, s32 _width, s32 _height) override {
		m_viewport[0] = _x; m_viewport[1] = _y; m_viewport[2] = _width; m_viewport[3] = _height;
		m_dirty = true;
	}
	void setScissor(s32 _x, s32 _y, s32 _width, s32 _height) override {
		m_scissor[0] = _x; m_scissor[1] = _y; m_scissor[2] = _width; m_scissor[3] = _height;
		m_dirty = true;
	}
	void setBlending(BlendParam _sfactor, BlendParam _dfactor) override {
		m_blendFactors[0] = m_blendFactors[2] = u32(_sfactor);
		m_blendFactors[1] = m_blendFactors[3] = u32(_dfactor);
		m_dirty = true;
	}
	void setBlendingSeparate(BlendParam _sc, BlendParam _dc, BlendParam _sa, BlendParam _da) override {
		m_blendFactors[0] = u32(_sc); m_blendFactors[1] = u32(_dc);
		m_blendFactors[2] = u32(_sa); m_blendFactors[3] = u32(_da);
		m_dirty = true;
	}
	void setBlendColor(f32 _red, f32 _green, f32 _blue, f32 _alpha) override {
		m_blendColor[0] = _red; m_blendColor[1] = _green; m_blendColor[2] = _blue; m_blendColor[3] = _alpha;
		m_dirty = true;
	}
	void clearColorBuffer(f32 _red, f32 _green, f32 _blue, f32 _alpha) override {
		GX2ColorBuffer * cb = drawColorBuffer();
		if (cb == nullptr)
			return;
		if (cb->surface.image == nullptr || cb->surface.width == 0 || cb->surface.height == 0) {
			gx2::log("CHECK: clear of a color buffer without memory (%ux%u)\n", cb->surface.width, cb->surface.height);
			return;
		}
		traceBegin("clear color", 0);
		GX2ClearColor(cb, _red, _green, _blue, _alpha);
		traceEnd();
		restoreAfterClear();
	}
	void clearDepthBuffer() override {
		GX2DepthBuffer * db = drawDepthBuffer();
		if (db == nullptr)
			return;
		traceBegin("clear depth", 0);
		GX2ClearDepthStencilEx(db, 1.0f, 0, GX2_CLEAR_FLAGS_DEPTH);
		traceEnd();
		if (Texture * t = drawDepthTexture())
			t->dbDirty = true;
		restoreAfterClear();
	}
	void setPolygonOffset(f32 _factor, f32 _units) override {
		m_offsetFactor = _factor;
		m_offsetUnits = _units;
		m_dirty = true;
	}

	/*---------------Texture-------------*/
	ObjectHandle createTexture(Parameter) override {
		const u32 name = m_nextName++;
		Texture & t = m_textures[name];
		GX2InitSampler(&t.sampler, GX2_TEX_CLAMP_MODE_WRAP, GX2_TEX_XY_FILTER_MODE_POINT);
		return ObjectHandle(name);
	}
	void deleteTexture(ObjectHandle _name) override {
		auto it = m_textures.find(u32(_name));
		if (it == m_textures.end())
			return;
		for (u32 & unit : m_units)
			if (unit == u32(_name))
				unit = 0;
		freeLater(it->second.tex.surface.image);
		if (it->second.dbAllocated)
			freeLater(it->second.db.surface.image);
		m_textures.erase(it);
	}
	// Mip levels: the simple texture coordinates (EnableInaccurateTextureCoordinates)
	// load an N64 mip chain (tile 1 and the tiles after it) as levels of one
	// texture, and the shaders read them with textureLod and textureSize(tex, lod).
	// With level 0 only, the console read a flat grey from the missing levels:
	// Perfect Dark's distant security cameras lost their texture (console GX2
	// replay of capture v9950, results/gx2-pd-cameras, 2026-10-30: texture 1
	// read 160,160,160 over the whole camera). Cemu reads level 0 there instead,
	// so it never showed this.
	void init2DTexture(const Context::InitTextureParams & _p) override {
		Texture * t = texture(_p.handle);
		if (t == nullptr)
			return;
		bindUnit(_p.textureUnitIndex, _p.handle);
		GX2Surface & s = t->tex.surface;
		if (_p.mipMapLevel != 0) {
			const u32 level = _p.mipMapLevel;
			if (!t->allocated || level >= s.mipLevels || _p.width != std::max(s.width >> level, 1u) ||
					_p.height != std::max(s.height >> level, 1u)) {
				if (!m_loggedMip)
					gx2::log("CHECK: mip level %u of %ux%u does not fit a %ux%u texture of %u levels\n",
						level, _p.width, _p.height, s.width, s.height, s.mipLevels);
				m_loggedMip = true;
				return;
			}
			if (_p.data != nullptr)
				upload(*t, 0, 0, _p.width, _p.height, u32(_p.format), u32(_p.dataType), _p.data, level);
			return;
		}
		const GX2SurfaceFormat format = surfaceFormat(u32(_p.internalFormat));
		const u32 mips = std::min<u32>(std::max<u32>(_p.mipMapLevels, 1), 13);
		if (!t->allocated || s.width != _p.width || s.height != _p.height || s.format != format || s.mipLevels != mips) {
			if (t->allocated)
				freeLater(s.image);
			// Not cleared when the data below fills it at once: zeroing new
			// textures was 8-24% of Mario Tennis's graphics core (2026-10-21).
			allocateTexture(*t, _p.width, _p.height, format, _p.data == nullptr, mips);
		}
		t->internalFormat = u32(_p.internalFormat);
		t->depth = u32(_p.internalFormat) == GL_DEPTH_COMPONENT || u32(_p.internalFormat) == GL_DEPTH_COMPONENT24;
		if (t->dbAllocated && (t->db.surface.width != t->tex.surface.width || t->db.surface.height != t->tex.surface.height)) {
			freeLater(t->db.surface.image);
			t->dbAllocated = false;
		}
		if (_p.data != nullptr)
			upload(*t, 0, 0, _p.width, _p.height, u32(_p.format), u32(_p.dataType), _p.data);
	}
	void update2DTexture(const Context::UpdateTextureDataParams & _p) override {
		if (_p.mipMapLevel != 0)
			return;
		Texture * t = texture(_p.handle);
		if (t == nullptr || !t->allocated || _p.data == nullptr)
			return;
		bindUnit(_p.textureUnitIndex, _p.handle);
		upload(*t, _p.x, _p.y, _p.width, _p.height, u32(_p.format), u32(_p.dataType), _p.data);
	}
	void setTextureParameters(const Context::TexParameters & _p) override {
		Texture * t = texture(_p.handle);
		if (t == nullptr)
			return;
		bindUnit(_p.textureUnitIndex, _p.handle);
		GX2TexXYFilterMode mag = GX2_TEX_XY_FILTER_MODE_POINT, min = GX2_TEX_XY_FILTER_MODE_POINT;
		GX2TexClampMode wrapS = GX2_TEX_CLAMP_MODE_WRAP, wrapT = GX2_TEX_CLAMP_MODE_WRAP;
		if (_p.magFilter.isValid()) mag = filterMode(u32(_p.magFilter));
		if (_p.minFilter.isValid()) min = filterMode(u32(_p.minFilter));
		if (_p.wrapS.isValid()) wrapS = clampMode(u32(_p.wrapS));
		if (_p.wrapT.isValid()) wrapT = clampMode(u32(_p.wrapT));
		GX2InitSamplerXYFilter(&t->sampler, mag, min, GX2_TEX_ANISO_RATIO_NONE);
		GX2InitSamplerClamping(&t->sampler, wrapS, wrapT, GX2_TEX_CLAMP_MODE_CLAMP);
		if (_p.minFilter.isValid()) {
			const u32 gl = u32(_p.minFilter);
			const GX2TexMipFilterMode mip = gl == GL_NEAREST_MIPMAP_NEAREST || gl == GL_LINEAR_MIPMAP_NEAREST ?
				GX2_TEX_MIP_FILTER_MODE_POINT : gl == GL_NEAREST_MIPMAP_LINEAR || gl == GL_LINEAR_MIPMAP_LINEAR ?
				GX2_TEX_MIP_FILTER_MODE_LINEAR : GX2_TEX_MIP_FILTER_MODE_NONE;
			GX2InitSamplerZMFilter(&t->sampler, GX2_TEX_Z_FILTER_MODE_POINT, mip);
		}
		if (_p.maxMipmapLevel.isValid())
			GX2InitSamplerLOD(&t->sampler, 0.0f, f32(s32(_p.maxMipmapLevel)), 0.0f);
	}
	void bindTexture(const Context::BindTextureParameters & _p) override {
		bindUnit(_p.textureUnitIndex, _p.texture);
	}
	void setTextureUnpackAlignment(s32 _param) override { m_unpack = _param > 0 ? _param : 1; }
	s32 getTextureUnpackAlignment() const override { return m_unpack; }
	s32 getMaxTextureSize() const override { return 8192; }
	f32 getMaxAnisotropy() const override { return 0.0f; }
	void bindImageTexture(const Context::BindImageTextureParameters &) override {}
	u32 convertInternalTextureFormat(u32 _format) const override { return _format; }
	// GX2 can sample the render target it draws into once the color buffer
	// cache is flushed. Without this GLideN64 copies the framebuffer with a
	// blit when a game samples it, and that copy is stale for later draws
	// (Pokemon Stadium 2's menu portals, also wrong on the PC without a barrier).
	void textureBarrier() override { finishRenderTarget(); }

	/*---------------Framebuffer-------------*/
	FramebufferTextureFormats * getFramebufferTextureFormats() override { return new GX2Formats; }
	ObjectHandle createFramebuffer() override {
		const u32 name = m_nextName++;
		m_framebuffers[name] = Framebuffer();
		return ObjectHandle(name);
	}
	void deleteFramebuffer(ObjectHandle _name) override {
		if (m_drawFbo == u32(_name)) {
			m_drawFbo = 0;
			m_targetDirty = true;
		}
		if (m_readFbo == u32(_name))
			m_readFbo = 0;
		m_framebuffers.erase(u32(_name));
	}
	void bindFramebuffer(BufferTargetParam _target, ObjectHandle _name) override {
		const u32 target = u32(_target);
		if (target == GL_READ_FRAMEBUFFER || target == GL_FRAMEBUFFER)
			m_readFbo = u32(_name);
		if ((target == GL_DRAW_FRAMEBUFFER || target == GL_FRAMEBUFFER) && m_drawFbo != u32(_name)) {
			finishRenderTarget();
			m_drawFbo = u32(_name);
			m_targetDirty = true;
		}
	}
	void addFrameBufferRenderTarget(const Context::FrameBufferRenderTarget & _p) override {
		auto it = m_framebuffers.find(u32(_p.bufferHandle));
		if (it == m_framebuffers.end())
			return;
		const u32 attachment = u32(_p.attachment);
		if (attachment == GL_COLOR_ATTACHMENT0) {
			it->second.color = u32(_p.textureHandle);
		} else if (attachment == GL_DEPTH_ATTACHMENT) {
			it->second.depth = u32(_p.textureHandle);
			it->second.depthTexture = u32(_p.textureTarget) != GL_RENDERBUFFER;
		}
		if (u32(_p.bufferHandle) == m_drawFbo)
			m_targetDirty = true;
	}
	ObjectHandle createRenderbuffer() override {
		const u32 name = m_nextName++;
		m_renderbuffers[name] = Renderbuffer();
		return ObjectHandle(name);
	}
	void initRenderbuffer(const Context::InitRenderbufferParams & _p) override {
		auto it = m_renderbuffers.find(u32(_p.handle));
		if (it == m_renderbuffers.end())
			return;
		Renderbuffer & rb = it->second;
		if (rb.allocated)
			freeLater(rb.db.surface.image);
		allocateDepth(rb.db, _p.width, _p.height);
		rb.allocated = true;
		m_targetDirty = true;
	}
	// Same-size copies only: GX2CopySurfaceEx for color, a depth buffer to
	// texture conversion for depth. GLideN64 draws a quad when this fails.
	// The buffers are the ones named in _p, bound first as the OpenGL backend
	// does: GLideN64's sub-texture copy (FrameBuffer::_getSubTexture) binds
	// nothing itself, and with whatever was bound Mario Tennis's shadow mask
	// copied the screen onto itself and stayed empty, so each shadow drew as
	// its whole dark square (2026-10-22).
	bool blitFramebuffers(const Context::BlitFramebuffersParams & _p) override {
		const s32 w = _p.srcX1 - _p.srcX0, h = _p.srcY1 - _p.srcY0;
		if (w != _p.dstX1 - _p.dstX0 || h != _p.dstY1 - _p.dstY0 || w <= 0 || h <= 0) {
			logOnce(m_loggedScaledBlit, "CHECK: scaled or flipped blit, not supported (drawn instead where GLideN64 can)\n");
			return false;
		}
		const char * dbg = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
		if (!strstr(dbg, "oldblit")) {
			bindFramebuffer(BufferTargetParam(GL_READ_FRAMEBUFFER), _p.readBuffer);
			bindFramebuffer(BufferTargetParam(GL_DRAW_FRAMEBUFFER), _p.drawBuffer);
		}
		const u32 mask = u32(_p.mask);
		if (strstr(dbg, "blitlog")) {
			const GX2ColorBuffer * s = colorBufferOfFbo(m_readFbo), * d = colorBufferOfFbo(m_drawFbo);
			gx2::log("blit mask %x fbo %u (asked %u) %ux%u fmt%u -> fbo %u (asked %u) %ux%u fmt%u: %d,%d %dx%d to %d,%d\n",
				mask, m_readFbo, u32(_p.readBuffer), s ? s->surface.width : 0, s ? s->surface.height : 0,
				s ? u32(s->surface.format) : 0, m_drawFbo, u32(_p.drawBuffer), d ? d->surface.width : 0,
				d ? d->surface.height : 0, d ? u32(d->surface.format) : 0, _p.srcX0, _p.srcY0, w, h, _p.dstX0, _p.dstY0);
		}
		bool ok = true;
		finishRenderTarget();
		if (mask & GL_COLOR_BUFFER_BIT)
			ok = blitColor(_p, w, h) && ok;
		if (mask & GL_DEPTH_BUFFER_BIT)
			ok = blitDepth(_p, w, h) && ok;
		restoreAfterClear();
		return ok;
	}
	void setDrawBuffers(u32) override {}
	PixelReadBuffer * createPixelReadBuffer(size_t _sizeInBytes) override { return new GX2PixelReadBuffer(_sizeInBytes); }
	ColorBufferReader * createColorBufferReader(CachedTexture * _pTexture) override { return new GX2ColorBufferReader(_pTexture); }

	/*---------------Shaders-------------*/
	bool isCombinerProgramBuilderObsolete() override {
		if (!m_combinerProgramBuilder)
			return true;
		return m_builderInaccurate != config.generalEmulation.enableInaccurateTextureCoordinates;
	}
	void resetCombinerProgramBuilder() override {
		if (!isCombinerProgramBuilderObsolete())
			return;
		m_builderInaccurate = config.generalEmulation.enableInaccurateTextureCoordinates;
		if (m_builderInaccurate)
			m_combinerProgramBuilder = std::make_unique<glsl::CombinerProgramBuilderFast>(m_glInfo, &m_useProgram);
		else
			m_combinerProgramBuilder = std::make_unique<glsl::CombinerProgramBuilderAccurate>(m_glInfo, &m_useProgram);
		m_specialShadersFactory.reset(new glsl::SpecialShadersFactory(m_glInfo, &m_useProgram,
			m_combinerProgramBuilder->getVertexShaderHeader(),
			m_combinerProgramBuilder->getFragmentShaderHeader(),
			m_combinerProgramBuilder->getFragmentShaderEnd()));
	}
	CombinerProgram * createCombinerProgram(Combiner & _color, Combiner & _alpha, const CombinerKey & _key) override {
		return m_combinerProgramBuilder->buildCombinerProgram(_color, _alpha, _key);
	}
	void createCombinerProgramAsync(Combiner & _color, Combiner & _alpha, const CombinerKey & _key) override {
		m_combinerProgramBuilder->beginCombinerProgram(_color, _alpha, _key);
	}
	void getCompiledCombinerPrograms(Combiners & _compiled, bool _wait) override {
		if (m_combinerProgramBuilder)
			m_combinerProgramBuilder->getCompiledCombinerPrograms(_compiled, _wait);
	}
	void dropPendingCombinerPrograms() override {
		if (m_combinerProgramBuilder)
			m_combinerProgramBuilder->dropPendingCombinerPrograms();
	}
	// GLideN64's shader storage, Our64 style: only the combiner keys are kept
	// (sd:/wiiu/our64/shaders/<game>.keys), the shaders are in the shader cache.
	bool saveShadersStorage(const Combiners & _combiners) override {
		std::vector<u64> keys;
		for (const auto & c : _combiners)
			keys.push_back(c.first.getMux());
		saveCombinerKeys(RSP.romname, keys);
		return true;
	}

	// Before the game starts: every combiner the game used before is built
	// from the shader cache. Those missing from it (new GLideN64 sources, a
	// cleared cache) compile in the background, after anything a draw waits for.
	bool loadShadersStorage(Combiners & _combiners) override {
		const std::vector<u64> keys = loadCombinerKeys(RSP.romname);
		our64_gx2_preload = Our64Preload();
		our64_gx2_preload.keys = unsigned(keys.size());
		if (keys.empty())
			return false;
		const OSTime start = OSGetSystemTime();
		const u64 readStart = our64_gx2_cache_read_ticks;
		const bool hwl = GBI.isHWLSupported();
		std::vector<u64> missing;
		shaderPackOpen(RSP.romname);
		setLinkMode(LinkMode::CacheOnly);
		unsigned done = 0;
		for (u64 mux : keys) {
			if (our64_gx2_shader_progress != nullptr)
				our64_gx2_shader_progress(done++, unsigned(keys.size()));
			const CombinerKey key(mux, false);
			GBI.setHWLSupported(key.isHWLSupported());
			const u32 deferred = deferredLinks();
			CombinerProgram * program = Combiner_Compile(key);
			if (program == nullptr)
				continue;
			if (deferredLinks() != deferred) {
				delete program;
				missing.push_back(mux);
				continue;
			}
			program->update(true);
			_combiners[program->getKey()] = program;
		}
		unsigned fromPack = 0, fromFiles = 0;
		const u64 readTicks = our64_gx2_cache_read_ticks - readStart;
		const OSTime packWrite = OSGetSystemTime();
		shaderPackClose(&fromPack, &fromFiles);
		const unsigned packWriteMs = unsigned(OSTicksToMilliseconds(OSGetSystemTime() - packWrite));
		const bool background = config.video.asyncShaderCompilation != 0;
		setLinkMode(LinkMode::Background);
		for (u64 mux : background ? missing : std::vector<u64>()) {
			GBI.setHWLSupported(CombinerKey(mux, false).isHWLSupported());
			CombinerInfo::get().compileInBackground(mux);
		}
		setLinkMode(LinkMode::Normal);
		GBI.setHWLSupported(hwl);
		our64_gx2_preload.loaded = unsigned(_combiners.size());
		our64_gx2_preload.fromPack = fromPack;
		our64_gx2_preload.fromFiles = fromFiles;
		our64_gx2_preload.missing = unsigned(missing.size());
		our64_gx2_preload.ms = unsigned(OSTicksToMilliseconds(OSGetSystemTime() - start));
		gx2::log("%s: %u combiners loaded in %u ms (%u ms opening and reading cache files; shaders: %u from the game's pack, "
			"%u from single files; %u ms writing the pack), %u not in the shader cache%s\n",
			RSP.romname, unsigned(_combiners.size()), unsigned(OSTicksToMilliseconds(OSGetSystemTime() - start)),
			unsigned(OSTicksToMilliseconds(readTicks)), fromPack, fromFiles, packWriteMs,
			unsigned(missing.size()), background ? " (compiling in the background)" : "");
		return !_combiners.empty();
	}
	ShaderProgram * createDepthFogShader() override { return m_specialShadersFactory->createShadowMapShader(); }
	TexrectDrawerShaderProgram * createTexrectDrawerDrawShader() override { return m_specialShadersFactory->createTexrectDrawerDrawShader(); }
	ShaderProgram * createTexrectDrawerClearShader() override { return m_specialShadersFactory->createTexrectDrawerClearShader(); }
	ShaderProgram * createTexrectUpscaleCopyShader() override { return m_specialShadersFactory->createTexrectUpscaleCopyShader(); }
	ShaderProgram * createTexrectColorAndDepthUpscaleCopyShader() override { return m_specialShadersFactory->createTexrectColorAndDepthUpscaleCopyShader(); }
	ShaderProgram * createTexrectDownscaleCopyShader() override { return m_specialShadersFactory->createTexrectDownscaleCopyShader(); }
	ShaderProgram * createTexrectColorAndDepthDownscaleCopyShader() override { return m_specialShadersFactory->createTexrectColorAndDepthDownscaleCopyShader(); }
	ShaderProgram * createGammaCorrectionShader() override { return m_specialShadersFactory->createGammaCorrectionShader(); }
	ShaderProgram * createFXAAShader() override { return m_specialShadersFactory->createFXAAShader(); }
	TextDrawerShaderProgram * createTextDrawerShader() override { return m_specialShadersFactory->createTextDrawerShader(); }
	void resetShaderProgram() override { m_useProgram.useProgram(ObjectHandle::null); }

	/*---------------Draw-------------*/
	struct Debug { u32 skip = 0; bool skipSet = false, noDepth = false, noBlend = false, noCull = false, trace = false, uniforms = false, noSampleFlush = false,
		lateZ = false, killOff = false, zTestAlways = false, gpuProf = false; };
	Debug m_debug;
	const char * m_debugParsed = nullptr;
	const Debug & debug() {
		if (our64_gx2_debug != m_debugParsed) {
			m_debugParsed = our64_gx2_debug;
			m_debug = Debug();
			const char * d = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
			if (const char * k = strstr(d, "skip=")) { m_debug.skip = u32(strtoul(k + 5, nullptr, 16)); m_debug.skipSet = true; }
			m_debug.noDepth = strstr(d, "nodepth") != nullptr;
			m_debug.noBlend = strstr(d, "noblend") != nullptr;
			m_debug.noCull = strstr(d, "nocull") != nullptr;
			m_debug.trace = strstr(d, "trace") != nullptr;
			m_debug.uniforms = strstr(d, "uniforms") != nullptr;
			m_debug.noSampleFlush = strstr(d, "nosampleflush") != nullptr;
			m_debug.lateZ = strstr(d, "latez") != nullptr;
			m_debug.killOff = strstr(d, "killoff") != nullptr;
			m_debug.zTestAlways = strstr(d, "ztestalways") != nullptr;
			m_debug.gpuProf = strstr(d, "gpuprof") != nullptr;
			m_traceIndex = 0;
			m_dirty = true;
		}
		return m_debug;
	}
	bool skipDraw() { return debug().skipSet && gDP.otherMode.l == m_debug.skip; }

	// "trace": log each GPU operation before it runs, then wait for the GPU
	// and log "ok", so the operation that hangs the GPU is the last line.
	u32 m_traceIndex = 0;
	bool tracing() { return debug().trace; }
	void traceBegin(const char * _what, u32 _count) {
		m_profWhat = _what;
		profTop();
		if (!tracing()) return;
		const GX2ColorBuffer * cb = drawColorBuffer();
		const Texture * t0 = texture(ObjectHandle(m_units[0]));
		const Texture * t1 = texture(ObjectHandle(m_units[1]));
		gx2::log("trace %u: %s n=%u l=%08x h=%08x mux=%08x:%08x fb=%u cb=%ux%u fmt%u depth=%d test=%d tex0=%ux%u fmt%u tex1=%ux%u fmt%u "
			"vp=%d,%d,%d,%d prog=%u\n", m_traceIndex, _what, _count, gDP.otherMode.l, gDP.otherMode.h,
			gDP.combine.muxs0, gDP.combine.muxs1, m_drawFbo, cb ? cb->surface.width : 0, cb ? cb->surface.height : 0,
			cb ? u32(cb->surface.format) : 0, drawDepthBuffer() != nullptr, m_depthTest,
			t0 && t0->allocated ? t0->tex.surface.width : 0, t0 && t0->allocated ? t0->tex.surface.height : 0,
			t0 && t0->allocated ? u32(t0->tex.surface.format) : 0,
			t1 && t1->allocated ? t1->tex.surface.width : 0, t1 && t1->allocated ? t1->tex.surface.height : 0,
			t1 && t1->allocated ? u32(t1->tex.surface.format) : 0,
			m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3],
			gx2::currentProgram() != nullptr ? gx2::currentProgram()->name : 0);
	}
	void traceEnd() {
		profMark(m_profWhat);
		m_profWhat = "untraced";
		if (!tracing()) return;
		GX2Flush();
		GX2DrawDone();
		gx2::log("trace %u: ok\n", m_traceIndex++);
	}

	void drawTriangles(const Context::DrawTriangleParameters & _p) override {
		if (skipDraw())
			return;
		Program * program = beginDraw(false);
		if (program == nullptr || _p.verticesCount == 0)
			return;
		const u32 count = _p.verticesCount;
		TriangleVertex * v = static_cast<TriangleVertex*>(ring().allocFilled(count * sizeof(TriangleVertex), 64));
		// The OpenGL backend sends the first vertex's light count as a constant.
		const f32 numLights = f32(_p.vertices[0].HWLight);
		for (u32 i = 0; i < count; ++i) {
			const SPVertex & sp = _p.vertices[i];
			TriangleVertex & d = v[i];
			d.x = sp.x; d.y = sp.y; d.z = sp.z; d.w = sp.w;
			if (_p.flatColors) {
				d.r = sp.flat_r; d.g = sp.flat_g; d.b = sp.flat_b; d.a = sp.flat_a;
			} else {
				d.r = sp.r; d.g = sp.g; d.b = sp.b; d.a = sp.a;
			}
			d.s = sp.s; d.t = sp.t;
			d.numLights = numLights;
			d.modify[0] = u8(sp.modify); d.modify[1] = u8(sp.modify >> 8);
			d.modify[2] = u8(sp.modify >> 16); d.modify[3] = u8(sp.modify >> 24);
		}
		attribWritten(v, count * sizeof(TriangleVertex));
		GX2SetAttribBuffer(0, count * sizeof(TriangleVertex), sizeof(TriangleVertex), v);
		if (debug().uniforms)
			logTriangleUniforms(*program, _p);
		const GX2PrimitiveMode mode = primitiveMode(u32(_p.mode));
		if (_p.elements != nullptr && _p.elementsCount != 0) {
			// Always u16, whatever elementsType says (as in the OpenGL backend).
			u16 * indices = static_cast<u16*>(ring().allocFilled(_p.elementsCount * sizeof(u16), 32));
			memcpy(indices, _p.elements, _p.elementsCount * sizeof(u16));
			attribWritten(indices, _p.elementsCount * sizeof(u16));
			traceBegin("tris indexed", _p.elementsCount);
			GX2DrawIndexedEx(mode, _p.elementsCount, GX2_INDEX_TYPE_U16, indices, 0, 1);
		} else {
			traceBegin("tris", count);
			GX2DrawEx(mode, count, 0, 1);
		}
		traceEnd();
	}
	void drawRects(const Context::DrawRectParameters & _p) override {
		if (skipDraw())
			return;
		Program * program = beginDraw(true);
		if (program == nullptr || _p.verticesCount == 0)
			return;
		const u32 count = _p.verticesCount;
		RectVertex * v = static_cast<RectVertex*>(ring().allocFilled(count * sizeof(RectVertex), 64));
		// A fill rectangle (texrect false) has no texture coordinates of its
		// own: GLideN64 leaves the last textured rectangle's in the vertices.
		// OpenGL turns the texcoord attributes off for it, so it samples at
		// 0,0; so does this. Taken as they were, Pokemon Stadium 2's panels
		// drew stretched slices of its digit strip over their borders
		// (2026-10-25).
		for (u32 i = 0; i < count; ++i) {
			const ::RectVertex & r = _p.vertices[i];
			v[i] = _p.texrect ? RectVertex{r.x, r.y, r.z, r.w, r.s0, r.t0, r.s1, r.t1}
			                  : RectVertex{r.x, r.y, r.z, r.w, 0.0f, 0.0f, 0.0f, 0.0f};
		}
		attribWritten(v, count * sizeof(RectVertex));
		GX2SetAttribBuffer(0, count * sizeof(RectVertex), sizeof(RectVertex), v);
		if (debug().uniforms)
			logRectUniforms(*program, v, count);
		// The profile tells fill from textured rectangles, and those over at least
		// half the scissor (a frame buffer copied or cleared) from small ones.
		float x0 = _p.vertices[0].x, x1 = x0, y0 = _p.vertices[0].y, y1 = y0;
		for (u32 i = 1; i < count; ++i) {
			x0 = std::min(x0, _p.vertices[i].x); x1 = std::max(x1, _p.vertices[i].x);
			y0 = std::min(y0, _p.vertices[i].y); y1 = std::max(y1, _p.vertices[i].y);
		}
		// N64 pixels, as the scissor
		const float scissor = (gDP.scissor.lrx - gDP.scissor.ulx) * (gDP.scissor.lry - gDP.scissor.uly);
		const bool big = scissor > 0.0f && (x1 - x0) * (y1 - y0) >= 0.5f * scissor;
		traceBegin(_p.texrect ? (big ? "rect textured, half the target or more" : "rect textured, small")
		                      : (big ? "rect fill, half the target or more" : "rect fill, small"), count);
		GX2DrawEx(primitiveMode(u32(_p.mode)), count, 0, 1);
		traceEnd();
	}
	void logRectUniforms(const Program & _p, const RectVertex * _v, u32 _count) {
		const RectVertex & a = _v[0], & b = _v[_count - 1];
		const Texture * t0 = texture(ObjectHandle(m_units[0]));
		gx2::log("rect %u: l=%08x h=%08x prog=%u (%g,%g st %g,%g)-(%g,%g st %g,%g) tex0=%ux%u\n", m_rectIndex++,
			gDP.otherMode.l, gDP.otherMode.h, _p.name, a.x, a.y, a.s0, a.t0, b.x, b.y, b.s0, b.t0,
			t0 && t0->allocated ? t0->tex.surface.width : 0, t0 && t0->allocated ? t0->tex.surface.height : 0);
		gx2::log("  blend %d factors %x %x %x %x exports %08x cb_shader_mask %08x cb_shader_control %08x\n",
			m_blend ? 1 : 0, blendMode(m_blendFactors[0]), blendMode(m_blendFactors[1]), blendMode(m_blendFactors[2]),
			blendMode(m_blendFactors[3]), _p.ps->regs.sq_pgm_exports_ps, _p.ps->regs.cb_shader_mask,
			_p.ps->regs.cb_shader_control);
		logUniformValues(_p);
		if (const char * k = strstr(our64_gx2_debug, "srclog=")) {
			Program & p = const_cast<Program &>(_p);
			if (gDP.otherMode.l == u32(strtoul(k + 7, nullptr, 16)) && !p.sourceLogged && !p.psSource.empty()) {
				p.sourceLogged = true;
				gx2::log("--- fragment shader of program %u\n", p.name);
				for (size_t at = 0; at < p.psSource.size(); at += 400)
					gx2::log("%s", p.psSource.substr(at, 400).c_str());
				gx2::log("\n---\n");
			}
		}
		const f32 * r = reinterpret_cast<const f32*>(_p.psRegs.data());
		for (u32 i = 0; i < _p.ps->uniformVarCount; ++i) {
			const GX2UniformVar & u = _p.ps->uniformVars[i];
			char line[512];
			int n = snprintf(line, sizeof(line), "  %s[%u] @%u:", u.name, u.count, u.offset);
			for (u32 e = 0; e < u.count && e < 4 && n < 400; ++e)
				for (u32 c = 0; c < 4; ++c) {
					const u32 at = u.offset + e * 4 + c;
					n += snprintf(line + n, sizeof(line) - n, " %g", at < _p.psRegs.size() ? r[at] : -999.0f);
				}
			gx2::log("%s\n", line);
		}
	}
	u32 m_rectIndex = 0;
	// The pixel shader's uniform registers as last set: name=value for each
	// (ints as ints), after each logged rectangle and triangle batch.
	void logUniformValues(const Program & _p) {
		const u32 * r = reinterpret_cast<const u32*>(_p.psRegs.data());
		char line[1024];
		int n = 0;
		for (u32 i = 0; i < _p.ps->uniformVarCount && n < 900; ++i) {
			const GX2UniformVar & u = _p.ps->uniformVars[i];
			const bool isInt = u.type == GX2_SHADER_VAR_TYPE_INT || u.type == GX2_SHADER_VAR_TYPE_INT2 ||
				u.type == GX2_SHADER_VAR_TYPE_INT4 || u.type == GX2_SHADER_VAR_TYPE_BOOL;
			const u32 comps = u.type == GX2_SHADER_VAR_TYPE_INT || u.type == GX2_SHADER_VAR_TYPE_FLOAT ||
				u.type == GX2_SHADER_VAR_TYPE_BOOL ? 1 : 4;
			n += snprintf(line + n, sizeof(line) - n, " %s=", u.name);
			for (u32 c = 0; c < comps && n < 900; ++c) {
				const u32 at = u.offset + c;
				const u32 bitsv = at < _p.psRegs.size() ? r[at] : 0xDEADBEEF;
				f32 fv;
				memcpy(&fv, &bitsv, 4);
				n += isInt ? snprintf(line + n, sizeof(line) - n, c ? ",%d" : "%d", s32(bitsv))
					: snprintf(line + n, sizeof(line) - n, c ? ",%g" : "%g", fv);
			}
		}
		gx2::log("  %s\n", line);
	}

	// "uniforms" also logs each triangle batch: its RDP modes and the pixel
	// shader's uniform registers.
	void logTriangleUniforms(const Program & _p, const Context::DrawTriangleParameters & _t) {
		const Texture * t0 = texture(ObjectHandle(m_units[0]));
		gx2::log("tris %u: l=%08x h=%08x mux=%08x%08x prog=%u n=%u tex0=%ux%u blend %d depth %d\n", m_rectIndex++,
			gDP.otherMode.l, gDP.otherMode.h, u32(gDP.combine.mux >> 32), u32(gDP.combine.mux), _p.name,
			_t.elementsCount, t0 && t0->allocated ? t0->tex.surface.width : 0,
			t0 && t0->allocated ? t0->tex.surface.height : 0, m_blend ? 1 : 0, m_depthTest ? 1 : 0);
		logUniformValues(_p);
	}

	void drawLine(f32 _width, SPVertex * _vertices) override {
		GX2SetLineWidth(_width);
		Context::DrawTriangleParameters p;
		p.mode = drawmode::LINES;
		p.verticesCount = 2;
		p.vertices = _vertices;
		drawTriangles(p);
	}
	f32 getMaxLineWidth() override { return 8.0f; }
	// Dual-source blending: the shader's second output is the blender's
	// destination factor, as on desktop OpenGL. Without it GLideN64 falls back
	// to fixed blend modes that draw some transparent texels opaque.
	// Blits are same-size copies (blitFramebuffers). Depth framebuffer
	// textures: see the depth texture note at the top. Texture barrier: see textureBarrier().
	// Debug variants "noblit", "nodepthtex" and "nobarrier" turn one off (read at RomOpen).
	bool isSupported(SpecialFeatures _feature) const override {
		const char * d = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
		if ((_feature == SpecialFeatures::BlitFramebuffer && strstr(d, "noblit")) ||
				(_feature == SpecialFeatures::DepthFramebufferTextures && strstr(d, "nodepthtex")) ||
				(_feature == SpecialFeatures::TextureBarrier && strstr(d, "nobarrier")))
			return false;
		return _feature == SpecialFeatures::DualSourceBlending || _feature == SpecialFeatures::BlitFramebuffer ||
			_feature == SpecialFeatures::DepthFramebufferTextures || _feature == SpecialFeatures::TextureBarrier ||
			_feature == SpecialFeatures::AsyncShaderCompilation;
	}
	s32 getMaxMSAALevel() override { return 0; }
	bool isError() const override { return false; }
	bool isFramebufferError() const override { return false; }

	/*---------------Window-------------*/
	GX2ColorBuffer * screen() { return &m_screen.cb; }

	// The screen holds rows bottom first (OpenGL order); the TV wants them
	// top first. Draws the screen upside down into a second buffer.
	GX2ColorBuffer * presentBuffer() {
		if (m_presentProgram == 0 && !createPresentProgram())
			return &m_screen.cb;
		finishRenderTarget();
		Program * p = currentProgram();
		glUseProgram(m_presentProgram);
		Program * present = currentProgram();
		GX2ColorBuffer * cb = colorBufferOf(m_present);
		GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
		GX2SetDepthBuffer(&m_screenDepth);
		GX2SetViewport(0.0f, 0.0f, f32(cb->surface.width), f32(cb->surface.height), 0.0f, 1.0f);
		GX2SetScissor(0, 0, cb->surface.width, cb->surface.height);
		GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
		GX2SetPolygonControl(GX2_FRONT_FACE_CW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
			GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
		GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
		GX2SetShaderMode(present->mode);
		GX2SetFetchShader(&present->fetch);
		GX2SetVertexShader(present->vs);
		GX2SetPixelShader(present->ps);
		for (const SamplerBinding & s : present->samplers) {
			GX2SetPixelTexture(&m_screen.tex, s.location);
			GX2SetPixelSampler(&m_dummy.sampler, s.location);
		}
		static const RectVertex quad[4] = {
			{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
			{ 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
		};
		RectVertex * v = static_cast<RectVertex*>(ring().alloc(sizeof(quad), 64));
		memcpy(v, quad, sizeof(quad));
		attribWritten(v, sizeof(quad));
		GX2SetAttribBuffer(0, sizeof(quad), sizeof(RectVertex), v);
		GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
		profMark("present flip");
		glUseProgram(p != nullptr ? p->name : 0);
		m_dirty = m_targetDirty = true;
		m_lastProgram = nullptr;
		return cb;
	}

	// Settings > Picture > TV look = CRT (2026-11-12): the frame the TV gets,
	// drawn again at the TV's own size with scanlines and a light aperture
	// grille. _src is a finished frame, rows top first (a picture copy, after
	// the 4:3 shaping and the notices). The GamePad keeps the clean frame.
	GX2ColorBuffer * crtBuffer(GX2ColorBuffer * _src, u32 _width, u32 _height, int _soft) {
		const int k = _soft ? 1 : 0;
		if (_src == nullptr || _src->surface.image == nullptr)
			return nullptr;
		if (m_crtFailed[k] || (m_crtProgram[k] == 0 && !createCrtProgram(k))) {
			m_crtFailed[k] = true; // once: not a compile every frame
			return nullptr;
		}
		if (m_crt.allocated && (m_crt.tex.surface.width != _width || m_crt.tex.surface.height != _height)) {
			// The TV's mode changed: the old pair goes when the GPU is done with it.
			freeLater(m_crt.tex.surface.image);
			if (m_crtDepth.surface.image != nullptr)
				freeLater(m_crtDepth.surface.image);
			m_crt = Texture();
			m_crtDepth = GX2DepthBuffer();
		}
		if (!m_crt.allocated) {
			allocateTexture(m_crt, _width, _height, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
			// 16 bits: never tested or written, only the TV's size matters (1.8 MB at 720p, not 3.7).
			allocateDepth(m_crtDepth, _width, _height, GX2_SURFACE_FORMAT_UNORM_R16);
			if (!m_crt.allocated || m_crtDepth.surface.image == nullptr) {
				if (m_crt.allocated)
					MEMFreeToDefaultHeap(m_crt.tex.surface.image);
				if (m_crtDepth.surface.image != nullptr)
					MEMFreeToDefaultHeap(m_crtDepth.surface.image);
				m_crt = Texture();
				m_crtDepth = GX2DepthBuffer();
				g_kept.crt = m_crt;
				g_kept.crtDepth = m_crtDepth;
				return nullptr;
			}
			GX2InitSampler(&m_crtSampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
			g_kept.crt = m_crt; // kept with the rest (init)
			g_kept.crtDepth = m_crtDepth;
			g_kept.crtSampler = m_crtSampler;
		}
		GX2SetContextState(m_ctx);
		finishRenderTarget();
		// The frame, read as a texture: the same memory, a texture's view of it.
		GX2Texture src = GX2Texture();
		src.surface = _src->surface;
		src.viewNumMips = 1;
		src.viewNumSlices = 1;
		src.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
		GX2InitTextureRegs(&src);
		GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
			src.surface.image, src.surface.imageSize);
		Program * p = currentProgram();
		glUseProgram(m_crtProgram[k]);
		Program * crt = currentProgram();
		GX2ColorBuffer * cb = colorBufferOf(m_crt);
		GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
		GX2SetDepthBuffer(&m_crtDepth); // unused (no depth test), but the TV's size
		GX2SetViewport(0.0f, 0.0f, f32(_width), f32(_height), 0.0f, 1.0f);
		GX2SetScissor(0, 0, _width, _height);
		GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
		GX2SetPolygonControl(GX2_FRONT_FACE_CW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
			GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
		GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
		GX2SetShaderMode(crt->mode);
		GX2SetFetchShader(&crt->fetch);
		GX2SetVertexShader(crt->vs);
		GX2SetPixelShader(crt->ps);
		for (const SamplerBinding & s : crt->samplers) {
			GX2SetPixelTexture(&src, s.location);
			GX2SetPixelSampler(&m_crtSampler, s.location);
		}
		// Rows stay as they are (the source is already top first): v = 0 at the top.
		static const RectVertex quad[4] = {
			{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
			{ 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
		};
		RectVertex * v = static_cast<RectVertex*>(ring().alloc(sizeof(quad), 64));
		memcpy(v, quad, sizeof(quad));
		attribWritten(v, sizeof(quad));
		GX2SetAttribBuffer(0, sizeof(quad), sizeof(RectVertex), v);
		GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb->surface.image, cb->surface.imageSize);
		profMark("TV look: CRT");
		glUseProgram(p != nullptr ? p->name : 0);
		m_dirty = m_targetDirty = true;
		m_lastProgram = nullptr;
		return cb;
	}

	// Frames alternate between two rings, so the CPU builds the next frame
	// while the GPU draws this one. Before a ring is reused, wait only for the
	// frame that last used it (the previous one); its frees are safe then too.
	void endFrame() {
		if (tracing())
			gx2::log("trace %u: end of frame\n", m_traceIndex);
		profMark("frame end (screen copies)");
		finishRenderTarget();
		GX2Flush();
		m_frameStamp[m_half] = GX2GetLastSubmittedTimeStamp();
		m_half ^= 1;
		const OSTime waitStart = OSGetSystemTime();
		GX2WaitTimeStamp(m_frameStamp[m_half]);
		our64_gx2_frame_wait_us += OSTicksToMicroseconds(OSGetSystemTime() - waitStart);
		ring().reset();
		g_vcFrame = vcFrameVariant();
		if (g_vcFrame)
			ring().invalidateVertexCache();
		for (void * p : m_pendingFree[m_half])
			MEMFreeToDefaultHeap(p);
		m_pendingFree[m_half].clear();
		// Presenting (a copy to the scan buffers) changes GPU state.
		restoreState();
		profFrame();
	}

	// Debug variant "gpuprof": every 10th frame the GPU writes its clock after
	// each traced operation (bottom of the pipe), and the GPU time since the
	// previous sample is summed per kind; logged every 10 profiled frames.
	// Command buffers go to the GPU at the frame's end, so a gap is the GPU's
	// own time, not waiting for the CPU (Stunt Racer's GPU wait, 2026-10-28).
	// The player's frame test profiles quietly (our64_gx2_prof_run): no wait
	// for the GPU, so the pacing it measures stays as it is. A profiled frame's
	// samples are read 10 frames later, once its commands have retired, and
	// summed for the whole run (our64_gx2_prof_report). Without the wait a gap
	// can also hold time the GPU sat idle, waiting for commands; in the heavy
	// scenes that matter it is behind and has commands waiting.
	static constexpr u32 kProfMax = 8192;
	u64 * m_prof = nullptr;
	// m_profTop[n]: a top-of-pipe sample taken before mark n's work (traceBegin,
	// player marks), 0 if none: the gap then starts no earlier than that, and
	// the time before it the GPU sat idle, waiting for commands.
	u64 * m_profTop = nullptr;
	const char * m_profLabel[kProfMax];
	u32 m_profN = 0, m_profFrame = 0, m_profFrames = 0, m_profKinds = 0;
	bool m_profOn = false;
	const char * m_profWhat = "untraced";
	struct ProfSum { const char * what; u64 gpu; u32 count; };
	ProfSum m_profSum[32];
	u64 m_profTotal = 0;
	OSTime m_profCpuTicks = 0;
	volatile bool m_profQuiet = false; // asked (any core)
	bool m_profQuietWas = false;        // as this core runs it
	bool m_profUnread = false;         // a quiet frame's samples wait to be read
	u64 m_profUnreadStamp = 0;
	ProfSum m_runSum[32];
	u32 m_runKinds = 0, m_runFrames = 0;
	u64 m_runTotal = 0, m_runMost = 0, m_runIdle = 0;

	void profMark(const char * _what) {
		if (!m_profOn || m_profN >= kProfMax)
			return;
		m_profLabel[m_profN] = _what;
		GX2SampleBottomGPUCycle(&m_prof[m_profN++]);
	}
	// Before an operation's commands: see m_profTop.
	void profTop() {
		if (!m_profOn || m_profN >= kProfMax)
			return;
		GX2SampleTopGPUCycle(&m_profTop[m_profN]);
	}

	bool vcFrameVariant() {
		const char * d = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
		if (strstr(d, "abvc"))
			return (OSTicksToMilliseconds(OSGetSystemTime()) / 20000) & 1;
		return strstr(d, "vcframe") != nullptr;
	}
	bool m_profVariant = false;

	// A profiled frame's samples, summed by kind into _sum; returns the frame's
	// GPU cycles (busy), and adds the cycles it sat idle waiting for commands to _idle.
	u64 profRead(ProfSum * _sum, u32 & _kinds, u64 & _total, u64 * _idle = nullptr) {
		DCInvalidateRange(m_prof, kProfMax * sizeof(u64));
		DCInvalidateRange(m_profTop, kProfMax * sizeof(u64));
		auto unset = [](u64 v) { return v == 0 || v == ~0ull; };
		// The clock as written, or byte-swapped: whichever counts up.
		const bool swap = m_profN > 1 && __builtin_bswap64(m_prof[m_profN - 1]) - __builtin_bswap64(m_prof[0]) <
			m_prof[m_profN - 1] - m_prof[0];
		auto bottom = [&](u32 i) { return swap ? __builtin_bswap64(m_prof[i]) : m_prof[i]; };
		// Top samples come from another packet: as written or swapped, whichever
		// falls inside the frame's bottom samples.
		int topSwap = -1;
		for (u32 i = 1; i < m_profN && topSwap < 0; ++i)
			if (!unset(m_profTop[i])) {
				const u64 lo = bottom(0), hi = bottom(m_profN - 1);
				const u64 a = m_profTop[i], b = __builtin_bswap64(m_profTop[i]);
				topSwap = a >= lo && a <= hi ? 0 : b >= lo && b <= hi ? 1 : 2; // 2: neither, unused
			}
		u64 frame = 0;
		for (u32 i = 1; i < m_profN; ++i) {
			if (unset(m_prof[i - 1]) || unset(m_prof[i]))
				continue;
			const u64 a = bottom(i - 1), b = bottom(i);
			if (b < a || b - a > 100000000ull)
				continue; // not a gap (over 3 s)
			u64 d = b - a, idle = 0;
			if ((topSwap == 0 || topSwap == 1) && !unset(m_profTop[i])) {
				const u64 t = topSwap ? __builtin_bswap64(m_profTop[i]) : m_profTop[i];
				if (t > a && t <= b) { // the commands came after the GPU had finished the ones before
					idle = t - a;
					d = b - t;
				}
			}
			if (_idle != nullptr)
				*_idle += idle;
			u32 k = 0;
			while (k < _kinds && strcmp(_sum[k].what, m_profLabel[i]) != 0)
				++k;
			if (k == _kinds) {
				if (_kinds == 32)
					continue;
				_sum[_kinds++] = {m_profLabel[i], 0, 0};
			}
			_sum[k].gpu += d;
			_sum[k].count++;
			_total += d;
			frame += d;
		}
		return frame;
	}

	void profFrame() {
		if (m_profOn) {
			m_profOn = false;
			if (m_profQuietWas) {
				m_profUnreadStamp = GX2GetLastSubmittedTimeStamp();
				m_profUnread = true;
			} else {
				GX2DrawDone();
				m_profCpuTicks += OSGetSystemTime() - m_profStart;
				const bool swap = m_profN > 1 && __builtin_bswap64(m_prof[m_profN - 1]) - __builtin_bswap64(m_prof[0]) <
					m_prof[m_profN - 1] - m_prof[0];
				profRead(m_profSum, m_profKinds, m_profTotal);
				if (++m_profFrames % 10 == 0) {
					gx2::log("GPU profile, %u frames (%s clock, %s): %llu GPU cycles a frame, %.2f ms CPU from start to done a frame\n",
						m_profFrames, swap ? "swapped" : "as written", m_profVariant ? "vcframe" : "per-draw invalidate",
						(unsigned long long)(m_profTotal / 10),
						OSTicksToMicroseconds(m_profCpuTicks) / 10000.0);
					for (u32 k = 0; k < m_profKinds; ++k)
						gx2::log("  %-28s %6u ops a frame, %10llu cycles a frame (%4.1f%%), %8llu an op\n", m_profSum[k].what,
							m_profSum[k].count / 10, (unsigned long long)(m_profSum[k].gpu / 10),
							m_profTotal ? 100.0 * m_profSum[k].gpu / m_profTotal : 0.0,
							(unsigned long long)(m_profSum[k].count ? m_profSum[k].gpu / m_profSum[k].count : 0));
					m_profKinds = 0;
					m_profTotal = 0;
					m_profCpuTicks = 0;
				}
			}
		}
		if (m_profQuiet != m_profQuietWas) {
			m_profQuietWas = m_profQuiet;
			m_profUnread = false;
			m_profFrame = 0;
			if (m_profQuietWas) {
				m_runKinds = m_runFrames = 0;
				m_runTotal = m_runMost = m_runIdle = 0;
			}
		}
		if (!(debug().gpuProf || m_profQuietWas) || ++m_profFrame % 10 != 0)
			return;
		if (m_profUnread) { // 10 frames on: retired long since, so the wait returns at once
			GX2WaitTimeStamp(m_profUnreadStamp);
			u64 total = 0;
			const u64 frame = profRead(m_runSum, m_runKinds, total, &m_runIdle);
			m_runTotal += frame;
			m_runFrames++;
			if (frame > m_runMost)
				m_runMost = frame;
			m_profUnread = false;
		}
		if (!m_profQuietWas && g_vcFrame != m_profVariant) { // a summary covers one variant only
			m_profVariant = g_vcFrame;
			m_profFrames = (m_profFrames + 9) / 10 * 10;
			m_profKinds = 0;
			m_profTotal = 0;
			m_profCpuTicks = 0;
		}
		if (m_prof == nullptr) {
			m_prof = static_cast<u64*>(MEMAllocFromDefaultHeapEx(kProfMax * sizeof(u64), 256));
			m_profTop = static_cast<u64*>(MEMAllocFromDefaultHeapEx(kProfMax * sizeof(u64), 256));
			if (m_prof == nullptr || m_profTop == nullptr)
				return;
		}
		memset(m_prof, 0, (m_profN > 0 ? m_profN : kProfMax) * sizeof(u64));
		memset(m_profTop, 0, (m_profN > 0 ? m_profN + 1 : kProfMax) * sizeof(u64));
		DCFlushRange(m_prof, kProfMax * sizeof(u64));
		DCFlushRange(m_profTop, kProfMax * sizeof(u64));
		m_profN = 0;
		m_profOn = true;
		m_profStart = OSGetSystemTime();
		profMark("start");
	}
	OSTime m_profStart = 0;

	void restoreState() {
		GX2SetContextState(m_ctx);
		m_dirty = m_targetDirty = true;
		m_lastProgram = nullptr;
	}

	void readScreen(u8 * _dest, u32 _width, u32 _height) {
		finishRenderTarget();
		GX2Surface linear = m_screen.tex.surface;
		linear.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
		linear.use = GX2_SURFACE_USE_TEXTURE;
		linear.image = nullptr;
		linear.mipmaps = nullptr;
		GX2CalcSurfaceSizeAndAlignment(&linear);
		linear.image = MEMAllocFromDefaultHeapEx(linear.imageSize, linear.alignment);
		if (linear.image == nullptr)
			return;
		DCInvalidateRange(linear.image, linear.imageSize);
		GX2CopySurface(&m_screen.tex.surface, 0, 0, &linear, 0, 0);
		GX2Flush();
		GX2DrawDone();
		DCInvalidateRange(linear.image, linear.imageSize);
		const u8 * src = static_cast<const u8*>(linear.image);
		for (u32 y = 0; y < _height && y < linear.height; ++y)
			for (u32 x = 0; x < _width && x < linear.width; ++x) {
				const u8 * p = src + (y * linear.pitch + x) * 4;
				u8 * d = _dest + (y * _width + x) * 3;
				d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
			}
		MEMFreeToDefaultHeap(linear.image);
		GX2SetContextState(m_ctx);
		m_dirty = m_targetDirty = true;
	}

private:
	Texture * texture(ObjectHandle _name) {
		auto it = m_textures.find(u32(_name));
		return it == m_textures.end() ? nullptr : &it->second;
	}

	void bindUnit(TextureUnitParam _unit, ObjectHandle _name) {
		const u32 unit = u32(_unit);
		if (unit < 8)
			m_units[unit] = u32(_name);
	}

	void logOnce(bool & _flag, const char * _message) {
		if (!_flag)
			gx2::log("%s", _message);
		_flag = true;
	}

	// The GPU may still read memory a queued draw uses, so frees wait for the frame end.
	void freeLater(void * _p) {
		if (_p != nullptr)
			m_pendingFree[m_half].push_back(_p);
	}

	void allocateTexture(Texture & _t, u32 _width, u32 _height, GX2SurfaceFormat _format, bool _clear = true, u32 _mips = 1) {
		GX2Texture & tex = _t.tex;
		const GX2Sampler sampler = _t.sampler;
		tex = GX2Texture();
		tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		tex.surface.width = _width > 0 ? _width : 1;
		tex.surface.height = _height > 0 ? _height : 1;
		tex.surface.depth = 1;
		tex.surface.mipLevels = _mips;
		tex.surface.format = _format;
		tex.surface.aa = GX2_AA_MODE1X;
		tex.surface.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
		tex.surface.tileMode = GX2_TILE_MODE_DEFAULT;
		tex.viewNumSlices = 1;
		tex.viewNumMips = _mips;
		switch (_format) {
		case GX2_SURFACE_FORMAT_UNORM_R8:
		case GX2_SURFACE_FORMAT_FLOAT_R16:
		case GX2_SURFACE_FORMAT_FLOAT_R32:
		case GX2_SURFACE_FORMAT_UINT_R32:
			tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
			break;
		case GX2_SURFACE_FORMAT_FLOAT_R32_G32:
			tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
			break;
		default:
			tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
		}
		GX2CalcSurfaceSizeAndAlignment(&tex.surface);
		GX2InitTextureRegs(&tex);
		if (_width == 0 || _height == 0 || _width > 8192 || _height > 8192)
			gx2::log("CHECK: texture of %ux%u (format %u)\n", _width, _height, u32(_format));
		// Levels 1 and up follow level 0 in the same block, so freeing the
		// image frees them too.
		const u32 bytes = textureBytes(tex.surface);
		tex.surface.image = MEMAllocFromDefaultHeapEx(bytes, tex.surface.alignment);
		if (tex.surface.image == nullptr)
			gx2::log("CHECK: out of memory for a %ux%u texture (%u bytes)\n", _width, _height, bytes);
		else if (_mips > 1)
			tex.surface.mipmaps = static_cast<u8*>(tex.surface.image) + mipOffset(tex.surface);
		if (tex.surface.image != nullptr && _clear) {
			memset(tex.surface.image, 0, bytes);
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, tex.surface.image, bytes);
		} else if (tex.surface.image != nullptr) {
			// Uncleared, the memory's previous owner may still have dirty
			// lines in the CPU cache; written back after the GPU's upload
			// they land on the texture as noise blocks that differ from run
			// to run (Mario Party 3's board, Mario Tennis's banner, 2026-10-22).
			DCFlushRange(tex.surface.image, bytes);
		}
		_t.sampler = sampler;
		_t.allocated = tex.surface.image != nullptr;
		_t.cbReady = false;
	}

	void allocateDepth(GX2DepthBuffer & _db, u32 _width, u32 _height,
			GX2SurfaceFormat _format = GX2_SURFACE_FORMAT_FLOAT_R32) {
		_db = GX2DepthBuffer();
		GX2Surface & s = _db.surface;
		s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		s.width = _width > 0 ? _width : 1;
		s.height = _height > 0 ? _height : 1;
		s.depth = 1;
		s.mipLevels = 1;
		s.format = _format;
		s.aa = GX2_AA_MODE1X;
		s.use = GX2_SURFACE_USE_DEPTH_BUFFER;
		s.tileMode = GX2_TILE_MODE_DEFAULT;
		_db.viewNumSlices = 1;
		_db.depthClear = 1.0f;
		GX2CalcSurfaceSizeAndAlignment(&s);
		GX2InitDepthBufferRegs(&_db);
		if (_width == 0 || _height == 0 || _width > 8192 || _height > 8192)
			gx2::log("CHECK: depth buffer of %ux%u\n", _width, _height);
		s.image = MEMAllocFromDefaultHeapEx(s.imageSize, s.alignment);
		if (s.image == nullptr)
			gx2::log("CHECK: out of memory for a %ux%u depth buffer\n", _width, _height);
		if (s.image != nullptr)
			GX2Invalidate(GX2_INVALIDATE_MODE_CPU, s.image, s.imageSize);
	}

	GX2ColorBuffer * colorBufferOf(Texture & _t) {
		if (!_t.allocated)
			return nullptr;
		if (!_t.cbReady) {
			_t.cb = GX2ColorBuffer();
			_t.cb.surface = _t.tex.surface;
			_t.cb.viewNumSlices = 1;
			GX2InitColorBufferRegs(&_t.cb);
			_t.cbReady = true;
		}
		return &_t.cb;
	}

	void createScreen(u32 _width, u32 _height) {
		m_screen = Texture();
		allocateTexture(m_screen, _width, _height, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
		colorBufferOf(m_screen);
		allocateDepth(m_screenDepth, _width, _height);
	}

	bool createPresentProgram() {
		// gx2_GL negates gl_Position.y in every vertex shader; this one
		// negates it first, so the quad is drawn as given: y = 1 at the top.
		const char * vs = "#version 330\nin vec4 aRectPosition;\nin vec2 aTexCoord0;\nout vec2 vUV;\n"
			"void main() { gl_Position = vec4(aRectPosition.x, -aRectPosition.y, 0.0, 1.0); vUV = aTexCoord0; }\n";
		const char * ps = "#version 330\nuniform sampler2D uTex0;\nin vec2 vUV;\nout vec4 fragColor;\n"
			"void main() { fragColor = texture(uTex0, vUV); }\n";
		const GLuint v = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(v, 1, &vs, nullptr);
		const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(f, 1, &ps, nullptr);
		const GLuint program = glCreateProgram();
		glAttachShader(program, v);
		glAttachShader(program, f);
		glLinkProgram(program);
		glDeleteShader(v);
		glDeleteShader(f);
		GLint linked = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (linked != GL_TRUE)
			return false;
		m_presentProgram = program;
		if (!m_present.allocated) {
			allocateTexture(m_present, m_screen.tex.surface.width, m_screen.tex.surface.height, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
			g_kept.present = m_present; // kept with the rest (init)
		}
		return m_present.allocated;
	}

	// The TV look's shader (crtBuffer). The frame doubles each of the N64's
	// lines (480 rows for 240), so a line is two source rows: each output row
	// takes its line's color (sampled at the line's middle) and a beam across
	// the line, 1 at its middle and `edge` at its borders (a parabola), wider
	// (brighter borders) for bright colors. `gain` makes up exactly what the
	// beam takes on average (1/3 of 1 - edge), so a line keeps its brightness
	// and only the scanlines' shape shows; the light aperture grille (R, G, B a
	// little stronger in turn, column by column) is made up the same way. The
	// first version (a gaussian, made up by hand) left two of a line's three
	// TV rows at half (55 against 27 in a dark scene), too dark (2026-11-12).
	// At 720p a line is 3 TV rows; at 1080p the copy to the scan buffer
	// stretches 720 to 1080. Soft CRT (after the Switch 2's N64 filter, from a
	// screenshot of it, 2026-11-12): the line's color blurred across about two
	// N64 pixels (taps 0.5 and 1 N64 pixel out, most weight in the middle: the
	// first, three pixels wide, made the dialogue hard to read on a TV), red and blue a little apart (composite-like
	// fringes), a little of the next line toward the line's borders, softer
	// scanlines, and the 4:3 picture's corners rounded and darkened.
	bool createCrtProgram(int _soft) {
		const char * vs = "#version 330\nin vec4 aRectPosition;\nin vec2 aTexCoord0;\nout vec2 vUV;\n"
			"void main() { gl_Position = vec4(aRectPosition.x, -aRectPosition.y, 0.0, 1.0); vUV = aTexCoord0; }\n";
		std::string psText = std::string("#version 330\n#define SOFT ") + (_soft ? "1" : "0") + "\n" +
			"uniform sampler2D uTex0;\n"
			"in vec2 vUV;\n"
			"out vec4 fragColor;\n"
			"void main() {\n"
			"  highp vec2 size = vec2(textureSize(uTex0, 0));\n"
			"  highp float lines = size.y * 0.5;\n"
			"  highp float y = vUV.y * lines;\n"
			"  highp float line = floor(y);\n"
			"  highp float d = y - line - 0.5;\n"
			"  highp float vy = (line + 0.5) / lines;\n"
			"  float t = 2.0 * d;\n"
			"#if SOFT\n"
			"  highp float px = 1.0 / size.x;\n"
			"  vec3 c = texture(uTex0, vec2(vUV.x - 2.0 * px, vy)).rgb * 0.06\n"
			"         + texture(uTex0, vec2(vUV.x - px, vy)).rgb * 0.20\n"
			"         + texture(uTex0, vec2(vUV.x, vy)).rgb * 0.48\n"
			"         + texture(uTex0, vec2(vUV.x + px, vy)).rgb * 0.20\n"
			"         + texture(uTex0, vec2(vUV.x + 2.0 * px, vy)).rgb * 0.06;\n"
			"  c.r = mix(c.r, texture(uTex0, vec2(vUV.x - px, vy)).r, 0.25);\n"
			"  c.b = mix(c.b, texture(uTex0, vec2(vUV.x + px, vy)).b, 0.25);\n"
			"  vec3 nb = texture(uTex0, vec2(vUV.x, clamp(vy + sign(d) / lines, 0.0, 1.0))).rgb;\n"
			"  c = mix(c, nb, 0.15 * abs(t));\n"
			"  float lum = dot(c, vec3(0.299, 0.587, 0.114));\n"
			"  float edge = mix(0.55, 0.8, lum);\n"
			"#else\n"
			"  vec3 c = texture(uTex0, vec2(vUV.x, vy)).rgb;\n"
			"  float lum = dot(c, vec3(0.299, 0.587, 0.114));\n"
			"  float edge = mix(0.45, 0.75, lum);\n"
			"#endif\n"
			"  float beam = 1.0 - (1.0 - edge) * t * t;\n"
			"  float gain = 1.0 / (1.0 - (1.0 - edge) / 3.0);\n"
			"  vec3 o = c * beam * gain;\n"
			"  float m = mod(floor(gl_FragCoord.x), 3.0);\n"
			"  vec3 mask = vec3(0.9) + 0.1 * vec3(step(m, 0.5), step(0.5, m) * step(m, 1.5), step(1.5, m));\n"
			"  o *= mask * (1.0 / 0.9333);\n"
			"#if SOFT\n"
			"  highp float gw = min(size.x, size.y * 4.0 / 3.0);\n"
			"  highp float gx = (vUV.x * size.x - (size.x - gw) * 0.5) / gw;\n"
			"  vec2 p = (vec2(gx, vUV.y) - 0.5) * vec2(4.0 / 3.0, 1.0);\n"
			"  vec2 hb = vec2(2.0 / 3.0, 0.5);\n"
			"  vec2 q = max(abs(p) - hb + 0.035, 0.0);\n"
			"  float corner = 1.0 - smoothstep(-0.004, 0.0, length(q) - 0.035);\n"
			"  vec2 n = p / hb;\n"
			"  float vig = 1.0 - 0.3 * smoothstep(0.3, 1.0, dot(n, n) * 0.5);\n"
			"  o *= corner * vig;\n"
			"#endif\n"
			"  fragColor = vec4(min(o, vec3(1.0)), 1.0);\n"
			"}\n";
		const char * ps = psText.c_str();
		const GLuint v = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(v, 1, &vs, nullptr);
		const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(f, 1, &ps, nullptr);
		const GLuint program = glCreateProgram();
		glAttachShader(program, v);
		glAttachShader(program, f);
		glLinkProgram(program);
		glDeleteShader(v);
		glDeleteShader(f);
		GLint linked = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (linked != GL_TRUE) {
			gx2::log("CHECK: the TV look's shader did not link\n");
			return false;
		}
		m_crtProgram[_soft ? 1 : 0] = program;
		return true;
	}

	void createDummyTexture() {
		allocateTexture(m_dummy, 1, 1, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
		GX2InitSampler(&m_dummy.sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_POINT);
	}

	GX2ColorBuffer * drawColorBuffer() {
		if (m_drawFbo == 0)
			return &m_screen.cb;
		auto it = m_framebuffers.find(m_drawFbo);
		if (it == m_framebuffers.end())
			return nullptr;
		Texture * t = texture(ObjectHandle(it->second.color));
		return t == nullptr ? nullptr : colorBufferOf(*t);
	}

	GX2DepthBuffer * drawDepthBuffer() { return depthBufferOf(m_drawFbo); }

	Texture * drawColorTexture() {
		if (m_drawFbo == 0)
			return &m_screen;
		auto it = m_framebuffers.find(m_drawFbo);
		return it == m_framebuffers.end() ? nullptr : texture(ObjectHandle(it->second.color));
	}

	// A framebuffer's depth: the screen's, a renderbuffer's, or a depth texture's own depth buffer.
	GX2DepthBuffer * depthBufferOf(u32 _fbo) {
		if (_fbo == 0)
			return &m_screenDepth;
		auto it = m_framebuffers.find(_fbo);
		if (it == m_framebuffers.end() || it->second.depth == 0)
			return nullptr;
		if (it->second.depthTexture) {
			Texture * t = texture(ObjectHandle(it->second.depth));
			return t != nullptr && ensureDepthBuffer(*t) ? &t->db : nullptr;
		}
		auto rb = m_renderbuffers.find(it->second.depth);
		return rb == m_renderbuffers.end() || !rb->second.allocated ? nullptr : &rb->second.db;
	}

	Texture * depthTextureOf(u32 _fbo) {
		auto it = m_framebuffers.find(_fbo);
		if (_fbo == 0 || it == m_framebuffers.end() || !it->second.depthTexture)
			return nullptr;
		return texture(ObjectHandle(it->second.depth));
	}
	Texture * drawDepthTexture() { return depthTextureOf(m_drawFbo); }

	GX2ColorBuffer * colorBufferOfFbo(u32 _fbo) {
		if (_fbo == 0)
			return &m_screen.cb;
		auto it = m_framebuffers.find(_fbo);
		if (it == m_framebuffers.end())
			return nullptr;
		Texture * t = texture(ObjectHandle(it->second.color));
		return t == nullptr ? nullptr : colorBufferOf(*t);
	}

	bool ensureDepthBuffer(Texture & _t) {
		if (!_t.allocated || !_t.depth)
			return false;
		if (!_t.dbAllocated) {
			allocateDepth(_t.db, _t.tex.surface.width, _t.tex.surface.height);
			_t.dbAllocated = _t.db.surface.image != nullptr;
			_t.dbDirty = false;
		}
		return _t.dbAllocated;
	}

	// Copies a depth buffer into a texture surface of the same size. Changes GPU state.
	void convertDepth(const GX2DepthBuffer & _db, GX2Surface & _dst) {
		GX2Invalidate(GX2_INVALIDATE_MODE_DEPTH_BUFFER, _db.surface.image, _db.surface.imageSize);
		traceBegin("depth to texture", 0);
		GX2ConvertDepthBufferToTextureSurface(&_db, &_dst, 0, 0);
		traceEnd();
		GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _dst.image, _dst.imageSize);
	}

	// Depth textures a draw samples get their texture surface refreshed
	// first. Returns true when GPU state was changed.
	bool refreshDepthTextures(const Program & _p) {
		bool changed = false;
		const Texture * target = drawDepthTexture();
		for (const SamplerBinding & s : _p.samplers) {
			Texture * t = s.unit < 8 ? texture(ObjectHandle(m_units[s.unit])) : nullptr;
			if (t == nullptr || !t->depth || !t->dbAllocated || !t->dbDirty || t == target)
				continue;
			convertDepth(t->db, t->tex.surface);
			t->dbDirty = false;
			changed = true;
		}
		return changed;
	}

	// Between formats (an RGBA8 frame buffer into an 8-bit one's R8 texture,
	// what GLideN64 does when a game reads a 16-bit buffer as I8), the copy is
	// drawn: GX2CopySurfaceEx does not convert, and an R8 target keeps the
	// source's red channel, as glBlitFramebuffer does.
	bool blitColor(const Context::BlitFramebuffersParams & _p, s32 _w, s32 _h) {
		GX2ColorBuffer * src = colorBufferOfFbo(m_readFbo);
		GX2ColorBuffer * dst = colorBufferOfFbo(m_drawFbo);
		if (src == nullptr || dst == nullptr) {
			logOnce(m_loggedBlitColor, "CHECK: color blit with a missing buffer\n");
			return false;
		}
		GX2Rect rect = {_p.srcX0, _p.srcY0, _p.srcX0 + _w, _p.srcY0 + _h};
		GX2Point point = {_p.dstX0, _p.dstY0};
		if (!clipCopy(src->surface, dst->surface, rect, point))
			return true;
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, src->surface.image, src->surface.imageSize);
		if (src->surface.format != dst->surface.format)
			return drawColorCopy(*src, *dst, rect, point);
		traceBegin("blit color", 0);
		GX2CopySurfaceEx(&src->surface, 0, 0, &dst->surface, 0, 0, 1, &rect, &point);
		traceEnd();
		GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, dst->surface.image, dst->surface.imageSize);
		return true;
	}

	// The rectangle of _src at _r drawn at _p in _dst, one texel a pixel, rows
	// in the same order (the present program, without its flip). The caller
	// restores the context state.
	bool drawColorCopy(const GX2ColorBuffer & _src, GX2ColorBuffer & _dst, const GX2Rect & _r, const GX2Point & _p) {
		if (m_presentProgram == 0 && !createPresentProgram())
			return false;
		Program * p = currentProgram();
		glUseProgram(m_presentProgram);
		Program * copy = currentProgram();
		GX2Texture tex = {};
		tex.surface = _src.surface;
		tex.viewNumSlices = 1;
		tex.viewNumMips = 1;
		tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
		GX2InitTextureRegs(&tex);
		const s32 w = _r.right - _r.left, h = _r.bottom - _r.top;
		traceBegin("blit color drawn", 0);
		GX2SetColorBuffer(&_dst, GX2_RENDER_TARGET_0);
		GX2SetDepthBuffer(&m_screenDepth);
		GX2SetViewport(f32(_p.x), f32(_p.y), f32(w), f32(h), 0.0f, 1.0f);
		GX2SetScissor(_p.x, _p.y, w, h);
		GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
		GX2SetPolygonControl(GX2_FRONT_FACE_CW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
			GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
		GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
		GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
			(GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
		GX2SetShaderMode(copy->mode);
		GX2SetFetchShader(&copy->fetch);
		GX2SetVertexShader(copy->vs);
		GX2SetPixelShader(copy->ps);
		for (const SamplerBinding & s : copy->samplers) {
			GX2SetPixelTexture(&tex, s.location);
			GX2SetPixelSampler(&m_dummy.sampler, s.location);
		}
		// y = 1 is the viewport's top, the first row written.
		const f32 sw = f32(_src.surface.width), sh = f32(_src.surface.height);
		const f32 u0 = f32(_r.left) / sw, u1 = f32(_r.right) / sw;
		const f32 v0 = f32(_r.top) / sh, v1 = f32(_r.bottom) / sh;
		const RectVertex quad[4] = {
			{-1.0f, -1.0f, 0.0f, 1.0f, u0, v1, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 0.0f, 1.0f, u1, v1, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 1.0f, u0, v0, 0.0f, 0.0f},
			{ 1.0f,  1.0f, 0.0f, 1.0f, u1, v0, 0.0f, 0.0f},
		};
		RectVertex * v = static_cast<RectVertex*>(ring().alloc(sizeof(quad), 64));
		memcpy(v, quad, sizeof(quad));
		attribWritten(v, sizeof(quad));
		GX2SetAttribBuffer(0, sizeof(quad), sizeof(RectVertex), v);
		GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
		traceEnd();
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, _dst.surface.image, _dst.surface.imageSize);
		GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _dst.surface.image, _dst.surface.imageSize);
		glUseProgram(p != nullptr ? p->name : 0);
		return true;
	}

	// Depth into a depth texture (GLideN64's depth buffer copies). Through a
	// scratch texture when the sizes or the rectangle differ.
	bool blitDepth(const Context::BlitFramebuffersParams & _p, s32 _w, s32 _h) {
		GX2DepthBuffer * src = depthBufferOf(m_readFbo);
		Texture * dst = drawDepthTexture();
		if (src == nullptr || dst == nullptr || !dst->allocated || !dst->depth) {
			logOnce(m_loggedBlitDepth, "CHECK: depth blit without a depth source or into a renderbuffer\n");
			return false;
		}
		const GX2Surface & ss = src->surface;
		GX2Surface & ds = dst->tex.surface;
		const bool whole = _p.srcX0 == 0 && _p.srcY0 == 0 && _p.dstX0 == 0 && _p.dstY0 == 0 &&
			u32(_w) == ss.width && u32(_h) == ss.height;
		if (whole && ss.width == ds.width && ss.height == ds.height) {
			convertDepth(*src, ds);
		} else {
			if (!m_depthScratch.allocated || m_depthScratch.tex.surface.width != ss.width ||
					m_depthScratch.tex.surface.height != ss.height) {
				if (m_depthScratch.allocated)
					freeLater(m_depthScratch.tex.surface.image);
				allocateTexture(m_depthScratch, ss.width, ss.height, GX2_SURFACE_FORMAT_FLOAT_R32);
				if (!m_depthScratch.allocated)
					return false;
			}
			convertDepth(*src, m_depthScratch.tex.surface);
			GX2Rect rect = {_p.srcX0, _p.srcY0, _p.srcX0 + _w, _p.srcY0 + _h};
			GX2Point point = {_p.dstX0, _p.dstY0};
			if (clipCopy(m_depthScratch.tex.surface, ds, rect, point)) {
				traceBegin("blit depth", 0);
				GX2CopySurfaceEx(&m_depthScratch.tex.surface, 0, 0, &ds, 0, 0, 1, &rect, &point);
				traceEnd();
				GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, ds.image, ds.imageSize);
			}
		}
		// The texture surface is now newer than the texture's own depth buffer.
		dst->dbDirty = false;
		return true;
	}

	// Clips a copy rectangle to both surfaces; false when nothing is left.
	static bool clipCopy(const GX2Surface & _src, const GX2Surface & _dst, GX2Rect & _r, GX2Point & _p) {
		if (_r.left < 0) { _p.x -= _r.left; _r.left = 0; }
		if (_r.top < 0) { _p.y -= _r.top; _r.top = 0; }
		if (_p.x < 0) { _r.left -= _p.x; _p.x = 0; }
		if (_p.y < 0) { _r.top -= _p.y; _p.y = 0; }
		_r.right = std::min<s32>(_r.right, s32(_src.width));
		_r.bottom = std::min<s32>(_r.bottom, s32(_src.height));
		_r.right = std::min<s32>(_r.right, _r.left + s32(_dst.width) - _p.x);
		_r.bottom = std::min<s32>(_r.bottom, _r.top + s32(_dst.height) - _p.y);
		return _r.right > _r.left && _r.bottom > _r.top;
	}

public:
	// Copies a texture into _c.linear on the GPU (converted to N64 RGBA5551
	// first when _pack16), without waiting. False when it cannot.
	// The color texture of the framebuffer bound for reading, 0 for none: what
	// glReadPixels reads in the OpenGL backend. GLideN64 binds an auxiliary
	// buffer's own framebuffer there and never fills its color copy texture
	// (Pokemon Snap's focus tiles read back black).
	u32 readColorTexture() const {
		if (m_readFbo == 0)
			return 0;
		auto it = m_framebuffers.find(m_readFbo);
		return it == m_framebuffers.end() ? 0 : it->second.color;
	}
	bool copyForReadback(u32 _name, bool _pack16, ReadbackCopy & _c) {
		Texture * t = texture(ObjectHandle(_name));
		if (t == nullptr || !t->allocated) {
			static unsigned n;
			if (n++ < 4) gx2::log("CHECK: readback of texture %u: %s\n", _name, t == nullptr ? "no such texture" : "not allocated");
			return false;
		}
		finishRenderTarget();
		const GX2Surface * src = &t->tex.surface;
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, src->image, src->imageSize);
		if (_pack16) {
			src = pack16(*t);
			if (src == nullptr) {
				static unsigned n;
				if (n++ < 4) gx2::log("CHECK: readback of texture %u: the 16-bit packing failed\n", _name);
				return false;
			}
		}
		GX2Surface & linear = _c.linear;
		if (linear.image == nullptr || linear.width != src->width || linear.height != src->height || linear.format != src->format) {
			if (linear.image != nullptr)
				freeLater(linear.image);
			linear = *src;
			linear.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
			linear.use = GX2_SURFACE_USE_TEXTURE;
			linear.image = nullptr;
			linear.mipmaps = nullptr;
			GX2CalcSurfaceSizeAndAlignment(&linear);
			linear.image = MEMAllocFromDefaultHeapEx(linear.imageSize, linear.alignment);
			_c.valid = false;
			if (linear.image == nullptr) {
				gx2::log("CHECK: out of memory for a %ux%u readback\n", linear.width, linear.height);
				return false;
			}
		}
		DCInvalidateRange(linear.image, linear.imageSize);
		traceBegin("readback", src->height);
		GX2CopySurface(src, 0, 0, &linear, 0, 0);
		traceEnd();
		GX2Flush();
		_c.stamp = GX2GetLastSubmittedTimeStamp();
		_c.valid = true;
		restoreAfterClear();
		return true;
	}

	// Frees memory the GPU may still be writing, at a safe time.
	void release(void * _p) { freeLater(_p); }

	bool canPack16() { testPack16(); return m_packFormat != GX2_SURFACE_FORMAT_INVALID; }
	// The pack shader writes each pixel's two bytes in the order the CPU reads
	// them (uSwap when the surface format holds them the other way), so the
	// packed pixels never need swapping on the CPU.
	bool pack16Swapped() const { return false; }

	// ---- Copies straight into N64 memory (Context::copyDepthToMemory,
	// ColorBufferReader::copyPixels16ToMemory). GX2 gives a linear 16-bit
	// surface a pitch in steps of 128 pixels (320 becomes 384), and bases it on
	// 256 bytes, so the N64 buffer (W x H pixels at any 2-byte address) is
	// treated as a 128-pixel-wide surface based at the address rounded down,
	// with the pixels shifted by k = the remainder in pixels: buffer pixel i
	// is surface pixel ((i + k) % 128, (i + k) / 128). The pack shaders draw
	// the pixels in that layout (uLayout), in up to three rectangles (the
	// first and last rows partial) so nothing outside the buffer is touched:
	// straight into the RDRAM surface as a colour buffer (method "draw"), or
	// into a texture that GX2CopySurfaceEx then copies (methods "rects" and
	// "one rect"; the console's third run left the first partial row empty
	// with one three-rectangle copy, 2026-10-30). The start-up self-test picks
	// the first method that works. finishMemoryCopies waits for the copies
	// and invalidates the CPU cache over each. Debug word "cpucopy" keeps the
	// CPU paths. ----

	bool memoryCopiesAvailable() override { return memoryCopyReady() && !debugWord("cpucopy"); }

	// Texture _name's first _h rows, packed to N64 RGBA5551, into _dst.
	bool copyPacked16ToMemory(u32 _name, void * _dst, u32 _w, u32 _h) {
		if (!memoryCopiesAvailable())
			return false;
		Texture * t = texture(ObjectHandle(_name));
		if (t == nullptr || !t->allocated || t->tex.surface.width < _w || t->tex.surface.height < _h)
			return false;
		u32 k = 0;
		if (!layoutShift(_dst, _w, _h, k, "color"))
			return false;
		finishRenderTarget();
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, t->tex.surface.image, t->tex.surface.imageSize);
		// GLideN64 dithers 16-bit RDRAM images unless the dithering pattern is on
		// at native resolution. The GPU uses the Bayer matrix for every mode.
		const bool dither = config.generalEmulation.rdramImageDitheringMode != Config::BufferDitheringMode::bdmDisable &&
			(config.generalEmulation.enableDitheringPattern == 0 || config.frameBufferEmulation.nativeResFactor != 1);
		const bool ok = packLayout(*t, _w, _h, k, dither, _dst, "color");
		restoreAfterClear();
		return ok;
	}

	bool copyDepthToMemory(const Context::DepthToMemoryParams & _p) override {
		if (!memoryCopiesAvailable() || _p.dstWidth == 0 || _p.dstHeight == 0 || _p.srcWidth <= 0 || _p.srcHeight <= 0)
			return false;
		const u32 fbo = u32(_p.fbo);
		// Everything that can fail comes before the GPU state changes.
		Texture * dt = depthTextureOf(fbo);
		GX2DepthBuffer * db = dt != nullptr ? (ensureDepthBuffer(*dt) ? &dt->db : nullptr) : depthBufferOf(fbo);
		if (db == nullptr || db->surface.image == nullptr)
			return false;
		u32 k = 0;
		if (!layoutShift(_p.dst, _p.dstWidth, _p.dstHeight, k, "depth"))
			return false;
		if (m_depthPackProgram == 0 && !createDepthPackProgram())
			return false;
		Texture * src = dt;
		if (dt == nullptr) {
			const GX2Surface & ss = db->surface;
			if (!m_depthScratch.allocated || m_depthScratch.tex.surface.width != ss.width ||
					m_depthScratch.tex.surface.height != ss.height) {
				if (m_depthScratch.allocated)
					freeLater(m_depthScratch.tex.surface.image);
				allocateTexture(m_depthScratch, ss.width, ss.height, GX2_SURFACE_FORMAT_FLOAT_R32);
				if (!m_depthScratch.allocated)
					return false;
			}
			src = &m_depthScratch;
		}
		convertDepth(*db, src->tex.surface);
		if (dt != nullptr)
			dt->dbDirty = false;
		Program * previous = currentProgram();
		glUseProgram(m_depthPackProgram);
		// Texels per N64 pixel, as texture coordinates.
		glUniform2f(m_depthPackTexelLocation, f32(_p.srcWidth) / f32(_p.dstWidth) / f32(src->tex.surface.width),
			f32(_p.srcHeight) / f32(_p.dstHeight) / f32(src->tex.surface.height));
		glUniform1f(m_depthPackSwapLocation, m_packSwap ? 1.0f : 0.0f);
		const bool ok = renderLayout(*src, _p.dstWidth, _p.dstWidth * _p.dstHeight, k, m_depthPackLayoutLocation,
			m_depthPackTargetLocation, _p.dst, "depth");
		glUseProgram(previous != nullptr ? previous->name : 0);
		restoreAfterClear();
		return ok;
	}

	void finishMemoryCopies() override {
		if (m_memoryCopies.empty())
			return;
		const OSTime t0 = OSGetSystemTime();
		const OSTime retired = GX2GetRetiredTimeStamp();
		u32 waited = 0;
		for (const MemoryCopy & c : m_memoryCopies) {
			if (c.stamp > retired) {
				++waited;
				GX2WaitTimeStamp(c.stamp);
			}
			DCInvalidateRange(c.dst, c.bytes);
		}
		m_memoryCopiesSettled += u32(m_memoryCopies.size());
		m_memoryCopiesWaited += waited;
		m_memoryCopyWaitUs += OSTicksToMicroseconds(OSGetSystemTime() - t0);
		m_memoryCopies.clear();
		if (++m_memoryCopySettles % 300 == 1) {
			gx2::log("N64 memory copies: %u settled in %u rounds, %u still on the GPU when settled, %.0f us waiting in all\n",
				m_memoryCopiesSettled, m_memoryCopySettles, m_memoryCopiesWaited, m_memoryCopyWaitUs);
		}
	}

private:
	bool debugWord(const char * _word) const { return our64_gx2_debug != nullptr && strstr(our64_gx2_debug, _word) != nullptr; }

	static const u32 kLayoutPitch = 128; // pixels: 256 bytes, GX2's pitch step for a linear 16-bit surface

	enum MemoryCopyMethod { kMemoryCopyDraw, kMemoryCopyRects, kMemoryCopyOneRect, kMemoryCopyMethods };
	static const char * memoryCopyMethodName(MemoryCopyMethod _m) {
		return _m == kMemoryCopyDraw ? "draw" : _m == kMemoryCopyRects ? "rects" : "one rect";
	}

	struct MemoryCopy {
		void * dst;
		u32 bytes;
		OSTime stamp;
	};

	// The shift in pixels of a buffer at _dst from the 256-byte base below it.
	// The range must be 32-byte aligned at both ends, so the cache operations
	// touch nothing beside it.
	bool layoutShift(const void * _dst, u32 _w, u32 _h, u32 & _k, const char * _what) {
		const uintptr_t a = uintptr_t(_dst);
		const u32 bytes = _w * _h * 2;
		if (_w == 0 || _h == 0 || (a & 31) != 0 || (bytes & 31) != 0) {
			if (m_memoryCopyRefusals++ < 4)
				gx2::log("CHECK: %s copy of %ux%u into %p refused (alignment or size)\n", _what, _w, _h, _dst);
			return false;
		}
		_k = u32(a & 0xFF) / 2;
		return true;
	}

	// The rectangles of the layout that hold buffer pixels 0.._n with shift _k:
	// the first row from _k, full rows, the last row to the end.
	static u32 layoutRegions(u32 _n, u32 _k, GX2Rect _rects[3]) {
		const u32 end = _k + _n;
		const u32 lastRow = (end - 1) / kLayoutPitch;
		u32 n = 0;
		auto add = [&](u32 x0, u32 y0, u32 x1, u32 y1) { _rects[n++] = {s32(x0), s32(y0), s32(x1), s32(y1)}; };
		if (lastRow == 0) {
			add(_k, 0, end, 1);
		} else {
			add(_k, 0, kLayoutPitch, 1);
			if (lastRow > 1)
				add(0, 1, kLayoutPitch, lastRow);
			add(0, lastRow, end - lastRow * kLayoutPitch, lastRow + 1);
		}
		return n;
	}

	// The linear surface over the buffer at _dst (shift _k, _rows rows), based
	// 2 * _k bytes below it. False when GX2 wants another pitch or alignment.
	bool linearSurface(void * _dst, u32 _k, u32 _rows, GX2Surface & _s, const char * _what) {
		_s = GX2Surface();
		_s.dim = GX2_SURFACE_DIM_TEXTURE_2D;
		_s.width = kLayoutPitch;
		_s.height = _rows;
		_s.depth = 1;
		_s.mipLevels = 1;
		_s.format = m_packFormat;
		_s.aa = GX2_AA_MODE1X;
		_s.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
		_s.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
		GX2CalcSurfaceSizeAndAlignment(&_s);
		const uintptr_t base = uintptr_t(_dst) - _k * 2;
		if (_s.pitch != kLayoutPitch || (base % _s.alignment) != 0) {
			if (m_memoryCopyRefusals++ < 4)
				gx2::log("CHECK: %s copy refused: GX2 wants pitch %u and alignment %u\n", _what, _s.pitch, _s.alignment);
			return false;
		}
		_s.image = reinterpret_cast<void*>(base);
		return true;
	}

	void recordMemoryCopy(void * _dst, u32 _n, u32 _k, u32 _rows, const char * _what) {
		GX2Flush();
		m_memoryCopies.push_back({_dst, _n * 2, GX2GetLastSubmittedTimeStamp()});
		++m_memoryCopyCount;
		if (m_memoryCopyCount <= 6 || m_memoryCopyCount % 300 == 0)
			gx2::log("%s copy %u (%s): %u pixels into %p (shift %u pixels, %u rows), %zu pending\n", _what, m_memoryCopyCount,
				memoryCopyMethodName(m_memoryCopyMethod), _n, _dst, _k, _rows, m_memoryCopies.size());
	}

	// Draws _src's _n pixels (rows of _w) with the current program (a pack
	// program: its uLayout and uTarget at the locations given) in the layout,
	// pixel i at ((i + _k) % pitch, (i + _k) / pitch): straight into the
	// buffer at _dst (method draw), or into the layout texture, which is
	// then copied into _dst (the other methods).
	bool renderLayout(Texture & _src, u32 _w, u32 _n, u32 _k, GLint _layoutLocation, GLint _targetLocation, void * _dst,
	                  const char * _what) {
		const u32 rows = (_n + _k + kLayoutPitch - 1) / kLayoutPitch;
		glUniform4i(_layoutLocation, s32(kLayoutPitch), s32(_k), s32(_w), 0);
		glUniform2f(_targetLocation, f32(kLayoutPitch), f32(rows));
		GX2Rect rects[3];
		const u32 regions = layoutRegions(_n, _k, rects);
		if (m_memoryCopyMethod == kMemoryCopyDraw) {
			GX2ColorBuffer cb = {};
			if (!linearSurface(_dst, _k, rows, cb.surface, _what))
				return false;
			cb.viewNumSlices = 1;
			GX2InitColorBufferRegs(&cb);
			// Dirty lines over the range are written back now, not over the GPU's pixels later.
			DCFlushRange(_dst, _n * 2);
			traceBegin(_what, rows);
			for (u32 r = 0; r < regions; ++r)
				drawCopy(*currentProgram(), _src, cb, &rects[r]);
			traceEnd();
			GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, cb.surface.image, cb.surface.imageSize);
			recordMemoryCopy(_dst, _n, _k, rows, _what);
			return true;
		}
		if (!m_layoutTarget.allocated || m_layoutTarget.tex.surface.height != rows) {
			if (m_layoutTarget.allocated)
				freeLater(m_layoutTarget.tex.surface.image);
			allocateTexture(m_layoutTarget, kLayoutPitch, rows, m_packFormat, false);
			if (!m_layoutTarget.allocated)
				return false;
		}
		drawCopy(*currentProgram(), _src, *colorBufferOf(m_layoutTarget));
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, m_layoutTarget.tex.surface.image, m_layoutTarget.tex.surface.imageSize);
		GX2Surface linear;
		if (!linearSurface(_dst, _k, rows, linear, _what))
			return false;
		DCFlushRange(_dst, _n * 2);
		GX2Point points[3];
		for (u32 r = 0; r < regions; ++r)
			points[r] = {rects[r].left, rects[r].top};
		traceBegin(_what, rows);
		if (m_memoryCopyMethod == kMemoryCopyRects) {
			for (u32 r = 0; r < regions; ++r)
				GX2CopySurfaceEx(&m_layoutTarget.tex.surface, 0, 0, &linear, 0, 0, 1, &rects[r], &points[r]);
		} else {
			GX2CopySurfaceEx(&m_layoutTarget.tex.surface, 0, 0, &linear, 0, 0, regions, rects, points);
		}
		traceEnd();
		recordMemoryCopy(_dst, _n, _k, rows, _what);
		return true;
	}

	// _src packed to N64 RGBA5551 into the buffer at _dst (see renderLayout).
	bool packLayout(Texture & _src, u32 _w, u32 _h, u32 _k, bool _dither, void * _dst, const char * _what) {
		testPack16();
		if (m_packFormat == GX2_SURFACE_FORMAT_INVALID || (m_packProgram == 0 && !createPackProgram()))
			return false;
		Program * previous = currentProgram();
		glUseProgram(m_packProgram);
		glUniform1f(m_packDitherLocation, _dither ? 1.0f : 0.0f);
		glUniform1f(m_packSwapLocation, m_packSwap ? 1.0f : 0.0f);
		const bool ok = renderLayout(_src, _w, _w * _h, _k, m_packLayoutLocation, m_packTargetLocation, _dst, _what);
		glUseProgram(previous != nullptr ? previous->name : 0);
		return ok;
	}

	bool memoryCopyReady() {
		if (!m_memoryCopyTested) {
			m_memoryCopyTested = true;
			testMemoryCopy();
		}
		return m_memoryCopyOk;
	}

	// A GPU-drawn pattern packed and copied into a heap buffer at the four
	// 64-byte offsets within 256, checked pixel by pixel with guard bytes
	// around it, with each method in turn until one passes. A failure of all
	// keeps the CPU paths.
	void testMemoryCopy() {
		testPack16();
		if (m_packFormat == GX2_SURFACE_FORMAT_INVALID || (m_patternProgram == 0 && !createPatternProgram()))
			return;
		const u32 w = 320, h = 4, bytes = w * h * 2;
		// An RGBA8 texture, as the colour copy's: a 16-bit pack-format texture
		// samples its channels in the reverse order from rendering into it
		// (the console's second self-test read F940 for 000B, 2026-10-30).
		Texture pattern;
		allocateTexture(pattern, w, h, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8);
		if (!pattern.allocated)
			return;
		Program * previous = currentProgram();
		glUseProgram(m_patternProgram);
		glUniform2f(m_patternTargetLocation, f32(w), f32(h));
		drawCopy(*currentProgram(), m_dummy, *colorBufferOf(pattern));
		glUseProgram(previous != nullptr ? previous->name : 0);
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, pattern.tex.surface.image, pattern.tex.surface.imageSize);
		const u32 total = 256 + 256 + bytes + 256;
		u8 * buf = static_cast<u8*>(MEMAllocFromDefaultHeapEx(total, 256));
		if (buf == nullptr) {
			freeLater(pattern.tex.surface.image);
			restoreAfterClear();
			return;
		}
		// The pattern program writes (x + y * 320) * 37 + 11 for the pixel at x, y.
		auto want = [](u32 i) { return u16((i * 37 + 11) & 0xFFFF); };
		for (u32 m = 0; m < kMemoryCopyMethods && !m_memoryCopyOk; ++m) {
			m_memoryCopyMethod = MemoryCopyMethod(m);
			bool ok = true;
			char why[240] = "";
			for (u32 offset = 0; offset < 256 && ok; offset += 64) {
				memset(buf, 0xA5, total);
				DCFlushRange(buf, total);
				u8 * dst = buf + 256 + offset;
				const u32 k = offset / 2;
				m_memoryCopyRefusals = 0;
				if (!packLayout(pattern, w, h, k, false, dst, "self-test")) {
					snprintf(why, sizeof(why), "refused at offset %u", offset);
					ok = false;
					break;
				}
				finishMemoryCopies();
				DCInvalidateRange(buf, total);
				const u16 * got = reinterpret_cast<const u16*>(dst);
				u32 wrong = 0, wrongFirstRow = 0, first = 0;
				for (u32 i = 0; i < w * h; ++i) {
					if (got[i] == want(i))
						continue;
					if (wrong++ == 0)
						first = i;
					wrongFirstRow += i + k < kLayoutPitch;
				}
				if (wrong != 0) {
					// Which buffer pixel's value the first wrong pixel holds, if any: a shifted copy shows here.
					s32 holds = -1;
					for (u32 j = 0; j < w * h && holds < 0; ++j)
						if (got[first] == want(j))
							holds = s32(j);
					snprintf(why, sizeof(why), "offset %u: %u of %u pixels wrong (%u in the first row); pixel %u reads %04X, wants %04X"
						", holds pixel %d's value", offset, wrong, w * h, wrongFirstRow, first, got[first], want(first), holds);
					ok = false;
				}
				for (u32 i = 0; i < 256 && ok; ++i) {
					if (buf[offset + i] != 0xA5 || buf[256 + offset + bytes + i] != 0xA5) {
						snprintf(why, sizeof(why), "offset %u: guard byte %u %s the buffer changed", offset, i,
							buf[offset + i] != 0xA5 ? "before" : "after");
						ok = false;
					}
				}
			}
			m_memoryCopyCount = 0;
			m_memoryCopyRefusals = 0;
			if (ok) {
				m_memoryCopyOk = true;
				gx2::log("GPU copies into N64 memory: self-test passed at offsets 0, 64, 128 and 192 with method %s\n",
					memoryCopyMethodName(m_memoryCopyMethod));
			} else {
				gx2::log("CHECK: GPU copies into N64 memory, method %s: %s\n", memoryCopyMethodName(m_memoryCopyMethod), why);
			}
		}
		if (!m_memoryCopyOk)
			gx2::log("CHECK: GPU copies into N64 memory are off; the CPU copies RDRAM\n");
		MEMFreeToDefaultHeap(buf);
		freeLater(pattern.tex.surface.image);
		restoreAfterClear();
	}

	// The pack programs' common parts. The pixel drawn is p (from the quad's
	// texture coordinates, so row 0 is the surface's first row whatever
	// gl_FragCoord's origin); s is the source pixel it packs: p itself, or
	// with uLayout = (pitch, k, W, 0) the buffer pixel i = p.y * pitch + p.x - k
	// at (i % W, i / W). The epilogue writes the 16-bit value v to the pack
	// format's channels: red holds bits 15..11, green 10..6, blue 5..1 and
	// alpha bit 0 of the 16-bit word as the GPU stores it, and uSwap exchanges
	// the two bytes first when the CPU reads the word the other way round.
	static const char * packPrologue() {
		return "uniform ivec4 uLayout;\nuniform vec2 uTarget;\nuniform float uSwap;\nin vec2 vUV;\nout vec4 fragColor;\n"
			"ivec2 sourcePixel(out ivec2 p) {\n"
			"  p = ivec2(vUV * uTarget);\n"
			"  if (uLayout.x == 0) return p;\n"
			"  int i = p.y * uLayout.x + p.x - uLayout.y;\n"
			"  return ivec2(i % uLayout.z, i / uLayout.z);\n"
			"}\n";
	}

	static const char * packEpilogue() {
		return "  if (uSwap > 0.5) v = ((v & 0xFF) << 8) | (v >> 8);\n"
			"  fragColor = vec4(float((v >> 11) & 31) / 31.0, float((v >> 6) & 31) / 31.0, float((v >> 1) & 31) / 31.0, float(v & 1));\n"
			"}\n";
	}

	static const char * packVertexShader() {
		return "#version 330\nin vec4 aRectPosition;\nin vec2 aTexCoord0;\nout vec2 vUV;\n"
			"void main() { gl_Position = vec4(aRectPosition.xy, 0.0, 1.0); vUV = aTexCoord0; }\n";
	}

	bool linkPackProgram(const std::string & _ps, GLuint & _program, GLint & _layout, GLint & _target, GLint & _swap) {
		const GLuint program = linkProgram(packVertexShader(), _ps.c_str());
		if (program == 0)
			return false;
		_program = program;
		glUseProgram(program);
		_layout = glGetUniformLocation(program, "uLayout");
		_target = glGetUniformLocation(program, "uTarget");
		_swap = glGetUniformLocation(program, "uSwap");
		glUniform1i(glGetUniformLocation(program, "uTex0"), 0);
		glUniform4i(_layout, 0, 0, 0, 0);
		return true;
	}

	// N64 16-bit depth from the frame's depth texture (uTexel: texels per N64
	// pixel, as texture coordinates), as DepthBufferToRDRAM's _FloatToUInt16
	// and DepthBufferList's zLUT compute it.
	bool createDepthPackProgram() {
		std::string ps = "#version 330\nuniform sampler2D uTex0;\nuniform vec2 uTexel;\n";
		ps += packPrologue();
		ps += "void main() {\n"
			"  ivec2 p;\n"
			"  ivec2 s = sourcePixel(p);\n"
			"  float z = texture(uTex0, (vec2(s) + 0.5) * uTexel).r;\n"
			"  int idx = 0x3FFFF;\n"
			"  if (z < 0.0) idx = 0; else if (z < 1.0) idx = min(0x3FFFF, int(floor(z * 262144.0 + 0.5)));\n"
			"  int e = 0;\n"
			"  if ((idx & 0x20000) != 0) { e = 1; if ((idx & 0x10000) != 0) { e = 2; if ((idx & 0x8000) != 0) { e = 3;\n"
			"    if ((idx & 0x4000) != 0) { e = 4; if ((idx & 0x2000) != 0) { e = 5; if ((idx & 0x1000) != 0) { e = 6;\n"
			"    if ((idx & 0x800) != 0) e = 7; } } } } } }\n"
			"  int mantissa = (idx >> (6 - min(6, e))) & 0x7FF;\n"
			"  int v = ((e << 11) | mantissa) << 2;\n";
		ps += packEpilogue();
		if (!linkPackProgram(ps, m_depthPackProgram, m_depthPackLayoutLocation, m_depthPackTargetLocation, m_depthPackSwapLocation))
			return false;
		m_depthPackTexelLocation = glGetUniformLocation(m_depthPackProgram, "uTexel");
		return true;
	}

	// The self-test's pattern: (x + y * 320) * 37 + 11 at pixel x, y, as the
	// colour (r, g, b in 31ths, a) the pack program packs back to that value
	// (no swap here: the pack program swaps when it packs it).
	bool createPatternProgram() {
		std::string ps = "#version 330\n";
		ps += packPrologue();
		ps += "void main() {\n"
			"  ivec2 p;\n"
			"  sourcePixel(p);\n"
			"  int v = ((p.x + p.y * 320) * 37 + 11) & 0xFFFF;\n";
		ps += packEpilogue();
		GLint swap = -1;
		if (!linkPackProgram(ps, m_patternProgram, m_patternLayoutLocation, m_patternTargetLocation, swap))
			return false;
		glUniform1f(swap, 0.0f);
		return true;
	}

	// Which GX2 16-bit format holds N64 RGBA5551 (r << 11 | g << 6 | b << 1 | a)
	// as the CPU reads it: cleared to known colors and read back once.
	void testPack16() {
		if (m_packTested)
			return;
		m_packTested = true;
		const GX2SurfaceFormat candidates[2] = {GX2_SURFACE_FORMAT_UNORM_A1_B5_G5_R5, GX2_SURFACE_FORMAT_UNORM_R5_G5_B5_A1};
		for (GX2SurfaceFormat f : candidates) {
			Texture t;
			allocateTexture(t, 8, 8, f);
			ReadbackCopy c;
			u16 got[2] = {0, 0};
			const f32 colors[2][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f, 0.0f}};
			bool ok = t.allocated;
			for (int i = 0; i < 2 && ok; ++i) {
				GX2ClearColor(colorBufferOf(t), colors[i][0], colors[i][1], colors[i][2], colors[i][3]);
				restoreAfterClear();
				GX2Surface linear = t.tex.surface;
				linear.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
				linear.use = GX2_SURFACE_USE_TEXTURE;
				linear.image = nullptr;
				linear.mipmaps = nullptr;
				GX2CalcSurfaceSizeAndAlignment(&linear);
				linear.image = MEMAllocFromDefaultHeapEx(linear.imageSize, linear.alignment);
				if (linear.image == nullptr) { ok = false; break; }
				DCInvalidateRange(linear.image, linear.imageSize);
				GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, t.tex.surface.image, t.tex.surface.imageSize);
				GX2CopySurface(&t.tex.surface, 0, 0, &linear, 0, 0);
				GX2Flush();
				GX2DrawDone();
				DCInvalidateRange(linear.image, linear.imageSize);
				got[i] = static_cast<const u16*>(linear.image)[0];
				MEMFreeToDefaultHeap(linear.image);
				restoreAfterClear();
			}
			if (t.allocated)
				MEMFreeToDefaultHeap(t.tex.surface.image);
			gx2::log("16-bit readback format %u: red reads %04X, blue %04X (N64 wants F801, 003E)\n", u32(f), got[0], got[1]);
			if (ok && got[0] == 0xF801 && got[1] == 0x003E) {
				m_packFormat = f;
				return;
			}
			if (ok && got[0] == 0x01F8 && got[1] == 0x3E00) {
				gx2::log("16-bit readback format %u matches byte-swapped; swapped while copying\n", u32(f));
				m_packFormat = f;
				m_packSwap = true;
				return;
			}
		}
		gx2::log("CHECK: no 16-bit format matches N64 pixels; the CPU converts readback\n");
	}

	// Draws _src into a 16-bit target as N64 RGBA5551, as ColorBufferToRDRAM::_RGBAtoRGBA16 does on the CPU.
	const GX2Surface * pack16(Texture & _src) {
		testPack16();
		Program * previous = currentProgram();
		if (m_packFormat == GX2_SURFACE_FORMAT_INVALID || (m_packProgram == 0 && !createPackProgram())) {
			glUseProgram(previous != nullptr ? previous->name : 0);
			return nullptr;
		}
		const GX2Surface & ss = _src.tex.surface;
		if (!m_packTarget.allocated || m_packTarget.tex.surface.width != ss.width || m_packTarget.tex.surface.height != ss.height) {
			if (m_packTarget.allocated)
				freeLater(m_packTarget.tex.surface.image);
			allocateTexture(m_packTarget, ss.width, ss.height, m_packFormat);
			if (!m_packTarget.allocated)
				return nullptr;
		}
		glUseProgram(m_packProgram);
		// GLideN64 dithers 16-bit RDRAM images unless the dithering pattern is on
		// at native resolution. The GPU uses the Bayer matrix for every mode.
		const bool dither = config.generalEmulation.rdramImageDitheringMode != Config::BufferDitheringMode::bdmDisable &&
			(config.generalEmulation.enableDitheringPattern == 0 || config.frameBufferEmulation.nativeResFactor != 1);
		glUniform1f(m_packDitherLocation, dither ? 1.0f : 0.0f);
		glUniform1f(m_packSwapLocation, m_packSwap ? 1.0f : 0.0f);
		glUniform4i(m_packLayoutLocation, 0, 0, 0, 0); // one texel a pixel
		glUniform2f(m_packTargetLocation, f32(ss.width), f32(ss.height));
		drawCopy(*currentProgram(), _src, *colorBufferOf(m_packTarget));
		glUseProgram(previous != nullptr ? previous->name : 0);
		GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, m_packTarget.tex.surface.image, m_packTarget.tex.surface.imageSize);
		return &m_packTarget.tex.surface;
	}

	bool createPackProgram() {
		// GLideN64's thresholdMapBayer, indexed [x & 3][y & 3].
		std::string ps = "#version 330\nuniform sampler2D uTex0;\nuniform float uDither;\n"
			"const float bayer[16] = float[16](-4.0, 2.0, -3.0, 4.0, 0.0, -2.0, 2.0, -1.0, -3.0, 3.0, -4.0, 3.0, 1.0, -1.0, 1.0, -2.0);\n";
		ps += packPrologue();
		ps += "void main() {\n"
			"  ivec2 p;\n"
			"  ivec2 s = sourcePixel(p);\n"
			"  vec4 c = floor(texelFetch(uTex0, s, 0) * 255.0 + 0.5);\n"
			"  ivec2 d = s & 3;\n"
			"  float t = bayer[d.x * 4 + d.y] * uDither;\n"
			"  ivec3 rgb = ivec3(floor(clamp(c.rgb + t, 0.0, 255.0) / 8.0));\n"
			"  int v = (rgb.r << 11) | (rgb.g << 6) | (rgb.b << 1) | (c.a > 0.0 ? 1 : 0);\n";
		ps += packEpilogue();
		if (!linkPackProgram(ps, m_packProgram, m_packLayoutLocation, m_packTargetLocation, m_packSwapLocation))
			return false;
		m_packDitherLocation = glGetUniformLocation(m_packProgram, "uDither");
		return true;
	}

	GLuint linkProgram(const char * _vs, const char * _ps) {
		const GLuint v = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(v, 1, &_vs, nullptr);
		const GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(f, 1, &_ps, nullptr);
		const GLuint program = glCreateProgram();
		glAttachShader(program, v);
		glAttachShader(program, f);
		glLinkProgram(program);
		glDeleteShader(v);
		glDeleteShader(f);
		GLint linked = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (linked != GL_TRUE) {
			gx2::log("CHECK: a backend program did not link\n");
			return 0;
		}
		return program;
	}

	// Draws _src over the whole of _dst with _p (a rectangle-layout program
	// whose sampler reads unit 0), point sampled, no depth or blending.
	void drawCopy(Program & _p, Texture & _src, GX2ColorBuffer & _dst, const GX2Rect * _scissor = nullptr) {
		finishRenderTarget();
		GX2SetColorBuffer(&_dst, GX2_RENDER_TARGET_0);
		GX2SetDepthBuffer(&m_screenDepth);
		GX2SetViewport(0.0f, 0.0f, f32(_dst.surface.width), f32(_dst.surface.height), 0.0f, 1.0f);
		if (_scissor != nullptr)
			GX2SetScissor(u32(_scissor->left), u32(_scissor->top), u32(_scissor->right - _scissor->left), u32(_scissor->bottom - _scissor->top));
		else
			GX2SetScissor(0, 0, _dst.surface.width, _dst.surface.height);
		GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
		GX2SetPolygonControl(GX2_FRONT_FACE_CW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE,
			GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
		GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
		GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
			(GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
		GX2SetShaderMode(_p.mode);
		GX2SetFetchShader(&_p.fetch);
		GX2SetVertexShader(_p.vs);
		GX2SetPixelShader(_p.ps);
		if (!_p.vsRegs.empty())
			GX2SetVertexUniformReg(0, _p.vsRegs.size(), _p.vsRegs.data());
		if (!_p.psRegs.empty())
			GX2SetPixelUniformReg(0, _p.psRegs.size(), _p.psRegs.data());
		_p.vsDirty = _p.psDirty = true;
		for (const SamplerBinding & s : _p.samplers) {
			GX2SetPixelTexture(&_src.tex, s.location);
			GX2SetPixelSampler(&m_dummy.sampler, s.location);
		}
		static const RectVertex quad[4] = {
			{-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
			{ 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f},
			{-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
			{ 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f},
		};
		RectVertex * v = static_cast<RectVertex*>(ring().alloc(sizeof(quad), 64));
		memcpy(v, quad, sizeof(quad));
		attribWritten(v, sizeof(quad));
		GX2SetAttribBuffer(0, sizeof(quad), sizeof(RectVertex), v);
		traceBegin("copy draw", 4);
		GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLE_STRIP, 4, 0, 1);
		traceEnd();
		m_dirty = m_targetDirty = true;
		m_lastProgram = nullptr;
	}

private:

	// Rendering into a texture leaves its pixels in the color buffer cache;
	// flush them and drop stale texture cache lines before anything samples it.
	void finishRenderTarget() {
		GX2ColorBuffer * cb = drawColorBuffer();
		if (cb != nullptr && cb->surface.image != nullptr)
			GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
				cb->surface.image, cb->surface.imageSize);
	}

	void restoreAfterClear() {
		GX2SetContextState(m_ctx);
		m_dirty = m_targetDirty = true;
		m_lastProgram = nullptr;
	}

	void upload(Texture & _t, u32 _x, u32 _y, u32 _width, u32 _height, u32 _format, u32 _type, const void * _data,
	            u32 _level = 0) {
		if (_width == 0 || _height == 0)
			return;
		const u32 levelWidth = std::max(_t.tex.surface.width >> _level, 1u);
		const u32 levelHeight = std::max(_t.tex.surface.height >> _level, 1u);
		if (_t.tex.surface.image == nullptr || _x + _width > levelWidth || _y + _height > levelHeight) {
			gx2::log("CHECK: texture upload %u,%u %ux%u outside a %ux%u texture (memory %p): skipped\n", _x, _y,
				_width, _height, _t.tex.surface.width, _t.tex.surface.height, _t.tex.surface.image);
			return;
		}
		// Debug variant "copypad": the copy's width rounded up to 32 pixels
		// (within the texture), the extra columns repeating the row's last
		// pixel. Resident Evil 2's backgrounds (420, 436 wide) came out as noise
		// or black on the console, right in Cemu (2026-10-26).
		const bool pad = our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "copypad") != nullptr && _x == 0;
		const u32 copyWidth = pad ? std::min<u32>((_width + 31) & ~31u, levelWidth) : _width;
		GX2Surface staging = _t.tex.surface;
		staging.width = copyWidth;
		staging.height = _height;
		staging.mipLevels = 1;
		staging.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
		staging.use = GX2_SURFACE_USE_TEXTURE;
		staging.image = nullptr;
		staging.mipmaps = nullptr;
		GX2CalcSurfaceSizeAndAlignment(&staging);
		u8 * image = static_cast<u8*>(ring().alloc(staging.imageSize, staging.alignment));
		staging.image = image;
		const GX2SurfaceFormat format = staging.format;
		const u32 rowBytes = _width * sourceBytes(_format, _type);
		const u32 srcStride = (rowBytes + m_unpack - 1) / m_unpack * m_unpack;
		const u8 * src = static_cast<const u8*>(_data);
		const u32 dstStride = staging.pitch * surfaceBytes(format);
		if (debug().uniforms && _width * _height <= 64) {
			const u32 bytes = sourceBytes(_format, _type);
			char line[1024];
			int n = snprintf(line, sizeof(line), "tex %ux%u type%u:", _width, _height, _type);
			for (u32 i = 0; i < _width * _height && n < 960; ++i)
				n += snprintf(line + n, sizeof(line) - n, bytes == 4 ? " %08X" : " %04X",
					bytes == 4 ? reinterpret_cast<const u32*>(_data)[i] : u32(reinterpret_cast<const u16*>(_data)[i]));
			gx2::log("%s\n", line);
		}
		const u32 texel = surfaceBytes(format);
		for (u32 y = 0; y < _height; ++y) {
			u8 * row = image + y * dstStride;
			convertRow(row, src + y * srcStride, _width, _format, _type, format);
			for (u32 x = _width; x < copyWidth; ++x)
				memcpy(row + x * texel, row + (_width - 1) * texel, texel);
		}
		GX2Invalidate(GX2_INVALIDATE_MODE_CPU, image, staging.imageSize);
		GX2Rect rect = {0, 0, s32(copyWidth), s32(_height)};
		GX2Point point = {s32(_x), s32(_y)};
		if (tracing())
			gx2::log("trace %u: upload %u,%u %ux%u into %ux%u fmt%u\n", m_traceIndex, _x, _y, _width, _height,
				_t.tex.surface.width, _t.tex.surface.height, u32(_t.tex.surface.format));
		GX2CopySurfaceEx(&staging, 0, 0, &_t.tex.surface, _level, 0, 1, &rect, &point);
		m_profWhat = "texture upload";
		traceEnd();
		GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, _t.tex.surface.image, textureBytes(_t.tex.surface));
		restoreAfterClear();  // the copy is a draw that changes state
	}

	// Applies the shadowed state and the current program; null if nothing can be drawn.
	Program * beginDraw(bool _rect) {
		Program * p = currentProgram();
		if (p == nullptr || !p->linked) {
			logOnce(m_loggedNoProgram, "draw without a linked program (skipped)\n");
			return nullptr;
		}
		if (p->rectLayout != _rect) {
			logOnce(m_loggedLayout, "program and draw use different vertex layouts (skipped)\n");
			return nullptr;
		}
		if (refreshDepthTextures(*p)) {
			restoreAfterClear();
			profMark("depth buffer to texture refresh");
		}
		GX2ColorBuffer * cb = drawColorBuffer();
		if (cb == nullptr)
			return nullptr;
		GX2DepthBuffer * db = drawDepthBuffer();
		if (m_depthTest && m_depthWrite)
			if (Texture * dt = drawDepthTexture())
				dt->dbDirty = true;
		if (Texture * ct = drawColorTexture())
			ct->cbDirty = true;
		if (m_targetDirty) {
			GX2SetColorBuffer(cb, GX2_RENDER_TARGET_0);
			// Dual-source blending reads the shader's second export through
			// target 1's format, so target 1 describes the same buffer (Mesa's
			// r600 driver sets CB_COLOR1_INFO to CB_COLOR0_INFO). Its channel
			// mask stays 0 (applyState): target 1 is never written.
			GX2SetColorBuffer(cb, GX2_RENDER_TARGET_1);
			GX2SetDepthBuffer(db != nullptr ? db : &m_screenDepth);
			m_hasDepth = db != nullptr && db->surface.width >= cb->surface.width && db->surface.height >= cb->surface.height;
			m_targetDirty = false;
			m_dirty = true;
		}
		if (m_dirty) {
			applyState(cb);
			m_dirty = false;
		}
		if (p != m_lastProgram) {
			// DB_SHADER_CONTROL debug variants: "latez" sets Z_ORDER to LATE_Z,
			// "killoff" clears KILL_ENABLE (discards no longer remove pixels).
			u32 db = p->dbShaderControl;
			if (debug().lateZ) db &= ~0x30u;
			if (debug().killOff) db &= ~0x40u;
			p->ps->regs.db_shader_control = db;
			GX2SetShaderMode(p->mode);
			GX2SetFetchShader(&p->fetch);
			GX2SetVertexShader(p->vs);
			GX2SetPixelShader(p->ps);
			p->vsDirty = p->psDirty = true;
			m_lastProgram = p;
		}
		if (p->vsDirty && !p->vsRegs.empty())
			GX2SetVertexUniformReg(0, p->vsRegs.size(), p->vsRegs.data());
		if (p->psDirty && !p->psRegs.empty())
			GX2SetPixelUniformReg(0, p->psRegs.size(), p->psRegs.data());
		p->vsDirty = p->psDirty = false;
		for (const SamplerBinding & s : p->samplers) {
			Texture * t = s.unit < 8 ? texture(ObjectHandle(m_units[s.unit])) : nullptr;
			if (t == nullptr || !t->allocated)
				t = &m_dummy;
			// A texture drawn as a color buffer since it was last sampled: write
			// the color cache back and drop stale texture cache lines first, or
			// the draw reads a mix of old and new pixels (sparkling).
			if (t->cbDirty && !debug().noSampleFlush) {
				profMark("draw setup (before a frame buffer is sampled)");
				profTop();
				GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
					t->tex.surface.image, t->tex.surface.imageSize);
				profMark("cache flush: a frame buffer sampled as a texture");
				t->cbDirty = false;
			}
			if (s.vertex) {
				GX2SetVertexTexture(&t->tex, s.location);
				GX2SetVertexSampler(&t->sampler, s.location);
			} else {
				GX2SetPixelTexture(&t->tex, s.location);
				GX2SetPixelSampler(&t->sampler, s.location);
			}
		}
		return p;
	}

	void applyState(const GX2ColorBuffer * _cb) {
		const u32 width = _cb->surface.width, height = _cb->surface.height;
		if (m_viewport[2] <= 0 || m_viewport[3] <= 0) {
			static unsigned n;
			if (n++ < 4) gx2::log("CHECK: viewport %d,%d %dx%d\n", m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
		}
		GX2SetViewport(f32(m_viewport[0]), f32(m_viewport[1]), f32(m_viewport[2]), f32(m_viewport[3]), 0.0f, 1.0f);
		s32 sx = 0, sy = 0, sw = s32(width), sh = s32(height);
		if (m_scissorTest) {
			sx = m_scissor[0]; sy = m_scissor[1]; sw = m_scissor[2]; sh = m_scissor[3];
		}
		if (sx < 0) { sw += sx; sx = 0; }
		if (sy < 0) { sh += sy; sy = 0; }
		if (sx + sw > s32(width)) sw = s32(width) - sx;
		if (sy + sh > s32(height)) sh = s32(height) - sy;
		if (sw < 0) sw = 0;
		if (sh < 0) sh = 0;
		GX2SetScissor(u32(sx), u32(sy), u32(sw), u32(sh));

		const bool depthTest = m_depthTest && m_hasDepth && !debug().noDepth;
		if (!depthTest && debug().zTestAlways) // the depth unit on, passing everything, writing nothing
			GX2SetDepthOnlyControl(TRUE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
		else
			GX2SetDepthOnlyControl(depthTest, depthTest && m_depthWrite, compareFunction(m_depthFunc));

		// Negating y in the vertex shader reverses the winding, so OpenGL's
		// counter-clockwise front faces are clockwise here.
		const bool cull = m_cull && !debug().noCull;
		const bool cullFront = cull && (m_cullMode == GL_FRONT || m_cullMode == GL_FRONT_AND_BACK);
		const bool cullBack = cull && (m_cullMode == GL_BACK || m_cullMode == GL_FRONT_AND_BACK);
		GX2SetPolygonControl(GX2_FRONT_FACE_CW, cullFront, cullBack, FALSE, GX2_POLYGON_MODE_TRIANGLE,
			GX2_POLYGON_MODE_TRIANGLE, m_polygonOffset, m_polygonOffset, FALSE);
		GX2SetPolygonOffset(m_offsetUnits, m_offsetFactor, m_offsetUnits, m_offsetFactor, 0.0f);

		GX2SetBlendControl(GX2_RENDER_TARGET_0, blendMode(m_blendFactors[0]), blendMode(m_blendFactors[1]),
			GX2_BLEND_COMBINE_MODE_ADD, TRUE, blendMode(m_blendFactors[2]), blendMode(m_blendFactors[3]),
			GX2_BLEND_COMBINE_MODE_ADD);
		GX2SetColorControl(GX2_LOGIC_OP_COPY, m_blend && !debug().noBlend ? 1 : 0, FALSE, TRUE);
		GX2SetBlendConstantColor(m_blendColor[0], m_blendColor[1], m_blendColor[2], m_blendColor[3]);
		// Only target 0 is written. The shader's second export (dual-source
		// blending) only feeds target 0's blend; writing target 1 too, a
		// target with no buffer of its own, is undefined. Mesa's r600 driver
		// enables only the targets it binds.
		GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
			(GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
		GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
		// Shaders write OpenGL clip coordinates (z from -1 to 1). Depth
		// clipping is off in GLideN64's NoClipping mode (draws without a
		// depth test): with z clipped there, Star Wars Episode I Racer's pod and
		// pilot on the select screen were cut away. In NoNearPlaneClipping (the
		// NoN microcodes: Racer, Majora's Mask, Banjo-Tooie, Conker) the
		// OpenGL backend clamps depth and clips only the far plane
		// (gl_ClipDistance); here the far plane alone stays clipped
		// (g_nearClipOff). With both planes clipped, Racer's engines in the
		// cockpit view were cut away (2026-10-22). Debug variant "nonzclip":
		// both planes, as before; "zclipoff": neither.
		const char * d = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
		const bool zClip = !m_depthClamp && m_clampMode != ClampMode::NoClipping && !strstr(d, "zclipoff");
		GX2SetRasterizerClipControlEx(TRUE, zClip, FALSE);
		if (zClip && m_clampMode == ClampMode::NoNearPlaneClipping && !strstr(d, "nonzclip")) {
			if (!g_nearClipOffFlushed) {
				DCFlushRange(g_nearClipOff, sizeof(g_nearClipOff));
				g_nearClipOffFlushed = true;
			}
			GX2CallDisplayList(g_nearClipOff, sizeof(g_nearClipOff));
		}
	}

	opengl::GLInfo m_glInfo;
	opengl::CachedUseProgram m_useProgram;
	std::unique_ptr<glsl::CombinerProgramBuilder> m_combinerProgramBuilder;
	std::unique_ptr<glsl::SpecialShadersFactory> m_specialShadersFactory;
	bool m_builderInaccurate = false;

	GX2ContextState * m_ctx = nullptr;
	Ring m_rings[2];
	u32 m_half = 0;
	OSTime m_frameStamp[2] = {0, 0};
	Ring & ring() { return m_rings[m_half]; }
	Texture m_screen;
	GX2DepthBuffer m_screenDepth = {};
	Texture m_dummy;
	Texture m_present;
	GLuint m_presentProgram = 0;
	Texture m_crt;
	GX2DepthBuffer m_crtDepth = {};
	GX2Sampler m_crtSampler = {};
	GLuint m_crtProgram[2] = {0, 0}; // CRT, Soft CRT
	bool m_crtFailed[2] = {false, false};
	std::unordered_map<u32, Texture> m_textures;
	std::unordered_map<u32, Framebuffer> m_framebuffers;
	std::unordered_map<u32, Renderbuffer> m_renderbuffers;
	std::vector<void*> m_pendingFree[2];
	u32 m_nextName = 1;
	u32 m_units[8] = {};
	u32 m_drawFbo = 0;
	u32 m_readFbo = 0;
	s32 m_unpack = 4;
	Program * m_lastProgram = nullptr;

	ClampMode m_clampMode = ClampMode::ClippingEnabled;
	bool m_dirty = true;
	bool m_targetDirty = true;
	bool m_hasDepth = true;
	bool m_blend = false, m_cull = false, m_depthTest = false, m_scissorTest = false;
	bool m_polygonOffset = false, m_depthClamp = false, m_depthWrite = true;
	u32 m_cullMode = GL_BACK;
	u32 m_depthFunc = GL_LESS;
	s32 m_viewport[4] = {0, 0, 1, 1};
	s32 m_scissor[4] = {0, 0, 1, 1};
	u32 m_blendFactors[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};
	f32 m_blendColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
	f32 m_offsetFactor = 0.0f, m_offsetUnits = 0.0f;

	bool m_loggedMip = false, m_loggedNoProgram = false, m_loggedLayout = false;
	bool m_loggedScaledBlit = false, m_loggedBlitColor = false, m_loggedBlitDepth = false;
	Texture m_depthScratch;       // depth blits that are not whole-surface conversions
	bool m_packTested = false;
	GX2SurfaceFormat m_packFormat = GX2_SURFACE_FORMAT_INVALID; // N64 RGBA5551 as a GX2 format
	bool m_packSwap = false;
	GLuint m_packProgram = 0;
	GLint m_packDitherLocation = -1, m_packSwapLocation = -1, m_packLayoutLocation = -1, m_packTargetLocation = -1;
	Texture m_packTarget;
	// Copies into N64 memory
	GLuint m_depthPackProgram = 0, m_patternProgram = 0;
	GLint m_depthPackTexelLocation = -1, m_depthPackSwapLocation = -1, m_depthPackLayoutLocation = -1, m_depthPackTargetLocation = -1;
	GLint m_patternLayoutLocation = -1, m_patternTargetLocation = -1;
	Texture m_layoutTarget;
	std::vector<MemoryCopy> m_memoryCopies;
	MemoryCopyMethod m_memoryCopyMethod = kMemoryCopyDraw;
	bool m_memoryCopyTested = false, m_memoryCopyOk = false;
	u32 m_memoryCopyCount = 0, m_memoryCopyRefusals = 0;
	u32 m_memoryCopySettles = 0, m_memoryCopiesSettled = 0, m_memoryCopiesWaited = 0;
	double m_memoryCopyWaitUs = 0;
};

GX2ColorBufferReader::~GX2ColorBufferReader()
{
	for (ReadbackCopy (&pair)[2] : m_copies)
		for (ReadbackCopy & c : pair)
			if (c.linear.image != nullptr && g_context != nullptr)
				g_context->release(c.linear.image);
}

// The copy to read: this frame's when synchronous (waiting for the GPU), else
// the previous one, which the GPU has long finished.
const ReadbackCopy * GX2ColorBufferReader::read(bool _pack16, bool _sync)
{
	ReadbackCopy * copies = m_copies[_pack16 ? 1 : 0];
	u32 & cur = m_cur[_pack16 ? 1 : 0];
	if (g_context == nullptr)
		return nullptr;
	// Whatever framebuffer GLideN64 bound for reading, as glReadPixels; the
	// copy texture when none is bound.
	u32 name = g_context->readColorTexture();
	if (name == 0)
		name = u32(m_pTexture->name);
	if (!g_context->copyForReadback(name, _pack16, copies[cur]))
		return nullptr;
	const ReadbackCopy * c = &copies[cur];
	if (!_sync) {
		cur ^= 1;
		c = &copies[cur];
		if (!c->valid || c->linear.format != copies[cur ^ 1].linear.format) {
			static unsigned n;
			if (n++ < 8) gx2::log("color copy reader %p %s slot %u: %s\n", (void *)this, _pack16 ? "16-bit" : "plain",
				cur, !c->valid ? "nothing copied into it yet" : "a different format from the new copy");
			return nullptr;
		}
	}
	GX2WaitTimeStamp(c->stamp);
	DCInvalidateRange(c->linear.image, c->linear.imageSize);
	return c;
}

const u8 * GX2ColorBufferReader::_readPixels(const ReadColorBufferParams & _params, u32 & _heightOffset, u32 & _stride,
                                            size_t & _gpuDataSize)
{
	if (_params.y0 < 0)
		return nullptr;
	const ReadbackCopy * c = read(false, _params.sync);
	if (c == nullptr)
		return nullptr;
	if (surfaceBytes(c->linear.format) != _params.colorFormatBytes) {
		static unsigned n;
		if (n++ < 4) gx2::log("CHECK: readback of a %u-byte texture as %u-byte pixels\n", surfaceBytes(c->linear.format),
			_params.colorFormatBytes);
		return nullptr;
	}
	_heightOffset = u32(_params.y0);
	_stride = c->linear.pitch;
	_gpuDataSize = size_t(c->linear.pitch) * c->linear.height * _params.colorFormatBytes;
	return static_cast<const u8*>(c->linear.image);
}

bool GX2ColorBufferReader::copyPixels16ToMemory(void * _dst, u32 _width, u32 _height)
{
	if (g_context == nullptr || !g_context->canPack16())
		return false;
	u32 name = g_context->readColorTexture();
	if (name == 0)
		name = u32(m_pTexture->name);
	return g_context->copyPacked16ToMemory(name, _dst, _width, _height);
}

const u16 * GX2ColorBufferReader::readPixels16(s32 _x0, s32 _y0, u32 _width, u32 _height, bool _sync)
{
	if (g_context == nullptr || _x0 != 0 || _y0 < 0 || !g_context->canPack16() || (our64_gx2_debug && strstr(our64_gx2_debug, "cpuconvert")))
		return nullptr;
	const ReadbackCopy * c = read(true, _sync);
	// Our64: the color copy's first reads and then one in 300 go to gx2-log.txt,
	// with how many pixels are not black, to see the copy reach N64 memory.
	static unsigned reads;
	const bool note = ++reads <= 6 || reads % 300 == 0;
	if (c == nullptr) {
		if (note) gx2::log("color copy %u: %ux%u from y %d, %s: no earlier copy\n", reads, _width, _height, _y0,
			_sync ? "sync" : "async");
		return nullptr;
	}
	const GX2Surface & l = c->linear;
	const u32 rows = u32(_y0) < l.height ? std::min(_height, l.height - u32(_y0)) : 0;
	const u32 cols = std::min(_width, l.width);
	m_rows16.assign(size_t(_width) * _height, 0);
	const u16 * src = static_cast<const u16*>(l.image);
	const bool swap = g_context->pack16Swapped();
	for (u32 y = 0; y < rows; ++y) {
		const u16 * row = src + size_t(u32(_y0) + y) * l.pitch;
		u16 * out = &m_rows16[size_t(y) * _width];
		if (swap)
			for (u32 x = 0; x < cols; ++x)
				out[x] = __builtin_bswap16(row[x]);
		else
			memcpy(out, row, cols * sizeof(u16));
	}
	if (note) {
		size_t lit = 0;
		for (u16 v : m_rows16)
			lit += (v & 0xFFFE) != 0;
		gx2::log("color copy %u: %ux%u from y %d, %s: %u rows, %zu of %zu pixels not black\n", reads, _width, _height,
			_y0, _sync ? "sync" : "async", rows, lit, m_rows16.size());
	}
	return m_rows16.data();
}

} // namespace

GX2ColorBuffer * screenColorBuffer()
{
	return g_context == nullptr ? nullptr : g_context->screen();
}

GX2ColorBuffer * presentColorBuffer()
{
	return g_context == nullptr ? nullptr : g_context->presentBuffer();
}

GX2ColorBuffer * crtColorBuffer(GX2ColorBuffer * _src, u32 _width, u32 _height, int _soft)
{
	return g_context == nullptr ? nullptr : g_context->crtBuffer(_src, _width, _height, _soft);
}

void endFrame()
{
	if (g_context != nullptr)
		g_context->endFrame();
}

void restoreState()
{
	if (g_context != nullptr)
		g_context->restoreState();
}

void readScreen(u8 * _dest, u32 _width, u32 _height)
{
	if (g_context != nullptr)
		g_context->readScreen(_dest, _width, _height);
}

} // namespace gx2

graphics::ContextImpl * our64_createGX2Context()
{
	return new gx2::GX2ContextImpl;
}

// The player's frame test (main.c): a quiet GPU profile of every 10th frame
// for the run (see profFrame). Marks name the GPU work done since the last
// one; labels are kept as pointers, so they are string literals.
extern "C" void our64_gx2_prof_run(int _on)
{
	if (gx2::g_context != nullptr)
		gx2::g_context->m_profQuiet = _on != 0;
}

extern "C" void our64_gx2_prof_mark(const char * _what)
{
	if (gx2::g_context != nullptr) {
		gx2::g_context->profMark(_what);
		gx2::g_context->profTop(); // the player's next copy follows at once
	}
}

// The run's profile as one line: GPU time a profiled frame, then each kind's
// share, the largest first. Called on the graphics core (it writes the sums).
extern "C" void our64_gx2_prof_report(char * _out, unsigned _size)
{
	if (_size == 0)
		return;
	_out[0] = 0;
	gx2::GX2ContextImpl * c = gx2::g_context;
	if (c == nullptr || c->m_runFrames == 0 || c->m_runTotal == 0) {
		snprintf(_out, _size, "GPU profile: no frames measured");
		return;
	}
	// The counter's rate from gx2.rpl's own conversion (as tv_pass.cpp): 27 MHz on the console.
	const u64 span = 1000000000ull;
	const u64 c0 = GX2GPUTimeToCPUTime(span), c1 = GX2GPUTimeToCPUTime(2 * span);
	const double rate = c1 > c0 ? double(span) / (double(c1 - c0) / double(OSTimerClockSpeed)) : 27000000.0;
	const double frames = double(c->m_runFrames);
	int n = snprintf(_out, _size, "GPU profile, %u frames: %.2f ms busy a frame (most %.2f), idle waiting for commands %.2f ms",
		c->m_runFrames, double(c->m_runTotal) / frames * 1000.0 / rate, double(c->m_runMost) * 1000.0 / rate,
		double(c->m_runIdle) / frames * 1000.0 / rate);
	bool done[32] = {};
	for (u32 shown = 0; shown < c->m_runKinds && n > 0 && unsigned(n) < _size; ++shown) {
		u32 best = 32;
		for (u32 k = 0; k < c->m_runKinds; ++k)
			if (!done[k] && (best == 32 || c->m_runSum[k].gpu > c->m_runSum[best].gpu))
				best = k;
		if (best == 32)
			break;
		done[best] = true;
		const gx2::GX2ContextImpl::ProfSum & s = c->m_runSum[best];
		n += snprintf(_out + n, _size - n, "%s %s %.2f ms (%.1f a frame)", shown == 0 ? ";" : ",", s.what,
			double(s.gpu) / frames * 1000.0 / rate, double(s.count) / frames);
	}
}
