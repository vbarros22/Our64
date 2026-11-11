// Our64 GX2 backend: the OpenGL shader calls GLideN64's GLSL generators make
// (OpenGLContext/GLSL), implemented with CafeGLSL and GX2.
//
// Shader objects only keep their source. glLinkProgram compiles the vertex
// and fragment source as a pair (uniform-register mode) and builds a fetch
// shader for the attributes the vertex shader reads. Compiling takes CafeGLSL
// up to seconds, so it runs on a worker thread (core 2): glLinkProgram returns
// at once, GL_COMPLETION_STATUS_ARB says whether the worker is done (what
// GLideN64's asynchronous shader compilation polls), and every other use of
// the program waits for it. Uniform values are kept
// in per-program images of the vertex and pixel uniform registers;
// gx2_ContextImpl uploads them when it draws with the program.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

#include <deque>
#include <dirent.h>

#include <cafeglsl/CafeGLSLCompiler.h>
#include <coreinit/core.h>
#include <coreinit/event.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/utils.h>

#include <Graphics/OpenGLContext/GLFunctions.h>
#include <RSP.h>
#include "gx2_Internal.h"
#include "gx2_ShaderText.h"

extern const char * our64_gx2_debug;

// Shader programs compiled so far and the time spent, for the player's log.
extern "C" {
unsigned our64_gx2_links;
double our64_gx2_link_us;
// While CafeGLSL compiles: when it started (OSTime) and the source sizes, for
// the app's hang watchdog. 0 when not compiling.
volatile long long our64_gx2_compile_start;
volatile unsigned our64_gx2_compile_vs_bytes, our64_gx2_compile_ps_bytes;
// Time spent in malloc and free per core, when the app counts it (the player
// links with --wrap=malloc); weak, so other apps leave them out.
extern volatile unsigned long long our64_alloc_ticks[3] __attribute__((weak));
extern volatile unsigned our64_alloc_calls[3] __attribute__((weak));
}

namespace gx2 {

void (*blendFuncHook)(u32 _sfactor, u32 _dfactor) = nullptr;

namespace {

struct Shader {
	GLenum type;
	std::string source;
};

// A uniform location: where its value lives in the vertex and pixel
// register images, or which samplers it sets.
struct Location {
	s32 vsOffset = -1;
	s32 psOffset = -1;
	bool vsBool = false;
	bool psBool = false;
	std::vector<u32> samplers;  // indices into Program::samplers
};

// A shader pair for the compile worker; the results are read once done is set.
struct CompileJob {
	std::string vs, ps;
	u64 key = 0;
	GX2VertexShader * vsOut = nullptr;
	GX2PixelShader * psOut = nullptr;
	GX2ShaderMode mode = GX2_SHADER_MODE_UNIFORM_REGISTER;
	bool failed = false;
	std::string log;
	// Where the compile time went: wall clock, the worker's own CPU time, how
	// often it slept (waiting on a lock), and malloc and free.
	u32 wallUs = 0, cpuUs = 0, wakes = 0, allocUs = 0, allocCalls = 0;
	bool background = false;  // a preload: queued after the jobs draws wait for
	volatile bool done = false;
};

struct ProgramObject {
	Program program;
	std::vector<GLuint> shaders;
	std::vector<Location> locations;
	std::map<std::string, GLint> locationByName;
	CompileJob * job = nullptr;  // compiling: link() finishes the program when it is done
};

std::unordered_map<GLuint, Shader> g_shaders;
std::unordered_map<GLuint, ProgramObject *> g_programs;
GLuint g_nextShader = 1;
GLuint g_nextProgram = 1;
ProgramObject * g_current = nullptr;

// Registers (4 components each) one element of a variable of this type takes.
u32 registersPerElement(GX2ShaderVarType _type)
{
	switch (_type) {
	case GX2_SHADER_VAR_TYPE_FLOAT2X2: case GX2_SHADER_VAR_TYPE_FLOAT2X3: case GX2_SHADER_VAR_TYPE_FLOAT2X4:
		return 2;
	case GX2_SHADER_VAR_TYPE_FLOAT3X2: case GX2_SHADER_VAR_TYPE_FLOAT3X3: case GX2_SHADER_VAR_TYPE_FLOAT3X4:
		return 3;
	case GX2_SHADER_VAR_TYPE_FLOAT4X2: case GX2_SHADER_VAR_TYPE_FLOAT4X3: case GX2_SHADER_VAR_TYPE_FLOAT4X4:
		return 4;
	default:
		return 1;
	}
}

bool isBool(GX2ShaderVarType _type)
{
	return _type == GX2_SHADER_VAR_TYPE_BOOL || _type == GX2_SHADER_VAR_TYPE_BOOL2 ||
		_type == GX2_SHADER_VAR_TYPE_BOOL3 || _type == GX2_SHADER_VAR_TYPE_BOOL4;
}

u32 registerImageSize(const GX2UniformVar * _vars, u32 _count)
{
	u32 size = 0;
	for (u32 i = 0; i < _count; ++i) {
		const u32 end = _vars[i].offset + _vars[i].count * registersPerElement(_vars[i].type) * 4;
		if (end > size)
			size = end;
	}
	return size;
}

struct AttributeFormat {
	const char * name;
	u32 offset;
	GX2AttribFormat format;
	u32 mask;
};

const u32 kMaskXYZW = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W);
const u32 kMaskXY01 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
const u32 kMaskX001 = GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);

