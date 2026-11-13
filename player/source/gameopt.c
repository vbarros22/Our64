/* Our64 player: per-game options (gameopt.h). */
#include "gameopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAMEOPT_MAX 512
static struct { char file[128]; int lines; int pic[GAMEOPT_PICTURE_KEYS]; } g_opt[GAMEOPT_MAX];
static int g_n;
static const char *const g_key[GAMEOPT_PICTURE_KEYS] = { "mode=", "shape=", "speed=", "filter=" };
static const int g_key_max[GAMEOPT_PICTURE_KEYS] = { 2, 1, 1, 3 };

static int find(const char *file)
{
	for (int i = 0; i < g_n; i++)
		if (!strcmp(g_opt[i].file, file))
			return i;
	return -1;
}

static int norm_lines(int lines) { return lines == 720 || lines == GAMEOPT_NATIVE ? lines : 480; }

static int is_default(int i)
{
	if (g_opt[i].lines != 480)
		return 0;
	for (int k = 0; k < GAMEOPT_PICTURE_KEYS; k++)
		if (g_opt[i].pic[k] >= 0)
			return 0;
	return 1;
}

static int add(const char *file)
{
	if (g_n >= GAMEOPT_MAX || !file[0] || strlen(file) >= sizeof(g_opt[0].file) || strchr(file, '\t') || strchr(file, '\n'))
		return -1;
	snprintf(g_opt[g_n].file, sizeof(g_opt[g_n].file), "%s", file);
	g_opt[g_n].lines = 480;
	for (int k = 0; k < GAMEOPT_PICTURE_KEYS; k++)
		g_opt[g_n].pic[k] = -1;
	return g_n++;
}

void gameopt_parse(const char *text)
{
	g_n = 0;
	char line[512];
	while (text && *text) {
		size_t n = strcspn(text, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) - 1 ? n : sizeof(line) - 1), text);
		text += n + (text[n] == '\n');
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
			line[--len] = 0;
		char *tab = strchr(line, '\t');
		if (!tab || tab == line)
			continue;
		*tab = 0;
		if (find(line) >= 0)
			continue;
		const int i = add(line);
		if (i < 0)
			continue;
		for (char *w = tab + 1; w && *w;) {
			char *next = strchr(w, '\t');
			if (next)
				*next++ = 0;
			if (!strcmp(w, "720p"))
				g_opt[i].lines = 720;
			else if (!strcmp(w, "native"))
				g_opt[i].lines = GAMEOPT_NATIVE;
			for (int k = 0; k < GAMEOPT_PICTURE_KEYS; k++) {
				const size_t kl = strlen(g_key[k]);
				if (!strncmp(w, g_key[k], kl) && w[kl] >= '0' && w[kl] <= '9' && !w[kl + 1] && w[kl] - '0' <= g_key_max[k])
					g_opt[i].pic[k] = w[kl] - '0';
			}
			w = next;
		}
		if (is_default(i))
			g_n--; /* nothing this Our64 knows was set on the line */
	}
}

int gameopt_read(const char *path)
{
	static char text[GAMEOPT_MAX * 170];
	FILE *f = fopen(path, "rb");
	size_t n = 0;
	if (f) {
		n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}
	text[n] = 0;
	gameopt_parse(text);
	return g_n;
}

int gameopt_text(char *out, unsigned size)
{
	int at = 0;
	char line[256];
	for (int i = 0; i < g_n; i++) {
		if (is_default(i))
			continue;
		int n = snprintf(line, sizeof(line), "%s", g_opt[i].file);
		if (g_opt[i].lines == 720)
			n += snprintf(line + n, sizeof(line) - (size_t)n, "\t720p");
		else if (g_opt[i].lines == GAMEOPT_NATIVE)
			n += snprintf(line + n, sizeof(line) - (size_t)n, "\tnative");
		for (int k = 0; k < GAMEOPT_PICTURE_KEYS; k++)
			if (g_opt[i].pic[k] >= 0)
				n += snprintf(line + n, sizeof(line) - (size_t)n, "\t%s%d", g_key[k], g_opt[i].pic[k]);
		n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
		if (out && (unsigned)(at + n) < size)
			memcpy(out + at, line, (size_t)n + 1);
		at += n;
	}
	if (out && size > 0 && at == 0)
		out[0] = 0;
	return at;
}

int gameopt_lines(const char *file)
{
	const int i = file ? find(file) : -1;
	return i >= 0 ? norm_lines(g_opt[i].lines) : 480;
}

int gameopt_set_lines(const char *file, int lines)
{
	int i = find(file);
	if (i < 0) {
		if (norm_lines(lines) == 480)
			return 0;
		if ((i = add(file)) < 0)
			return -1;
	}
	g_opt[i].lines = norm_lines(lines);
	return 0;
}

int gameopt_picture(const char *file, int key)
{
	const int i = file ? find(file) : -1;
	return i >= 0 && key >= 0 && key < GAMEOPT_PICTURE_KEYS ? g_opt[i].pic[key] : -1;
}

int gameopt_set_picture(const char *file, int key, int value)
{
	if (key < 0 || key >= GAMEOPT_PICTURE_KEYS)
		return -1;
	int i = find(file);
	if (i < 0) {
		if (value < 0)
			return 0;
		if ((i = add(file)) < 0)
			return -1;
	}
	g_opt[i].pic[key] = value < 0 || value > g_key_max[key] ? -1 : value;
	return 0;
}

void gameopt_renamed(const char *from, const char *to)
{
	const int i = find(from);
	if (i >= 0 && strlen(to) < sizeof(g_opt[0].file) && find(to) < 0)
		snprintf(g_opt[i].file, sizeof(g_opt[i].file), "%s", to);
}
