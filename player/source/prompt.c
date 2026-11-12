/* Our64 player: the screen mode prompt and its words (see prompt.h). */
#include "prompt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static const char *const g_mode_name[MODES] = { "TV only", "TV + GamePad", "GamePad only" };

const char *mode_name(int mode) { return mode >= 0 && mode < MODES ? g_mode_name[mode] : "?"; }
int mode_uses_gamepad(int mode) { return mode == MODE_BOTH || mode == MODE_GAMEPAD; }

/* ---- settings.cfg ---- */

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static const char *const g_picture_name[PICTURES] = { "4:3", "16:9 stretched" };

const char *picture_name(int picture) { return picture >= 0 && picture < PICTURES ? g_picture_name[picture] : "?"; }
static const char *const g_filter_name[FILTERS] = { "Smooth", "N64" };
const char *filter_name(int filter) { return filter >= 0 && filter < FILTERS ? g_filter_name[filter] : "?"; }
static const char *const g_look_name[LOOKS] = { "Clean", "CRT" };
const char *look_name(int look) { return look >= 0 && look < LOOKS ? g_look_name[look] : "?"; }
static const char *const g_theme_name[THEMES] = { "Standard", "Comrade" };
const char *theme_name(int theme) { return theme >= 0 && theme < THEMES ? g_theme_name[theme] : "?"; }
static const char *const g_layout_name[LAYOUTS] = { "Covers", "List" };
const char *layout_name(int layout) { return layout >= 0 && layout < LAYOUTS ? g_layout_name[layout] : "?"; }
static const char *const g_start_name[STARTS] = { "All", "Recent", "Favorites" };
const char *start_name(int start) { return start >= 0 && start < STARTS ? g_start_name[start] : "?"; }
static const char *const g_start_key[STARTS] = { "all", "recent", "favorites" }; /* as written to settings.cfg */

int settings_parse(const char *text, our_settings *s)
{
	s->mode = -1;
	s->picture = PICTURE_43;
	s->filter = FILTER_SMOOTH;
	s->look = LOOK_CLEAN;
	s->theme = THEME_STANDARD;
	s->layout = LAYOUT_COVERS;
	s->last_game[0] = 0;
	for (int i = 0; i < 4; i++)
		s->rumble[i] = 1;
	memset(s->recent, 0, sizeof(s->recent));
	s->rom_folder[0] = 0;
	s->rom_drive[0] = 0;
	s->auto_resume = 1;
	s->start_on = START_RECENT;
	int recent_line = 0;
	int result = SETTINGS_MISSING;
	char line[1024]; /* a full Recent games line is up to 5 x 131 bytes */
	while (*text) {
		size_t n = strcspn(text, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), text);
		text += n + (text[n] == '\n');
		char *k = trim(line);
		if (!*k || *k == '#')
			continue;
		char *eq = strchr(k, '=');
		if (!eq) {
			result = SETTINGS_DAMAGED;
			continue;
		}
		*eq = 0;
		k = trim(k);
		char *v = trim(eq + 1);
		int found = -1;
		if (!strcasecmp(k, "Screen mode")) {
			for (int m = 0; m < MODES; m++)
				if (!strcasecmp(v, g_mode_name[m]))
					found = s->mode = m;
		} else if (!strcasecmp(k, "Picture")) {
			for (int p = 0; p < PICTURES; p++)
				if (!strcasecmp(v, g_picture_name[p]))
					found = s->picture = p;
		} else if (!strcasecmp(k, "Texture filter")) {
			for (int f = 0; f < FILTERS; f++)
				if (!strcasecmp(v, g_filter_name[f]))
					found = s->filter = f;
		} else if (!strcasecmp(k, "TV look")) {
			for (int l = 0; l < LOOKS; l++)
				if (!strcasecmp(v, g_look_name[l]))
					found = s->look = l;
		} else if (!strcasecmp(k, "Theme")) {
			for (int t = 0; t < THEMES; t++)
				if (!strcasecmp(v, g_theme_name[t]))
					found = s->theme = t;
		} else if (!strcasecmp(k, "Our ROMs")) {
			for (int l = 0; l < LAYOUTS; l++)
				if (!strcasecmp(v, g_layout_name[l]))
					found = s->layout = l;
		} else if (!strcasecmp(k, "Last game")) {
			snprintf(s->last_game, sizeof(s->last_game), "%s", v);
			found = 1;
		} else if (!strcasecmp(k, "Recent games")) {
			int n = 0;
			recent_line = found = 1;
			for (char *w = strtok(v, "|"); w && n < RECENT_GAMES; w = strtok(NULL, "|")) {
				w = trim(w);
				if (*w)
					snprintf(s->recent[n++], sizeof(s->recent[0]), "%s", w);
			}
		} else if (!strcasecmp(k, "Rumble")) { /* "on, on, off, on": players 1-4 */
			int r[4], n = 0;
			found = 1;
			for (char *w = strtok(v, " ,"); w && n < 4; w = strtok(NULL, " ,"))
				r[n++] = !strcasecmp(w, "on") ? 1 : !strcasecmp(w, "off") ? 0 : -1;
			for (int i = 0; i < n; i++)
				if (r[i] < 0)
					found = -1;
			if (n == 4 && found > 0)
				memcpy(s->rumble, r, sizeof(r));
			else
				found = -1;
		} else if (!strcasecmp(k, "Auto-resume")) {
			if (!strcasecmp(v, "on") || !strcasecmp(v, "off"))
				found = s->auto_resume = !strcasecmp(v, "on");
		} else if (!strcasecmp(k, "Start on")) {
			for (int i = 0; i < STARTS; i++)
				if (!strcasecmp(v, g_start_key[i]))
					found = s->start_on = i;
		} else if (!strcasecmp(k, "ROM folder")) {
			snprintf(s->rom_folder, sizeof(s->rom_folder), "%s", v);
			found = 1;
		} else if (!strcasecmp(k, "ROM drive")) {
			size_t n = strlen(v); /* cut between UTF-8 characters, never inside one */
			if (n >= sizeof(s->rom_drive))
				for (n = sizeof(s->rom_drive) - 1; n > 0 && ((unsigned char)v[n] & 0xC0) == 0x80; n--)
					;
			memcpy(s->rom_drive, v, n);
			s->rom_drive[n] = 0;
			found = 1;
		} else {
			continue; /* a setting this Our64 does not know: kept by nobody, harmless */
		}
		if (found < 0)
			result = SETTINGS_DAMAGED;
		else if (result == SETTINGS_MISSING)
			result = 0;
	}
	if (!recent_line && s->last_game[0]) /* from before the Recent tab: the last game starts it */
		snprintf(s->recent[0], sizeof(s->recent[0]), "%s", s->last_game);
	return result;
}