const AttributeFormat kTriangleAttributes[] = {
	{"aPosition", offsetof(TriangleVertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, kMaskXYZW},
	{"aColor", offsetof(TriangleVertex, r), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, kMaskXYZW},
	{"aTexCoord", offsetof(TriangleVertex, s), GX2_ATTRIB_FORMAT_FLOAT_32_32, kMaskXY01},
	{"aNumLights", offsetof(TriangleVertex, numLights), GX2_ATTRIB_FORMAT_FLOAT_32, kMaskX001},
	{"aModify", offsetof(TriangleVertex, modify), GX2_ATTRIB_FORMAT_SNORM_8_8_8_8, kMaskXYZW},
	// Coverage is off, so the shaders never use it; any data will do.
	{"aBaryCoords", offsetof(TriangleVertex, s), GX2_ATTRIB_FORMAT_FLOAT_32_32, kMaskXY01},
};

const AttributeFormat kRectAttributes[] = {
	{"aRectPosition", offsetof(RectVertex, x), GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32, kMaskXYZW},
	{"aTexCoord0", offsetof(RectVertex, s0), GX2_ATTRIB_FORMAT_FLOAT_32_32, kMaskXY01},
	{"aTexCoord1", offsetof(RectVertex, s1), GX2_ATTRIB_FORMAT_FLOAT_32_32, kMaskXY01},
	{"aBaryCoords", offsetof(RectVertex, s0), GX2_ATTRIB_FORMAT_FLOAT_32_32, kMaskXY01},
};

bool buildFetchShader(Program & _p)
{
	const GX2VertexShader * vs = _p.vs;
	_p.rectLayout = false;
	for (u32 i = 0; i < vs->attribVarCount; ++i)
		if (strcmp(vs->attribVars[i].name, "aRectPosition") == 0)
			_p.rectLayout = true;
	const AttributeFormat * table = _p.rectLayout ? kRectAttributes : kTriangleAttributes;
	const u32 tableSize = _p.rectLayout ? sizeof(kRectAttributes) / sizeof(kRectAttributes[0])
		: sizeof(kTriangleAttributes) / sizeof(kTriangleAttributes[0]);

	GX2AttribStream streams[16];
	u32 count = 0;
	for (u32 i = 0; i < vs->attribVarCount && count < 16; ++i) {
		const GX2AttribVar & var = vs->attribVars[i];
		const AttributeFormat * format = nullptr;
		for (u32 j = 0; j < tableSize; ++j)
			if (strcmp(var.name, table[j].name) == 0)
				format = &table[j];
		if (format == nullptr) {
			_p.log += "unknown vertex attribute ";
			_p.log += var.name;
			_p.log += "\n";
			return false;
		}
		GX2AttribStream & s = streams[count++];
		s.location = var.location;
		s.buffer = 0;
		s.offset = format->offset;
		s.format = format->format;
		s.type = GX2_ATTRIB_INDEX_PER_VERTEX;
		s.aluDivisor = 0;
		s.mask = format->mask;
		s.endianSwap = GX2_ENDIAN_SWAP_DEFAULT;
	}
	const u32 size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
	_p.fetchProgram = static_cast<u8*>(MEMAllocFromDefaultHeapEx(size, GX2_SHADER_PROGRAM_ALIGNMENT));
	if (_p.fetchProgram == nullptr) {
		_p.log += "out of memory for the fetch shader\n";
		return false;
	}
	GX2InitFetchShaderEx(&_p.fetch, _p.fetchProgram, count, streams, GX2_FETCH_SHADER_TESSELLATION_NONE,
		GX2_TESSELLATION_MODE_DISCRETE);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, _p.fetchProgram, size);
	return true;
}

// The compile worker: one thread, one job at a time. CafeGLSL is not known
// to be thread safe, so every call into it holds g_compilerMutex.
OSThread g_compileThread __attribute__((aligned(16)));
u8 g_compileStack[1024 * 1024] __attribute__((aligned(16)));
OSMutex g_compilerMutex, g_jobsMutex;
OSEvent g_jobPosted, g_jobDone;
std::deque<CompileJob *> g_jobs;
bool g_compileThreadStarted = false, g_compileThreadFailed = false;
LinkMode g_linkMode = LinkMode::Normal;
u32 g_deferredLinks = 0;
volatile bool g_compileQuit = false;
volatile bool g_jobRunning = false;

// The calling thread's CPU time (ns) and wake count. The OS updates them when
// the thread is switched out, so this sleeps for a moment first.
void threadTimes(u64 & _cpuNs, u64 & _wakes)
{
	OSSleepTicks(OSMicrosecondsToTicks(50));
	OSThread * self = OSGetCurrentThread();
	_cpuNs = self->coreTimeConsumedNs;
	_wakes = self->wakeCount;
}

void allocTimes(u64 & _ticks, u32 & _calls)
{
	const u32 core = OSGetCoreId();
	_ticks = &our64_alloc_ticks != nullptr && core < 3 ? our64_alloc_ticks[core] : 0;
	_calls = &our64_alloc_calls != nullptr && core < 3 ? our64_alloc_calls[core] : 0;
}

