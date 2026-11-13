/* Our64 player: the game list's games (see library.h). */
#include "library.h"
#include "theme.h"
#include "prompt.h"
#include "saves.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

int library_header(const unsigned char *first64, unsigned long size, library_rom *r)
{
	unsigned char h[64];
	uint32_t magic = be32(first64);
	memset(r, 0, sizeof(*r));
	if (size < 0x101000) /* the header and boot code take 4 KB; no game fits in less than 1 MB */
		return -1;
	for (int i = 0; i < 64; i++) {
		switch (magic) {
		case 0x80371240: h[i] = first64[i]; break;          /* .z64 */
		case 0x37804012: h[i] = first64[i ^ 1]; break;      /* .v64: bytes swapped in pairs */
		case 0x40123780: h[i] = first64[i ^ 3]; break;      /* .n64: little-endian words */
		default: return -1;
		}
	}
	r->crc1 = be32(h + 16);
	r->crc2 = be32(h + 20);
	r->country = (char)h[62];
	memcpy(r->code, h + 59, 4);
	r->code[4] = 0;
	for (int i = 0; i < 4; i++)
		if (!isalnum(h[59 + i]))
			r->code[0] = 0; /* not a game code: no lookup by it */
	saves_game_id(r->id, sizeof(r->id), h + 32, r->crc1);
	return 0;
}

void library_game_init(library_game *g, const char *file)
{
	memset(g, 0, sizeof(*g));
	snprintf(g->file, sizeof(g->file), "%s", file);
	ui_game_title(g->title, sizeof(g->title), file);
}

int library_category_of(const char *title)
{
	unsigned char c = (unsigned char)ui_sort_title(title)[0];
	return isalpha(c) ? LIBRARY_HASH + 1 + toupper(c) - 'A' : LIBRARY_HASH;
}

const char *library_category_label(int category, char *out, unsigned size)
{
	if (category <= LIBRARY_FAVORITES)
		snprintf(out, size, "\u2605"); /* the system font's black star */
	else if (category == LIBRARY_RECENT)
		snprintf(out, size, "Recent");
	else if (category == LIBRARY_ALL)
		snprintf(out, size, "All");
	else if (category == LIBRARY_SEARCH)
		snprintf(out, size, "%s", ""); /* a magnifier is drawn for it */
	else if (category == LIBRARY_HASH)
		snprintf(out, size, "#");
	else
		snprintf(out, size, "%c", 'A' + category - LIBRARY_HASH - 1);
	return out;
}

void library_mark_recent(library_game *g, int n, const char (*recent)[128], int k)
{
	for (int i = 0; i < n; i++) {
		g[i].recent = 0;
		for (int r = 0; r < k && recent[r][0]; r++)
			if (!strcasecmp(g[i].file, recent[r]))
				g[i].recent = 1;
	}
}

/* Favorites, read once; the games' flags follow it. */
#define FAVORITES_MAX 2000
static char (*g_favorites)[128];
static int g_favorite_count;

static int favorite_at(const char *file)
{
	for (int i = 0; i < g_favorite_count; i++)
		if (!strcasecmp(g_favorites[i], file))
			return i;
	return -1;
}

int library_favorites_read(const char *path)
{
	g_favorite_count = 0;
	if (!g_favorites && !(g_favorites = malloc(FAVORITES_MAX * sizeof(*g_favorites))))
		return -1;
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	char line[300];
	while (g_favorite_count < FAVORITES_MAX && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#' && favorite_at(line) < 0)
			snprintf(g_favorites[g_favorite_count++], sizeof(g_favorites[0]), "%.127s", line);
	}
	fclose(f);
	return g_favorite_count;
}

void library_mark_favorites(library_game *g, int n)
{
	for (int i = 0; i < n; i++)
		g[i].favorite = favorite_at(g[i].file) >= 0;
}

int library_favorite_toggle(library_game *g)
{
	if (!g_favorites && !(g_favorites = malloc(FAVORITES_MAX * sizeof(*g_favorites))))
		return g->favorite;
	const int at = favorite_at(g->file);
	if (at >= 0) {
		memmove(g_favorites[at], g_favorites[at + 1], (size_t)(g_favorite_count - at - 1) * sizeof(g_favorites[0]));
		g_favorite_count--;
	} else if (g_favorite_count < FAVORITES_MAX) {
		snprintf(g_favorites[g_favorite_count++], sizeof(g_favorites[0]), "%s", g->file);
	}
	return g->favorite = at < 0 && favorite_at(g->file) >= 0;
}

