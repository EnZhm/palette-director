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
#include <string.h>

#include "palette-lock-filter.h"
#include "palette/palette.h"

#define S_PALETTE "palette.%d.name"
#define S_MODE "palette.%d.mode"
#define S_NCOLOURS "palette.%d.n_colours"
#define S_COLOUR "palette.%d.colour.%d"

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
	int live;
	int prev;
	float pfade;
	int rotation; /* CPU-side array rotation offset; 0 until P4 */
	int edit_palette;
};

/* ------------------------------------------------------------------------- */
/* settings <-> set (obs_data serialisation)                                  */

static void settings_from_set(obs_data_t *s, const struct pl_set *set)
{
	obs_data_set_string(s, "set_name", set->name);
	obs_data_set_string(s, "synesthesia_playlist", set->synesthesia_playlist);
	obs_data_set_int(s, "n_palettes", set->n_palettes);
	obs_data_set_double(s, "strength", set->strength);
	obs_data_set_double(s, "contrast", set->contrast);
	obs_data_set_double(s, "preserve", set->preserve);
	obs_data_set_double(s, "crossfade_beats", set->crossfade_beats);

	for (int i = 0; i < set->n_palettes; i++) {
		char key[64];
		const struct pl_palette *p = &set->palettes[i];
		snprintf(key, sizeof(key), S_PALETTE, i);
		obs_data_set_string(s, key, p->name);
		snprintf(key, sizeof(key), S_MODE, i);
		obs_data_set_int(s, key, p->mode);
		snprintf(key, sizeof(key), S_NCOLOURS, i);
		obs_data_set_int(s, key, p->n_colours);
		for (int j = 0; j < p->n_colours; j++) {
			snprintf(key, sizeof(key), S_COLOUR, i, j);
			obs_data_set_int(s, key, (long long)pl_colour_to_argb(p->colours[j]));
		}
	}
}

static void set_from_settings(obs_data_t *s, struct pl_set *set)
{
	pl_set_init(set);

	const char *name = obs_data_get_string(s, "set_name");
	snprintf(set->name, sizeof(set->name), "%s", name && name[0] ? name : "Default Set");

	const char *pl = obs_data_get_string(s, "synesthesia_playlist");
	snprintf(set->synesthesia_playlist, sizeof(set->synesthesia_playlist), "%s", pl ? pl : "");

	set->n_palettes = (int)obs_data_get_int(s, "n_palettes");
	if (set->n_palettes < 1)
		set->n_palettes = 1;
	if (set->n_palettes > PL_MAX_PALETTES)
		set->n_palettes = PL_MAX_PALETTES;

	set->strength = (float)obs_data_get_double(s, "strength");
	set->contrast = (float)obs_data_get_double(s, "contrast");
	set->preserve = (float)obs_data_get_double(s, "preserve");
	set->crossfade_beats = (float)obs_data_get_double(s, "crossfade_beats");

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
		p->n_colours = (int)obs_data_get_int(s, key);
		if (p->n_colours < 2)
			p->n_colours = 2;
		if (p->n_colours > PL_MAX_COLOURS)
			p->n_colours = PL_MAX_COLOURS;

		for (int j = 0; j < p->n_colours; j++) {
			snprintf(key, sizeof(key), S_COLOUR, i, j);
			p->colours[j] = pl_colour_from_argb((unsigned)obs_data_get_int(s, key));
		}
	}
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
	settings_from_set(settings, &set);
	obs_data_set_int(settings, "live_palette", 0);
	obs_data_set_int(settings, "edit_palette", 0);
}

static void palette_lock_update(void *data, obs_data_t *settings)
{
	struct palette_lock_filter *f = data;

	set_from_settings(settings, &f->set);

	int live = (int)obs_data_get_int(settings, "live_palette");
	int edit = (int)obs_data_get_int(settings, "edit_palette");
	if (live < 0 || live >= f->set.n_palettes)
		live = 0;
	if (edit < 0 || edit >= f->set.n_palettes)
		edit = 0;

	f->live = live;
	f->edit_palette = edit;
	if (f->prev < 0 || f->prev >= f->set.n_palettes)
		f->prev = live;
}

static void *palette_lock_create(obs_data_t *settings, obs_source_t *source)
{
	struct palette_lock_filter *f = bzalloc(sizeof(*f));
	f->context = source;
	f->live = 0;
	f->prev = 0;
	f->pfade = 1.0f;
	f->rotation = 0;
	f->edit_palette = 0;

	set_from_settings(settings, &f->set);

	int live = (int)obs_data_get_int(settings, "live_palette");
	int edit = (int)obs_data_get_int(settings, "edit_palette");
	if (live >= 0 && live < f->set.n_palettes)
		f->live = live;
	if (edit >= 0 && edit < f->set.n_palettes)
		f->edit_palette = edit;
	f->prev = f->live;

	char *effect_path = obs_module_file("effects/palette-lock.effect");
	f->effect = gs_effect_create_from_file(effect_path, NULL);
	bfree(effect_path);

	if (!f->effect) {
		blog(LOG_WARNING, "[palette-lock] failed to load palette-lock.effect");
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
		blog(LOG_INFO, "[palette-lock] filter created: '%s', %d palette(s)", f->set.name, f->set.n_palettes);
	}

	return f;
}

static void palette_lock_destroy(void *data)
{
	struct palette_lock_filter *f = data;
	if (f->effect) {
		gs_effect_destroy(f->effect);
		f->effect = NULL;
	}
	bfree(f);
}

