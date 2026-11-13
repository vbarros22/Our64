/* PC tests for player/source/covertitles_find.c (the No-Intro names by
 * header CRCs) and covers.c (cover file names, URLs, HTTP replies and the
 * fetch's rules). */
#include "covertitles.h"
#include "covers.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failed = 1; } } while (0)

static void test_title_by_header_crcs(void)
{
	const char *t = covertitles_find(0x635A2BFF, 0x8B022326, 'E'); /* mupen64plus.ini: Super Mario 64 (U) [!] */
	CHECK(t && !strcmp(t, "Super Mario 64 (USA)"));
	t = covertitles_find(0x635A2BFF, 0x8B022326, 'J'); /* a pair only one name has: any country */
	CHECK(t && !strcmp(t, "Super Mario 64 (USA)"));
	CHECK(covertitles_find(0x12345678, 0x9ABCDEF0, 'E') == NULL);
	CHECK(covertitles_find(0, 0, 'E') == NULL);
	CHECK(covertitles_find(0xFFFFFFFF, 0xFFFFFFFF, 'E') == NULL);
}

/* Ocarina of Time's USA and Japan carts share their header CRCs: the
 * header's country byte picks the name. */
static void test_shared_pair_follows_the_country(void)
{
	const char *t = covertitles_find(0xEC7011B7, 0x7616D72B, 'E');
	CHECK(t && !strcmp(t, "Legend of Zelda, The - Ocarina of Time (USA)"));
	t = covertitles_find(0xEC7011B7, 0x7616D72B, 'J');
	CHECK(t && !strcmp(t, "Zelda no Densetsu - Toki no Ocarina (Japan)"));
	t = covertitles_find(0xC00CA948, 0x8E60D34B, 'X'); /* one of Europe's bytes */
	CHECK(t && !strcmp(t, "South Park - Chef's Luv Shack (Europe)"));
	/* a country no row lists, and none at all: the pair's first row */
	t = covertitles_find(0xEC7011B7, 0x7616D72B, 'K');
	CHECK(t && !strcmp(t, "Zelda no Densetsu - Toki no Ocarina (Japan)"));
	t = covertitles_find(0xEC7011B7, 0x7616D72B, 0);
	CHECK(t && !strcmp(t, "Zelda no Densetsu - Toki no Ocarina (Japan)"));
}

static void test_table_is_sorted(void)
{
	CHECK(covertitles_count > 0);
	for (unsigned i = 1; i < covertitles_count; i++) {
		const covertitle *a = &covertitles[i - 1], *b = &covertitles[i];
		CHECK(a->crc1 < b->crc1 || (a->crc1 == b->crc1 && a->crc2 <= b->crc2));
		/* a pair on two rows: every row of it names its countries */
		if (a->crc1 == b->crc1 && a->crc2 == b->crc2)
			CHECK(a->countries && a->countries[0] && b->countries && b->countries[0]);
	}
	/* every pair's first row is found by the pair; a row by one of its
	 * countries unless an earlier row of the pair lists it too */
	for (unsigned i = 0, first = 0; i < covertitles_count; i++) {
		const covertitle *r = &covertitles[i];
		if (i == 0 || r[-1].crc1 != r->crc1 || r[-1].crc2 != r->crc2) {
			first = i;
			CHECK(covertitles_find(r->crc1, r->crc2, 0) == r->name);
		}
		for (const char *c = r->countries; c && *c; c++) {
			unsigned j = first;
			while (!strchr(covertitles[j].countries, *c))
				j++;
			CHECK(covertitles_find(r->crc1, r->crc2, *c) == covertitles[j].name);
		}
	}
}

static void test_thumb_name(void)
{
	char out[64];
	covers_thumb_name("Mario & Luigi: A/B?", out, sizeof out);
	CHECK(!strcmp(out, "Mario _ Luigi_ A_B_"));
	/* RetroArch's set (gfx_thumbnail.c): & * / : ` " < > ? \ | */
	covers_thumb_name("&*/:`\"<>?\\|", out, sizeof out);
	CHECK(!strcmp(out, "___________"));
	covers_thumb_name("Banjo-Kazooie (USA) (Rev 1)", out, sizeof out);
	CHECK(!strcmp(out, "Banjo-Kazooie (USA) (Rev 1)"));
	covers_thumb_name("Pok\xc3\xa9mon Snap (USA)", out, sizeof out); /* UTF-8 passes through */
	CHECK(!strcmp(out, "Pok\xc3\xa9mon Snap (USA)"));
}