int library_favorites_text(char *out, unsigned size)
{
	int n = snprintf(out, out ? size : 0, "# Our64 favorites: one ROM file per line.\n");
	for (int i = 0; i < g_favorite_count; i++)
		n += snprintf(out ? out + n : NULL, out && (unsigned)n < size ? size - n : 0, "%s\n", g_favorites[i]);
	return n;
}

/* The star first, then Recent and All (Zach, 2026-10-25), as ui/'s bar is
 * sized (2026-10-24): the letters 24 apart, 25 until the star came. */
#define BAR_Y 12       /* the row of covers: at the top, no header (2026-10-30) */
#define BAR_Y_LIST 126 /* the list: under the header */
#define BAR_H 32
#define BAR_SEARCH_W 30 /* the magnifier, first (2026-11-11); Recent and All gave it the room */
#define BAR_STAR_W 34
#define BAR_RECENT_W 74
#define BAR_ALL_W 46
#define BAR_STEP 24

void library_bar_rect(int category, int r[4])
{
	const int left = (854 - (BAR_SEARCH_W + BAR_STAR_W + BAR_RECENT_W + BAR_ALL_W + (LIBRARY_LAST_LETTER + 1 - LIBRARY_HASH) * BAR_STEP)) / 2;
	const int x0 = left + BAR_SEARCH_W;
	if (category == LIBRARY_SEARCH) {
		r[0] = left;
		r[1] = theme_roms_list() ? BAR_Y_LIST : BAR_Y;
		r[2] = BAR_SEARCH_W;
		r[3] = BAR_H;
		return;
	}
	r[0] = category == LIBRARY_FAVORITES ? x0
	     : category == LIBRARY_RECENT ? x0 + BAR_STAR_W
	     : category == LIBRARY_ALL ? x0 + BAR_STAR_W + BAR_RECENT_W
	     : x0 + BAR_STAR_W + BAR_RECENT_W + BAR_ALL_W + (category - LIBRARY_HASH) * BAR_STEP;
	r[1] = theme_roms_list() ? BAR_Y_LIST : BAR_Y;
	r[2] = category == LIBRARY_FAVORITES ? BAR_STAR_W : category == LIBRARY_RECENT ? BAR_RECENT_W
	     : category == LIBRARY_ALL ? BAR_ALL_W : BAR_STEP;
	r[3] = BAR_H;
}

int library_bar_hit(int x, int y)
{
	for (int c = 0; c < LIBRARY_CATEGORIES; c++) {
		int r[4];
		library_bar_rect(c, r);
		if (x >= r[0] && x < r[0] + r[2] && y >= r[1] - 6 && y < r[1] + r[3] + 6)
			return c;
	}
	return -1;
}

static int compare(const void *a, const void *b)
{
	const library_game *x = a, *y = b;
	int cx = library_category_of(x->title), cy = library_category_of(y->title);
	if (cx != cy)
		return cx - cy;
	int c = strcasecmp(ui_sort_title(x->title), ui_sort_title(y->title));
	return c ? c : strcmp(x->file, y->file);
}

void library_sort(library_game *g, int n) { qsort(g, (size_t)n, sizeof(*g), compare); }

int library_show(const library_game *g, int n, int category, int *shown)
{
	int k = 0;
	if (category == LIBRARY_SEARCH)
		shown[k++] = LIBRARY_CARD; /* the search card, then what it found */
	for (int i = 0; i < n; i++)
		if (!g[i].deleted && (category == LIBRARY_ALL || (category == LIBRARY_RECENT ? g[i].recent
		                                : category == LIBRARY_SEARCH ? library_search_matches(&g[i])
		                                : category == LIBRARY_FAVORITES ? g[i].favorite
		                                                                : library_category_of(g[i].title) == category)))
			shown[k++] = i;
	return k;
}

/* ---- Search ---- */

static char g_search[128];

void library_search_set(const char *text) { snprintf(g_search, sizeof(g_search), "%s", text ? text : ""); }