void runJob(CompileJob & _j)
{
	static char compileLog[4096];
	u64 cpu0, wakes0, alloc0;
	u32 calls0;
	threadTimes(cpu0, wakes0);
	allocTimes(alloc0, calls0);
	const OSTime compileStart = OSGetSystemTime();
	our64_gx2_compile_vs_bytes = unsigned(_j.vs.size());
	our64_gx2_compile_ps_bytes = unsigned(_j.ps.size());
	our64_gx2_compile_start = compileStart;
	OSLockMutex(&g_compilerMutex);
	compileLog[0] = '\0';
	const GX2ShaderMode mode = CompileShaderPair(_j.vs.c_str(), _j.ps.c_str(), GLSL_COMPILE_REGISTER,
		&_j.vsOut, &_j.psOut, compileLog, sizeof(compileLog), GLSL_COMPILER_FLAG_NONE);
	OSUnlockMutex(&g_compilerMutex);
	const OSTime compileEnd = OSGetSystemTime();
	u64 cpu1, wakes1, alloc1;
	u32 calls1;
	allocTimes(alloc1, calls1);
	threadTimes(cpu1, wakes1);
	_j.wallUs = u32(OSTicksToMicroseconds(compileEnd - compileStart));
	_j.cpuUs = u32((cpu1 - cpu0) / 1000);
	_j.wakes = u32(wakes1 - wakes0) - 1;
	_j.allocUs = u32(OSTicksToMicroseconds(alloc1 - alloc0));
	_j.allocCalls = calls1 - calls0;
	our64_gx2_compile_start = 0;
	our64_gx2_links++;
	our64_gx2_link_us += OSTicksToMicroseconds(compileEnd - compileStart);
	_j.failed = mode == GLSL_SHADER_MODE_ERROR;
	if (_j.failed)
		_j.log = compileLog;
	else {
		_j.mode = mode;
		// Debug word "notransfix" leaves CafeGLSL's code as it made it (here and as the cache loads).
		if (our64_gx2_debug == nullptr || strstr(our64_gx2_debug, "notransfix") == nullptr) {
			gx2::fixTransSlotWrites(_j.vsOut->program, _j.vsOut->size, _j.vsOut->regs.sq_pgm_resources_vs);
			gx2::fixTransSlotWrites(_j.psOut->program, _j.psOut->size, _j.psOut->regs.sq_pgm_resources_ps);
		}
		shaderCacheStore(_j.key, _j.vsOut, _j.psOut, _j.mode);
	}
}

int compileMain(int, const char **)
{
	while (!g_compileQuit) {
		OSWaitEvent(&g_jobPosted);
		for (;;) {
			OSLockMutex(&g_jobsMutex);
			CompileJob * j = g_jobs.empty() ? nullptr : g_jobs.front();
			if (j != nullptr)
				g_jobs.pop_front();
			g_jobRunning = j != nullptr;
			OSUnlockMutex(&g_jobsMutex);
			if (j == nullptr)
				break;
			runJob(*j);
			__sync_synchronize();
			j->done = true;
			g_jobRunning = false;
			OSSignalEvent(&g_jobDone);
		}
	}
	return 0;
}

// Starts the worker on core 2 below the app's watchdog (priority 20). Without
// it, jobs run on the calling thread.
bool startCompileThread()
{
	if (g_compileThreadStarted || g_compileThreadFailed)
		return g_compileThreadStarted;
	OSInitMutex(&g_compilerMutex);
	OSInitMutex(&g_jobsMutex);
	OSInitEvent(&g_jobPosted, FALSE, OS_EVENT_MODE_AUTO);
	OSInitEvent(&g_jobDone, FALSE, OS_EVENT_MODE_AUTO);
	if (!OSCreateThread(&g_compileThread, (OSThreadEntryPointFn)compileMain, 0, nullptr, g_compileStack + sizeof(g_compileStack),
			sizeof(g_compileStack), 24, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		g_compileThreadFailed = true;
		return false;
	}
	g_compileQuit = false;
	OSSetThreadName(&g_compileThread, "Our64 shader compiler");
	OSResumeThread(&g_compileThread);
	g_compileThreadStarted = true;
	return true;
}

void postJob(CompileJob * _j)
{
	if (!startCompileThread()) {
		runJob(*_j);
		_j->done = true;
		return;
	}
	OSLockMutex(&g_jobsMutex);
	auto at = g_jobs.end();
	if (!_j->background)
		for (at = g_jobs.begin(); at != g_jobs.end() && !(*at)->background; ++at) {}
	g_jobs.insert(at, _j);
	OSUnlockMutex(&g_jobsMutex);
	OSSignalEvent(&g_jobPosted);
}

// Takes a job off the queue if the worker has not started it.
bool cancelJob(CompileJob * _j)
{
	if (!g_compileThreadStarted)
		return false;
	OSLockMutex(&g_jobsMutex);
	bool found = false;
	for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it)
		if (*it == _j) {
			g_jobs.erase(it);
			found = true;
			break;
		}
	OSUnlockMutex(&g_jobsMutex);
	return found;
}

void waitJob(CompileJob & _j)
{
	while (!_j.done)
		OSWaitEvent(&g_jobDone);
	__sync_synchronize();
}

void freeProgram(Program & _p)
{
	if (_p.cached)
		freeCachedShaders(_p.vs, _p.ps);
	else if (_p.vs != nullptr || _p.ps != nullptr) {
		if (g_compileThreadStarted)
			OSLockMutex(&g_compilerMutex);
		FreeShaders(_p.vs, _p.ps);
		if (g_compileThreadStarted)
			OSUnlockMutex(&g_compilerMutex);
	}
	_p.cached = false;
	_p.vs = nullptr;
	_p.ps = nullptr;
	if (_p.fetchProgram != nullptr)
		MEMFreeToDefaultHeap(_p.fetchProgram);
	_p.fetchProgram = nullptr;
	_p.linked = false;
}

