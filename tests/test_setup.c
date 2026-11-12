/* PC tests for player/source/setup.c: Our Settings, Our Controllers, Change
 * order and Edit buttons (ux/flows/setup; rulings in docs/ux/requirements/2026-10-16-setup.md). */
#include "setup.h"

#include <stdio.h>
#include <string.h>

static int g_failed, g_run;

#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; return; } } while (0)
#define BIT(s) (1u << (s))

static int tap_settings(settings_ui *s, int target)
{
	settings_event(s, PEV_TOUCH_DOWN, target);
	return settings_event(s, PEV_TOUCH_UP, target);
}

/* ---- Our Settings ---- */

static void settings_rows_are_four_pages(void)
{
	settings_ui s;
	settings_open(&s);
	CHECK(!strcmp(settings_row_name(SET_CONTROLLERS), "Controllers") && !strcmp(settings_row_name(SET_PICTURE), "Picture"));
	CHECK(!strcmp(settings_row_name(SET_GAMES), "Games") && !strcmp(settings_row_name(SET_THEME), "Theme"));
	CHECK(settings_event(&s, PEV_OK, -1) == SACT_CONTROLLERS);
	settings_event(&s, PEV_DOWN, -1);
	CHECK(settings_event(&s, PEV_OK, -1) == SACT_PICTURE);
	settings_event(&s, PEV_DOWN, -1);
	CHECK(settings_event(&s, PEV_OK, -1) == SACT_GAMES);
	settings_event(&s, PEV_DOWN, -1);
	CHECK(settings_event(&s, PEV_OK, -1) == SACT_THEME);
	settings_event(&s, PEV_DOWN, -1);
	CHECK(s.row == SET_THEME); /* stops at the end */
	CHECK(settings_event(&s, PEV_LEFT, -1) == SACT_NONE && settings_event(&s, PEV_BACK, -1) == SACT_BACK);
	int r[4];
	settings_row_rect(SET_GAMES, r);
	settings_event(&s, PEV_TOUCH_DOWN, settings_hit(r[0] + 5, r[1] + 5));
	CHECK(settings_event(&s, PEV_TOUCH_UP, settings_hit(r[0] + 5, r[1] + 5)) == SACT_NONE && s.row == SET_GAMES);
	settings_event(&s, PEV_TOUCH_DOWN, settings_hit(r[0] + 5, r[1] + 5));
	CHECK(settings_event(&s, PEV_TOUCH_UP, settings_hit(r[0] + 5, r[1] + 5)) == SACT_GAMES);
}

/* Settings > Theme: the colors and Our ROMs' layout, each applied with A (2026-10-30). */
static void theme_page_applies_colors_and_layout(void)
{
	look_ui l;
	look_open(&l, THEME_STANDARD, LAYOUT_COVERS);
	CHECK(l.row == LOOK_COLORS && !strcmp(look_row_name(LOOK_COLORS), "Colors") && !strcmp(look_row_name(LOOK_LAYOUT), "Our ROMs"));
	CHECK(look_event(&l, PEV_OK, -1) == LACT_NONE);
	look_event(&l, PEV_RIGHT, -1);
	CHECK(l.pick[LOOK_COLORS] == THEME_COMRADE && l.theme == THEME_STANDARD);
	CHECK(look_event(&l, PEV_OK, -1) == LACT_COLORS);
	look_applied(&l);
	CHECK(l.theme == THEME_COMRADE && !strcmp(look_value_name(LOOK_COLORS, THEME_COMRADE), "Comrade"));
	look_event(&l, PEV_DOWN, -1);
	look_event(&l, PEV_RIGHT, -1);
	look_event(&l, PEV_RIGHT, -1);
	CHECK(l.pick[LOOK_LAYOUT] == LAYOUT_LIST && !strcmp(look_value_name(LOOK_LAYOUT, LAYOUT_LIST), "List"));
	look_event(&l, PEV_UP, -1);
	CHECK(l.pick[LOOK_LAYOUT] == LAYOUT_COVERS); /* leaving a row drops its pick */
	look_event(&l, PEV_DOWN, -1);
	look_event(&l, PEV_RIGHT, -1);
	CHECK(look_event(&l, PEV_OK, -1) == LACT_LAYOUT);
	look_applied(&l);
	CHECK(l.layout == LAYOUT_LIST && l.theme == THEME_COMRADE);
	CHECK(look_event(&l, PEV_BACK, -1) == LACT_BACK && look_event(&l, PEV_HOME, -1) == LACT_HOME);
	int r[4];
	settings_value_rect(LOOK_COLORS, THEME_STANDARD, r);
	CHECK(look_hit(r[0] + 3, r[1] + 3) == LOOK_COLORS * 8 + THEME_STANDARD + 1);
}

static void settings_leads_to_controllers_and_back(void)
{
	settings_ui s;
	settings_open(&s);
	CHECK(s.row == SET_CONTROLLERS && SET_CONTROLLERS == 0);
	settings_event(&s, PEV_UP, -1);
	CHECK(s.row == SET_CONTROLLERS);
	CHECK(settings_event(&s, PEV_HOME, -1) == SACT_HOME);
	CHECK(!strcmp(settings_row_about(&s), "Players, rumble and buttons"));
	pause_key keys[6];
	int n = settings_legend(&s, INPUT_PRO, keys, 6);
	CHECK(n == 4 && !strcmp(keys[3].label, "OK") && !keys[3].dim); /* every row opens a page: never dimmed */
}

static void settings_taps(void)
{
	settings_ui s;
	int r[4];
	settings_open(&s);
	settings_row_rect(SET_PICTURE, r);
	int t = settings_hit(r[0] + r[2] - 10, r[1] + 3); /* the row's right end, by its ">" */
	CHECK(t == SET_PICTURE);
	CHECK(tap_settings(&s, t) == SACT_NONE && s.row == SET_PICTURE);
	CHECK(tap_settings(&s, t) == SACT_PICTURE);
	settings_row_rect(SET_CONTROLLERS, r);
	t = settings_hit(r[0] + 3, r[1] + 3);
	CHECK(tap_settings(&s, t) == SACT_NONE && s.row == SET_CONTROLLERS);
	CHECK(tap_settings(&s, t) == SACT_CONTROLLERS);
	CHECK(settings_hit(5, 5) == -1);
}

/* ---- Settings > Picture ---- */

