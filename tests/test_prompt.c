/* PC tests for player/source/prompt.c: the screen mode prompt, its words, settings.cfg. */
#include "prompt.h"

#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_failed = 1; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { printf("  FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); g_failed = 1; } } while (0)

static void test_first_launch_and_last_choice(void)
{
	prompt p;
	prompt_init(&p, -1, 1);
	CHECK(p.selected == MODE_BOTH);
	prompt_init(&p, MODE_GAMEPAD, 1);
	CHECK(p.selected == MODE_GAMEPAD);
	/* A at once chooses the pre-selected card */
	CHECK(prompt_event(&p, PROMPT_ACCEPT, -1) == MODE_GAMEPAD);
}

static void test_moves_stop_at_the_ends(void)
{
	prompt p;
	prompt_init(&p, MODE_BOTH, 1);
	CHECK(prompt_event(&p, PROMPT_LEFT, -1) == -1);
	CHECK(p.selected == MODE_TV);
	prompt_event(&p, PROMPT_LEFT, -1);
	CHECK(p.selected == MODE_TV); /* no wrap */
	prompt_event(&p, PROMPT_RIGHT, -1);
	prompt_event(&p, PROMPT_RIGHT, -1);
	prompt_event(&p, PROMPT_RIGHT, -1);
	CHECK(p.selected == MODE_GAMEPAD);
}

static void test_no_gamepad(void)
{
	prompt p;
	prompt_init(&p, MODE_GAMEPAD, 0);
	CHECK(p.selected == MODE_TV);
	CHECK(!prompt_enabled(&p, MODE_BOTH) && !prompt_enabled(&p, MODE_GAMEPAD));
	prompt_event(&p, PROMPT_RIGHT, -1);
	CHECK(p.selected == MODE_TV); /* the GamePad cards cannot be selected */
	/* a touch cannot happen without one, but a stale one must not choose either */
	CHECK(prompt_event(&p, PROMPT_TOUCH_DOWN, MODE_GAMEPAD) == -1);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, MODE_GAMEPAD) == -1);
	/* it turns on: its cards come back, the selection stays */
	prompt_set_gamepad(&p, 1);
	CHECK(prompt_enabled(&p, MODE_GAMEPAD) && p.selected == MODE_TV);
	/* selected on a GamePad card and it goes off: back to TV only */
	prompt_event(&p, PROMPT_RIGHT, -1);
	prompt_set_gamepad(&p, 0);
	CHECK(p.selected == MODE_TV);
}

static void test_touch(void)
{
	prompt p;
	prompt_init(&p, MODE_BOTH, 1);
	/* a tap on another card selects it, and does not choose */
	CHECK(prompt_event(&p, PROMPT_TOUCH_DOWN, MODE_TV) == -1);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, MODE_TV) == -1);
	CHECK(p.selected == MODE_TV);
	/* a tap on the selected card chooses it */
	prompt_event(&p, PROMPT_TOUCH_DOWN, MODE_TV);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, MODE_TV) == MODE_TV);
	/* down on one card, up on another or in a gap: nothing */
	prompt_init(&p, MODE_BOTH, 1);
	prompt_event(&p, PROMPT_TOUCH_DOWN, MODE_BOTH);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, MODE_GAMEPAD) == -1);
	CHECK(p.selected == MODE_BOTH);
	prompt_event(&p, PROMPT_TOUCH_DOWN, MODE_BOTH);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, -1) == -1);
	/* down in a gap, up on the selected card: nothing */
	prompt_event(&p, PROMPT_TOUCH_DOWN, -1);
	CHECK(prompt_event(&p, PROMPT_TOUCH_UP, MODE_BOTH) == -1);
}

static void test_layout(void)
{
	int r[4], last_right = 0;
	for (int i = 0; i < MODES; i++) {
		prompt_card_rect(i, r);
		CHECK(r[2] >= 62 && r[3] >= 62);              /* touch targets */
		CHECK(r[0] > last_right);                     /* left to right, with gaps */
		CHECK(prompt_card_at(r[0] + r[2] / 2, r[1] + r[3] / 2) == i);
		CHECK(prompt_card_at(r[0] - 1, r[1] + 1) != i);
		last_right = r[0] + r[2];
	}
	CHECK(last_right < UI_W);
	prompt_card_rect(0, r);
	CHECK(prompt_card_at(r[0] + r[2] + 2, r[1] + 5) == -1); /* the gap */
}

