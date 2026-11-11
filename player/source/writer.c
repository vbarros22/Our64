/* Our64 player: one thread on core 2 for every SD write (see writer.h). */
#include "writer.h"
#include "report.h"

#include <coreinit/condition.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>

static void (*g_job[WRITER_SLOTS])(void); /* waiting to run */
static int g_running = -1;                /* the slot running now */
static int g_on;
static OSMutex g_lock;
static OSCondition g_wake, g_idle;
static OSThread g_thread __attribute__((aligned(16)));
static uint8_t g_stack[64 * 1024] __attribute__((aligned(16)));

static int writer(int argc, const char **argv)
{
	(void)argc; (void)argv;
	OSLockMutex(&g_lock);
	for (;;) {
		int s = 0;
		while (s < WRITER_SLOTS && !g_job[s])
			s++;
		if (s == WRITER_SLOTS) {
			OSSignalCond(&g_idle);
			if (!g_on)
				break;
			OSWaitCond(&g_wake, &g_lock);
			continue;
		}
		void (*job)(void) = g_job[s];
		g_job[s] = 0;
		g_running = s;
		OSUnlockMutex(&g_lock);
		job();
		OSLockMutex(&g_lock);
		g_running = -1;
	}
	OSUnlockMutex(&g_lock);
	return 0;
}

int writer_start(void)
{
	OSInitMutex(&g_lock);
	OSInitCond(&g_wake);
	OSInitCond(&g_idle);
	g_on = 1;
	if (!OSCreateThread(&g_thread, (OSThreadEntryPointFn)writer, 0, NULL, g_stack + sizeof(g_stack), sizeof(g_stack), 24,
	                    OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		g_on = 0;
		return -1;
	}
	OSSetThreadName(&g_thread, "Our64 SD writer");
	OSResumeThread(&g_thread);
	return 0;
}

void writer_post(int slot, void (*job)(void))
{
	OSLockMutex(&g_lock);
	g_job[slot] = job;
	OSSignalCond(&g_wake);
	OSUnlockMutex(&g_lock);
}

int writer_pending(int slot)
{
	OSLockMutex(&g_lock);
	int p = g_job[slot] != 0 || g_running == slot;
	OSUnlockMutex(&g_lock);
	return p;
}

void writer_drain(void)
{
	if (!g_on)
		return;
	OSLockMutex(&g_lock);
	for (;;) {
		int busy = g_running >= 0;
		for (int s = 0; s < WRITER_SLOTS; s++)
			busy |= g_job[s] != 0;
		if (!busy)
			break;
		OSWaitCond(&g_idle, &g_lock);
	}
	OSUnlockMutex(&g_lock);
}

void writer_stop(void)
{
	if (!g_on)
		return;
	writer_drain();
	OSLockMutex(&g_lock);
	g_on = 0;
	OSSignalCond(&g_wake);
	OSUnlockMutex(&g_lock);
	int ret;
	OSJoinThread(&g_thread, &ret);
}

void writer_lock(void) { OSLockMutex(&g_lock); }
void writer_unlock(void) { OSUnlockMutex(&g_lock); }

void writer_slow(const char *what, double ms)
{
	if (ms > 2000)
		report_line("SLOW SD: %s took %.1f s to write", what, ms / 1e3);
}