static void picture_applies_what_left_right_picked(void)
{
	picture_ui p;
	picture_open(&p, MODE_BOTH, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 1);
	CHECK(p.row == PIC_SCREEN && picture_event(&p, PEV_OK, -1) == PICT_NONE);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_GAMEPAD && p.mode == MODE_BOTH);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_GAMEPAD); /* stops at the end */
	CHECK(picture_event(&p, PEV_OK, -1) == PICT_MODE);
	picture_applied(&p);
	CHECK(p.mode == MODE_GAMEPAD);
	picture_event(&p, PEV_DOWN, -1);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(picture_event(&p, PEV_OK, -1) == PICT_SHAPE);
	picture_applied(&p);
	CHECK(p.shape == PICTURE_WIDE);
	CHECK(!strcmp(picture_row_name(PIC_SCREEN), "Screen mode") && !strcmp(picture_row_name(PIC_SHAPE), "Shape"));
	CHECK(!strcmp(picture_value_name(PIC_SHAPE, PICTURE_WIDE), "16:9 stretched"));
	picture_event(&p, PEV_DOWN, -1); /* Texture filter: Smooth, then N64 */
	CHECK(p.row == PIC_FILTER && picture_event(&p, PEV_OK, -1) == PICT_NONE);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_FILTER] == FILTER_N64 && p.filter == FILTER_SMOOTH);
	CHECK(picture_event(&p, PEV_OK, -1) == PICT_FILTER);
	picture_applied(&p);
	CHECK(p.filter == FILTER_N64 && p.shape == PICTURE_WIDE && p.mode == MODE_GAMEPAD);
	picture_event(&p, PEV_DOWN, -1); /* TV look: Clean, then CRT */
	CHECK(p.row == PIC_LOOK);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_LOOK] == LOOK_CRT && p.look == LOOK_CLEAN && picture_event(&p, PEV_OK, -1) == PICT_LOOK);
	picture_applied(&p);
	CHECK(p.look == LOOK_CRT && p.filter == FILTER_N64);
	picture_event(&p, PEV_DOWN, -1);
	CHECK(p.row == PIC_LOOK); /* the last row */
	CHECK(!strcmp(picture_row_name(PIC_LOOK), "TV look") && !strcmp(picture_value_name(PIC_LOOK, LOOK_CRT), "CRT"));
	CHECK(!strcmp(picture_row_name(PIC_FILTER), "Texture filter"));
	CHECK(!strcmp(picture_value_name(PIC_FILTER, FILTER_SMOOTH), "Smooth") && !strcmp(picture_value_name(PIC_FILTER, FILTER_N64), "N64"));
	CHECK(picture_event(&p, PEV_BACK, -1) == PICT_BACK && picture_event(&p, PEV_HOME, -1) == PICT_HOME);
}

static void picture_lines_are_the_same_whatever_is_picked(void)
{
	picture_ui p;
	picture_open(&p, MODE_BOTH, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 1);
	p.row = PIC_SCREEN; /* Screen mode and Shape: the same whatever is picked (Zach, 2026-10-23) */
	CHECK(!strcmp(picture_row_about(&p), "Which screens show the game. The prompt at each start begins on it."));
	p.row = PIC_SHAPE;
	CHECK(!strcmp(picture_row_about(&p), "4:3 is the games' own shape; 16:9 stretches it to fill a wide TV."));
}

static void gamepad_only_is_skipped_without_a_gamepad_on_picture(void)
{
	picture_ui p;
	picture_open(&p, MODE_BOTH, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 0);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_BOTH); /* GamePad only skipped: nothing to the right */
	picture_open(&p, MODE_TV, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 0);
	picture_event(&p, PEV_RIGHT, -1);
	picture_event(&p, PEV_RIGHT, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_TV); /* both GamePad modes need the GamePad, as on the prompt */
}

static void leaving_a_picture_row_drops_its_unapplied_pick(void)
{
	picture_ui p;
	picture_open(&p, MODE_BOTH, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 1);
	picture_event(&p, PEV_LEFT, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_TV);
	picture_event(&p, PEV_DOWN, -1);
	CHECK(p.pick[PIC_SCREEN] == MODE_BOTH);
	pause_key keys[6];
	picture_legend(&p, INPUT_PRO, keys, 6);
	CHECK(keys[3].dim); /* nothing picked on Shape: nothing to apply */
	picture_event(&p, PEV_RIGHT, -1);
	picture_legend(&p, INPUT_PRO, keys, 6);
	CHECK(!keys[3].dim);
}

static void picture_taps(void)
{
	picture_ui p;
	picture_open(&p, MODE_BOTH, PICTURE_43, FILTER_SMOOTH, LOOK_CLEAN, 1);
	int r[4];
	settings_value_rect(PIC_SCREEN, MODE_TV, r);
	int t = picture_hit(r[0] + 2, r[1] + 2);
	CHECK(t == PIC_SCREEN * 8 + MODE_TV + 1);
	picture_event(&p, PEV_TOUCH_DOWN, t);
	picture_event(&p, PEV_TOUCH_UP, t);
	CHECK(p.row == PIC_SCREEN && p.pick[PIC_SCREEN] == MODE_TV);
	settings_value_rect(PIC_SHAPE, PICTURE_WIDE, r);
	t = picture_hit(r[0] + 2, r[1] + 2);
	picture_event(&p, PEV_TOUCH_DOWN, t);
	picture_event(&p, PEV_TOUCH_UP, t);
	CHECK(p.row == PIC_SHAPE && p.pick[PIC_SHAPE] == PICTURE_WIDE && p.pick[PIC_SCREEN] == MODE_BOTH); /* leaving drops the pick */
	picture_event(&p, PEV_TOUCH_DOWN, t);
	CHECK(picture_event(&p, PEV_TOUCH_UP, t) == PICT_SHAPE);
}

/* ---- Our Controllers ---- */

static void controllers_cards_then_actions(void)
{
	controllers_ui c;
	controllers_open(&c);
	c.ports[0].type = INPUT_GAMEPAD;
	c.ports[1].type = INPUT_CLASSIC;
	c.ports[2].type = -1;
	c.ports[3].type = -1;
	c.rumble[1] = 1;
	CHECK(c.card == 0 && !c.in_actions);
	controllers_event(&c, PEV_RIGHT, -1);
	CHECK(c.card == 1);
	controllers_event(&c, PEV_DOWN, -1);
	CHECK(c.in_actions && c.action == CTL_RUMBLE);
	CHECK(!strcmp(controllers_action_name(&c, CTL_RUMBLE), "Rumble: On"));
	CHECK(controllers_event(&c, PEV_OK, -1) == CACT_RUMBLE);
	controllers_event(&c, PEV_RIGHT, -1);
	CHECK(controllers_event(&c, PEV_OK, -1) == CACT_EDIT);
	controllers_event(&c, PEV_RIGHT, -1);
	CHECK(controllers_event(&c, PEV_OK, -1) == CACT_ORDER);
	controllers_event(&c, PEV_RIGHT, -1);
	CHECK(c.action == CTL_ORDER); /* the last action: no Pair a controller */
	controllers_event(&c, PEV_UP, -1);
	CHECK(!c.in_actions && c.card == 1);
	CHECK(controllers_event(&c, PEV_BACK, -1) == CACT_BACK);
	CHECK(controllers_event(&c, PEV_HOME, -1) == CACT_HOME);
}

