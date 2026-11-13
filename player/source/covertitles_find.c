/* Our64 player: covertitles_find, a binary search over the generated table
 * (covertitles.c stays data only). Plain C. */
#include "covertitles.h"

#include <stddef.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

const char *covertitles_find(uint32_t crc1, uint32_t crc2, char country)
{
	/* the pair's first row: the first not below (crc1, crc2) */
	unsigned lo = 0, hi = covertitles_count;
	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		const covertitle *t = &covertitles[mid];
		if (t->crc1 < crc1 || (t->crc1 == crc1 && t->crc2 < crc2))
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == covertitles_count || covertitles[lo].crc1 != crc1 || covertitles[lo].crc2 != crc2)
		return NULL;
	/* a pair two names share: the row that lists the country */
	for (unsigned i = lo; country && i < covertitles_count && covertitles[i].crc1 == crc1 &&
			covertitles[i].crc2 == crc2; i++)
		if (covertitles[i].countries && strchr(covertitles[i].countries, country))
			return covertitles[i].name;
	return covertitles[lo].name;
}

const char *covertitles_by_code(const char *code)
{
	if (!code)
		return NULL;
	unsigned lo = 0, hi = covercodes_count;
	while (lo < hi) {
		const unsigned mid = lo + (hi - lo) / 2;
		const int c = strncmp(covercodes[mid].code, code, 4);
		if (!c)
			return covercodes[mid].name;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* title's key: lower-case letters and digits only, tags in () and [] left out,
 * and "the" dropped where it is an article: at the start, or as ", The". */
static void title_key(const char *t, char *out, unsigned size)
{
	unsigned n = 0;
	int depth = 0;
	if (!strncasecmp(t, "the ", 4))
		t += 4;
	for (; *t && n + 1 < size; t++) {
		if (*t == '(' || *t == '[') {
			depth++;
			continue;
		}
		if (*t == ')' || *t == ']') {
			if (depth)
				depth--;
			continue;
		}
		if (depth)
			continue;
		if (*t == ',' && !strncasecmp(t, ", the", 5) && !isalnum((unsigned char)t[5])) {
			t += 4;
			continue;
		}
		if (isalnum((unsigned char)*t))
			out[n++] = (char)tolower((unsigned char)*t);
	}
	out[n] = 0;
}

static const char *const g_region[][2] = {
	{ "E", "USA" }, { "J", "Japan" }, { "PXY", "Europe" }, { "D", "Germany" }, { "F", "France" },
	{ "I", "Italy" }, { "S", "Spain" }, { "U", "Australia" }, { "B", "Brazil" },
};
static const char *const g_tags[] = { "(Rev", "(Beta", "(Proto", "(Demo", "(Kiosk", "GameCube",
	"Virtual Console", "Collection", "(Debug", "(Pirate", "(Unl" };

/* Lower is better: not in the country's region costs 100, each tag 1. */
static int score(const char *name, char country)
{
	int s = 100;
	for (unsigned i = 0; country && i < sizeof(g_region) / sizeof(g_region[0]); i++)
		if (strchr(g_region[i][0], country) && strstr(name, g_region[i][1]))
			s = 0;
	for (unsigned i = 0; i < sizeof(g_tags) / sizeof(g_tags[0]); i++)
		if (strstr(name, g_tags[i]))
			s++;
	return s;
}

const char *covertitles_by_title(const char *title, char country)
{
	char want[256], key[256];
	if (!title)
		return NULL;
	title_key(title, want, sizeof(want));
	if (!want[0])
		return NULL;
	const char *best = NULL;
	int best_score = 0;
	for (unsigned i = 0; i < covertitles_count + covercodes_count; i++) {
		const char *name = i < covertitles_count ? covertitles[i].name : covercodes[i - covertitles_count].name;
		title_key(name, key, sizeof(key));
		if (strcmp(key, want))
			continue;
		const int s = score(name, country);
		if (!best || s < best_score) {
			best = name;
			best_score = s;
		}
	}
	return best;
}