static void test_settings(void)
{
	CHECK(settings_parse_mode("Screen mode = TV + GamePad\n") == MODE_BOTH);
	CHECK(settings_parse_mode("# Our64 settings\nscreen mode=tv only\n") == MODE_TV);
	CHECK(settings_parse_mode("Screen mode = GamePad only") == MODE_GAMEPAD);
	CHECK(settings_parse_mode("") == SETTINGS_MISSING);
	CHECK(settings_parse_mode("Speed display = on\n") == SETTINGS_MISSING);
	CHECK(settings_parse_mode("Screen mode = banana\n") == SETTINGS_DAMAGED);
	CHECK(settings_parse_mode("\x01\x02garbage") == SETTINGS_DAMAGED);
	char text[128];
	for (int m = 0; m < MODES; m++) {
		CHECK(settings_format(text, sizeof(text), m) > 0);
		CHECK(settings_parse_mode(text) == m);
	}
	CHECK(settings_read_mode("build/no-such-settings.cfg") == SETTINGS_MISSING);
}

static void test_settings_file(void)
{
	our_settings s;
	CHECK(settings_parse("", &s) == SETTINGS_MISSING && s.mode == -1 && s.picture == PICTURE_43 && !s.last_game[0]);
	CHECK(settings_parse("Screen mode = GamePad only\nPicture = 16:9 stretched\nLast game = Super Mario 64 (USA).z64\n", &s) == 0);
	CHECK(s.mode == MODE_GAMEPAD && s.picture == PICTURE_WIDE && !strcmp(s.last_game, "Super Mario 64 (USA).z64"));
	CHECK(settings_parse("Screen mode = banana\nLast game = Banjo.z64\n", &s) == SETTINGS_DAMAGED);
	CHECK(s.mode == -1 && !strcmp(s.last_game, "Banjo.z64")); /* the good lines are kept */
	CHECK(settings_parse("picture=4:3\n", &s) == 0 && s.picture == PICTURE_43);
	CHECK(s.rumble[0] && s.rumble[3]);
	CHECK(settings_parse("Rumble = on, off, on, off\n", &s) == 0 && s.rumble[0] && !s.rumble[1] && !s.rumble[3]);
	CHECK(settings_parse("Rumble = on, maybe\n", &s) == SETTINGS_DAMAGED && s.rumble[1]);
	CHECK(s.theme == THEME_STANDARD); /* the default */
	CHECK(settings_parse("Theme = Comrade\n", &s) == 0 && s.theme == THEME_COMRADE);
	CHECK(settings_parse("Theme = Tartan\n", &s) == SETTINGS_DAMAGED && s.theme == THEME_STANDARD);
	CHECK(s.layout == LAYOUT_COVERS); /* the default */
	CHECK(settings_parse("Our ROMs = List\n", &s) == 0 && s.layout == LAYOUT_LIST);
	our_settings w = { MODE_TV, PICTURE_WIDE, THEME_COMRADE, LAYOUT_LIST, "Dr. Mario 64 (USA).z64", { 1, 0, 1, 1 } };
	char text[2048]; /* as setup_wiiu.c writes it */
	CHECK(settings_format_all(text, sizeof(text), &w) > 0);
	CHECK(settings_parse(text, &s) == 0 && s.mode == MODE_TV && s.picture == PICTURE_WIDE && s.theme == THEME_COMRADE && s.layout == LAYOUT_LIST);
	CHECK(!strcmp(s.last_game, "Dr. Mario 64 (USA).z64") && !s.rumble[1] && s.rumble[2]);
	CHECK(!strcmp(picture_name(PICTURE_43), "4:3") && !strcmp(picture_name(PICTURE_WIDE), "16:9 stretched"));
	/* Texture filter: Smooth by default, written and read back */
	CHECK(settings_parse("", &s) == SETTINGS_MISSING && s.filter == FILTER_SMOOTH);
	CHECK(settings_parse("Texture filter = N64\n", &s) == 0 && s.filter == FILTER_N64);
	CHECK(settings_parse("texture filter = smooth\n", &s) == 0 && s.filter == FILTER_SMOOTH);
	CHECK(settings_parse("Texture filter = blurry\n", &s) == SETTINGS_DAMAGED && s.filter == FILTER_SMOOTH);
	w.filter = FILTER_N64;
	CHECK(settings_format_all(text, sizeof(text), &w) > 0 && strstr(text, "\nTexture filter = N64\n"));
	CHECK(settings_parse(text, &s) == 0 && s.filter == FILTER_N64 && s.picture == PICTURE_WIDE && s.theme == THEME_COMRADE);
	/* TV look: Clean by default, written and read back */
	CHECK(settings_parse("", &s) == SETTINGS_MISSING && s.look == LOOK_CLEAN);
	CHECK(settings_parse("TV look = CRT\n", &s) == 0 && s.look == LOOK_CRT);
	CHECK(settings_parse("TV look = sepia\n", &s) == SETTINGS_DAMAGED && s.look == LOOK_CLEAN);
	CHECK(settings_parse("TV look = Soft CRT\n", &s) == 0 && s.look == LOOK_SOFT && !strcmp(look_name(LOOK_SOFT), "Soft CRT"));
	w.look = LOOK_CRT;
	CHECK(settings_format_all(text, sizeof(text), &w) > 0 && strstr(text, "\nTV look = CRT\n"));
	CHECK(settings_parse(text, &s) == 0 && s.look == LOOK_CRT && s.filter == FILTER_N64);
	/* GamePad look: its own, Clean by default */
	CHECK(settings_parse("", &s) == SETTINGS_MISSING && s.pad_look == LOOK_CLEAN);
	CHECK(settings_parse("GamePad look = Soft CRT\n", &s) == 0 && s.pad_look == LOOK_SOFT && s.look == LOOK_CLEAN);
	w.pad_look = LOOK_SOFT;
	CHECK(settings_format_all(text, sizeof(text), &w) > 0 && strstr(text, "\nGamePad look = Soft CRT\n"));
	CHECK(settings_parse(text, &s) == 0 && s.pad_look == LOOK_SOFT && s.look == LOOK_CRT);
}