static void an_empty_port_has_no_buttons_to_edit(void)
{
	controllers_ui c;
	controllers_open(&c);
	for (int i = 0; i < INPUT_PORTS; i++)
		c.ports[i].type = -1;
	c.card = 2;
	CHECK(!controllers_action_enabled(&c, CTL_EDIT));
	c.in_actions = 1;
	c.action = CTL_EDIT;
	CHECK(controllers_event(&c, PEV_OK, -1) == CACT_NONE && c.refused == 1); /* counted: the button flashes */
	CHECK(!controllers_action_enabled(&c, CTL_RUMBLE)); /* no controller, no rumble to switch (Zach, 2026-10-29) */
	CHECK(controllers_action_enabled(&c, CTL_ORDER));
	/* it says why, and what to do (Zach, 2026-10-23) */
	CHECK(!strcmp(controllers_action_reason(&c, CTL_EDIT), "No controller"));
	CHECK(!strcmp(controllers_action_reason(&c, CTL_ORDER), ""));
	c.ports[2].type = INPUT_PRO;
	CHECK(!strcmp(controllers_action_reason(&c, CTL_EDIT), ""));
}

static void a_on_a_card_goes_to_its_actions(void)
{
	controllers_ui c;
	controllers_open(&c);
	c.ports[0].type = INPUT_PRO;
	CHECK(controllers_event(&c, PEV_OK, -1) == CACT_NONE && c.in_actions && c.action == CTL_RUMBLE);
}

/* ---- Change order ---- */

static void change_order_takes_each_controller_once(void)
{
	order_ui o;
	order_open(&o, 3);
	CHECK(!order_press(&o, 2));
	CHECK(!order_press(&o, 2) && o.count == 1); /* the same controller again: ignored */
	CHECK(!order_press(&o, 0));
	CHECK(order_press(&o, 4) && o.count == 3);
	CHECK(o.ids[0] == 2 && o.ids[1] == 0 && o.ids[2] == 4);
}

/* ---- Edit buttons ---- */

static mapping_ui g_m;
static input_map g_all, g_prof;

/* game: NULL from Our ROMs; profile: the game has one (C-Right on ZR) */
static void open_pro(const char *game, int profile)
{
	input_default_map(INPUT_PRO, &g_all);
	g_prof = g_all;
	g_prof.sources[N64_CRIGHT] = BIT(SRC_ZR);
	mapping_open(&g_m, INPUT_PRO, &g_all, profile ? &g_prof : NULL, game);
}

static int mev(int e) { return mapping_event(&g_m, e, -1, 0); }

/* What setup_wiiu.c does with MACT_WRITE when the card takes it. */
static int written(int act)
{
	if (act != MACT_WRITE)
		return 0;
	mapping_written(&g_m);
	return 1;
}

static void opens_on_the_list_with_the_buttons_in_use(void)
{
	char head[64], sub[96];
	open_pro(NULL, 0);
	CHECK(g_m.screen == MAP_LIST && g_m.row == N64_A && g_m.tab == MAP_TAB_ALL && !g_m.has_game);
	mapping_headline(&g_m, head, sizeof(head), sub, sizeof(sub));
	CHECK(!strcmp(head, "PRO CONTROLLER") && !strcmp(sub, "All games"));
	open_pro("Super Mario 64", 0);
	CHECK(g_m.tab == MAP_TAB_ALL && g_m.has_game);
	mapping_headline(&g_m, head, sizeof(head), sub, sizeof(sub));
	CHECK(!strcmp(sub, "")); /* the tabs name the scope */
	open_pro("Super Mario 64", 1); /* the profile is in use: its tab */
	CHECK(g_m.tab == MAP_TAB_GAME && g_m.edit.sources[N64_CRIGHT] == BIT(SRC_ZR));
	CHECK(mev(PEV_BACK) == MACT_BACK && mev(PEV_HOME) == MACT_HOME); /* nothing unsaved to ask about */
}

static void a_captures_y_adds_x_clears_each_written_at_once(void)
{
	open_pro(NULL, 0);
	CHECK(mev(PEV_OK) == MACT_CAPTURE && g_m.screen == MAP_CAPTURE && !g_m.capture_add);
	CHECK(written(mapping_capture(&g_m, BIT(SRC_ZL))) && g_m.screen == MAP_LIST);
	CHECK(g_m.edit.sources[N64_A] == BIT(SRC_ZL) && g_m.all.sources[N64_A] == BIT(SRC_ZL));
	CHECK(mev(MEV_ADD) == MACT_CAPTURE && g_m.capture_add);
	CHECK(written(mapping_capture(&g_m, BIT(SRC_A))));
	CHECK(g_m.edit.sources[N64_A] == (BIT(SRC_ZL) | BIT(SRC_A)));
	CHECK(written(mev(MEV_CLEAR)) && g_m.edit.sources[N64_A] == 0);
	CHECK(mev(MEV_CLEAR) == MACT_NONE && g_m.refused == 1); /* nothing to clear: the row flashes */
	char label[64], value[96];
	mapping_row_text(&g_m, N64_A, label, sizeof(label), value, sizeof(value));
	CHECK(!strcmp(label, "N64 A") && !strcmp(value, "None"));
}

/* An N64 input can have any number of buttons, and a button may press any
 * number of N64 inputs (Zach, 2026-10-31). Change swaps (Zach, 2026-10-29:
 * the other input gets this row's old buttons) and takes the button from
 * every other input, even when that leaves one with none; Add shares it.
 * Either way the photo blinks the other input. */
