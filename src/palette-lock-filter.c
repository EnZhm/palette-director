/*
Palette Director
Copyright (C) 2026 EnZhm

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>/
*/

#include <obs-module.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/threading.h>

#include "palette-lock-filter.h"
#include "palette-lock-host.h"
#include "palette/palette.h"

#define S_PALETTE "palette.%d.name"
#define S_MODE "palette.%d.mode"
#define S_NCOLOURS "palette.%d.n_colours"
#define S_COLOUR "palette.%d.colour.%d"

#define PD_N_NAMED_HOTKEYS 7

struct palette_lock_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_palA;
	gs_eparam_t *p_palB;
	gs_eparam_t *p_nA;
	gs_eparam_t *p_modeA;
	gs_eparam_t *p_nB;
	gs_eparam_t *p_modeB;
	gs_eparam_t *p_pfade;
	gs_eparam_t *p_amt;
	gs_eparam_t *p_contrast;
	gs_eparam_t *p_preserve;

	struct pl_set set;
	unsigned long long gen;   /* palette-layout generation pushed to the host */
	int live_setting;         /* last "live_palette" value seen in settings */
	volatile long force_live; /* live palette to force on the next update (-1 = none) */
	int edit_palette;

	struct pd_host *host;
	obs_hotkey_id hk_named[PD_N_NAMED_HOTKEYS];
	obs_hotkey_id hk_palette[PL_MAX_PALETTES];
};

/* ------------------------------------------------------------------------- */
/* settings <-> set (obs_data serialisation)                                  */

/* def = true writes obs_data defaults: get_defaults runs on the live settings object
 * (libobs obs_source_create_internal), so plain setters there would clobber user values. */
#define SET_STRING(s, k, v) (def ? obs_data_set_default_string(s, k, v) : obs_data_set_string(s, k, v))
#define SET_INT(s, k, v) (def ? obs_data_set_default_int(s, k, v) : obs_data_set_int(s, k, v))
#define SET_DOUBLE(s, k, v) (def ? obs_data_set_default_double(s, k, v) : obs_data_set_double(s, k, v))
#define SET_BOOL(s, k, v) (def ? obs_data_set_default_bool(s, k, v) : obs_data_set_bool(s, k, v))

static void write_set(obs_data_t *s, const struct pl_set *set, bool def)
{
	SET_STRING(s, "set_name", set->name);
	SET_STRING(s, "synesthesia_playlist", set->synesthesia_playlist);
	SET_INT(s, "n_palettes", set->n_palettes);
	SET_DOUBLE(s, "strength", set->strength);
	SET_DOUBLE(s, "contrast", set->contrast);
	SET_DOUBLE(s, "preserve", set->preserve);
	SET_DOUBLE(s, "crossfade_beats", set->crossfade_beats);

	SET_INT(s, "trigger", set->trigger);
	SET_INT(s, "every_phrases", set->every_phrases);
	SET_DOUBLE(s, "change_chance", set->change_chance);
	SET_INT(s, "order", set->order);
	SET_STRING(s, "breakdown_palette", set->breakdown);

	SET_BOOL(s, "dir_enabled", set->dir_enabled);
	SET_INT(s, "phrase_beats", set->phrase_beats);
	SET_DOUBLE(s, "lock_fade_beats", set->lock_fade_beats);
	SET_DOUBLE(s, "fallback_bpm", set->fallback_bpm);
	SET_BOOL(s, "random_off", set->random_off);
	SET_DOUBLE(s, "random_chance", set->random_chance);
	SET_INT(s, "bars_min", set->bars_min);
	SET_INT(s, "bars_max", set->bars_max);
	SET_INT(s, "cooldown_phrases", set->cooldown_phrases);
	SET_BOOL(s, "off_on_breakdown", set->off_on_breakdown);
	SET_BOOL(s, "off_on_drop", set->off_on_drop);
	SET_INT(s, "drop_bars", set->drop_bars);
	SET_DOUBLE(s, "breathing", set->breathing);
	SET_BOOL(s, "rotation", set->rotation);
	SET_INT(s, "seed", (long long)set->seed);

	for (int i = 0; i < set->n_palettes; i++) {
		char key[64];
		const struct pl_palette *p = &set->palettes[i];
		snprintf(key, sizeof(key), S_PALETTE, i);
		SET_STRING(s, key, p->name);
		snprintf(key, sizeof(key), S_MODE, i);
		SET_INT(s, key, p->mode);
		snprintf(key, sizeof(key), S_NCOLOURS, i);
		SET_INT(s, key, p->n_colours);
		for (int j = 0; j < p->n_colours; j++) {
			snprintf(key, sizeof(key), S_COLOUR, i, j);
			SET_INT(s, key, (long long)pl_colour_to_obs(p->colours[j]));
		}
	}
}

#undef SET_STRING
#undef SET_INT
#undef SET_DOUBLE
#undef SET_BOOL

static void settings_from_set(obs_data_t *s, const struct pl_set *set)
{
	write_set(s, set, false);
}

static int clampi(long long v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : (int)v);
}

static float clampf(double v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : (float)v);
}