void settings_played(our_settings *s, const char *file)
{
	char keep[RECENT_GAMES][128];
	int n = 0;
	snprintf(keep[n++], sizeof(keep[0]), "%s", file);
	for (int i = 0; i < RECENT_GAMES && n < RECENT_GAMES && s->recent[i][0]; i++)
		if (strcasecmp(s->recent[i], file))
			snprintf(keep[n++], sizeof(keep[0]), "%s", s->recent[i]);
	memset(s->recent, 0, sizeof(s->recent));
	memcpy(s->recent, keep, sizeof(keep[0]) * n);
	snprintf(s->last_game, sizeof(s->last_game), "%s", file);
}

int settings_read(const char *path, our_settings *s)
{
	FILE *f = fopen(path, "rb");
	char text[4096];
	size_t n = 0;
	if (f) {
		n = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
	}
	text[n] = 0;
	if (strlen(text) != n) { /* a zero byte: not text */
		settings_parse("", s);
		return SETTINGS_DAMAGED;
	}
	return settings_parse(text, s);
}

int settings_format_all(char *out, unsigned size, const our_settings *s)
{
	char recent[RECENT_GAMES * 131] = "";
	for (int i = 0; i < RECENT_GAMES && s->recent[i][0]; i++) {
		size_t at = strlen(recent);
		snprintf(recent + at, sizeof(recent) - at, "%s%s", i ? " | " : "", s->recent[i]);
	}
	return snprintf(out, size, "# Our64 settings. Screen mode: TV only, TV + GamePad or GamePad only. "
	                           "Picture: 4:3 or 16:9 stretched. Texture filter: Smooth or N64. TV look: Clean or CRT. Theme: Standard or Comrade. Our ROMs: Covers or List. "
	                           "Auto-resume: on or off. "
	                           "Start on: all, recent or favorites.\n"
	                           "Screen mode = %s\nPicture = %s\nTexture filter = %s\nTV look = %s\nTheme = %s\nOur ROMs = %s\nROM folder = %s\nROM drive = %s\nLast game = %s\nRecent games = %s\n"
	                           "Rumble = %s, %s, %s, %s\nAuto-resume = %s\nStart on = %s\n",
	                s->mode >= 0 ? mode_name(s->mode) : mode_name(MODE_BOTH), picture_name(s->picture), filter_name(s->filter), look_name(s->look), theme_name(s->theme), layout_name(s->layout), s->rom_folder, s->rom_drive, s->last_game,
	                recent, s->rumble[0] ? "on" : "off", s->rumble[1] ? "on" : "off", s->rumble[2] ? "on" : "off",
	                s->rumble[3] ? "on" : "off", s->auto_resume ? "on" : "off",
	                g_start_key[s->start_on >= 0 && s->start_on < STARTS ? s->start_on : START_RECENT]);
}

