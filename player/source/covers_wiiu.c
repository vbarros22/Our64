/* Our64 player: Settings > Games > Fetch covers on the console. One thread,
 * low priority on core 2 (the loader's and the SD writer's: the game runs on
 * the code generation core, graphics on core 0), downloads each missing
 * cover from libretro-thumbnails over plain HTTP (covers.h) and writes it to
 * <root>/art/<game id>.png with savefile_write: a .tmp renamed into place,
 * so a cut-off write never leaves a cover the library would show. The menu
 * thread starts it, reads its progress each frame and stops it (B, HOME,
 * leaving the page, the system closing); covers_stop joins it.
 *
 * Every wait is cut into 100 ms slices that look at the stop flag, except
 * the name lookup (gethostbyname has no timeout of its own) and the card
 * writes. */
#include "covers.h"
#include "covertitles.h"
#include "library.h"
#include "report.h"
#include "saves.h"

#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nn/ac/ac_c.h>
#include <png.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLICE_MS 100
#define NET_TIMEOUT_MS 10000 /* to connect, and between two pieces of a reply */
#define AC_TIMEOUT_MS 30000  /* to bring the network up */

/* nn::ac::GetConnectStatus: wut declares it only for C++ (nn/ac/ac_cpp.h). */
enum { AC_STATUS_FAILED = -1, AC_STATUS_OK = 0, AC_STATUS_PROCESSING = 1 };
extern NNResult covers_ac_status(int32_t *status) __asm__("GetConnectStatus__Q2_2nn2acFPQ3_2nn2ac6Status");

static OSThread g_thread __attribute__((aligned(16)));
static uint8_t g_stack[64 * 1024] __attribute__((aligned(16)));
static int g_on;               /* the thread was made and not joined yet (the menu's) */
static volatile int g_stop;
static OSMutex g_lock;
static int g_lock_ready;
static char g_root[256];
static covers_game *g_games;   /* the copy: read by the thread, kept until the next start */
static int g_total;
static unsigned char *g_buf;   /* one reply, COVERS_MAX_REPLY and a byte to see it is over */
/* under g_lock */
static int g_done, g_saved, g_not_found, g_failed, g_finished, g_result;
static int *g_listed;          /* indices into g_games with a cover on the card now, in order */
static int g_listed_n;
static int g_taken;            /* the menu's: listed ids handed out so far */

/* What one game came to, past covers.h's outcomes. */
enum { GAME_SAVED = 10, GAME_THERE, GAME_WRITE_FAILED, GAME_STOPPED };

static int would_block(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS; }

/* fd ready for events: 1; timed out 0; -1 an error; -2 asked to stop. */
static int wait_for(int fd, int events)
{
	for (int waited = 0; waited < NET_TIMEOUT_MS; waited += SLICE_MS) {
		if (g_stop)
			return -2;
		struct pollfd p = { fd, events, 0 };
		const int r = poll(&p, 1, SLICE_MS);
		if (r < 0)
			return -1;
		if (r > 0)
			return p.revents & POLLNVAL ? -1 : 1; /* send and recv say what an error was */
	}
	return 0;
}

/* The request to addr, the whole reply into g_buf: its length; -1 a network
 * failure (no connection, reset, 10 s with nothing); -2 asked to stop; -3
 * over COVERS_MAX_REPLY. */
static int http_get(const struct sockaddr_in *addr, const char *req, int len)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	int n = -1, w;
	const int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
		goto out;
	if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
		if (!would_block() && errno != EALREADY)
			goto out;
		if ((w = wait_for(fd, POLLOUT)) != 1) {
			n = w == -2 ? -2 : -1;
			goto out;
		}
		int err = 0;
		socklen_t el = sizeof(err);
		if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) && err)
			goto out;
	}
	for (int sent = 0; sent < len;) {
		const ssize_t k = send(fd, req + sent, (size_t)(len - sent), 0);
		if (k > 0) {
			sent += (int)k;
		} else if (k < 0 && would_block()) {
			if ((w = wait_for(fd, POLLOUT)) != 1) {
				n = w == -2 ? -2 : -1;
				goto out;
			}
		} else {
			goto out;
		}
	}
	for (unsigned got = 0;;) {
		const ssize_t k = recv(fd, g_buf + got, COVERS_MAX_REPLY + 1 - got, 0);
		if (k > 0) {
			got += (unsigned)k;
			if (got > COVERS_MAX_REPLY) {
				n = -3;
				goto out;
			}
		} else if (k == 0) { /* HTTP/1.0, Connection: close: the end of the reply */
			n = (int)got;
			goto out;
		} else if (would_block()) {
			if ((w = wait_for(fd, POLLIN)) != 1) {
				n = w == -2 ? -2 : -1;
				goto out;
			}
		} else {
			goto out;
		}
	}