static void recent_games_keep_the_last_five_played_newest_first(void)
{
	our_settings s;
	/* a file from before the Recent tab: its last game starts the list */
	CHECK(settings_parse("Last game = Banjo.z64\n", &s) == 0 && !strcmp(s.recent[0], "Banjo.z64") && !s.recent[1][0]);
	const char *played[] = { "A.z64", "B.z64", "C.z64", "D.z64", "E.z64", "C.z64" };
	for (int i = 0; i < 6; i++)
		settings_played(&s, played[i]);
	CHECK(!strcmp(s.last_game, "C.z64") && !strcmp(s.recent[0], "C.z64") && !strcmp(s.recent[1], "E.z64"));
	CHECK(!strcmp(s.recent[3], "B.z64") && !strcmp(s.recent[4], "A.z64")); /* Banjo fell off; C only once */
	char text[1024];
	CHECK(settings_format_all(text, sizeof(text), &s) > 0);
	our_settings r;
	CHECK(settings_parse(text, &r) == 0 && !strcmp(r.recent[0], "C.z64") && !strcmp(r.recent[4], "A.z64"));
	CHECK(settings_parse("Recent games = \nLast game = X.z64\n", &r) == 0 && !r.recent[0][0]); /* none played since */
}

static void rom_folder_round_trip(void)
{
	our_settings s;
	CHECK(settings_parse("ROM folder = usb:1A2B-3C4D/Games/N64\n", &s) == 0);
	CHECK(!strcmp(s.rom_folder, "usb:1A2B-3C4D/Games/N64"));
	char text[2048];
	settings_format_all(text, sizeof(text), &s);
	our_settings back;
	CHECK(settings_parse(text, &back) == 0);
	CHECK(!strcmp(back.rom_folder, s.rom_folder));
}

/* The USB drive's label beside its folder (2026-11-04), so the folder reads in
 * full while the drive is unplugged; a settings.cfg from before has none. */
static void rom_drive_round_trips_and_may_be_missing(void)
{
	our_settings s;
	CHECK(settings_parse("ROM folder = usb:1A2B3C4D/ROMs/N64\nROM drive = OUR64 EXFAT\n", &s) == 0);
	CHECK(!strcmp(s.rom_drive, "OUR64 EXFAT"));
	char text[2048];
	settings_format_all(text, sizeof(text), &s);
	CHECK(strstr(text, "\nROM drive = OUR64 EXFAT\n"));
	our_settings back;
	CHECK(settings_parse(text, &back) == 0 && !strcmp(back.rom_drive, "OUR64 EXFAT"));
	CHECK(!strcmp(back.rom_folder, "usb:1A2B3C4D/ROMs/N64"));
	/* an older settings.cfg: no ROM drive line, nothing damaged */
	CHECK(settings_parse("ROM folder = usb:1A2B3C4D/ROMs/N64\n", &s) == 0);
	CHECK(s.rom_drive[0] == 0);
	/* written empty, read empty */
	settings_format_all(text, sizeof(text), &s);
	CHECK(strstr(text, "\nROM drive = \n") && settings_parse(text, &back) == 0 && back.rom_drive[0] == 0);
	/* a label too long for it is cut between characters, never inside one */
	char line[128] = "ROM drive = ";
	for (int i = 0; i < 16; i++)
		strcat(line, "\xC3\xA9"); /* 32 bytes */
	strcat(line, "\xC3\xA9\n");
	CHECK(settings_parse(line, &s) == 0 && strlen(s.rom_drive) == 32);
}