static void set_from_settings(obs_data_t *s, struct pl_set *set)
{
	pl_set_init(set);

	const char *name = obs_data_get_string(s, "set_name");
	snprintf(set->name, sizeof(set->name), "%s", name && name[0] ? name : "Default Set");

	const char *pl = obs_data_get_string(s, "synesthesia_playlist");
	snprintf(set->synesthesia_playlist, sizeof(set->synesthesia_playlist), "%s", pl ? pl : "");

	set->n_palettes = clampi(obs_data_get_int(s, "n_palettes"), 1, PL_MAX_PALETTES);

	set->strength = clampf(obs_data_get_double(s, "strength"), 0.0f, 1.0f);
	set->contrast = clampf(obs_data_get_double(s, "contrast"), 0.5f, 2.0f);
	set->preserve = clampf(obs_data_get_double(s, "preserve"), 0.0f, 1.0f);
	set->crossfade_beats = clampf(obs_data_get_double(s, "crossfade_beats"), 0.0f, 16.0f);

	set->trigger = clampi(obs_data_get_int(s, "trigger"), PL_TRIGGER_DROP, PL_TRIGGER_MANUAL);
	set->every_phrases = clampi(obs_data_get_int(s, "every_phrases"), 1, 16);
	set->change_chance = clampf(obs_data_get_double(s, "change_chance"), 0.0f, 1.0f);
	set->order = clampi(obs_data_get_int(s, "order"), PL_ORDER_SEQUENCE, PL_ORDER_SHUFFLE);
	const char *bd = obs_data_get_string(s, "breakdown_palette");
	snprintf(set->breakdown, sizeof(set->breakdown), "%s", bd ? bd : "");

	set->dir_enabled = obs_data_get_bool(s, "dir_enabled");
	set->phrase_beats = obs_data_get_int(s, "phrase_beats") == 16 ? 16 : 32;
	set->lock_fade_beats = clampf(obs_data_get_double(s, "lock_fade_beats"), 0.25f, 8.0f);
	set->fallback_bpm = clampf(obs_data_get_double(s, "fallback_bpm"), 60.0f, 200.0f);
	set->random_off = obs_data_get_bool(s, "random_off");
	set->random_chance = clampf(obs_data_get_double(s, "random_chance"), 0.0f, 1.0f);
	set->bars_min = clampi(obs_data_get_int(s, "bars_min"), 1, 8);
	set->bars_max = clampi(obs_data_get_int(s, "bars_max"), 1, 16);
	set->cooldown_phrases = clampi(obs_data_get_int(s, "cooldown_phrases"), 0, 8);
	set->off_on_breakdown = obs_data_get_bool(s, "off_on_breakdown");
	set->off_on_drop = obs_data_get_bool(s, "off_on_drop");
	set->drop_bars = clampi(obs_data_get_int(s, "drop_bars"), 1, 8);
	set->breathing = clampf(obs_data_get_double(s, "breathing"), 0.0f, 1.0f);
	set->rotation = obs_data_get_bool(s, "rotation");
	set->seed = (unsigned)clampi(obs_data_get_int(s, "seed"), 0, 99999);

	for (int i = 0; i < set->n_palettes; i++) {
		char key[64];
		struct pl_palette *p = &set->palettes[i];

		snprintf(key, sizeof(key), S_PALETTE, i);
		const char *pn = obs_data_get_string(s, key);
		snprintf(p->name, sizeof(p->name), "%s", pn && pn[0] ? pn : "Palette");

		snprintf(key, sizeof(key), S_MODE, i);
		p->mode = (int)obs_data_get_int(s, key);
		if (p->mode != PL_MODE_GRADIENT && p->mode != PL_MODE_DUOTONE)
			p->mode = PL_MODE_GRADIENT;

		snprintf(key, sizeof(key), S_NCOLOURS, i);
		p->n_colours = clampi(obs_data_get_int(s, key), 2, PL_MAX_COLOURS);

		for (int j = 0; j < p->n_colours; j++) {
			snprintf(key, sizeof(key), S_COLOUR, i, j);
			p->colours[j] = pl_colour_from_obs((unsigned)obs_data_get_int(s, key));
		}
	}
}

/* ------------------------------------------------------------------------- */
/* hotkeys (spec 5.2)                                                         */

static const struct {
	const char *name;
	const char *text;
	int cmd;
	obs_key_t key;
} named_hotkeys[PD_N_NAMED_HOTKEYS] = {
	{"PaletteDirector.ForceOff", "HotkeyForceOff", PD_CMD_FORCE_OFF, OBS_KEY_F13},
	{"PaletteDirector.ForceOn", "HotkeyForceOn", PD_CMD_FORCE_ON, OBS_KEY_F14},
	{"PaletteDirector.Pause", "HotkeyPause", PD_CMD_TOGGLE_PAUSE, OBS_KEY_F15},
	{"PaletteDirector.Rotate", "HotkeyRotate", PD_CMD_ROTATE_NOW, OBS_KEY_F16},
	{"PaletteDirector.Panic", "HotkeyPanic", PD_CMD_PANIC, OBS_KEY_F17},
	{"PaletteDirector.Next", "HotkeyNext", PD_CMD_NEXT, OBS_KEY_F18},
	{"PaletteDirector.Prev", "HotkeyPrev", PD_CMD_PREV, OBS_KEY_F19},
};

static void hk_named(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	(void)hotkey;
	struct palette_lock_filter *f = data;
	for (int i = 0; i < PD_N_NAMED_HOTKEYS; i++) {
		if (f->hk_named[i] != id)
			continue;
		if (named_hotkeys[i].cmd == PD_CMD_FORCE_OFF)
			pd_host_command(f->host, pressed ? PD_CMD_FORCE_OFF : PD_CMD_RELEASE_FORCE, -1,
					PD_ORIGIN_HOTKEY);
		else if (pressed)
			pd_host_command(f->host, named_hotkeys[i].cmd, -1, PD_ORIGIN_HOTKEY);
		return;
	}
}