out:
	close(fd);
	return n;
}

/* One title's cover into g_buf: COVERS_GOT_PNG with the body's place, else
 * COVERS_NOT_FOUND, COVERS_NOT_PNG, COVERS_NET_FAILED or GAME_STOPPED. */
static int download(const char *title, const struct sockaddr_in *addr, unsigned *body, unsigned *body_n)
{
	char name[256], path[800], req[1024];
	covers_thumb_name(title, name, sizeof(name));
	covers_path(name, path, sizeof(path));
	const int len = path[0] ? covers_request(path, req, sizeof(req)) : -1;
	if (!name[0] || len < 0)
		return COVERS_NOT_FOUND; /* a name too long to ask for */
	const int n = http_get(addr, req, len);
	if (n == -2)
		return GAME_STOPPED;
	if (n == -3)
		return COVERS_NOT_PNG; /* too big to be a cover */
	if (n < 0)
		return COVERS_NET_FAILED;
	return covers_status_outcome(covers_reply(g_buf, (unsigned)n, body, body_n));
}

/* A PNG the loader can draw, decoded whole into a scratch buffer: from
 * memory (p, n) or from the card (path). 1 it is; 0 it is not (cut off,
 * damaged, or past the loader's 16 M pixels); -1 no memory to tell. */
static int png_whole(const char *path, const unsigned char *p, unsigned n)
{
	png_image im;
	memset(&im, 0, sizeof(im));
	im.version = PNG_IMAGE_VERSION;
	if (path ? !png_image_begin_read_from_file(&im, path) : !covers_is_png(p, n) || !png_image_begin_read_from_memory(&im, p, n))
		return 0;
	int r = 0;
	if (im.width > 0 && im.height > 0 && (uint64_t)im.width * im.height <= LIBRARY_FIT_MAX_PIXELS) {
		im.format = PNG_FORMAT_RGBA;
		void *scratch = malloc(PNG_IMAGE_SIZE(im));
		r = !scratch ? -1 : png_image_finish_read(&im, NULL, scratch, 0, NULL) != 0;
		free(scratch);
	}
	png_image_free(&im);
	return r;
}

static int one_game(const covers_game *gm, const struct sockaddr_in *addr)
{
	char path[400], title[256], second[256];
	snprintf(path, sizeof(path), "%s/art/%s.png", g_root, gm->id);
	struct stat st;
	const char *table = covertitles_find(gm->crc1, gm->crc2, gm->country);
	if (table)
		snprintf(title, sizeof(title), "%s", table);
	else
		covers_file_title(gm->file, title, sizeof(title));
	if (!stat(path, &st)) { /* a cover already on the card is never replaced, unless it can't be drawn */
		if (png_whole(path, NULL, 0)) {
			report_line("COVERS: %s: already on the card", title);
			return GAME_THERE;
		}
		const int gone = !remove(path);
		report_line("COVERS: %s: the cover on the card can't be read: %s", title, gone ? "removed" : "NOT removed");
		if (!gone)
			return GAME_WRITE_FAILED;
	}
	unsigned body = 0, body_n = 0;
	int r = download(title, addr, &body, &body_n);
	if (r == COVERS_NOT_FOUND && covers_second_title(table, gm->file, second, sizeof(second))) {
		snprintf(title, sizeof(title), "%s", second);
		r = download(title, addr, &body, &body_n);
	}
	if (r == GAME_STOPPED)
		return r;
	if (r == COVERS_GOT_PNG && png_whole(NULL, g_buf + body, body_n) != 1)
		r = COVERS_NOT_PNG; /* no memory to check it counts too: an unchecked cover is never saved */
	if (r == COVERS_GOT_PNG)
		r = savefile_write(path, g_buf + body, body_n) ? GAME_WRITE_FAILED : GAME_SAVED;
	report_line("COVERS: %s: %s", title, r == GAME_SAVED ? "saved" : r == COVERS_NOT_FOUND ? "not found"
	            : r == COVERS_NOT_PNG ? "not a PNG" : r == GAME_WRITE_FAILED ? "NOT written" : "network error");
	return r;
}