static void rom_folder_default(void)
{
	our_settings s;
	settings_parse("Screen mode = TV only\n", &s);
	CHECK(s.rom_folder[0] == 0); /* "": sd:/wiiu/our64/roms */
}

static void a_long_recent_list_and_folder_still_fit(void)
{
	our_settings s;
	settings_parse("", &s);
	for (int i = 0; i < RECENT_GAMES; i++)
		memset(s.recent[i], 'a' + i, sizeof(s.recent[i]) - 1), s.recent[i][sizeof(s.recent[i]) - 1] = 0;
	memset(s.rom_folder, 'f', sizeof(s.rom_folder) - 1);
	s.rom_folder[sizeof(s.rom_folder) - 1] = 0;
	memset(s.rom_drive, 'd', sizeof(s.rom_drive) - 1);
	s.rom_drive[sizeof(s.rom_drive) - 1] = 0;
	char text[2048];
	int n = settings_format_all(text, sizeof(text), &s);
	CHECK(n > 0 && n < (int)sizeof(text));
	our_settings back;
	settings_parse(text, &back);
	CHECK(!strcmp(back.recent[4], s.recent[4]) && !strcmp(back.rom_folder, s.rom_folder));
	CHECK(!strcmp(back.rom_drive, s.rom_drive));
}

static void auto_resume_and_start_on_round_trip(void)
{
	our_settings s, back;
	CHECK(settings_parse("Screen mode = TV only\n", &s) == 0 && s.auto_resume == 1 && s.start_on == START_RECENT);
	CHECK(settings_parse("Auto-resume = off\nStart on = favorites\n", &s) == 0 && !s.auto_resume && s.start_on == START_FAVORITES);
	CHECK(settings_parse("auto-resume=ON\nstart on = All\n", &s) == 0 && s.auto_resume && s.start_on == START_ALL);
	CHECK(settings_parse("Auto-resume = maybe\n", &s) == SETTINGS_DAMAGED && s.auto_resume == 1);
	CHECK(settings_parse("Start on = Letters\n", &s) == SETTINGS_DAMAGED && s.start_on == START_RECENT);
	char text[2048];
	for (int on = 0; on < 2; on++)
		for (int t = 0; t < STARTS; t++) {
			settings_parse("", &s);
			s.auto_resume = on;
			s.start_on = t;
			settings_format_all(text, sizeof(text), &s);
			CHECK(settings_parse(text, &back) == 0 && back.auto_resume == on && back.start_on == t);
		}
	s.auto_resume = 0;
	s.start_on = START_ALL;
	settings_format_all(text, sizeof(text), &s);
	CHECK(strstr(text, "\nAuto-resume = off\n") && strstr(text, "\nStart on = all\n"));
	CHECK(!strcmp(start_name(START_ALL), "All") && !strcmp(start_name(START_RECENT), "Recent") &&
	      !strcmp(start_name(START_FAVORITES), "Favorites"));
}

/* 2026-10-16 21:40 and friends, as UTC. */
#define DAY 86400
#define T(d, h, m) ((time_t)1760572800 + (d) * DAY + (h) * 3600 + (m) * 60) /* 2025-10-16 00:00 + days */

static void test_when(void)
{
	char s[64];
	time_t now = T(365, 22, 0); /* 2026-10-16 22:00 */
	ui_when(s, sizeof(s), now, T(365, 21, 40), 0);
	CHECK_STR(s, "today 21:40");
	ui_when(s, sizeof(s), now, T(365, 21, 40), 1);
	CHECK_STR(s, "today 9:40 PM");
	ui_when(s, sizeof(s), now, T(365, 0, 5), 1);
	CHECK_STR(s, "today 12:05 AM");
	ui_when(s, sizeof(s), now, T(364, 12, 0), 1);
	CHECK_STR(s, "yesterday 12:00 PM");
	ui_when(s, sizeof(s), now, T(363, 9, 3), 0);
	CHECK_STR(s, "14 Oct 09:03");
	ui_when(s, sizeof(s), now, T(363, 9, 3), 1);
	CHECK_STR(s, "Oct 14, 9:03 AM");
	ui_when(s, sizeof(s), now, T(0, 21, 40), 0); /* last year */
	CHECK_STR(s, "16 Oct 2025 21:40");
	ui_when(s, sizeof(s), now, T(0, 21, 40), 1);
	CHECK_STR(s, "Oct 16, 2025, 9:40 PM");
	/* a clock set back: never "today" for a time ahead of now */
	ui_when(s, sizeof(s), T(365, 8, 0), T(365, 9, 0), 0);
	CHECK_STR(s, "16 Oct 09:00");
}