static void change_swaps_and_add_shares(void)
{
	char label[64], value[96];
	open_pro(NULL, 0);
	CHECK(g_all.sources[N64_CUP] & BIT(SRC_X));
	const uint32_t cright = g_all.sources[N64_CRIGHT], cup = g_all.sources[N64_CUP];
	g_m.row = N64_CRIGHT;
	mev(PEV_OK);
	const unsigned seq = g_m.other_seq;
	CHECK(written(mapping_capture(&g_m, BIT(SRC_X))));
	CHECK(g_m.edit.sources[N64_CRIGHT] == BIT(SRC_X));
	CHECK(g_m.edit.sources[N64_CUP] == ((cup & ~BIT(SRC_X)) | cright)); /* C-Up got C-Right's old buttons */
	CHECK(g_m.other_row == N64_CUP && g_m.other_seq == seq + 1);
	CHECK(!strcmp(g_m.line, "Swapped C-Right and C-Up")); /* said beside the tabs while it blinks */
	g_m.row = N64_A; /* Add shares B with N64 B, which keeps it */
	mev(MEV_ADD);
	mapping_capture(&g_m, BIT(SRC_B));
	CHECK(g_m.edit.sources[N64_A] == (BIT(SRC_A) | BIT(SRC_B)) && g_m.edit.sources[N64_B] == BIT(SRC_B));
	CHECK(g_m.other_row == N64_B && g_m.other_seq == seq + 2);
	CHECK(!strcmp(g_m.line, "Shared with B"));
	g_m.row = N64_R; /* N64 R has none: changing it to B takes B from N64 A, which keeps A, and from N64 B */
	g_m.edit.sources[N64_R] = 0;
	mev(PEV_OK);
	mapping_capture(&g_m, BIT(SRC_B));
	CHECK(g_m.edit.sources[N64_R] == BIT(SRC_B) && g_m.edit.sources[N64_A] == BIT(SRC_A));
	CHECK(g_m.edit.sources[N64_B] == 0 && g_m.other_seq == seq + 4);
	CHECK(!strcmp(g_m.line, "Unmapped B"));
	mapping_row_text(&g_m, N64_B, label, sizeof(label), value, sizeof(value));
	CHECK(!strcmp(value, "None"));
	g_m.row = N64_B; /* N64 B has none: changing it to ZL takes ZL from N64 Z, which keeps ZR */
	mev(PEV_OK);
	mapping_capture(&g_m, BIT(SRC_ZL));
	CHECK(g_m.edit.sources[N64_B] == BIT(SRC_ZL) && g_m.edit.sources[N64_Z] == BIT(SRC_ZR));
	CHECK(!strcmp(g_m.line, "Moved from Z to B")); /* not "Swapped": Z got nothing */
	g_m.row = N64_A;
	mev(PEV_OK); /* the row's own button again: nothing else changes, nothing blinks */
	mapping_capture(&g_m, BIT(SRC_A));
	CHECK(g_m.edit.sources[N64_A] == BIT(SRC_A) && g_m.other_seq == seq + 5);
}

static void a_write_that_fails_puts_the_buttons_back(void)
{
	open_pro(NULL, 0);
	mev(PEV_OK);
	CHECK(mapping_capture(&g_m, BIT(SRC_ZL)) == MACT_WRITE);
	mapping_fail(&g_m, "Couldn't save your buttons", "OUR-0203");
	CHECK(g_m.screen == MAP_FAILED && g_m.edit.sources[N64_A] == BIT(SRC_A));
	CHECK(mev(PEV_RIGHT) == MACT_NONE && g_m.screen == MAP_FAILED); /* one button: OK */
	CHECK(mev(PEV_OK) == MACT_NONE && g_m.screen == MAP_LIST);
}

static void a_capture_gives_up_after_five_seconds(void)
{
	open_pro(NULL, 0);
	mapping_event(&g_m, PEV_OK, -1, 100);
	CHECK(g_m.screen == MAP_CAPTURE);
	CHECK(mapping_capture(&g_m, 0) == MACT_NONE);
	CHECK(!mapping_tick(&g_m, 104.9));
	CHECK(mapping_tick(&g_m, 105.0) && g_m.screen == MAP_LIST && g_m.edit.sources[N64_A] == BIT(SRC_A));
	CHECK(!strcmp(g_m.line, "Nothing pressed: unchanged"));
	mapping_event(&g_m, PEV_OK, -1, 200);
	CHECK(mapping_event(&g_m, PEV_HOME, -1, 200) == MACT_NONE && g_m.screen == MAP_LIST);
}

/* L or R switches tabs; the game's tab shows All games until its first change makes the profile. */
static void tabs_and_the_first_change_makes_the_profile(void)
{
	char head[64], sub[96];
	open_pro(NULL, 0);
	CHECK(mev(MEV_TAB) == MACT_NONE && g_m.tab == MAP_TAB_ALL && g_m.refused == 1); /* no game: no tab */
	open_pro("Super Mario 64", 0);
	mev(MEV_TAB);
	CHECK(g_m.tab == MAP_TAB_GAME && input_map_equal(&g_m.edit, &g_all));
	mapping_headline(&g_m, head, sizeof(head), sub, sizeof(sub));
	CHECK(!sub[0]); /* the tabs name the scope (Zach, 2026-10-29: "Uses All games" made no sense) */
	CHECK(!mapping_can_reset(&g_m)); /* no profile to remove */
	mev(PEV_OK);
	CHECK(written(mapping_capture(&g_m, BIT(SRC_ZL))) && g_m.has_profile && g_m.profile.sources[N64_A] == BIT(SRC_ZL));
	mapping_headline(&g_m, head, sizeof(head), sub, sizeof(sub));
	CHECK(!sub[0]); /* the last action is not shown (Zach, 2026-10-29) */
	mev(MEV_TAB);
	CHECK(g_m.tab == MAP_TAB_ALL && g_m.edit.sources[N64_A] == BIT(SRC_A)); /* All games kept its own */
	mev(MEV_TAB);
	CHECK(g_m.tab == MAP_TAB_GAME && g_m.edit.sources[N64_A] == BIT(SRC_ZL));
}

static void reset_then_undo_on_all_games(void)
{
	pause_key k[8];
	open_pro(NULL, 0);
	CHECK(!mapping_can_reset(&g_m) && mev(MEV_RESET) == MACT_NONE && g_m.refused == 1); /* already Our64's */
	mev(PEV_OK);
	written(mapping_capture(&g_m, BIT(SRC_ZL)));
	CHECK(mapping_can_reset(&g_m) && written(mev(MEV_RESET)));
	input_map def;
	input_default_map(INPUT_PRO, &def);
	CHECK(input_map_equal(&g_m.edit, &def) && g_m.can_undo);
	int n = mapping_legend(&g_m, INPUT_PRO, k, 8);
	CHECK(k[n - 1].glyph == GLYPH_MINUS && !strcmp(k[n - 1].label, "Undo reset") && !k[n - 1].dim);
	CHECK(written(mev(MEV_RESET)) && g_m.edit.sources[N64_A] == BIT(SRC_ZL) && !g_m.can_undo);
	written(mev(MEV_RESET));
	g_m.row = N64_B; /* any other change ends the undo */
	written(mev(MEV_CLEAR));
	CHECK(!g_m.can_undo && mapping_can_reset(&g_m) && !strcmp(mapping_legend(&g_m, INPUT_PRO, k, 8) ? k[4].label : "", "Reset"));
}