// Splits "name[3]" into "name" and 3.
std::string baseName(const char * _name, u32 & _index)
{
	std::string name(_name);
	_index = 0;
	const size_t bracket = name.find('[');
	if (bracket != std::string::npos) {
		_index = static_cast<u32>(atoi(name.c_str() + bracket + 1));
		name.resize(bracket);
	}
	return name;
}

ProgramObject * programObject(GLuint _name)
{
	auto it = g_programs.find(_name);
	return it == g_programs.end() ? nullptr : it->second;
}

Location * currentLocation(GLint _location)
{
	if (g_current == nullptr || _location < 0 || u32(_location) >= g_current->locations.size())
		return nullptr;
	return &g_current->locations[_location];
}

// Writes _count elements of _components values each, one register apart.
void writeUniform(GLint _location, GLsizei _count, u32 _components, const u32 * _values)
{
	Location * loc = currentLocation(_location);
	if (loc == nullptr)
		return;
	Program & p = g_current->program;
	auto write = [&](std::vector<u32> & _regs, s32 _offset, bool _bool, bool & _dirty) {
		if (_offset < 0)
			return;
		for (GLsizei e = 0; e < _count; ++e)
			for (u32 c = 0; c < _components; ++c) {
				const u32 index = u32(_offset) + u32(e) * 4 + c;
				if (index >= _regs.size())
					return;
				// Mesa's booleans are 0 or ~0 on hardware with native integers.
				const u32 value = _bool ? (_values[e * _components + c] != 0 ? ~0u : 0u) : _values[e * _components + c];
				if (_regs[index] != value) {
					_regs[index] = value;
					_dirty = true;
				}
			}
	};
	write(p.vsRegs, loc->vsOffset, loc->vsBool, p.vsDirty);
	write(p.psRegs, loc->psOffset, loc->psBool, p.psDirty);
	if (!loc->samplers.empty() && _count > 0)
		for (u32 s : loc->samplers)
			p.samplers[s].unit = _values[0];
}

u32 bits(f32 _value)
{
	u32 b;
	memcpy(&b, &_value, sizeof(b));
	return b;
}

} // namespace

Program * currentProgram()
{
	return g_current == nullptr ? nullptr : &g_current->program;
}

void setLinkMode(LinkMode _mode)
{
	g_linkMode = _mode;
}

u32 deferredLinks()
{
	return g_deferredLinks;
}

void initCompiler()
{
	static bool ready = false;
	if (!ready)
		InitGLSLCompiler();
	ready = true;
}

namespace {

struct BenchRun {
	const char * source;
	CompileJob times;
	bool ok;
};

int benchMain(int, const char ** _argv)
{
	BenchRun & b = *reinterpret_cast<BenchRun *>(_argv);
	static char compileLog[1024];
	u64 cpu0, wakes0, alloc0, cpu1, wakes1, alloc1;
	u32 calls0, calls1;
	threadTimes(cpu0, wakes0);
	allocTimes(alloc0, calls0);
	const OSTime start = OSGetSystemTime();
	GX2PixelShader * ps = nullptr;
	b.ok = CompilePixelShader(b.source, GLSL_COMPILE_REGISTER, &ps, compileLog, sizeof(compileLog),
		GLSL_COMPILER_FLAG_NONE) != GLSL_SHADER_MODE_ERROR;
	const OSTime end = OSGetSystemTime();
	allocTimes(alloc1, calls1);
	threadTimes(cpu1, wakes1);
	FreePixelShader(ps);
	b.times.wallUs = u32(OSTicksToMicroseconds(end - start));
	b.times.cpuUs = u32((cpu1 - cpu0) / 1000);
	b.times.wakes = u32(wakes1 - wakes0) - 1;
	b.times.allocUs = u32(OSTicksToMicroseconds(alloc1 - alloc0));
	b.times.allocCalls = calls1 - calls0;
	return 0;
}

} // namespace

// Compiles each .frag file in _dir on every core in turn, with the game not
// yet running, and logs the times: what a compile costs on an idle console.
void shaderBench(const char * _dir)
{
	initCompiler();
	DIR * d = opendir(_dir);
	if (d == nullptr)
		return;
	static OSThread thread __attribute__((aligned(16)));
	static u8 stack[1024 * 1024] __attribute__((aligned(16)));
	while (struct dirent * e = readdir(d)) {
		const size_t n = strlen(e->d_name);
		if (n < 6 || strcmp(e->d_name + n - 5, ".frag") != 0)
			continue;
		const std::string path = std::string(_dir) + "/" + e->d_name;
		FILE * f = fopen(path.c_str(), "rb");
		if (f == nullptr)
			continue;
		std::string source;
		char buf[4096];
		size_t got;
		while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
			source.append(buf, got);
		fclose(f);
		for (u32 core : {0u, 1u, 2u}) {
			BenchRun b{source.c_str(), {}, false};
			if (!OSCreateThread(&thread, (OSThreadEntryPointFn)benchMain, 0, reinterpret_cast<char *>(&b), stack + sizeof(stack),
					sizeof(stack), 16, OSThreadAttributes(1u << core)))
				continue;
			OSResumeThread(&thread);
			int result;
			OSJoinThread(&thread, &result);
			log("bench %s on core %u: %s, %u ms (CPU %u ms, %u sleeps, malloc %u ms in %u calls)\n", e->d_name, core,
				b.ok ? "ok" : "failed", b.times.wallUs / 1000, b.times.cpuUs / 1000, b.times.wakes,
				b.times.allocUs / 1000, b.times.allocCalls);
		}
	}
	closedir(d);
}