static void test_title(void)
{
	char s[64];
	ui_game_title(s, sizeof(s), "sd:/wiiu/our64/roms/Super Mario 64 (USA).z64");
	CHECK_STR(s, "Super Mario 64");
	ui_game_title(s, sizeof(s), "Mario Kart 64 (E) (V1.1) [!].n64");
	CHECK_STR(s, "Mario Kart 64");
	ui_game_title(s, sizeof(s), "zelda.v64");
	CHECK_STR(s, "zelda");
	ui_game_title(s, sizeof(s), "(USA).z64"); /* nothing but tags: keep the name */
	CHECK_STR(s, "(USA)");
	ui_game_title(s, 8, "A Very Long Game Title.z64");
	CHECK(strlen(s) == 7);
	/* No-Intro names: the article back in front, " - " as a colon */
	ui_game_title(s, sizeof(s), "Legend of Zelda, The - Majora's Mask (USA).z64");
	CHECK_STR(s, "The Legend of Zelda: Majora's Mask");
	ui_game_title(s, sizeof(s), "American Tail, An - Fievel's Gold Rush (USA) (En,Es).z64");
	CHECK_STR(s, "An American Tail: Fievel's Gold Rush");
	ui_game_title(s, sizeof(s), "Bug's Life, A (USA).z64");
	CHECK_STR(s, "A Bug's Life");
	ui_game_title(s, sizeof(s), "Legend of Zelda, The - Ocarina of Time - Master Quest (USA).z64");
	CHECK_STR(s, "The Legend of Zelda: Ocarina of Time: Master Quest");
	ui_game_title(s, sizeof(s), "Hey You, Pikachu! (USA).z64"); /* a comma that is not an article */
	CHECK_STR(s, "Hey You, Pikachu!");
	ui_game_title(s, sizeof(s), "Spider-Man (USA).z64");
	CHECK_STR(s, "Spider-Man");
	CHECK_STR(ui_sort_title("The Legend of Zelda"), "Legend of Zelda");
	CHECK_STR(ui_sort_title("An American Tail"), "American Tail");
	CHECK_STR(ui_sort_title("Theme Park"), "Theme Park");
	CHECK_STR(ui_sort_title("A"), "A");
}

static void test_line(void)
{
	char s[256];
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_RESUME, "today 9:40 PM");
	CHECK_STR(s, "Super Mario 64: resumes from today 9:40 PM");
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_FIRST, "");
	CHECK_STR(s, "Super Mario 64: starts at the title screen");
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_FRESH, "today 9:40 PM");
	CHECK(strstr(s, "B held") && strstr(s, "saves are kept") && strstr(s, "9:40 PM") && strstr(s, "HOME"));
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_OTHER_VERSION, "");
	CHECK(strstr(s, "title screen") && strstr(s, "another Our64 version") && strstr(s, "saves are kept"));
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_DAMAGED, "");
	CHECK(strstr(s, "damaged") && strstr(s, "saves are kept"));
	prompt_line(s, sizeof(s), "Super Mario 64", LAUNCH_OTHER_GAME, "");
	CHECK(strstr(s, "another game"));
	/* never cut mid-way without room: the title gives way, not the time */
	prompt_line(s, 48, "A Forty Character Game Title Goes Right Here", LAUNCH_RESUME, "today 9:40 PM");
	CHECK(strstr(s, "today 9:40 PM") != NULL);
}

int main(void)
{
	test_first_launch_and_last_choice();
	test_moves_stop_at_the_ends();
	test_no_gamepad();
	test_touch();
	test_layout();
	test_settings();
	test_settings_file();
	recent_games_keep_the_last_five_played_newest_first();
	rom_folder_round_trip();
	rom_folder_default();
	rom_drive_round_trips_and_may_be_missing();
	a_long_recent_list_and_folder_still_fit();
	auto_resume_and_start_on_round_trip();
	test_when();
	test_title();
	test_line();
	if (g_failed)
		return 1;
	printf("  all passed\n");
	return 0;
}