static void reset_on_the_game_tab_removes_its_profile(void)
{
	open_pro("Super Mario 64", 1);
	CHECK(mapping_can_reset(&g_m) && mev(MEV_RESET) == MACT_REMOVE_PROFILE);
	mapping_profile_removed(&g_m);
	CHECK(!g_m.has_profile && input_map_equal(&g_m.edit, &g_all) && g_m.can_undo);
	CHECK(written(mev(MEV_RESET)) && g_m.has_profile && g_m.edit.sources[N64_CRIGHT] == BIT(SRC_ZR));
	/* a removal that fails changes nothing */
	CHECK(mev(MEV_RESET) == MACT_REMOVE_PROFILE);
	mapping_fail(&g_m, "Couldn't remove the profile", "OUR-0203");
	CHECK(g_m.has_profile && g_m.edit.sources[N64_CRIGHT] == BIT(SRC_ZR) && g_m.screen == MAP_FAILED);
}

static void stick_values_step_and_stop(void)
{
	char label[64], value[64];
	open_pro(NULL, 0);
	g_m.row = MAP_DEAD_ZONE;
	float dz = g_m.edit.dead_zone;
	CHECK(written(mev(PEV_RIGHT)) && g_m.edit.dead_zone > dz);
	for (int i = 0; i < 40; i++) written(mev(PEV_RIGHT));
	CHECK(g_m.edit.dead_zone <= 0.401f && g_m.edit.dead_zone >= 0.399f);
	CHECK(mev(PEV_RIGHT) == MACT_NONE); /* at the end: nothing to write */
	for (int i = 0; i < 40; i++) mev(PEV_LEFT);
	CHECK(g_m.edit.dead_zone == 0);
	mapping_row_text(&g_m, MAP_DEAD_ZONE, label, sizeof(label), value, sizeof(value));
	CHECK(!strcmp(label, "Dead zone") && !strcmp(value, "0%"));
	CHECK(mev(PEV_OK) == MACT_NONE && mev(MEV_ADD) == MACT_NONE && mev(MEV_CLEAR) == MACT_NONE);
	g_m.row = MAP_RANGE;
	for (int i = 0; i < 40; i++) mev(PEV_RIGHT);
	CHECK(g_m.edit.range == 100);
	for (int i = 0; i < 40; i++) mev(PEV_LEFT);
	CHECK(g_m.edit.range == 60);
}

static void the_list_scrolls_to_keep_the_selection_shown(void)
{
	open_pro(NULL, 0);
	for (int i = 0; i < 10; i++) mev(PEV_DOWN);
	CHECK(g_m.row == 10 && g_m.row >= g_m.top && g_m.row < g_m.top + MAP_SHOWN);
	for (int i = 10; i < MAP_RANGE; i++) mev(PEV_DOWN);
	CHECK(g_m.row == MAP_RANGE && g_m.top == MAP_ROWS - MAP_SHOWN);
	/* the list wraps both ways (Zach, 2026-10-19) */
	mev(PEV_DOWN);
	CHECK(g_m.row == N64_A && g_m.top == 0);
	mev(PEV_UP);
	CHECK(g_m.row == MAP_RANGE && g_m.top == MAP_ROWS - MAP_SHOWN);
}

static int has_key(const pause_key *k, int n, int glyph, const char *label)
{
	for (int i = 0; i < n; i++)
		if (k[i].glyph == glyph && !strcmp(k[i].label, label))
			return 1;
	return 0;
}

static void the_legend_follows_the_row_and_the_controller(void)
{
	pause_key k[8];
	open_pro(NULL, 0);
	int n = mapping_legend(&g_m, INPUT_PRO, k, 8);
	CHECK(n == 5 && has_key(k, n, GLYPH_B, "Back") && has_key(k, n, GLYPH_A, "Change") && has_key(k, n, GLYPH_Y, "Add")
	      && has_key(k, n, GLYPH_X, "Unmap") && has_key(k, n, GLYPH_MINUS, "Reset"));
	CHECK(k[4].dim); /* already Our64's buttons */
	n = mapping_legend(&g_m, INPUT_WIIMOTE, k, 8); /* no X or Y: plus adds, minus clears; no reset */
	CHECK(n == 4 && has_key(k, n, GLYPH_1, "Back") && has_key(k, n, GLYPH_2, "Change") && has_key(k, n, GLYPH_PLUS, "Add")
	      && has_key(k, n, GLYPH_MINUS, "Unmap"));
	g_m.row = MAP_DEAD_ZONE;
	n = mapping_legend(&g_m, INPUT_PRO, k, 8);
	CHECK(n == 3 && has_key(k, n, GLYPH_DPAD, "Change") && !has_key(k, n, GLYPH_A, "Change"));
	g_m.row = N64_A;
	mapping_event(&g_m, PEV_OK, -1, 0);
	n = mapping_legend(&g_m, INPUT_PRO, k, 8);
	CHECK(n == 1 && k[0].glyph == GLYPH_HOME && !strcmp(k[0].label, "Cancel"));
}

static int has_event(const int *ev, int n, int e)
{
	for (int i = 0; i < n; i++)
		if (ev[i] == e)
			return 1;
	return 0;
}

static void each_controller_has_its_buttons_for_the_actions(void)
{
	int ev[8], n;
	n = mapping_events(INPUT_PRO, BIT(SRC_Y) | BIT(SRC_X) | BIT(SRC_MINUS) | BIT(SRC_L), ev, 8);
	CHECK(n == 4 && has_event(ev, n, MEV_ADD) && has_event(ev, n, MEV_CLEAR) && has_event(ev, n, MEV_RESET)
	      && has_event(ev, n, MEV_TAB) && !has_event(ev, n, PEV_DELETE));
	n = mapping_events(INPUT_GAMEPAD, BIT(SRC_R) | BIT(SRC_A), ev, 8);
	CHECK(n == 2 && has_event(ev, n, MEV_TAB) && has_event(ev, n, PEV_OK));
	n = mapping_events(INPUT_WIIMOTE, BIT(SRC_MINUS) | BIT(SRC_PLUS) | BIT(SRC_2), ev, 8);
	CHECK(n == 3 && has_event(ev, n, MEV_CLEAR) && has_event(ev, n, MEV_ADD) && has_event(ev, n, PEV_OK)
	      && !has_event(ev, n, PEV_DELETE) && !has_event(ev, n, MEV_RESET));
}

static void taps_select_a_row_then_change_it_and_switch_tabs_at_once(void)
{
	open_pro("Super Mario 64", 0);
	int r[4];
	mapping_tab_rect(MAP_TAB_GAME, r);
	int t = mapping_hit(&g_m, r[0] + 5, r[1] + 5);
	CHECK(t == MAP_TAB_TARGET + MAP_TAB_GAME);
	mapping_event(&g_m, PEV_TOUCH_DOWN, t, 0);
	CHECK(mapping_event(&g_m, PEV_TOUCH_UP, t, 0) == MACT_NONE && g_m.tab == MAP_TAB_GAME);
	mapping_row_rect(1, r);
	t = mapping_hit(&g_m, r[0] + 5, r[1] + 5);
	CHECK(t == N64_B);
	mapping_event(&g_m, PEV_TOUCH_DOWN, t, 0);
	CHECK(mapping_event(&g_m, PEV_TOUCH_UP, t, 0) == MACT_NONE && g_m.row == N64_B);
	mapping_event(&g_m, PEV_TOUCH_DOWN, t, 0);
	CHECK(mapping_event(&g_m, PEV_TOUCH_UP, t, 0) == MACT_CAPTURE);
	open_pro(NULL, 0); /* no game: no tabs to tap */
	mapping_tab_rect(MAP_TAB_GAME, r);
	CHECK(mapping_hit(&g_m, r[0] + 5, r[1] + 5) == -1);
}