static void hk_palette(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	(void)hotkey;
	struct palette_lock_filter *f = data;
	if (!pressed)
		return;
	for (int i = 0; i < PL_MAX_PALETTES; i++) {
		if (f->hk_palette[i] == id) {
			pd_host_command(f->host, PD_CMD_GO_LIVE, i, PD_ORIGIN_HOTKEY);
			return;
		}
	}
}

/* Spec 5.2 asks for obs_hotkey_register_source, but libobs refuses it for private sources
 * (obs-hotkey.c: `if (!source || source->context.private ...) return OBS_INVALID_HOTKEY_ID`),
 * and every filter loaded from a scene collection is private (obs.c obs_load_source_type(
 * filter_data, true)). So hotkeys are registered per instance through the frontend registerer
 * (shown in Settings > Hotkeys with the filter name) and their bindings are saved in the
 * filter's own settings ("hotkey_bindings"), which keeps them per instance and with the
 * scene collection. */
static void hotkey_key(char *out, size_t n, int i)
{
	if (i < PD_N_NAMED_HOTKEYS)
		snprintf(out, n, "%s", named_hotkeys[i].name);
	else
		snprintf(out, n, "PaletteDirector.Palette%d", i - PD_N_NAMED_HOTKEYS + 1);
}

static obs_hotkey_id *hotkey_slot(struct palette_lock_filter *f, int i)
{
	return i < PD_N_NAMED_HOTKEYS ? &f->hk_named[i] : &f->hk_palette[i - PD_N_NAMED_HOTKEYS];
}

static void register_hotkeys(struct palette_lock_filter *f, obs_data_t *settings)
{
	const char *fname = obs_source_get_name(f->context);
	obs_data_t *saved = obs_data_get_obj(settings, "hotkey_bindings");
	const bool first = !obs_data_get_bool(settings, "hotkeys_initialised");

	for (int i = 0; i < PD_N_NAMED_HOTKEYS + PL_MAX_PALETTES; i++) {
		char key[64], name[192], desc[256];
		hotkey_key(key, sizeof(key), i);
		snprintf(name, sizeof(name), "%s.%s", key, fname ? fname : "");
		if (i < PD_N_NAMED_HOTKEYS)
			snprintf(desc, sizeof(desc), "%s [%s]", obs_module_text(named_hotkeys[i].text), fname);
		else
			snprintf(desc, sizeof(desc), "%s %d [%s]", obs_module_text("HotkeyPaletteGoLive"),
				 i - PD_N_NAMED_HOTKEYS + 1, fname);
		obs_hotkey_id id =
			obs_hotkey_register_frontend(name, desc, i < PD_N_NAMED_HOTKEYS ? hk_named : hk_palette, f);
		*hotkey_slot(f, i) = id;

		obs_data_array_t *arr = saved ? obs_data_get_array(saved, key) : NULL;
		if (arr) {
			obs_hotkey_load(id, arr);
			obs_data_array_release(arr);
		} else if (first) {
			/* default bindings (mock v2 hotkey table), applied once per filter instance */
			obs_key_combination_t combo;
			if (i < PD_N_NAMED_HOTKEYS) {
				combo.modifiers = 0;
				combo.key = named_hotkeys[i].key;
			} else {
				combo.modifiers = INTERACT_CONTROL_KEY;
				combo.key = (obs_key_t)(OBS_KEY_1 + (i - PD_N_NAMED_HOTKEYS));
			}
			obs_hotkey_load_bindings(id, &combo, 1);
		}
	}
	obs_data_release(saved);
	obs_data_set_bool(settings, "hotkeys_initialised", true);
	blog(LOG_INFO, "[palette-lock] '%s': %d hotkeys registered (%s bindings)", fname,
	     PD_N_NAMED_HOTKEYS + PL_MAX_PALETTES, first ? "default" : "saved");
}

static void save_hotkeys(struct palette_lock_filter *f, obs_data_t *settings)
{
	obs_data_t *saved = obs_data_create();
	for (int i = 0; i < PD_N_NAMED_HOTKEYS + PL_MAX_PALETTES; i++) {
		obs_hotkey_id id = *hotkey_slot(f, i);
		if (id == OBS_INVALID_HOTKEY_ID)
			continue;
		char key[64];
		hotkey_key(key, sizeof(key), i);
		obs_data_array_t *arr = obs_hotkey_save(id);
		if (arr) {
			obs_data_set_array(saved, key, arr);
			obs_data_array_release(arr);
		}
	}
	obs_data_set_obj(settings, "hotkey_bindings", saved);
	obs_data_release(saved);
}