int settings_parse_mode(const char *text)
{
	int mode = SETTINGS_MISSING;
	char line[160];
	while (*text) {
		size_t n = strcspn(text, "\n");
		snprintf(line, sizeof(line), "%.*s", (int)(n < sizeof(line) ? n : sizeof(line) - 1), text);
		text += n + (text[n] == '\n');
		char *s = trim(line);
		if (!*s || *s == '#')
			continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			for (char *c = s; *c; c++)
				if (!isprint((unsigned char)*c))
					return SETTINGS_DAMAGED;
			continue;
		}
		*eq = 0;
		if (strcasecmp(trim(s), "Screen mode"))
			continue;
		char *v = trim(eq + 1);
		mode = SETTINGS_DAMAGED;
		for (int m = 0; m < MODES; m++)
			if (!strcasecmp(v, g_mode_name[m]))
				mode = m;
		if (mode == SETTINGS_DAMAGED)
			return mode;
	}
	return mode;
}

int settings_read_mode(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return SETTINGS_MISSING;
	char text[2048];
	size_t n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;
	if (strlen(text) != n)
		return SETTINGS_DAMAGED; /* a zero byte: not text */
	return settings_parse_mode(text);
}

int settings_format(char *out, unsigned size, int mode)
{
	return snprintf(out, size, "# Our64 settings. Screen mode: TV only, TV + GamePad or GamePad only.\n"
	                           "Screen mode = %s\n", mode_name(mode));
}

/* ---- the prompt ---- */

int prompt_enabled(const prompt *p, int mode)
{
	return mode >= 0 && mode < MODES && (p->gamepad || !mode_uses_gamepad(mode));
}

void prompt_init(prompt *p, int last, int gamepad)
{
	p->selected = last >= 0 && last < MODES ? last : MODE_BOTH;
	p->touched = -1;
	prompt_set_gamepad(p, gamepad);
}

void prompt_set_gamepad(prompt *p, int connected)
{
	p->gamepad = connected;
	if (!prompt_enabled(p, p->selected))
		p->selected = MODE_TV;
}

int prompt_event(prompt *p, int event, int card)
{
	switch (event) {
	case PROMPT_LEFT:
	case PROMPT_RIGHT: {
		int step = event == PROMPT_LEFT ? -1 : 1;
		for (int m = p->selected + step; m >= 0 && m < MODES; m += step)
			if (prompt_enabled(p, m)) {
				p->selected = m;
				break;
			}
		return -1;
	}
	case PROMPT_ACCEPT:
		return prompt_enabled(p, p->selected) ? p->selected : -1;
	case PROMPT_TOUCH_DOWN:
		p->touched = prompt_enabled(p, card) ? card : -1;
		return -1;
	case PROMPT_TOUCH_UP: {
		int was = p->touched;
		p->touched = -1;
		if (was < 0 || was != card)
			return -1;
		if (p->selected != card) {
			p->selected = card;
			return -1;
		}
		return card;
	}
	}
	return -1;
}

/* Three cards across the middle, with room above for the headline and below
 * for the line and the legend. */
#define CARD_W 236
#define CARD_H 196
#define CARD_GAP 28
#define CARD_Y 128

void prompt_card_rect(int card, int r[4])
{
	int x0 = (UI_W - (MODES * CARD_W + (MODES - 1) * CARD_GAP)) / 2;
	r[0] = x0 + card * (CARD_W + CARD_GAP);
	r[1] = CARD_Y;
	r[2] = CARD_W;
	r[3] = CARD_H;
}

int prompt_card_at(int x, int y)
{
	for (int i = 0; i < MODES; i++) {
		int r[4];
		prompt_card_rect(i, r);
		if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3])
			return i;
	}
	return -1;
}

/* ---- words ---- */