/* ---- Settings > Games (2026-11-04) ---- */

static void games_rows_and_fetch_progress(void)
{
	games_ui g;
	games_open(&g, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	CHECK(!strcmp(games_row_name(GAMES_FOLDER), "ROM folder") && !strcmp(games_row_name(GAMES_COVERS), "Fetch covers"));
	char v[64];
	games_value(&g, GAMES_FOLDER, v, sizeof(v));
	CHECK(!strcmp(v, "SD card: /wiiu/our64/roms"));
	games_value(&g, GAMES_COVERS, v, sizeof(v));
	CHECK(!strcmp(v, "")); /* no count: opening the page reads no game */
	CHECK(games_event(&g, PEV_OK, -1) == GACT_FOLDER);
	games_event(&g, PEV_DOWN, -1);
	CHECK(games_event(&g, PEV_OK, -1) == GACT_FETCH);
	games_reading(&g, 0, 200); /* the headers first, on the fetch screen */
	games_fetch_step(&g, v, sizeof(v));
	CHECK(!strcmp(v, "Reading your games: 0 of 200") && games_fetch_progress(&g) == 0.0f);
	games_reading(&g, 80, 200);
	games_fetch_step(&g, v, sizeof(v));
	CHECK(!strcmp(v, "Reading your games: 80 of 200") && games_fetch_progress(&g) == 0.4f);
	CHECK(games_event(&g, PEV_BACK, -1) == GACT_STOP);      /* B stops the reading too */
	games_fetching(&g, 3, 12);
	games_fetch_step(&g, v, sizeof(v)); /* the fetch screen's line under the bar */
	CHECK(!strcmp(v, "3 of 12") && games_fetch_progress(&g) == 0.25f);
	CHECK(games_event(&g, PEV_OK, -1) == GACT_NONE);       /* already running */
	CHECK(games_event(&g, PEV_UP, -1) == GACT_NONE && g.row == GAMES_COVERS); /* stays while it runs */
	CHECK(games_event(&g, PEV_BACK, -1) == GACT_STOP);      /* B stops, does not leave */
	games_fetched(&g);
	CHECK(!g.running && !g.reading);
	CHECK(games_event(&g, PEV_OK, -1) == GACT_FETCH);       /* again, whenever asked */
	CHECK(games_event(&g, PEV_BACK, -1) == GACT_BACK);
}

static void fetch_covers_is_always_available(void)
{
	games_ui g;
	games_open(&g, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	g.row = GAMES_COVERS;
	CHECK(games_event(&g, PEV_OK, -1) == GACT_FETCH);
	pause_key k[6];
	int n = games_legend(&g, INPUT_PRO, k, 6);
	CHECK(n == 4 && !strcmp(k[3].label, "OK") && !k[3].dim);
	g.row = GAMES_FOLDER;
	n = games_legend(&g, INPUT_PRO, k, 6);
	CHECK(n == 4 && !k[3].dim);
}

static void games_legend_and_lines_while_fetching(void)
{
	games_ui g;
	games_open(&g, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	CHECK(!strcmp(games_row_about(&g), "Where we find your games"));
	g.row = GAMES_COVERS;
	CHECK(!strcmp(games_row_about(&g), "Fetches box art from the internet. Requires Wi-Fi"));
	games_fetching(&g, 0, 5);
	CHECK(games_fetch_progress(&g) == 0.0f);
	games_fetching(&g, 0, 0); /* no games: an empty bar, never a division by zero */
	CHECK(games_fetch_progress(&g) == 0.0f);
	games_fetching(&g, 0, 5);
	pause_key k[6];
	int n = games_legend(&g, INPUT_PRO, k, 6);
	CHECK(n == 2 && k[0].glyph == GLYPH_HOME && k[1].glyph == GLYPH_B && !strcmp(k[1].label, "Stop"));
	CHECK(games_event(&g, PEV_HOME, -1) == GACT_HOME); /* the loop stops the fetch first */
	games_reading(&g, 10, 50); /* while the headers are read: the same keys */
	n = games_legend(&g, INPUT_PRO, k, 6);
	CHECK(n == 2 && !strcmp(k[1].label, "Stop"));
}

static int tap_games(games_ui *g, int target)
{
	games_event(g, PEV_TOUCH_DOWN, target);
	return games_event(g, PEV_TOUCH_UP, target);
}

static void games_taps_select_then_act(void)
{
	games_ui g;
	games_open(&g, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	int r[4];
	settings_row_rect(GAMES_COVERS, r);
	const int t = games_hit(r[0] + 5, r[1] + 5);
	CHECK(t == GAMES_COVERS && games_hit(5, 5) == -1);
	CHECK(tap_games(&g, t) == GACT_NONE && g.row == GAMES_COVERS); /* a tap selects */
	CHECK(tap_games(&g, t) == GACT_FETCH);                          /* a tap on the selected acts */
	games_fetching(&g, 1, 3);
	settings_row_rect(GAMES_FOLDER, r);
	CHECK(tap_games(&g, games_hit(r[0] + 5, r[1] + 5)) == GACT_NONE && g.row == GAMES_COVERS); /* not while it runs */
}

/* Auto-Resume and Start on, below Fetch covers (2026-11-06). */
static void auto_resume_and_start_on_rows(void)
{
	games_ui g;
	games_open(&g, "SD card: /wiiu/our64/roms", 1, START_RECENT);
	CHECK(!strcmp(games_row_name(GAMES_RESUME), "Auto-Resume") && !strcmp(games_row_name(GAMES_START), "Start-up ROM Category"));
	CHECK(GAMES_RESUME == GAMES_COVERS + 1 && GAMES_START == GAMES_RESUME + 1 && GAMES_ROWS == 4);
	char v[64];
	games_value(&g, GAMES_RESUME, v, sizeof(v));
	CHECK(!strcmp(v, "On"));
	games_value(&g, GAMES_START, v, sizeof(v));
	CHECK(!strcmp(v, "Recent"));
	for (int i = 0; i < 4; i++)
		games_event(&g, PEV_DOWN, -1);
	CHECK(g.row == GAMES_START); /* stops at the last row */
	CHECK(!strcmp(games_row_about(&g), "The category shown when Our64 starts"));
	CHECK(games_event(&g, PEV_OK, -1) == GACT_START);
	games_event(&g, PEV_UP, -1);
	CHECK(g.row == GAMES_RESUME && !strcmp(games_row_about(&g), "Saves where you stop and picks up there next time"));
	CHECK(games_event(&g, PEV_OK, -1) == GACT_RESUME_OFF); /* On: the caller asks before turning it off */
	g.auto_resume = 0;                                       /* confirmed */
	games_value(&g, GAMES_RESUME, v, sizeof(v));
	CHECK(!strcmp(v, "Off"));
	CHECK(games_event(&g, PEV_OK, -1) == GACT_RESUME_ON);  /* Off: on at once, nothing asked */
	pause_key k[6];
	CHECK(games_legend(&g, INPUT_PRO, k, 6) == 4 && !k[3].dim);
	games_open(&g, "SD card: /wiiu/our64/roms", 0, START_FAVORITES);
	games_value(&g, GAMES_RESUME, v, sizeof(v));
	CHECK(!strcmp(v, "Off"));
	games_value(&g, GAMES_START, v, sizeof(v));
	CHECK(!strcmp(v, "Favorites"));
	int r[4];
	settings_row_rect(GAMES_START, r);
	const int t = games_hit(r[0] + 5, r[1] + 5);
	CHECK(t == GAMES_START && tap_games(&g, t) == GACT_NONE && tap_games(&g, t) == GACT_START);
}

static void start_on_picks_all_recent_or_favorites(void)
{
	start_ui s;
	start_open(&s, START_RECENT);
	CHECK(s.row == START_RECENT && s.in_use == START_RECENT); /* opens on the one in use */
	CHECK(START_ALL == 0 && START_RECENT == 1 && START_FAVORITES == 2);
	CHECK(start_event(&s, PEV_LEFT, -1) == STACT_NONE && s.row == START_RECENT); /* rows, no arrows */
	start_event(&s, PEV_UP, -1);
	start_event(&s, PEV_UP, -1);
	CHECK(s.row == START_ALL);
	start_event(&s, PEV_DOWN, -1);
	start_event(&s, PEV_DOWN, -1);
	start_event(&s, PEV_DOWN, -1);
	CHECK(s.row == START_FAVORITES);
	CHECK(start_event(&s, PEV_OK, -1) == STACT_PICK && s.row == START_FAVORITES);
	CHECK(start_event(&s, PEV_BACK, -1) == STACT_BACK && start_event(&s, PEV_HOME, -1) == STACT_HOME);
	int r[4];
	settings_row_rect(START_ALL, r);
	const int t = start_hit(r[0] + 5, r[1] + 5);
	CHECK(t == START_ALL && start_hit(5, 5) == -1);
	start_event(&s, PEV_TOUCH_DOWN, t);
	CHECK(start_event(&s, PEV_TOUCH_UP, t) == STACT_NONE && s.row == START_ALL); /* a tap selects */
	start_event(&s, PEV_TOUCH_DOWN, t);
	CHECK(start_event(&s, PEV_TOUCH_UP, t) == STACT_PICK);                     /* a tap on the selected picks */
	pause_key k[6];
	int n = start_legend(&s, INPUT_PRO, k, 6);
	CHECK(n == 4 && k[0].glyph == GLYPH_HOME && !strcmp(k[3].label, "OK"));
	start_open(&s, 7); /* a bad value: Recent */
	CHECK(s.row == START_RECENT);
}

/* ---- Settings > Games > ROM folder (2026-11-04) ---- */

static void browser_opens_drives_then_folders_and_uses_one(void)
{
	static browse_ui b;
	browse_open(&b);
	browse_add(&b, "SD card", "sd:/", 0);
	browse_add(&b, "USB GAMES", "usb1p1:/", 0);
	browse_add(&b, "USB drive 2: no FAT32 or exFAT partition", "", 1); /* shown, greyed */
	CHECK(b.depth == 0 && b.count == 3 && browse_rows(&b) == 3);    /* no Use row on the drive list */
	browse_event(&b, PEV_DOWN, -1);
	browse_event(&b, PEV_DOWN, -1);
	CHECK(b.row == 2 && browse_event(&b, PEV_OK, -1) == BACT_NONE);  /* greyed */
	browse_event(&b, PEV_UP, -1);
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_OPEN && !strcmp(b.path, "usb1p1:/"));
	/* the caller lists usb1p1:/ */
	browse_entered(&b, 4);                 /* 4 ROMs right in this folder */
	browse_add(&b, "Games", "usb1p1:/Games", 0);
	CHECK(b.depth == 1 && b.row == 0 && browse_rows(&b) == 2); /* Use this folder, then Games */
	char v[32];
	browse_row_value(&b, 0, v, sizeof(v));
	CHECK(!strcmp(browse_row_name(&b, 0), "Use this folder") && !strcmp(v, "4 games"));
	browse_event(&b, PEV_DOWN, -1);
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_OPEN && !strcmp(b.path, "usb1p1:/Games"));
	browse_entered(&b, 0);
	CHECK(b.depth == 2);
	CHECK(!strcmp(browse_title(&b), "USB GAMES: /Games")); /* a full path, as Settings > Games shows it */
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_USE && !strcmp(b.path, "usb1p1:/Games"));
	CHECK(browse_event(&b, PEV_BACK, -1) == BACT_UP && !strcmp(b.path, "usb1p1:/") && b.depth == 1);
	browse_entered(&b, 4);
	browse_add(&b, "Games", "usb1p1:/Games", 0);
	CHECK(b.row == 1); /* back on the folder it came out of */
	CHECK(browse_event(&b, PEV_BACK, -1) == BACT_UP && b.depth == 0 && !strcmp(b.path, ""));
	CHECK(browse_event(&b, PEV_BACK, -1) == BACT_BACK);
}

static void browser_scrolls_long_lists(void)
{
	static browse_ui b;
	browse_open(&b);
	b.depth = 1;
	snprintf(b.path, sizeof(b.path), "sd:/");
	browse_entered(&b, 0);
	char name[16], path[32];
	for (int i = 0; i < 30; i++) {
		snprintf(name, sizeof(name), "Folder %02d", i);
		snprintf(path, sizeof(path), "sd:/Folder %02d", i);
		browse_add(&b, name, path, 0);
	}
	for (int i = 0; i < 20; i++)
		browse_event(&b, PEV_DOWN, -1);
	CHECK(b.row == 20 && b.top <= 20 && b.top + BROWSE_SHOWN > 20);
	int r[4];
	browse_row_rect(0, r);
	CHECK(browse_hit(&b, r[0] + 3, r[1] + 3) == b.top); /* the first row on screen */
}

static void browser_titles_values_and_legend(void)
{
	static browse_ui b;
	browse_open(&b);
	CHECK(!strcmp(browse_title(&b), "Choose a drive"));
	browse_add(&b, "SD card", "sd:/", 0);
	browse_add(&b, "USB drive 2: no FAT32 or exFAT partition", "", 1);
	pause_key k[6];
	int n = browse_legend(&b, INPUT_PRO, k, 6);
	CHECK(n == 4 && !strcmp(k[1].label, "Back") && !strcmp(k[3].label, "OK") && !k[3].dim);
	browse_event(&b, PEV_DOWN, -1);
	n = browse_legend(&b, INPUT_PRO, k, 6);
	CHECK(k[3].dim); /* a greyed row */
	browse_event(&b, PEV_UP, -1);
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_OPEN);
	browse_entered(&b, 1);
	CHECK(!strcmp(browse_title(&b), "SD card: /"));
	n = browse_legend(&b, INPUT_PRO, k, 6);
	CHECK(!strcmp(k[1].label, "Up"));
	char v[32];
	browse_row_value(&b, 0, v, sizeof(v));
	CHECK(!strcmp(v, "1 game"));
	browse_add(&b, "wiiu", "sd:/wiiu", 0);
	browse_row_value(&b, 1, v, sizeof(v));
	CHECK(!strcmp(v, "") && !strcmp(browse_row_name(&b, 1), "wiiu"));
	browse_event(&b, PEV_DOWN, -1);
	browse_event(&b, PEV_OK, -1);
	browse_entered(&b, 0);
	browse_row_value(&b, 0, v, sizeof(v));
	CHECK(!strcmp(v, "No games"));
	browse_add(&b, "our64", "sd:/wiiu/our64", 0);
	browse_event(&b, PEV_DOWN, -1);
	browse_event(&b, PEV_OK, -1);
	browse_entered(&b, 0);
	browse_add(&b, "roms", "sd:/wiiu/our64/roms", 0);
	browse_event(&b, PEV_DOWN, -1);
	browse_event(&b, PEV_OK, -1);
	browse_entered(&b, 12);
	CHECK(!strcmp(browse_title(&b), "SD card: /wiiu/our64/roms"));
}