/* The network up (nn::ac). wut's socket layer already asked for it as Our64
 * started (ACConnectAsync) and closes it as Our64 ends, so usually it is up;
 * if not, this asks again and waits. *ours: this connection is ours to
 * close. */
static int network_up(int *ours)
{
	BOOL on = FALSE;
	*ours = 0;
	if (NNResult_IsSuccess(ACIsApplicationConnected(&on)) && on)
		return COVERS_OK;
	if (NNResult_IsFailure(ACConnectAsync())) {
		report_line("COVERS: ACConnectAsync failed");
		return COVERS_NO_NETWORK;
	}
	*ours = 1;
	for (int waited = 0; waited < AC_TIMEOUT_MS && !g_stop; waited += SLICE_MS) {
		int32_t status = AC_STATUS_PROCESSING;
		if (NNResult_IsFailure(covers_ac_status(&status)) || status == AC_STATUS_FAILED) {
			report_line("COVERS: not connected (status %d)", (int)status);
			return COVERS_NO_NETWORK;
		}
		if (status == AC_STATUS_OK)
			return COVERS_OK;
		OSSleepTicks(OSMillisecondsToTicks(SLICE_MS));
	}
	if (!g_stop)
		report_line("COVERS: not connected after %d s", AC_TIMEOUT_MS / 1000);
	return g_stop ? COVERS_OK : COVERS_NO_NETWORK; /* stopped: the loop below does nothing */
}

/* The server's address, looked up once. A network with no internet fails
 * here first: that is "not connected", not the server's fault. */
static int resolve(struct sockaddr_in *addr)
{
	struct hostent *h = gethostbyname(COVERS_HOST);
	if (!h || h->h_addrtype != AF_INET || !h->h_addr_list || !h->h_addr_list[0]) {
		report_line("COVERS: can't look up " COVERS_HOST);
		return COVERS_NO_NETWORK;
	}
	memset(addr, 0, sizeof(*addr));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(80);
	memcpy(&addr->sin_addr, h->h_addr_list[0], sizeof(addr->sin_addr));
	return COVERS_OK;
}

static int worker(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	const OSTime t0 = OSGetSystemTime();
	int ours = 0, result = COVERS_NO_NETWORK;
	const int ac = NNResult_IsSuccess(ACInitialize());
	if (!ac)
		report_line("COVERS: ACInitialize failed");
	else
		result = network_up(&ours);
	struct sockaddr_in addr;
	if (result == COVERS_OK && !g_stop)
		result = resolve(&addr);
	char dir[300];
	snprintf(dir, sizeof(dir), "%s/art", g_root);
	if (result == COVERS_OK && !g_stop)
		mkdir(dir, 0777); /* a failure shows as the first cover not written */
	int streak = 0;
	for (int i = 0; i < g_total && result == COVERS_OK && !g_stop; i++) {
		const int r = one_game(&g_games[i], &addr);
		if (r == GAME_STOPPED)
			break;
		if (r == GAME_WRITE_FAILED)
			result = COVERS_CARD_FULL;
		else if (covers_server_gone(&streak, r == GAME_SAVED || r == GAME_THERE ? COVERS_GOT_PNG : r))
			result = COVERS_NO_SERVER;
		OSLockMutex(&g_lock);
		g_done = i + 1;
		if (r == GAME_SAVED || r == GAME_THERE)
			g_listed[g_listed_n++] = i;
		g_saved += r == GAME_SAVED;
		g_not_found += r == COVERS_NOT_FOUND;
		g_failed += r == COVERS_NET_FAILED || r == COVERS_NOT_PNG;
		OSUnlockMutex(&g_lock);
	}
	if (ours)
		ACClose();
	if (ac)
		ACFinalize();
	/* the counts are this thread's own to write: read without the lock */
	report_line("COVERS: %d saved, %d not found, %d couldn't be downloaded, %d of %d tried in %.1f s%s%s", g_saved,
	            g_not_found, g_failed, g_done, g_total,
	            OSTicksToMilliseconds(OSGetSystemTime() - t0) / 1000.0, g_stop ? ", stopped" : "",
	            result == COVERS_NO_NETWORK ? ", no network" : result == COVERS_NO_SERVER ? ", server not reached"
	            : result == COVERS_CARD_FULL ? ", card full" : "");
	OSLockMutex(&g_lock);
	g_result = result;
	g_finished = 1;
	OSUnlockMutex(&g_lock);
	return 0;
}