static const char *const g_month[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void clock_text(char *out, unsigned size, const struct tm *t, int us)
{
	if (us)
		snprintf(out, size, "%d:%02d %s", (t->tm_hour + 11) % 12 + 1, t->tm_min, t->tm_hour < 12 ? "AM" : "PM");
	else
		snprintf(out, size, "%02d:%02d", t->tm_hour, t->tm_min);
}

void ui_clock(char *out, unsigned size, time_t t, int us)
{
	struct tm tm;
	gmtime_r(&t, &tm);
	clock_text(out, size, &tm, us);
}

void ui_when(char *out, unsigned size, time_t now, time_t then, int us)
{
	struct tm n, t;
	gmtime_r(&now, &n);
	gmtime_r(&then, &t);
	char clock[16];
	clock_text(clock, sizeof(clock), &t, us);
	long days = (long)(now / 86400) - (long)(then / 86400);
	if (then <= now && days == 0)
		snprintf(out, size, "today %s", clock);
	else if (then <= now && days == 1)
		snprintf(out, size, "yesterday %s", clock);
	else if (t.tm_year == n.tm_year)
		snprintf(out, size, us ? "%2$s %1$d, %3$s" : "%d %s %s", t.tm_mday, g_month[t.tm_mon], clock);
	else if (us)
		snprintf(out, size, "%s %d, %d, %s", g_month[t.tm_mon], t.tm_mday, t.tm_year + 1900, clock);
	else
		snprintf(out, size, "%d %s %d %s", t.tm_mday, g_month[t.tm_mon], t.tm_year + 1900, clock);
}

void ui_game_title(char *out, unsigned size, const char *file)
{
	const char *slash = strrchr(file, '/');
	const char *name = slash ? slash + 1 : file;
	char s[256];
	snprintf(s, sizeof(s), "%s", name);
	char *dot = strrchr(s, '.');
	if (dot && dot != s)
		*dot = 0;
	/* trailing (...) and [...] groups */
	char *end = s + strlen(s);
	for (;;) {
		while (end > s && end[-1] == ' ')
			end--;
		if (end == s || (end[-1] != ')' && end[-1] != ']'))
			break;
		char open = end[-1] == ')' ? '(' : '[';
		char *o = end - 1;
		while (o > s && *o != open)
			o--;
		if (*o != open || o == s)
			break;
		end = o;
	}
	if (end > s)
		*end = 0;
	/* No-Intro's file names: "Legend of Zelda, The - Majora's Mask" reads
	 * "The Legend of Zelda: Majora's Mask". Each part between " - " gets its
	 * trailing ", The", ", A" or ", An" back in front; " - " becomes ": ". */
	static const char *const articles[] = { "The", "A", "An" };
	char t[256];
	unsigned n = 0;
	const char *part = s;
	for (;;) {
		const char *dash = strstr(part, " - ");
		size_t len = dash ? (size_t)(dash - part) : strlen(part);
		const char *art = NULL;
		for (unsigned i = 0; i < 3 && !art; i++) {
			size_t a = strlen(articles[i]);
			if (len > a + 2 && part[len - a - 2] == ',' && part[len - a - 1] == ' ' &&
			    !strncmp(part + len - a, articles[i], a))
				art = articles[i];
		}
		if (art) {
			size_t a = strlen(art);
			n += snprintf(t + n, sizeof(t) - n, "%s %.*s", art, (int)(len - a - 2), part);
		} else
			n += snprintf(t + n, sizeof(t) - n, "%.*s", (int)len, part);
		if (n >= sizeof(t))
			n = sizeof(t) - 1;
		if (!dash)
			break;
		n += snprintf(t + n, sizeof(t) - n, ": ");
		if (n >= sizeof(t))
			n = sizeof(t) - 1;
		part = dash + 3;
	}
	snprintf(out, size, "%s", t);
}

const char *ui_sort_title(const char *title)
{
	static const char *const articles[] = { "The ", "A ", "An " };
	for (unsigned i = 0; i < 3; i++) {
		size_t a = strlen(articles[i]);
		if (!strncasecmp(title, articles[i], a) && title[a])
			return title + a;
	}
	return title;
}

void prompt_line(char *out, unsigned size, const char *title, int launch, const char *when)
{
	char rest[200];
	switch (launch) {
	case LAUNCH_RESUME:
		snprintf(rest, sizeof(rest), "resumes from %s", when);
		break;
	case LAUNCH_FRESH:
		snprintf(rest, sizeof(rest), "starts at the title screen (B held). Your in-game saves are kept; "
		                             "the %s autosave is replaced after a minute. HOME keeps it.", when);
		break;
	case LAUNCH_DAMAGED:
		snprintf(rest, sizeof(rest), "starts at the title screen: the autosave is damaged. Your in-game saves are kept.");
		break;
	case LAUNCH_OTHER_VERSION:
		snprintf(rest, sizeof(rest), "starts at the title screen: the autosave is from another Our64 version. "
		                             "Your in-game saves are kept.");
		break;
	case LAUNCH_OTHER_GAME:
		snprintf(rest, sizeof(rest), "starts at the title screen: the autosave is for another game. "
		                             "Your in-game saves are kept.");
		break;
	default:
		snprintf(rest, sizeof(rest), "starts at the title screen");
		break;
	}
	/* The title gives way first: what A will do must always fit. */
	int room = (int)size - (int)strlen(rest) - 3;
	if (room < 1)
		snprintf(out, size, "%s", rest);
	else
		snprintf(out, size, "%.*s: %s", room, title, rest);
}