/* A folder that couldn't be opened: Use this folder is greyed, with no count. */
static void browser_greys_use_for_a_folder_it_couldnt_open(void)
{
	static browse_ui b;
	browse_open(&b);
	browse_add(&b, "SD card", "sd:/", 0);
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_OPEN);
	browse_entered(&b, -1);
	char v[32];
	browse_row_value(&b, 0, v, sizeof(v));
	CHECK(browse_row_off(&b, 0) && !strcmp(v, "") && !strcmp(browse_row_name(&b, 0), "Use this folder"));
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_NONE);
	pause_key k[6];
	browse_legend(&b, INPUT_PRO, k, 6);
	CHECK(k[3].dim);
	CHECK(browse_event(&b, PEV_BACK, -1) == BACT_UP); /* B still goes up */
	browse_entered(&b, 0);                           /* a folder it could open, even empty: usable */
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_NONE); /* the drive list has no Use row */
	browse_add(&b, "SD card", "sd:/", 0);
	CHECK(browse_event(&b, PEV_OK, -1) == BACT_OPEN);
	browse_entered(&b, 0);
	CHECK(!browse_row_off(&b, 0) && browse_event(&b, PEV_OK, -1) == BACT_USE);
}

static void browser_taps_select_then_open(void)
{
	static browse_ui b;
	browse_open(&b);
	browse_add(&b, "SD card", "sd:/", 0);
	browse_add(&b, "USB GAMES", "usb1p1:/", 0);
	int r[4];
	browse_row_rect(1, r);
	const int t = browse_hit(&b, r[0] + 3, r[1] + 3);
	CHECK(t == 1);
	browse_row_rect(2, r);
	CHECK(browse_hit(&b, r[0] + 3, r[1] + 3) == -1); /* no row there */
	browse_event(&b, PEV_TOUCH_DOWN, t);
	CHECK(browse_event(&b, PEV_TOUCH_UP, t) == BACT_NONE && b.row == 1);
	browse_event(&b, PEV_TOUCH_DOWN, t);
	CHECK(browse_event(&b, PEV_TOUCH_UP, t) == BACT_OPEN && !strcmp(b.path, "usb1p1:/"));
}