void stopCompileThread()
{
	if (!g_compileThreadStarted)
		return;
	g_compileQuit = true;
	OSSignalEvent(&g_jobPosted);
	int result;
	OSJoinThread(&g_compileThread, &result);
	g_compileThreadStarted = false;
}

} // namespace gx2

using namespace gx2;

extern const char * our64_gx2_debug;
extern "C" const char * our64_gx2_shader_source_dir;
const char * our64_gx2_shader_source_dir = nullptr;

// Debug variant words that rewrite fragment shaders before they compile, for
// finding which construct the compiler gets wrong: "rw1" writes the clamp as
// mix(), "rw2" makes every lowp/mediump value highp, "nodiscard" drops every
// discard, "earlydiscard" leaves discards where GLideN64 put them (see
// gx2_ShaderText.h, deferDiscards), "killmark" paints each discarded pixel
// instead (see below),
// "nofragdepth" stops writing gl_FragDepth, and "solid" makes 1- and
// 2-cycle combiners (those with the blender) draw opaque red, so only which
// pixels a draw covers is left to see.
std::string debugRewrite(const std::string & _ps)
{
	const char * d = our64_gx2_debug != nullptr ? our64_gx2_debug : "";
	std::string ps = _ps;
	if (strstr(d, "rw1"))
		replaceAll(ps, "texCoord += vClampEn*(clampedCoord-texCoord);", "texCoord = mix(texCoord, clampedCoord, vClampEn);");
	// "hplow" and "hpmed": half of rw2 each (only lowp, only mediump made highp).
	if (strstr(d, "hplow"))
		replaceAll(ps, "lowp ", "highp ");
	if (strstr(d, "hpmed"))
		replaceAll(ps, "mediump ", "highp ");
	if (strstr(d, "rw2")) {
		replaceAll(ps, "lowp ", "highp ");
		replaceAll(ps, "mediump ", "highp ");
		replaceAll(ps, "precision highp", "precision highp");
	}
	if (strstr(d, "nodiscard"))
		replaceAll(ps, "discard;", "{}");
	if (strstr(d, "killmark")) {
		// Each discard instead paints its pixel and ends the shader: the
		// first (alpha test) green, the second (coverage) blue, the third
		// (depth texture) yellow, any other magenta.
		static const char * const kColors[] = {"0.0, 1.0, 0.0", "0.0, 0.0, 1.0", "1.0, 1.0, 0.0", "1.0, 0.0, 1.0"};
		int k = 0;
		for (size_t at = ps.find("discard;"); at != std::string::npos; at = ps.find("discard;", at), ++k) {
			const std::string mark = std::string("{ fragColor = vec4(") + kColors[k < 3 ? k : 3] +
				", 1.0); fragColor1 = vec4(0.0); gl_FragDepth = 0.5; return; }";
			ps.replace(at, 8, mark);
			at += mark.size();
		}
	}
	if (strstr(d, "solid") && ps.find("muxPM") != std::string::npos)
		replaceAll(ps, "gl_FragDepth = fragDepth;",
			"fragColor = vec4(1.0, 0.0, 0.0, 1.0); fragColor1 = vec4(0.0); gl_FragDepth = fragDepth;");
	if (strstr(d, "nofragdepth"))
		replaceAll(ps, "gl_FragDepth = fragDepth;", "");
	// RE2's depth: "nolut" skips the N64 depth table (uZlutImage), "showdepth"
	// paints each pixel with the depth it writes, as grey.
	if (strstr(d, "nolut"))
		replaceAll(ps, "return clamp(float(iN64z)/65532.0, 0.0, 1.0);", "return FragDepth;");
	if (strstr(d, "showdepth"))
		replaceAll(ps, "gl_FragDepth = fragDepth;",
			"fragColor = vec4(vec3(fragDepth), 1.0); fragColor1 = vec4(0.0); gl_FragDepth = fragDepth;");
	if (strstr(d, "showlut"))
		ps = showLut(ps);
	// The simple texture coordinates' 3-point filter: "nofilter" samples once
	// with the sampler ("samplerfilter" in glLinkProgram keeps GLideN64's
	// texture() reads).
	if (strstr(d, "nofilter"))
		replaceAll(ps, "if (uTextureFilterMode == 0) name = texture(tex, texCoord);", "if (true) name = texture(tex, texCoord);");
	// "showtc" paints each pixel of a draw sampling a 16x4 texture (Pokemon
	// Stadium 2's panel borders) with its texture 0 coordinate (red s, green
	// t, after the clamp/wrap/mirror; blue the interpolated s), "showtex"
	// with texture 0's sample: which of the two goes wrong.
	if (ps.find("  texCoord0 = clampWrapMirror(") != std::string::npos && ps.find("READ_TEX(readtex0") != std::string::npos &&
			ps.find("muxPM") != std::string::npos) {
		if (strstr(d, "showtc"))
			replaceAll(ps, "gl_FragDepth = fragDepth;",
				"if (textureSize(uTex0, 0) == ivec2(16, 4)) { fragColor = vec4(texCoord0, clamp(vTexCoord0.x, 0.0, 1.0), 1.0); fragColor1 = vec4(0.0); } gl_FragDepth = fragDepth;");
		// "showsize" paints every draw whose texture 0 is under 320 wide with
		// its size: red width / 64, green height / 64, blue 1.
		if (strstr(d, "showsize"))
			replaceAll(ps, "gl_FragDepth = fragDepth;",
				"if (textureSize(uTex0, 0).x < 320) { fragColor = vec4(vec2(textureSize(uTex0, 0)) / 64.0, 1.0, 1.0); "
				"fragColor1 = vec4(0.0); } gl_FragDepth = fragDepth;");
		// "showfbalpha" paints draws sampling a texture 320 or more wide (frame
		// buffers) with the sample's alpha as grey.
		if (strstr(d, "showfbalpha"))
			replaceAll(ps, "gl_FragDepth = fragDepth;",
				"if (textureSize(uTex0, 0).x >= 320) { fragColor = vec4(readtex0.aaa, 1.0); fragColor1 = vec4(0.0); } "
				"gl_FragDepth = fragDepth;");
		if (strstr(d, "showtex"))
			replaceAll(ps, "gl_FragDepth = fragDepth;",
				"if (textureSize(uTex0, 0) == ivec2(16, 4)) { fragColor = vec4(readtex0.rgb, 1.0); fragColor1 = vec4(0.0); } gl_FragDepth = fragDepth;");
	}
	return ps;
}