static void unregister_hotkeys(struct palette_lock_filter *f)
{
	for (int i = 0; i < PD_N_NAMED_HOTKEYS; i++)
		if (f->hk_named[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(f->hk_named[i]);
	for (int i = 0; i < PL_MAX_PALETTES; i++)
		if (f->hk_palette[i] != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(f->hk_palette[i]);
}

/* ------------------------------------------------------------------------- */
/* audio source default: a source named like "Mixer", else the first audio input */

struct audio_pick {
	char mixer[256];
	char first[256];
};

static bool enum_audio_pick(void *param, obs_source_t *src)
{
	struct audio_pick *ap = param;
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
		return true;
	const char *n = obs_source_get_name(src);
	if (!n || !n[0])
		return true;
	if (!ap->first[0])
		snprintf(ap->first, sizeof(ap->first), "%s", n);
	if (!ap->mixer[0] && (strstr(n, "Mixer") || strstr(n, "mixer")))
		snprintf(ap->mixer, sizeof(ap->mixer), "%s", n);
	return true;
}

/* ------------------------------------------------------------------------- */
/* source_info callbacks                                                      */

static const char *palette_lock_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("PaletteLock");
}

static void palette_lock_defaults(obs_data_t *settings)
{
	struct pl_set set;
	pl_set_default(&set);
	write_set(settings, &set, true);
	obs_data_set_default_int(settings, "live_palette", 0);
	obs_data_set_default_int(settings, "edit_palette", 0);
	obs_data_set_default_string(settings, "audio_source", "");
}

static void push_to_host(struct palette_lock_filter *f, obs_data_t *settings)
{
	long force = os_atomic_exchange_long(&f->force_live, -1);
	int live_setting = (int)obs_data_get_int(settings, "live_palette");
	if (force < 0 && live_setting != f->live_setting)
		force = live_setting; /* changed from outside (scene load, websocket) */
	f->live_setting = live_setting;
	if (force >= f->set.n_palettes)
		force = f->set.n_palettes - 1;
	f->gen = pd_host_update(f->host, &f->set, (int)force, obs_data_get_string(settings, "audio_source"));
}

static void palette_lock_update(void *data, obs_data_t *settings)
{
	struct palette_lock_filter *f = data;

	set_from_settings(settings, &f->set);

	int edit = (int)obs_data_get_int(settings, "edit_palette");
	if (edit < 0 || edit >= f->set.n_palettes)
		edit = 0;
	f->edit_palette = edit;

	push_to_host(f, settings);
}

static void palette_lock_save(void *data, obs_data_t *settings)
{
	struct palette_lock_filter *f = data;
	save_hotkeys(f, settings);
	int live = pd_host_live_palette(f->host);
	if (live >= 0 && live < f->set.n_palettes) {
		obs_data_set_int(settings, "live_palette", live);
		f->live_setting = live;
	}
}

static void *palette_lock_create(obs_data_t *settings, obs_source_t *source)
{
	struct palette_lock_filter *f = bzalloc(sizeof(*f));
	f->context = source;
	f->edit_palette = 0;
	f->force_live = -1;
	for (int i = 0; i < PD_N_NAMED_HOTKEYS; i++)
		f->hk_named[i] = OBS_INVALID_HOTKEY_ID;
	for (int i = 0; i < PL_MAX_PALETTES; i++)
		f->hk_palette[i] = OBS_INVALID_HOTKEY_ID;

	if (!obs_data_has_user_value(settings, "audio_source")) {
		struct audio_pick ap = {{0}, {0}};
		obs_enum_sources(enum_audio_pick, &ap);
		const char *pick = ap.mixer[0] ? ap.mixer : ap.first;
		if (pick[0])
			obs_data_set_string(settings, "audio_source", pick);
	}

	set_from_settings(settings, &f->set);
	int edit = (int)obs_data_get_int(settings, "edit_palette");
	if (edit >= 0 && edit < f->set.n_palettes)
		f->edit_palette = edit;

	f->host = pd_host_create(source);
	int live = (int)obs_data_get_int(settings, "live_palette");
	if (live < 0 || live >= f->set.n_palettes)
		live = 0;
	f->live_setting = live;
	f->force_live = live; /* restore the saved live palette */
	push_to_host(f, settings);

	register_hotkeys(f, settings);

	char *effect_path = obs_module_file("effects/palette-lock.effect");
	char *effect_err = NULL;
	/* Source create callbacks run outside the graphics context; without
	 * obs_enter_graphics() gs_effect_create_from_file() returns NULL and logs
	 * only at LOG_DEBUG (libobs/graphics/graphics.c gs_valid). */
	obs_enter_graphics();
	f->effect = gs_effect_create_from_file(effect_path, &effect_err);
	obs_leave_graphics();

	if (!f->effect) {
		blog(LOG_ERROR, "[palette-lock] failed to load palette-lock.effect (%s): %s",
		     effect_path ? effect_path : "(null path)", effect_err ? effect_err : "no error text");
	} else {
		f->p_palA = gs_effect_get_param_by_name(f->effect, "palA");
		f->p_palB = gs_effect_get_param_by_name(f->effect, "palB");
		f->p_nA = gs_effect_get_param_by_name(f->effect, "nA");
		f->p_modeA = gs_effect_get_param_by_name(f->effect, "modeA");
		f->p_nB = gs_effect_get_param_by_name(f->effect, "nB");
		f->p_modeB = gs_effect_get_param_by_name(f->effect, "modeB");
		f->p_pfade = gs_effect_get_param_by_name(f->effect, "pfade");
		f->p_amt = gs_effect_get_param_by_name(f->effect, "amt");
		f->p_contrast = gs_effect_get_param_by_name(f->effect, "contrast");
		f->p_preserve = gs_effect_get_param_by_name(f->effect, "preserve");
		blog(LOG_INFO, "[palette-lock] filter created: '%s', %d palette(s), live %d, audio '%s'", f->set.name,
		     f->set.n_palettes, live, obs_data_get_string(settings, "audio_source"));
	}
	bfree(effect_err);
	bfree(effect_path);

	return f;
}

static void palette_lock_destroy(void *data)
{
	struct palette_lock_filter *f = data;
	unregister_hotkeys(f);
	pd_host_destroy(f->host); /* joins the worker, detaches the audio capture callback */
	f->host = NULL;
	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
		f->effect = NULL;
	}
	bfree(f);
}

static void palette_lock_filter_add(void *data, obs_source_t *parent)
{
	struct palette_lock_filter *f = data;
	pd_host_set_attached(f->host, true);
	blog(LOG_INFO, "[palette-lock] '%s' added to '%s'", obs_source_get_name(f->context),
	     parent ? obs_source_get_name(parent) : "?");
}

static void palette_lock_filter_remove(void *data, obs_source_t *parent)
{
	struct palette_lock_filter *f = data;
	pd_host_set_attached(f->host, false); /* worker detaches the audio capture callback */
	blog(LOG_INFO, "[palette-lock] '%s' removed from '%s'", obs_source_get_name(f->context),
	     parent ? obs_source_get_name(parent) : "?");
}

static void upload_palette(float out[PL_MAX_COLOURS * 4], const struct pl_palette *p)
{
	for (int i = 0; i < PL_MAX_COLOURS * 4; i++)
		out[i] = 0.0f;
	for (int i = 0; i < p->n_colours && i < PL_MAX_COLOURS; i++) {
		out[i * 4 + 0] = p->colours[i].r;
		out[i * 4 + 1] = p->colours[i].g;
		out[i * 4 + 2] = p->colours[i].b;
		out[i * 4 + 3] = 0.0f;
	}
}

static void palette_lock_render(void *data, gs_effect_t *unused)
{
	struct palette_lock_filter *f = data;
	(void)unused;

	/* Never draw nothing: on any failure pass the source through untouched. */
	if (!f->effect || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	/* Director state from the lock-free triple buffer (no locks on the render thread) */
	struct pd_render_state rs = {0, 0, -1, 1.0f, 0, 1.0f, 0.0f};
	if (!pd_host_render_state(f->host, &rs))
		rs.live = f->live_setting;
	const int n = f->set.n_palettes;
	if (rs.live < 0 || rs.live >= n)
		rs.live = 0;
	if (rs.gen != f->gen || rs.prev >= n)
		rs.prev = -1; /* indices from an older palette list: no crossfade from a stale slot */
	if (rs.prev < 0)
		rs.pfade = 1.0f;

	/* A/B palettes, colours pre-rotated CPU-side (mock render(): A keeps its order while fading) */
	struct pl_palette pb = f->set.palettes[rs.live];
	struct pl_palette pa = rs.prev >= 0 ? f->set.palettes[rs.prev] : pb;
	pl_rotate_colours(&pb, rs.rotation);
	if (rs.prev < 0 || rs.prev == rs.live)
		pa = pb;

	float palA[PL_MAX_COLOURS * 4];
	float palB[PL_MAX_COLOURS * 4];
	upload_palette(palA, &pa);
	upload_palette(palB, &pb);

	/* amt = strength x breathing x lock_mix (mock: 1 - b + b*min(1, bass*1.2)) */
	float bass = rs.bass * 1.2f;
	if (bass > 1.0f)
		bass = 1.0f;
	const float breath = 1.0f - f->set.breathing + f->set.breathing * bass;
	const float amt = f->set.strength * breath * rs.mix;

	gs_effect_set_val(f->p_palA, palA, sizeof(palA));
	gs_effect_set_val(f->p_palB, palB, sizeof(palB));
	gs_effect_set_int(f->p_nA, pa.n_colours);
	gs_effect_set_int(f->p_modeA, pa.mode);
	gs_effect_set_int(f->p_nB, pb.n_colours);
	gs_effect_set_int(f->p_modeB, pb.mode);
	gs_effect_set_float(f->p_pfade, rs.pfade);
	gs_effect_set_float(f->p_amt, amt);
	gs_effect_set_float(f->p_contrast, f->set.contrast);
	gs_effect_set_float(f->p_preserve, f->set.preserve);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* properties                                                                 */

static void commit(struct palette_lock_filter *f, int force_live, int edit)
{
	obs_data_t *s = obs_source_get_settings(f->context);
	settings_from_set(s, &f->set);
	obs_data_set_int(s, "edit_palette", edit);
	if (force_live >= 0) {
		obs_data_set_int(s, "live_palette", force_live);
		f->live_setting = force_live;
		os_atomic_exchange_long(&f->force_live, force_live);
	}
	obs_source_update(f->context, s);
	obs_data_release(s);
}

static bool btn_noop(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	(void)property;
	(void)data;
	return false;
}

static bool btn_palette_action(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	struct palette_lock_filter *f = data;
	const char *name = obs_property_name(property);

	int live = pd_host_live_palette(f->host);
	if (live < 0 || live >= f->set.n_palettes)
		live = f->live_setting;
	const int live_before = live;
	int edit = f->edit_palette;

	if (strcmp(name, "palette.add") == 0) {
		char nm[PL_NAME_MAX];
		snprintf(nm, sizeof(nm), "Palette %d", f->set.n_palettes + 1);
		int idx = -1;
		if (!pl_add_palette(&f->set, nm, &idx))
			return false;
		edit = idx;
	} else if (strcmp(name, "palette.dup") == 0) {
		int idx = -1;
		if (!pl_duplicate_palette(&f->set, edit, &live, &edit, &idx))
			return false;
		edit = idx;
	} else if (strcmp(name, "palette.del") == 0) {
		const char *gone = f->set.palettes[edit].name;
		blog(LOG_INFO, "[palette-lock] '%s': palette '%s' deleted%s", obs_source_get_name(f->context), gone,
		     edit == live ? " (was live: next palette takes over)" : "");
		if (!pl_delete_palette(&f->set, edit, &live, &edit))
			return false;
		commit(f, live, edit); /* indices changed: re-sync the Director's live palette */
		return true;
	} else if (strcmp(name, "palette.up") == 0) {
		pl_move_palette(&f->set, edit, -1, &live, &edit);
	} else if (strcmp(name, "palette.dn") == 0) {
		pl_move_palette(&f->set, edit, 1, &live, &edit);
	} else if (strcmp(name, "palette.live") == 0) {
		if (edit == live)
			return false;
		/* go live through the Director so the crossfade runs on the beat clock */
		pd_host_command(f->host, PD_CMD_GO_LIVE, edit, PD_ORIGIN_UI);
		return true;
	} else {
		return false;
	}

	commit(f, live != live_before ? live : -1, edit);
	return true;
}

static bool btn_colour_action(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	struct palette_lock_filter *f = data;
	const char *name = obs_property_name(property);
	int edit = f->edit_palette;

	if (edit < 0 || edit >= f->set.n_palettes)
		return false;
	struct pl_palette *p = &f->set.palettes[edit];

	if (strcmp(name, "colour.add") == 0) {
		if (!pl_add_colour(p))
			return false;
	} else if (strcmp(name, "colour.sort") == 0) {
		pl_sort_colours(p);
	} else if (strcmp(name, "colour.rev") == 0) {
		pl_reverse_colours(p);
	} else {
		int e = -1, j = -1;
		char act[8] = {0};
		if (sscanf(name, "sw.%d.%d.%7s", &e, &j, act) != 3)
			return false;
		if (e != edit)
			return false;
		if (strcmp(act, "up") == 0)
			pl_move_colour(p, j, -1);
		else if (strcmp(act, "dn") == 0)
			pl_move_colour(p, j, 1);
		else if (strcmp(act, "rm") == 0) {
			if (!pl_remove_colour(p, j))
				return false;
		} else
			return false;
	}

	commit(f, -1, f->edit_palette);
	return true;
}

static bool btn_reroll(obs_properties_t *props, obs_property_t *property, void *data)
{
	(void)props;
	(void)property;
	struct palette_lock_filter *f = data;
	f->set.seed = (unsigned)(rand() % 100000);
	commit(f, -1, f->edit_palette);
	return true;
}

static bool on_edit_changed(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)priv;
	(void)props;
	(void)property;
	(void)settings;
	return true; /* re-bind the editor controls to the new selection */
}

static bool on_trigger_changed(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)priv;
	(void)property;
	const int trig = (int)obs_data_get_int(settings, "trigger");
	obs_property_set_visible(obs_properties_get(props, "every_phrases"), trig == PL_TRIGGER_PHRASES);
	obs_property_set_visible(obs_properties_get(props, "change_chance"), trig == PL_TRIGGER_RANDOM);
	return true;
}

static bool on_rename(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	(void)props;
	(void)property;
	struct palette_lock_filter *f = priv;
	int edit = f->edit_palette;
	if (edit < 0 || edit >= f->set.n_palettes)
		return false;

	char key[64];
	snprintf(key, sizeof(key), S_PALETTE, edit);
	const char *newname = obs_data_get_string(settings, key);

	/* enforce the unique, non-empty name rule; reject and revert on violation */
	if (!pl_rename_palette(&f->set, edit, newname))
		obs_data_set_string(settings, key, f->set.palettes[edit].name);
	/* the breakdown reference follows the renamed palette */
	obs_data_set_string(settings, "breakdown_palette", f->set.breakdown);

	return true; /* refresh the list so it shows the (possibly reverted) name */
}

static bool enum_audio_list(void *param, obs_source_t *src)
{
	obs_property_t *list = param;
	if ((obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) == 0)
		return true;
	const char *n = obs_source_get_name(src);
	if (n && n[0])
		obs_property_list_add_string(list, n, n);
	return true;
}

static obs_properties_t *palette_lock_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	struct palette_lock_filter *f = data;
	if (!f)
		return props;

	int edit = f->edit_palette;
	if (edit < 0 || edit >= f->set.n_palettes)
		edit = 0;
	const struct pl_palette *ep = &f->set.palettes[edit];
	int live = pd_host_live_palette(f->host);
	if (live < 0 || live >= f->set.n_palettes)
		live = f->live_setting;
	const int bd_idx = pl_breakdown_index(&f->set);

	/* ---- Performance set (UI only until P6 set-file I/O) ---- */
	obs_properties_t *gset = obs_properties_create();
	obs_property_t *set_sel = obs_properties_add_list(gset, "set_name", obs_module_text("Set"), OBS_COMBO_TYPE_LIST,
							  OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(set_sel, f->set.name, f->set.name);
	obs_properties_add_button2(gset, "set.new", obs_module_text("New"), btn_noop, f);
	obs_properties_add_button2(gset, "set.dup", obs_module_text("Duplicate"), btn_noop, f);
	obs_properties_add_button2(gset, "set.ren", obs_module_text("Rename"), btn_noop, f);
	obs_properties_add_button2(gset, "set.del", obs_module_text("Delete"), btn_noop, f);
	obs_properties_add_button2(gset, "set.save", obs_module_text("Save"), btn_noop, f);
	obs_properties_add_button2(gset, "set.load", obs_module_text("LoadFile"), btn_noop, f);
	obs_properties_add_text(gset, "synesthesia_playlist", obs_module_text("SynesthesiaPlaylist"), OBS_TEXT_DEFAULT);
	obs_properties_add_group(props, "performance_set", obs_module_text("PerformanceSet"), OBS_GROUP_NORMAL, gset);

	/* ---- Palettes ---- */
	obs_properties_t *gpal = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(gpal, "edit_palette", obs_module_text("Palettes"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < f->set.n_palettes; i++) {
		char label[PL_NAME_MAX + 32];
		const char *mode = f->set.palettes[i].mode == PL_MODE_DUOTONE ? "duo" : "grad";
		snprintf(label, sizeof(label), "%s %d. %s · %s%s", (i == live) ? "●" : " ", i + 1,
			 f->set.palettes[i].name, mode, i == bd_idx ? " · breakdown" : "");
		obs_property_list_add_int(list, label, i);
	}
	obs_property_set_modified_callback2(list, on_edit_changed, f);

	char key[64];
	snprintf(key, sizeof(key), S_PALETTE, edit);
	obs_property_t *pname = obs_properties_add_text(gpal, key, "Name", OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback2(pname, on_rename, f);

	snprintf(key, sizeof(key), S_MODE, edit);
	obs_property_t *pmode = obs_properties_add_list(gpal, key, "Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pmode, "Gradient map", PL_MODE_GRADIENT);
	obs_property_list_add_int(pmode, "Duotone (first + last)", PL_MODE_DUOTONE);
	obs_property_set_modified_callback2(pmode, on_edit_changed, f);

	for (int j = 0; j < ep->n_colours; j++) {
		char label[32];
		snprintf(key, sizeof(key), S_COLOUR, edit, j);
		snprintf(label, sizeof(label), "Colour %d", j + 1);
		obs_property_t *sw = obs_properties_add_color(gpal, key, label);

		/* duotone uses only the first and last colour; dim the middle swatches */
		if (ep->mode == PL_MODE_DUOTONE && j > 0 && j < ep->n_colours - 1)
			obs_property_set_enabled(sw, false);

		char btn[32];
		snprintf(btn, sizeof(btn), "sw.%d.%d.up", edit, j);
		obs_properties_add_button2(gpal, btn, "▲", btn_colour_action, f);
		snprintf(btn, sizeof(btn), "sw.%d.%d.dn", edit, j);
		obs_properties_add_button2(gpal, btn, "▼", btn_colour_action, f);
		snprintf(btn, sizeof(btn), "sw.%d.%d.rm", edit, j);
		obs_properties_add_button2(gpal, btn, "✕", btn_colour_action, f);
	}

	obs_property_t *b;
	b = obs_properties_add_button2(gpal, "colour.add", "＋ Add colour", btn_colour_action, f);
	obs_property_set_enabled(b, ep->n_colours < PL_MAX_COLOURS);
	obs_properties_add_button2(gpal, "colour.sort", "Sort dark → bright", btn_colour_action, f);
	obs_properties_add_button2(gpal, "colour.rev", "Reverse", btn_colour_action, f);

	b = obs_properties_add_button2(gpal, "palette.live", "▶ Go live", btn_palette_action, f);
	obs_property_set_enabled(b, edit != live);
	obs_properties_add_button2(gpal, "palette.up", "▲ Move up", btn_palette_action, f);
	obs_properties_add_button2(gpal, "palette.dn", "▼ Move down", btn_palette_action, f);
	b = obs_properties_add_button2(gpal, "palette.add", "＋ New palette", btn_palette_action, f);
	obs_property_set_enabled(b, f->set.n_palettes < PL_MAX_PALETTES);
	b = obs_properties_add_button2(gpal, "palette.dup", "Duplicate", btn_palette_action, f);
	obs_property_set_enabled(b, f->set.n_palettes < PL_MAX_PALETTES);
	b = obs_properties_add_button2(gpal, "palette.del", "Delete", btn_palette_action, f);
	obs_property_set_enabled(b, f->set.n_palettes > 1);

	obs_properties_add_group(props, "palettes", obs_module_text("Palettes"), OBS_GROUP_NORMAL, gpal);

	/* ---- Colour ---- */
	obs_properties_t *gcol = obs_properties_create();
	obs_properties_add_float_slider(gcol, "strength", "Strength", 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(gcol, "contrast", "Contrast in", 0.5, 2.0, 0.01);
	obs_properties_add_float_slider(gcol, "preserve", "Keep brightness", 0.0, 1.0, 0.01);
	obs_properties_add_group(props, "colour", obs_module_text("Colour"), OBS_GROUP_NORMAL, gcol);

	/* ---- Palette changes ---- */
	obs_properties_t *gchg = obs_properties_create();
	obs_property_t *trig = obs_properties_add_list(gchg, "trigger", obs_module_text("ChangeOn"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(trig, obs_module_text("ChangeOn.Drop"), PL_TRIGGER_DROP);
	obs_property_list_add_int(trig, obs_module_text("ChangeOn.Phrases"), PL_TRIGGER_PHRASES);
	obs_property_list_add_int(trig, obs_module_text("ChangeOn.Random"), PL_TRIGGER_RANDOM);
	obs_property_list_add_int(trig, obs_module_text("ChangeOn.Manual"), PL_TRIGGER_MANUAL);
	obs_property_set_modified_callback2(trig, on_trigger_changed, f);
	obs_property_t *every =
		obs_properties_add_int_slider(gchg, "every_phrases", obs_module_text("EveryN"), 1, 16, 1);
	obs_property_set_visible(every, f->set.trigger == PL_TRIGGER_PHRASES);
	obs_property_t *chance = obs_properties_add_float_slider(gchg, "change_chance",
								 obs_module_text("ChancePerPhrase"), 0.0, 1.0, 0.01);
	obs_property_set_visible(chance, f->set.trigger == PL_TRIGGER_RANDOM);
	obs_property_t *order = obs_properties_add_list(gchg, "order", obs_module_text("Order"), OBS_COMBO_TYPE_LIST,
							OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(order, obs_module_text("Order.Sequence"), PL_ORDER_SEQUENCE);
	obs_property_list_add_int(order, obs_module_text("Order.Shuffle"), PL_ORDER_SHUFFLE);
	obs_properties_add_float_slider(gchg, "crossfade_beats", "Crossfade (beats, 0 = cut)", 0.0, 16.0, 0.5);
	obs_property_t *bdl = obs_properties_add_list(gchg, "breakdown_palette", obs_module_text("BreakdownPalette"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(bdl, obs_module_text("None"), "");
	for (int i = 0; i < f->set.n_palettes; i++)
		obs_property_list_add_string(bdl, f->set.palettes[i].name, f->set.palettes[i].name);
	obs_properties_add_group(props, "palette_changes", obs_module_text("PaletteChanges"), OBS_GROUP_NORMAL, gchg);

	/* ---- Director ---- */
	obs_properties_t *gdir = obs_properties_create();
	obs_properties_add_bool(gdir, "dir_enabled", obs_module_text("Enabled"));
	obs_property_t *aud = obs_properties_add_list(gdir, "audio_source", obs_module_text("AudioSource"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(aud, obs_module_text("AudioSource.None"), "");
	obs_enum_sources(enum_audio_list, aud);
	obs_properties_add_int(gdir, "fallback_bpm", obs_module_text("FallbackBpm"), 60, 200, 1);
	obs_property_t *phr = obs_properties_add_list(gdir, "phrase_beats", obs_module_text("PhraseLength"),
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(phr, "16 beats", 16);
	obs_property_list_add_int(phr, "32 beats", 32);
	obs_properties_add_float_slider(gdir, "lock_fade_beats", obs_module_text("LockFade"), 0.25, 8.0, 0.25);
	obs_properties_add_group(props, "director", obs_module_text("Director"), OBS_GROUP_NORMAL, gdir);

	/* ---- Lock behaviours ---- */
	obs_properties_t *gbeh = obs_properties_create();
	obs_properties_add_bool(gbeh, "random_off", obs_module_text("RandomOff"));
	obs_properties_add_float_slider(gbeh, "random_chance", obs_module_text("ChancePerPhrase"), 0.0, 1.0, 0.01);
	obs_properties_add_int_slider(gbeh, "bars_min", obs_module_text("BarsOffMin"), 1, 8, 1);
	obs_properties_add_int_slider(gbeh, "bars_max", obs_module_text("BarsOffMax"), 1, 16, 1);
	obs_properties_add_int_slider(gbeh, "cooldown_phrases", obs_module_text("Cooldown"), 0, 8, 1);
	obs_properties_add_bool(gbeh, "off_on_breakdown", obs_module_text("OffOnBreakdown"));
	obs_properties_add_bool(gbeh, "off_on_drop", obs_module_text("OffOnDrop"));
	obs_properties_add_int_slider(gbeh, "drop_bars", obs_module_text("RawBarsAtDrop"), 1, 8, 1);
	obs_properties_add_float_slider(gbeh, "breathing", obs_module_text("Breathing"), 0.0, 1.0, 0.01);
	obs_properties_add_bool(gbeh, "rotation", obs_module_text("Rotation"));
	obs_properties_add_int(gbeh, "seed", obs_module_text("Seed"), 0, 99999, 1);
	obs_properties_add_button2(gbeh, "seed.reroll", obs_module_text("Reroll"), btn_reroll, f);
	obs_properties_add_group(props, "lock_behaviours", obs_module_text("LockBehaviours"), OBS_GROUP_NORMAL, gbeh);

	/* ---- Hotkeys (read-only listing; bindings live in Settings > Hotkeys) ---- */
	obs_properties_t *ghk = obs_properties_create();
	obs_property_t *info =
		obs_properties_add_text(ghk, "hotkeys.info", obs_module_text("HotkeysInfo"), OBS_TEXT_INFO);
	obs_property_text_set_info_type(info, OBS_TEXT_INFO_NORMAL);
	obs_properties_add_group(props, "hotkeys", obs_module_text("Hotkeys"), OBS_GROUP_NORMAL, ghk);

	return props;
}

/* ------------------------------------------------------------------------- */

static struct obs_source_info palette_lock_filter_info = {
	.id = "palette_lock_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = palette_lock_get_name,
	.create = palette_lock_create,
	.destroy = palette_lock_destroy,
	.get_defaults = palette_lock_defaults,
	.get_properties = palette_lock_properties,
	.update = palette_lock_update,
	.save = palette_lock_save,
	.filter_add = palette_lock_filter_add,
	.filter_remove = palette_lock_filter_remove,
	.video_render = palette_lock_render,
};

void palette_lock_filter_load(void)
{
	obs_register_source(&palette_lock_filter_info);
}