#define RUN(fn) do { g_run++; int before = g_failed; fn(); if (g_failed == before) printf("  ok   %s\n", #fn); } while (0)

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	RUN(settings_rows_are_four_pages);
	RUN(settings_leads_to_controllers_and_back);
	RUN(settings_taps);
	RUN(picture_applies_what_left_right_picked);
	RUN(picture_lines_are_the_same_whatever_is_picked);
	RUN(gamepad_only_is_skipped_without_a_gamepad_on_picture);
	RUN(leaving_a_picture_row_drops_its_unapplied_pick);
	RUN(picture_taps);
	RUN(controllers_cards_then_actions);
	RUN(an_empty_port_has_no_buttons_to_edit);
	RUN(a_on_a_card_goes_to_its_actions);
	RUN(change_order_takes_each_controller_once);
	RUN(opens_on_the_list_with_the_buttons_in_use);
	RUN(theme_page_applies_colors_and_layout);
	RUN(a_captures_y_adds_x_clears_each_written_at_once);
	RUN(change_swaps_and_add_shares);
	RUN(a_write_that_fails_puts_the_buttons_back);
	RUN(a_capture_gives_up_after_five_seconds);
	RUN(tabs_and_the_first_change_makes_the_profile);
	RUN(reset_then_undo_on_all_games);
	RUN(reset_on_the_game_tab_removes_its_profile);
	RUN(stick_values_step_and_stop);
	RUN(the_list_scrolls_to_keep_the_selection_shown);
	RUN(the_legend_follows_the_row_and_the_controller);
	RUN(each_controller_has_its_buttons_for_the_actions);
	RUN(taps_select_a_row_then_change_it_and_switch_tabs_at_once);
	RUN(games_rows_and_fetch_progress);
	RUN(fetch_covers_is_always_available);
	RUN(games_legend_and_lines_while_fetching);
	RUN(games_taps_select_then_act);
	RUN(auto_resume_and_start_on_rows);
	RUN(start_on_picks_all_recent_or_favorites);
	RUN(browser_opens_drives_then_folders_and_uses_one);
	RUN(browser_scrolls_long_lists);
	RUN(browser_titles_values_and_legend);
	RUN(browser_greys_use_for_a_folder_it_couldnt_open);
	RUN(browser_taps_select_then_open);
	printf("%d of %d failed\n", g_failed, g_run);
	return g_failed != 0;
}