const char *library_search_text(void) { return g_search; }

static int holds(const char *hay, const char *word, size_t n)
{
	for (; *hay; hay++)
		if (!strncasecmp(hay, word, n))
			return 1;
	return 0;
}

int library_search_matches(const library_game *g)
{
	int words = 0;
	for (const char *w = g_search; *w;) {
		while (*w == ' ')
			w++;
		const size_t n = strcspn(w, " ");
		if (!n)
			break;
		words++;
		if (!holds(g->title, w, n) && !holds(g->file, w, n))
			return 0;
		w += n;
	}
	return words > 0;
}

int library_start_category(int start_on, const int has[LIBRARY_CATEGORIES], int last_recent)
{
	if (start_on == START_RECENT && has[LIBRARY_RECENT] && last_recent)
		return LIBRARY_RECENT;
	if (start_on == START_FAVORITES && has[LIBRARY_FAVORITES])
		return LIBRARY_FAVORITES;
	return LIBRARY_ALL;
}

void library_categories(const library_game *g, int n, int has[LIBRARY_CATEGORIES])
{
	memset(has, 0, sizeof(int) * LIBRARY_CATEGORIES);
	for (int i = 0; i < n; i++) {
		if (g[i].deleted)
			continue;
		has[LIBRARY_ALL] = 1;
		has[LIBRARY_SEARCH] = 1; /* there to be searched whenever there is a game */
		has[library_category_of(g[i].title)] = 1;
		has[LIBRARY_RECENT] |= g[i].recent;
		has[LIBRARY_FAVORITES] |= g[i].favorite;
	}
}

/* The bar wraps: left from its first category goes to its last letter with
 * games, right from the last to the first (Zach, 2026-10-20). */
int library_next_category(const int has[LIBRARY_CATEGORIES], int category, int dir)
{
	for (int k = 1; k < LIBRARY_CATEGORIES; k++) {
		int c = ((category + dir * k) % LIBRARY_CATEGORIES + LIBRARY_CATEGORIES) % LIBRARY_CATEGORIES;
		if (has[c])
			return c;
	}
	return category;
}

int library_shares(const library_game *g, int n, int i)
{
	for (int k = 0; k < n; k++)
		if (k != i && g[k].readable && g[i].readable && !strcmp(g[k].rom.id, g[i].rom.id))
			return k;
	return -1;
}

int library_find(const library_game *g, int n, const char *file)
{
	for (int i = 0; i < n; i++)
		if (!strcasecmp(g[i].file, file))
			return i;
	return -1;
}

int library_is_rom(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!strncmp(name, "._", 2))
		return 0;
	return dot && (!strcasecmp(dot, ".z64") || !strcasecmp(dot, ".n64") || !strcasecmp(dot, ".v64"));
}

static int utf8_continues(char c) { return ((unsigned char)c & 0xC0) == 0x80; }

/* The byte where character k starts (the end when s has fewer). */
static size_t utf8_at(const char *s, int k)
{
	size_t i = 0;
	for (; s[i] && k > 0; k--)
		for (i++; utf8_continues(s[i]); i++)
			;
	return i;
}

static int utf8_count(const char *s)
{
	int n = 0;
	for (; *s; s++)
		n += !utf8_continues(*s);
	return n;
}

void library_cut_middle(const char *s, int max, char *out, unsigned size)
{
	if (!size)
		return;
	char cut[512];
	const int n = utf8_count(s);
	if (max < 3 || n <= max) {
		snprintf(cut, sizeof(cut), "%s", s);
	} else {
		const int head = (max - 1) / 2, tail = max - 1 - head;
		const size_t h = utf8_at(s, head), t = utf8_at(s, n - tail);
		snprintf(cut, sizeof(cut), "%.*s\xE2\x80\xA6%s", (int)h, s, s + t);
	}
	size_t len = strlen(cut);
	if (len >= size) { /* never half a character */
		len = size - 1;
		while (len > 0 && utf8_continues(cut[len]))
			len--;
	}
	memcpy(out, cut, len);
	out[len] = 0;
}

/* Where each source pixel along one axis lands among n_out output pixels:
 * measured in units where a source pixel is n_out long and an output pixel
 * n_in, source pixel i spans [i * n_out, (i + 1) * n_out), so it falls in
 * output pixel at[i] with weight[i] units and in the next with the rest
 * (n_out <= n_in: never more than two). */