static void palette_lock_tick(void *data, float seconds)
{
	struct palette_lock_filter *f = data;
	if (f->pfade < 1.0f) {
		/* fixed 128 BPM clock (Director arrives in P4) */
		float beats = f->set.crossfade_beats;
		float denom = beats > 0.01f ? beats : 0.01f;
		f->pfade += (128.0f / 60.0f) * seconds / denom;
		if (f->pfade > 1.0f)
			f->pfade = 1.0f;
	}
}

static void palette_lock_render(void *data, gs_effect_t *unused)
{
	struct palette_lock_filter *f = data;
	(void)unused;

	if (!f->effect || !obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	/* A/B palettes, colours pre-rotated CPU-side */
	struct pl_palette pa = f->set.palettes[f->prev >= 0 ? f->prev : 0];
	struct pl_palette pb = f->set.palettes[f->live];
	pl_rotate_colours(&pa, f->rotation);
	pl_rotate_colours(&pb, f->rotation);

	float palA[PL_MAX_COLOURS * 3] = {0};
	float palB[PL_MAX_COLOURS * 3] = {0};
	for (int i = 0; i < pa.n_colours && i < PL_MAX_COLOURS; i++) {
		palA[i * 3 + 0] = pa.colours[i].r;
		palA[i * 3 + 1] = pa.colours[i].g;
		palA[i * 3 + 2] = pa.colours[i].b;
	}
	for (int i = 0; i < pb.n_colours && i < PL_MAX_COLOURS; i++) {
		palB[i * 3 + 0] = pb.colours[i].r;
		palB[i * 3 + 1] = pb.colours[i].g;
		palB[i * 3 + 2] = pb.colours[i].b;
	}

	gs_effect_set_val(f->p_palA, palA, sizeof(palA));
	gs_effect_set_val(f->p_palB, palB, sizeof(palB));
	gs_effect_set_int(f->p_nA, pa.n_colours);
	gs_effect_set_int(f->p_modeA, pa.mode);
	gs_effect_set_int(f->p_nB, pb.n_colours);
	gs_effect_set_int(f->p_modeB, pb.mode);
	gs_effect_set_float(f->p_pfade, f->pfade);
	/* amt = strength * breathing * mix; breathing and mix are 0/1 in P2 */
	gs_effect_set_float(f->p_amt, f->set.strength);
	gs_effect_set_float(f->p_contrast, f->set.contrast);
	gs_effect_set_float(f->p_preserve, f->set.preserve);

	obs_source_process_filter_end(f->context, f->effect, 0, 0);
}

/* ------------------------------------------------------------------------- */
/* properties                                                                 */

static void commit(struct palette_lock_filter *f, int live, int edit)
{
	obs_data_t *s = obs_source_get_settings(f->context);
	settings_from_set(s, &f->set);
	obs_data_set_int(s, "live_palette", live);
	obs_data_set_int(s, "edit_palette", edit);
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

	int live = f->live;
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
		if (!pl_duplicate_palette(&f->set, edit, &idx))
			return false;
		edit = idx;
	} else if (strcmp(name, "palette.del") == 0) {
		if (!pl_delete_palette(&f->set, edit, &live, &edit))
			return false;
	} else if (strcmp(name, "palette.up") == 0) {
		pl_move_palette(&f->set, edit, -1, &live, &edit);
	} else if (strcmp(name, "palette.dn") == 0) {
		pl_move_palette(&f->set, edit, 1, &live, &edit);
	} else if (strcmp(name, "palette.live") == 0) {
		if (edit == live)
			return false;
		f->prev = live;
		live = edit;
		f->pfade = f->set.crossfade_beats <= 0.0f ? 1.0f : 0.0f;
	} else {
		return false;
	}

	commit(f, live, edit);
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

	commit(f, f->live, f->edit_palette);
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
		char label[PL_NAME_MAX + 16];
		const char *mode = f->set.palettes[i].mode == PL_MODE_DUOTONE ? "duo" : "grad";
		snprintf(label, sizeof(label), "%s %d. %s · %s", (i == f->live) ? "●" : " ", i + 1,
			 f->set.palettes[i].name, mode);
		obs_property_list_add_int(list, label, i);
	}
	obs_property_set_modified_callback2(list, on_edit_changed, f);

	char key[64];
	snprintf(key, sizeof(key), S_PALETTE, edit);
	obs_property_t *pname = obs_properties_add_text(gpal, key, "Name", OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback2(pname, on_edit_changed, f);

	snprintf(key, sizeof(key), S_MODE, edit);
	obs_property_t *pmode = obs_properties_add_list(gpal, key, "Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(pmode, "Gradient map", PL_MODE_GRADIENT);
	obs_property_list_add_int(pmode, "Duotone (first + last)", PL_MODE_DUOTONE);
	obs_property_set_modified_callback2(pmode, on_edit_changed, f);

	for (int j = 0; j < ep->n_colours; j++) {
		char label[32];
		snprintf(key, sizeof(key), S_COLOUR, edit, j);
		snprintf(label, sizeof(label), "Colour %d", j + 1);
		obs_properties_add_color(gpal, key, label);

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
	obs_property_set_enabled(b, edit != f->live);
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

	/* ---- Palette changes (crossfade only; triggers/order arrive in P4) ---- */
	obs_properties_t *gchg = obs_properties_create();
	obs_properties_add_float_slider(gchg, "crossfade_beats", "Crossfade (beats, 0 = cut)", 0.0, 16.0, 0.5);
	obs_properties_add_group(props, "palette_changes", obs_module_text("PaletteChanges"), OBS_GROUP_NORMAL, gchg);

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
	.video_tick = palette_lock_tick,
	.video_render = palette_lock_render,
};

void palette_lock_filter_load(void)
{
	obs_register_source(&palette_lock_filter_info);
}