static void test_path(void)
{
	char out[256];
	covers_path("Super Mario 64 (USA)", out, sizeof out);
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/Super%20Mario%2064%20%28USA%29.png"));
	covers_path("A-Z_a.z~09", out, sizeof out); /* the unreserved set stays */
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A-Z_a.z~09.png"));
	covers_path("Pok\xc3\xa9mon, Mario's +!", out, sizeof out); /* UTF-8 bytes each as %XX */
	CHECK(!strcmp(out, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/Pok%C3%A9mon%2C%20Mario%27s%20%2B%21.png"));
}

static void test_request(void)
{
	char out[512];
	int n = covers_request("/Nintendo%20-%20Nintendo%2064/Named_Boxarts/X.png", out, sizeof out);
	CHECK(n == (int)strlen(out));
	CHECK(!strncmp(out, "GET /Nintendo%20-%20Nintendo%2064/Named_Boxarts/X.png HTTP/1.0\r\n", 64));
	CHECK(strstr(out, "\r\nHost: thumbnails.libretro.com\r\n") != NULL);
	CHECK(strstr(out, "\r\nUser-Agent: Our64/" OUR64_VERSION "\r\n") != NULL);
	CHECK(strstr(out, "\r\nConnection: close\r\n") != NULL);
	CHECK(n > 4 && !strcmp(out + n - 4, "\r\n\r\n"));
	CHECK(strstr(out, "\r\n\r\n") == out + n - 4); /* the only blank line is the last */
}

static int reply(const char *r, unsigned *body, unsigned *body_n)
{
	*body = *body_n = 12345;
	return covers_reply((const unsigned char *)r, (unsigned)strlen(r), body, body_n);
}

static void test_reply(void)
{
	unsigned body, body_n;
	const char *ok = "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r\n\x89PNG\r\n\x1a\n";
	CHECK(reply(ok, &body, &body_n) == 200);
	CHECK(body == (unsigned)(strstr(ok, "\x89PNG") - ok) && body_n == 8);
	CHECK(reply("HTTP/1.1 404 Not Found\r\nContent-Length: 9\r\n\r\nnot found", &body, &body_n) == 404);
	CHECK(reply("HTTP/1.0 503 Service Unavailable\r\n\r\n", &body, &body_n) == 503);
	CHECK(reply("garbage", &body, &body_n) == -1);
	CHECK(reply("", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 OK\r\n\r\nx", &body, &body_n) == -1);
	/* Content-Length is the body, whatever follows it */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n123456789", &body, &body_n) == 200);
	CHECK(body_n == 5);
	/* a cut-off download: less than Content-Length arrived */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n123456789", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999\r\n\r\n123", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Length: five\r\n\r\n12345", &body, &body_n) == -1);
	/* header names in any case */
	CHECK(reply("HTTP/1.1 200 OK\r\ncontent-LENGTH:3\r\n\r\n123456789", &body, &body_n) == 200);
	CHECK(body_n == 3);
	/* the headers never ended */
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n\r", &body, &body_n) == -1);
	/* chunked: the body is not the file (an HTTP/1.0 request should never get it) */
	CHECK(reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\n12345\r\n0\r\n\r\n", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\ntransfer-encoding: gzip, CHUNKED\r\n\r\nx", &body, &body_n) == -1);
	CHECK(reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: identity\r\n\r\n123", &body, &body_n) == 200);
	CHECK(reply("HTTP/1.1 404 Not Found\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", &body, &body_n) == 404);
	/* only the n bytes given are read: a reply cut inside its headers */
	CHECK(covers_reply((const unsigned char *)ok, 20, &body, &body_n) == -1);
}

static void test_file_title(void)
{
	char out[64];
	covers_file_title("Banjo-Kazooie (USA).z64", out, sizeof out);
	CHECK(!strcmp(out, "Banjo-Kazooie (USA)"));
	covers_file_title("/vol/external01/wiiu/our64/roms/Super Mario 64 (USA).n64", out, sizeof out);
	CHECK(!strcmp(out, "Super Mario 64 (USA)"));
	covers_file_title("roms/Dr. Mario 64 (USA).v1.z64", out, sizeof out); /* only the last extension */
	CHECK(!strcmp(out, "Dr. Mario 64 (USA).v1"));
	covers_file_title("NoExtension", out, sizeof out);
	CHECK(!strcmp(out, "NoExtension"));
	covers_file_title("roms.old/NoExtension", out, sizeof out); /* a dot in a folder is not an extension */
	CHECK(!strcmp(out, "NoExtension"));
}

/* Every output is NUL-terminated inside size; what does not fit gives "". */
static void test_small_buffers(void)
{
	char out[16];
	unsigned body, body_n;
	(void)body; (void)body_n;
	for (unsigned size = 0; size <= 8; size++) {
		memset(out, 'X', sizeof out);
		covers_thumb_name("Super Mario 64 (USA)", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		covers_path("Super Mario 64 (USA)", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		CHECK(covers_request("/x.png", out, size) == -1);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
		memset(out, 'X', sizeof out);
		covers_file_title("dir/Super Mario 64 (USA).z64", out, size);
		CHECK(out[size] == 'X' && (size == 0 || out[0] == 0));
	}
	/* exactly fitting */
	char fit[5];
	covers_thumb_name("a:b?", fit, sizeof fit);
	CHECK(!strcmp(fit, "a_b_"));
	covers_file_title("abcd.z64", fit, sizeof fit);
	CHECK(!strcmp(fit, "abcd"));
	covers_file_title("abcde.z64", fit, sizeof fit);
	CHECK(fit[0] == 0);
	char path[64];
	covers_path("A", path, sizeof "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png");
	CHECK(!strcmp(path, "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png"));
	covers_path("&", path, sizeof "/Nintendo%20-%20Nintendo%2064/Named_Boxarts/A.png"); /* %26 is 2 more */
	CHECK(path[0] == 0);
}

static void test_outcomes(void)
{
	CHECK(covers_status_outcome(200) == COVERS_GOT_PNG);
	CHECK(covers_status_outcome(404) == COVERS_NOT_FOUND);
	CHECK(covers_status_outcome(403) == COVERS_NET_FAILED); /* "not found" is only a 404 */
	CHECK(covers_status_outcome(410) == COVERS_NET_FAILED);
	CHECK(covers_status_outcome(-1) == COVERS_NET_FAILED); /* garbled or cut off */
	CHECK(covers_status_outcome(503) == COVERS_NET_FAILED);
	CHECK(covers_status_outcome(301) == COVERS_NET_FAILED); /* a redirect is not followed */
	CHECK(covers_is_png((const unsigned char *)"\x89PNG\r\n\x1a\nIHDR", 12));
	CHECK(!covers_is_png((const unsigned char *)"\x89PNG\r\n\x1a", 7));
	CHECK(!covers_is_png((const unsigned char *)"<html><body>", 12));
}

/* Three failed downloads in a row (the network, or a body that is not a
 * whole PNG) stop the fetch; a cover or a 404 between them starts the count
 * again. */
static void test_server_gone(void)
{
	int streak = 0;
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(!covers_server_gone(&streak, COVERS_NOT_FOUND));
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(!covers_server_gone(&streak, COVERS_GOT_PNG));
	CHECK(!covers_server_gone(&streak, COVERS_NOT_PNG));
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(covers_server_gone(&streak, COVERS_NET_FAILED));
	streak = 0;
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(!covers_server_gone(&streak, COVERS_NET_FAILED));
	CHECK(covers_server_gone(&streak, COVERS_NOT_PNG));
}

static void test_second_title(void)
{
	char out[64];
	/* the table's name 404'd: the file's name, when it differs */
	CHECK(covers_second_title("Banjo-Kazooie (USA) (Rev 1)", "Banjo-Kazooie (USA).z64", out, sizeof out));
	CHECK(!strcmp(out, "Banjo-Kazooie (USA)"));
	CHECK(!covers_second_title("Banjo-Kazooie (USA)", "Banjo-Kazooie (USA).z64", out, sizeof out));
	CHECK(out[0] == 0);
	/* no table name: the first try was the file's, so none */
	CHECK(!covers_second_title(NULL, "My Hack (USA).z64", out, sizeof out));
	CHECK(out[0] == 0);
	/* a file name too long for out */
	CHECK(!covers_second_title("A", "A very long file name.z64", out, 8));
	CHECK(out[0] == 0);
}

static covers_game game(uint32_t crc1, uint32_t crc2, char country, const char *code, const char *file)
{
	covers_game g;
	memset(&g, 0, sizeof g);
	g.crc1 = crc1;
	g.crc2 = crc2;
	g.country = country;
	snprintf(g.code, sizeof g.code, "%s", code);
	snprintf(g.file, sizeof g.file, "%s", file);
	return g;
}

/* 2026-11-13, Vinicius's card: five changed ROMs (CRCs in no table) fetched no
 * cover. The game code and the file's title find each one's name now. */
static void test_titles(void)
{
	char t[COVERS_TITLES][256];
	CHECK(covertitles_by_code("NZSE") && !strcmp(covertitles_by_code("NZSE"), "Legend of Zelda, The - Majora's Mask (USA)"));
	CHECK(covertitles_by_code("ZZZZ") == NULL && covertitles_by_code(NULL) == NULL);
	for (unsigned i = 1; i < covercodes_count; i++)
		CHECK(strncmp(covercodes[i - 1].code, covercodes[i].code, 4) < 0);
	for (unsigned i = 0; i < covercodes_count; i++)
		CHECK(covertitles_by_code(covercodes[i].code) == covercodes[i].name);

	covers_game g = game(0x72B277B8, 0xF771F9A1, 'E', "NZSE", "The Legend of Zelda - Majora's Mask.z64");
	int n = covers_titles(&g, t);
	CHECK(n == 2 && !strcmp(t[0], "Legend of Zelda, The - Majora's Mask (USA)") && !strcmp(t[1], "The Legend of Zelda - Majora's Mask"));
	g = game(0x65825A56, 0xCC1B70D0, 'E', "CZLE", "The Legend of Zelda - Ocarina of Time.z64");
	n = covers_titles(&g, t);
	CHECK(n == 2 && !strcmp(t[0], "Legend of Zelda, The - Ocarina of Time (USA)"));
	g = game(0xC16C421B, 0xA21580F7, 'E', "NDQE", "Disney's Donald Duck - Goin' Quackers.z64");
	n = covers_titles(&g, t);
	CHECK(n == 2 && !strcmp(t[0], "Donald Duck - Goin' Quackers (USA) (En,Fr,De,Es,It)"));
	g = game(0x67C38660, 0x94038DC6, 'P', "NF2P", "F-1 World Grand Prix II.z64");
	n = covers_titles(&g, t);
	CHECK(n == 2 && !strcmp(t[0], "F-1 World Grand Prix II (Europe) (En,Fr,De,Es)"));
	/* a translation that changed the code too (NEDJ, the cart's is NEVJ): by its title */
	g = game(0x9748C4C5, 0x2806585F, 'J', "NEDJ", "Neon Genesis Evangelion.z64");
	n = covers_titles(&g, t);
	CHECK(n == 2 && !strcmp(t[0], "Neon Genesis Evangelion (Japan)") && !strcmp(t[1], "Neon Genesis Evangelion"));
	/* a known ROM: its CRCs' name first, nothing tried twice */
	g = game(0x635A2BFF, 0x8B022326, 'E', "NSME", "Super Mario 64 (USA).z64");
	n = covers_titles(&g, t);
	CHECK(n == 1 && !strcmp(t[0], "Super Mario 64 (USA)"));
	/* the title: the country's region and fewer tags first, either article */
	CHECK(!strcmp(covertitles_by_title("Legend of Zelda, The - Ocarina of Time", 'P'), "Legend of Zelda, The - Ocarina of Time (Europe) (En,Fr,De)"));
	CHECK(!strcmp(covertitles_by_title("the legend of zelda ocarina of time (U) [!]", 'E'), "Legend of Zelda, The - Ocarina of Time (USA)"));
	CHECK(covertitles_by_title("Nothing Like It 64", 'E') == NULL && covertitles_by_title("(USA)", 'E') == NULL);
	/* nothing known: the file's own name */
	g = game(1, 2, 'E', "", "My Hack.z64");
	n = covers_titles(&g, t);
	CHECK(n == 1 && !strcmp(t[0], "My Hack"));
}

static void summary(int result, int stopped, int saved, int not_found, int failed, const char *want)
{
	char out[128];
	covers_summary(covers_final(result, saved, failed), stopped, saved, not_found, failed, out, sizeof out);
	if (strcmp(out, want))
		printf("  got \"%s\", want \"%s\"\n", out, want);
	CHECK(!strcmp(out, want));
}

static void test_summary(void)
{
	summary(COVERS_OK, 0, 9, 3, 0, "Fetched 9 covers. 3 not found.");
	summary(COVERS_OK, 0, 1, 0, 0, "Fetched 1 cover.");
	summary(COVERS_OK, 0, 2, 1, 0, "Fetched 2 covers. 1 not found.");
	summary(COVERS_OK, 0, 2, 0, 3, "Fetched 2 covers. 3 couldn't be downloaded.");
	summary(COVERS_OK, 0, 2, 1, 3, "Fetched 2 covers. 1 not found. 3 couldn't be downloaded.");
	summary(COVERS_OK, 0, 0, 4, 0, "No covers found.");
	summary(COVERS_OK, 0, 0, 0, 0, "All games have covers."); /* every one was on the card already */
	summary(COVERS_OK, 1, 2, 0, 0, "Stopped: fetched 2 covers.");
	summary(COVERS_OK, 1, 1, 1, 0, "Stopped: fetched 1 cover. 1 not found.");
	summary(COVERS_OK, 1, 0, 0, 0, "Stopped.");
	/* any failed download with nothing saved: the server, however few were tried */
	summary(COVERS_OK, 0, 0, 0, 1, "Couldn't reach the cover server. Try again later. OUR-0402");
	summary(COVERS_OK, 0, 0, 5, 1, "Couldn't reach the cover server. Try again later. OUR-0402");
	summary(COVERS_OK, 1, 0, 0, 2, "Couldn't reach the cover server. Try again later. OUR-0402");
	CHECK(covers_final(COVERS_OK, 1, 1) == COVERS_OK);
	CHECK(covers_final(COVERS_OK, 0, 0) == COVERS_OK);
	CHECK(covers_final(COVERS_CARD_FULL, 0, 1) == COVERS_CARD_FULL);
	/* a failure's line, whatever was saved before it */
	summary(COVERS_NO_NETWORK, 0, 0, 0, 0, "Not connected to the internet. Check the Wii U's Internet settings. OUR-0401");
	summary(COVERS_NO_SERVER, 0, 4, 1, 3, "Couldn't reach the cover server. Try again later. OUR-0402");
	summary(COVERS_CARD_FULL, 1, 4, 0, 0, "Couldn't save a cover: check the SD card's lock switch or free space. OUR-0205");
	summary(COVERS_NO_START, 0, 0, 0, 0, "Couldn't start fetching covers. OUR-0403");
	/* cut to the line's size */
	char tiny[8];
	covers_summary(COVERS_OK, 0, 9, 3, 0, tiny, sizeof tiny);
	CHECK(strlen(tiny) == 7);
}

int main(void)
{
	test_title_by_header_crcs();
	test_shared_pair_follows_the_country();
	test_table_is_sorted();
	test_thumb_name();
	test_path();
	test_request();
	test_reply();
	test_file_title();
	test_small_buffers();
	test_outcomes();
	test_server_gone();
	test_second_title();
	test_titles();
	test_summary();
	if (!g_failed)
		printf("  all passed\n");
	return g_failed;
}