int covers_start(const char *root, const covers_game *games, int n)
{
	if (!g_lock_ready) {
		OSInitMutex(&g_lock);
		g_lock_ready = 1;
	}
	if (covers_running() || n <= 0)
		return -1;
	covers_stop(); /* the last one's thread, finished: joined */
	free(g_games);
	free(g_listed);
	g_games = malloc(sizeof(*g_games) * (size_t)n);
	g_listed = malloc(sizeof(*g_listed) * (size_t)n);
	g_buf = malloc(COVERS_MAX_REPLY + 1);
	if (!g_games || !g_listed || !g_buf) {
		free(g_buf);
		g_buf = NULL;
		report_line("COVERS: no memory to start");
		return -1;
	}
	memcpy(g_games, games, sizeof(*g_games) * (size_t)n);
	snprintf(g_root, sizeof(g_root), "%s", root);
	g_total = n;
	g_done = g_saved = g_not_found = g_failed = g_finished = g_listed_n = g_taken = 0;
	g_result = COVERS_OK;
	g_stop = 0;
	/* below the loader (26) and the SD writer (24), on core 2 with them */
	if (!OSCreateThread(&g_thread, (OSThreadEntryPointFn)worker, 0, NULL, g_stack + sizeof(g_stack), sizeof(g_stack), 28,
	                    OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
		free(g_buf);
		g_buf = NULL;
		report_line("COVERS: no thread");
		return -1;
	}
	OSSetThreadName(&g_thread, "Our64 fetch covers");
	g_on = 1;
	report_line("COVERS: fetching %d", n);
	OSResumeThread(&g_thread);
	return 0;
}

int covers_running(void)
{
	if (!g_on)
		return 0;
	OSLockMutex(&g_lock);
	const int r = !g_finished;
	OSUnlockMutex(&g_lock);
	return r;
}

void covers_progress(int *done, int *total, int *saved)
{
	if (!g_lock_ready) {
		*done = *total = *saved = 0;
		return;
	}
	OSLockMutex(&g_lock);
	*done = g_done;
	*total = g_total;
	*saved = g_saved;
	OSUnlockMutex(&g_lock);
}

int covers_failed(void)
{
	if (!g_lock_ready)
		return 0;
	OSLockMutex(&g_lock);
	const int n = g_failed;
	OSUnlockMutex(&g_lock);
	return n;
}

int covers_not_found(void)
{
	if (!g_lock_ready)
		return 0;
	OSLockMutex(&g_lock);
	const int n = g_not_found;
	OSUnlockMutex(&g_lock);
	return n;
}

const char *covers_take_saved(void)
{
	const char *id = NULL;
	if (!g_lock_ready)
		return NULL;
	OSLockMutex(&g_lock);
	if (g_taken < g_listed_n)
		id = g_games[g_listed[g_taken++]].id;
	OSUnlockMutex(&g_lock);
	return id;
}

void covers_stop(void)
{
	if (!g_on)
		return;
	g_stop = 1;
	int ret;
	OSJoinThread(&g_thread, &ret);
	g_on = 0;
	free(g_buf);
	g_buf = NULL;
}

int covers_result(void) { return g_result; }