extern "C" {

GLuint APIENTRY glCreateShader(GLenum type)
{
	const GLuint name = g_nextShader++;
	g_shaders[name] = Shader{type, std::string()};
	return name;
}

void APIENTRY glShaderSource(GLuint shader, GLsizei count, const GLchar * const * string, const GLint * length)
{
	auto it = g_shaders.find(shader);
	if (it == g_shaders.end())
		return;
	std::string & source = it->second.source;
	source.clear();
	for (GLsizei i = 0; i < count; ++i) {
		if (length != nullptr && length[i] >= 0)
			source.append(string[i], size_t(length[i]));
		else
			source.append(string[i]);
	}
}

void APIENTRY glCompileShader(GLuint) {}

void APIENTRY glDeleteShader(GLuint shader)
{
	g_shaders.erase(shader);
}

void APIENTRY glGetShaderiv(GLuint, GLenum pname, GLint * params)
{
	*params = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}

void APIENTRY glGetShaderInfoLog(GLuint, GLsizei bufSize, GLsizei * length, GLchar * infoLog)
{
	if (length != nullptr)
		*length = 0;
	if (bufSize > 0)
		infoLog[0] = '\0';
}

GLuint APIENTRY glCreateProgram(void)
{
	const GLuint name = g_nextProgram++;
	g_programs[name] = new ProgramObject;
	return name;
}

void APIENTRY glAttachShader(GLuint program, GLuint shader)
{
	if (ProgramObject * po = programObject(program))
		po->shaders.push_back(shader);
}

void APIENTRY glBindAttribLocation(GLuint, GLuint, const GLchar *) {}

void APIENTRY glProgramParameteri(GLuint, GLenum, GLint) {}

// The second half of linking, once the shaders exist: fetch shader, uniform
// register images, samplers.
static void finishLink(GLuint _name, ProgramObject & _po)
{
	Program & p = _po.program;
	if (p.mode == GLSL_SHADER_MODE_ERROR) {
		p.vs = nullptr;
		p.ps = nullptr;
		return;
	}
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.vs->program, p.vs->size);
	GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p.ps->program, p.ps->size);
	if (!buildFetchShader(p)) {
		gx2::log("link %u: %s", _name, p.log.c_str());
		freeProgram(p);
		return;
	}
	p.dbShaderControl = p.ps->regs.db_shader_control;
	p.vsRegs.assign(registerImageSize(p.vs->uniformVars, p.vs->uniformVarCount), 0);
	p.psRegs.assign(registerImageSize(p.ps->uniformVars, p.ps->uniformVarCount), 0);
	p.vsDirty = p.psDirty = true;
	p.samplers.clear();
	for (u32 i = 0; i < p.vs->samplerVarCount; ++i)
		p.samplers.push_back(SamplerBinding{p.vs->samplerVars[i].location, 0, true});
	for (u32 i = 0; i < p.ps->samplerVarCount; ++i)
		p.samplers.push_back(SamplerBinding{p.ps->samplerVars[i].location, 0, false});
	_po.locations.clear();
	_po.locationByName.clear();
	p.linked = true;
	p.name = _name;
}

// Takes a finished job's shaders into the program (waiting for the worker
// unless _poll), then finishes linking. False while it is still compiling.
static bool collectJob(GLuint _name, ProgramObject & _po, bool _poll)
{
	CompileJob * j = _po.job;
	if (j == nullptr)
		return true;
	if (_poll && !j->done)
		return false;
	waitJob(*j);
	Program & p = _po.program;
	gx2::log("compiled program %u: pixel %u bytes, %u ms (CPU %u ms, %u sleeps, malloc %u ms in %u calls)\n", _name,
		unsigned(j->ps.size()), j->wallUs / 1000, j->cpuUs / 1000, j->wakes, j->allocUs / 1000, j->allocCalls);
	p.vs = j->vsOut;
	p.ps = j->psOut;
	p.mode = j->failed ? GLSL_SHADER_MODE_ERROR : j->mode;
	if (j->failed) {
		p.log = j->log;
		gx2::log("link %u failed: %s\n--- vertex\n%s\n--- fragment\n%s\n---\n", _name, j->log.c_str(), j->vs.c_str(),
			j->ps.c_str());
	}
	delete j;
	_po.job = nullptr;
	finishLink(_name, _po);
	return true;
}

static ProgramObject * linkedObject(GLuint _name)
{
	ProgramObject * po = programObject(_name);
	if (po != nullptr)
		collectJob(_name, *po, false);
	return po;
}