static void fit_axis(int n_in, int n_out, int *at, uint32_t *weight)
{
	for (int i = 0; i < n_in; i++) {
		const uint32_t start = (uint32_t)i * (uint32_t)n_out, end = start + (uint32_t)n_out;
		const uint32_t o = start / (uint32_t)n_in, edge = (o + 1) * (uint32_t)n_in;
		at[i] = (int)o;
		weight[i] = end <= edge ? (uint32_t)n_out : edge - start;
	}
}

int library_fit_rgba(const unsigned char *src, int w, int h, int max_w, int max_h, unsigned char *out, int *ow, int *oh)
{
	if (w <= 0 || h <= 0 || max_w <= 0 || max_h <= 0 || (uint64_t)w * (uint64_t)h > LIBRARY_FIT_MAX_PIXELS)
		return -1;
	if (w <= max_w && h <= max_h) {
		memcpy(out, src, (size_t)w * h * 4);
		*ow = w;
		*oh = h;
		return 0;
	}
	/* the side that is further over decides; the other follows, rounded */
	int tw, th;
	if ((uint64_t)w * max_h >= (uint64_t)h * max_w) {
		tw = max_w;
		th = (int)(((uint64_t)h * max_w + w / 2) / w);
	} else {
		th = max_h;
		tw = (int)(((uint64_t)w * max_h + h / 2) / h);
	}
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;
	int *xat = malloc(sizeof(int) * (size_t)w), *yat = malloc(sizeof(int) * (size_t)h);
	uint32_t *xw = malloc(sizeof(uint32_t) * (size_t)w), *yw = malloc(sizeof(uint32_t) * (size_t)h);
	uint32_t *row = malloc(sizeof(uint32_t) * 4 * (size_t)tw);     /* one source row, summed across */
	uint32_t *acc = calloc(8 * (size_t)tw, sizeof(uint32_t));       /* the output row being summed, and the next */
	int ok = xat && yat && xw && yw && row && acc;
	if (ok) {
		fit_axis(w, tw, xat, xw);
		fit_axis(h, th, yat, yw);
		/* each output pixel's weights add up to w across and h down: w * h in
		 * all, at most 16 M, so 255 times it fits 32 bits */
		const uint32_t total = (uint32_t)w * (uint32_t)h, half = total / 2;
		uint32_t *cur = acc, *next = acc + 4 * (size_t)tw;
		int y_out = 0;
		for (int y = 0; y < h; y++) {
			if (yat[y] > y_out) { /* the output row is whole: written, and the next one moves up */
				unsigned char *o = out + (size_t)y_out * tw * 4;
				for (int i = 0; i < 4 * tw; i++)
					o[i] = (unsigned char)((cur[i] + half) / total);
				uint32_t *t = cur;
				cur = next;
				next = t;
				memset(next, 0, sizeof(uint32_t) * 4 * (size_t)tw);
				y_out++;
			}
			memset(row, 0, sizeof(uint32_t) * 4 * (size_t)tw);
			const unsigned char *s = src + (size_t)y * w * 4;
			for (int x = 0; x < w; x++, s += 4) {
				uint32_t *r = row + 4 * xat[x];
				const uint32_t a = xw[x], b = (uint32_t)tw - a;
				r[0] += s[0] * a; r[1] += s[1] * a; r[2] += s[2] * a; r[3] += s[3] * a;
				if (b) {
					r[4] += s[0] * b; r[5] += s[1] * b; r[6] += s[2] * b; r[7] += s[3] * b;
				}
			}
			const uint32_t a = yw[y], b = (uint32_t)th - a;
			for (int i = 0; i < 4 * tw; i++) {
				cur[i] += row[i] * a;
				if (b)
					next[i] += row[i] * b;
			}
		}
		unsigned char *o = out + (size_t)y_out * tw * 4; /* the last row */
		for (int i = 0; i < 4 * tw; i++)
			o[i] = (unsigned char)((cur[i] + half) / total);
		*ow = tw;
		*oh = th;
	}
	free(xat);
	free(yat);
	free(xw);
	free(yw);
	free(row);
	free(acc);
	return ok ? 0 : -1;
}