void APIENTRY glLinkProgram(GLuint program)
{
	ProgramObject * po = programObject(program);
	if (po == nullptr)
		return;
	collectJob(program, *po, false);
	Program & p = po->program;
	freeProgram(p);
	p.log.clear();
	const std::string * vsSource = nullptr;
	const std::string * psSource = nullptr;
	for (GLuint s : po->shaders) {
		auto it = g_shaders.find(s);
		if (it == g_shaders.end())
			continue;
		if (it->second.type == GL_VERTEX_SHADER)
			vsSource = &it->second.source;
		else if (it->second.type == GL_FRAGMENT_SHADER)
			psSource = &it->second.source;
	}
	if (vsSource == nullptr || psSource == nullptr) {
		p.log = "program needs a vertex and a fragment shader";
		gx2::log("link %u: %s\n", program, p.log.c_str());
		return;
	}
	CompileJob * j = new CompileJob;
	j->vs = flipVertexShader(addMissingOutputs(*vsSource, *psSource));
	// "noclipdist" drops the vertex shader's gl_ClipDistance write (the far
	// plane GLideN64 clips only in NoNearPlaneClipping on OpenGL).
	if (our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "noclipdist"))
		replaceAll(j->vs, "gl_ClipDistance[0] = gl_Position.w - gl_Position.z;", "");
	const bool early = (our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "earlydiscard") != nullptr) ||
		keepDiscardsInPlace(RSP.romname);
	std::string psText = debugRewrite(*psSource);
	if (allHighp(RSP.romname) && (our64_gx2_debug == nullptr || strstr(our64_gx2_debug, "nogamehighp") == nullptr))
		psText = highpEverywhere(psText);
	if (our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "highpdepth") != nullptr)
		psText = highpDepth(psText);
	if (our64_gx2_debug == nullptr || strstr(our64_gx2_debug, "samplerfilter") == nullptr)
		psText = fetchTexelFilter(psText, our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "fetchall") != nullptr,
			our64_gx2_debug == nullptr ? 4 : strstr(our64_gx2_debug, "fetchtex") ? 2 : strstr(our64_gx2_debug, "fetchsel") ? 1 :
			strstr(our64_gx2_debug, "fetchedge") ? 0 : strstr(our64_gx2_debug, "fetchclamp") ? 3 : 4);
	j->ps = early ? psText : deferDiscards(psText);
	if (!early && our64_gx2_debug != nullptr)
		j->ps = deferredDiscardVariant(j->ps, our64_gx2_debug);
	j->ps = zlutAsRGBA8(dualSourceAsTarget1(j->ps));
	j->key = shaderCacheKey(j->vs, j->ps, GLSL_COMPILE_REGISTER);
	if (our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "srclog="))
		p.psSource = j->ps;
	p.cached = shaderCacheLoad(j->key, &p.vs, &p.ps, &p.mode);
	if (p.cached) {
		delete j;
		finishLink(program, *po);
		return;
	}
	// No compiler in Cemu (CafeGLSL hangs there): with a source folder set,
	// a pair missing from the cache is saved as <key>.vert and <key>.frag for
	// Our64's tools/shader-dump --cache to compile on the PC, and left unlinked.
	if (our64_gx2_shader_source_dir != nullptr) {
		char base[48];
		snprintf(base, sizeof(base), "/%016llx", (unsigned long long)j->key);
		const std::string path = std::string(our64_gx2_shader_source_dir) + base;
		for (int k = 0; k < 2; ++k) {
			if (FILE * f = fopen((path + (k == 0 ? ".vert" : ".frag")).c_str(), "w")) {
				fputs(k == 0 ? j->vs.c_str() : j->ps.c_str(), f);
				fclose(f);
			}
		}
		p.log = "not in the shader cache (source saved)";
		++g_deferredLinks;
		delete j;
		return;
	}
	if (g_linkMode == LinkMode::CacheOnly) {
		p.log = "not in the shader cache";
		++g_deferredLinks;
		delete j;
		return;
	}
	j->background = g_linkMode == LinkMode::Background;
	if (our64_gx2_debug != nullptr && strstr(our64_gx2_debug, "complog"))
		gx2::log("compiling %u\n--- fragment\n%s\n---\n", program, j->ps.c_str());
	po->job = j;
	postJob(j);
}

void APIENTRY glGetProgramiv(GLuint program, GLenum pname, GLint * params)
{
	if (pname == GL_COMPLETION_STATUS_ARB) {
		ProgramObject * po = programObject(program);
		*params = po == nullptr || collectJob(program, *po, true) ? GL_TRUE : GL_FALSE;
		return;
	}
	ProgramObject * po = linkedObject(program);
	switch (pname) {
	case GL_LINK_STATUS:
		*params = po != nullptr && po->program.linked ? GL_TRUE : GL_FALSE;
		break;
	case GL_INFO_LOG_LENGTH:
		*params = po == nullptr ? 0 : GLint(po->program.log.size() + 1);
		break;
	default:
		*params = 0;
	}
}

void APIENTRY glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei * length, GLchar * infoLog)
{
	ProgramObject * po = linkedObject(program);
	const std::string text = po == nullptr ? std::string() : po->program.log;
	if (bufSize <= 0)
		return;
	const size_t n = text.size() < size_t(bufSize - 1) ? text.size() : size_t(bufSize - 1);
	memcpy(infoLog, text.data(), n);
	infoLog[n] = '\0';
	if (length != nullptr)
		*length = GLsizei(n);
}

void APIENTRY glDeleteProgram(GLuint program)
{
	ProgramObject * po = programObject(program);
	if (po == nullptr)
		return;
	// A queued compile is dropped; one the worker is running is waited for.
	if (po->job != nullptr && cancelJob(po->job)) {
		delete po->job;
		po->job = nullptr;
	}
	collectJob(program, *po, false);
	if (g_current == po)
		g_current = nullptr;
	// The GPU may still be reading the shaders of draws already queued.
	GX2DrawDone();
	freeProgram(po->program);
	delete po;
	g_programs.erase(program);
}

void APIENTRY glUseProgram(GLuint program)
{
	g_current = linkedObject(program);
}

GLint APIENTRY glGetUniformLocation(GLuint program, const GLchar * name)
{
	ProgramObject * po = linkedObject(program);
	if (po == nullptr || !po->program.linked)
		return -1;
	auto known = po->locationByName.find(name);
	if (known != po->locationByName.end())
		return known->second;
	u32 index = 0;
	const std::string base = baseName(name, index);
	const Program & p = po->program;
	Location loc;
	for (u32 i = 0; i < p.vs->uniformVarCount; ++i)
		if (base == p.vs->uniformVars[i].name) {
			loc.vsOffset = s32(p.vs->uniformVars[i].offset + index * registersPerElement(p.vs->uniformVars[i].type) * 4);
			loc.vsBool = isBool(p.vs->uniformVars[i].type);
		}
	for (u32 i = 0; i < p.ps->uniformVarCount; ++i)
		if (base == p.ps->uniformVars[i].name) {
			loc.psOffset = s32(p.ps->uniformVars[i].offset + index * registersPerElement(p.ps->uniformVars[i].type) * 4);
			loc.psBool = isBool(p.ps->uniformVars[i].type);
		}
	u32 s = 0;
	for (u32 i = 0; i < p.vs->samplerVarCount; ++i, ++s)
		if (base == p.vs->samplerVars[i].name)
			loc.samplers.push_back(s);
	for (u32 i = 0; i < p.ps->samplerVarCount; ++i, ++s)
		if (base == p.ps->samplerVars[i].name)
			loc.samplers.push_back(s);
	GLint result = -1;
	if (loc.vsOffset >= 0 || loc.psOffset >= 0 || !loc.samplers.empty()) {
		result = GLint(po->locations.size());
		po->locations.push_back(loc);
	}
	po->locationByName[name] = result;
	return result;
}

void APIENTRY glUniform1i(GLint location, GLint v0)
{
	const u32 v[1] = {u32(v0)};
	writeUniform(location, 1, 1, v);
}

void APIENTRY glUniform2i(GLint location, GLint v0, GLint v1)
{
	const u32 v[2] = {u32(v0), u32(v1)};
	writeUniform(location, 1, 2, v);
}

void APIENTRY glUniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3)
{
	const u32 v[4] = {u32(v0), u32(v1), u32(v2), u32(v3)};
	writeUniform(location, 1, 4, v);
}

void APIENTRY glUniform1f(GLint location, GLfloat v0)
{
	const u32 v[1] = {bits(v0)};
	writeUniform(location, 1, 1, v);
}

void APIENTRY glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
	const u32 v[2] = {bits(v0), bits(v1)};
	writeUniform(location, 1, 2, v);
}

void APIENTRY glUniform3fv(GLint location, GLsizei count, const GLfloat * value)
{
	std::vector<u32> v(size_t(count) * 3);
	for (size_t i = 0; i < v.size(); ++i)
		v[i] = bits(value[i]);
	writeUniform(location, count, 3, v.data());
}

void APIENTRY glUniform4fv(GLint location, GLsizei count, const GLfloat * value)
{
	std::vector<u32> v(size_t(count) * 4);
	for (size_t i = 0; i < v.size(); ++i)
		v[i] = bits(value[i]);
	writeUniform(location, count, 4, v.data());
}

const GLubyte * APIENTRY glGetString(GLenum name)
{
	switch (name) {
	case GL_RENDERER: return reinterpret_cast<const GLubyte*>("GX2 (CafeGLSL)");
	case GL_VERSION: return reinterpret_cast<const GLubyte*>("3.3");
	default: return reinterpret_cast<const GLubyte*>("");
	}
}

void APIENTRY glGetIntegerv(GLenum pname, GLint * data)
{
	*data = pname == GL_MAX_VERTEX_ATTRIBS ? 16 : 0;
}

void APIENTRY glBlendFunc(GLenum sfactor, GLenum dfactor)
{
	if (blendFuncHook != nullptr)
		blendFuncHook(sfactor, dfactor);
}

// Program binaries are not supported (ShaderProgramBinary is off).
void APIENTRY glGetProgramBinary(GLuint, GLsizei, GLsizei * length, GLenum *, void *)
{
	if (length != nullptr)
		*length = 0;
}

void APIENTRY glProgramBinary(GLuint program, GLenum, const void *, GLsizei)
{
	if (ProgramObject * po = linkedObject(program))
		po->program.linked = false;
}

} // extern "C"

// For the player: sd:/wiiu/our64/shader-bench holds .frag files to time.
// Compiles queued or running on the worker, for the player's shader builder:
// a game's preload queues the keys missing from the cache, and they must be
// done (and saved) before the game closes, which drops what is still queued.
extern "C" unsigned our64_gx2_jobs_pending(void)
{
	if (!g_compileThreadStarted)
		return 0;
	OSLockMutex(&g_jobsMutex);
	const unsigned n = unsigned(g_jobs.size()) + (g_jobRunning ? 1 : 0);
	OSUnlockMutex(&g_jobsMutex);
	return n;
}

extern "C" void our64_gx2_shader_bench(const char * _dir)
{
	gx2::shaderBench(_dir);
}
