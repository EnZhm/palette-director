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

#include "palette.h"

#include <stdio.h>
#include <string.h>

void pl_set_init(struct pl_set *s)
{
	memset(s, 0, sizeof(*s));
	s->strength = 1.0f;
	s->contrast = 1.1f;
	s->preserve = 0.0f;
	s->input_black = 0.0f;
	s->input_white = 1.0f;
	s->brightness_source = PL_BSRC_PERCEIVED;
	s->crossfade_beats = 2.0f;

	/* mock v2 DEF_DIR */
	s->trigger = PL_TRIGGER_DROP;
	s->every_phrases = 4;
	s->change_chance = 0.25f;
	s->order = PL_ORDER_SEQUENCE;
	s->breakdown[0] = '\0';
	s->dir_enabled = true;
	s->phrase_beats = 32;
	s->lock_fade_beats = 1.0f;
	s->fallback_bpm = 128.0f;
	s->random_off = true;
	s->random_chance = 0.15f;
	s->bars_min = 1;
	s->bars_max = 4;
	s->cooldown_phrases = 2;
	s->off_on_breakdown = false;
	s->off_on_drop = false;
	s->drop_bars = 2;
	s->breathing = 0.0f;
	s->rotation = false;
	s->seed = 7;
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

float pl_lock_amount(float coverage, float breathing, float bass, float lock_mix, bool silent)
{
	float b = silent ? 0.0f : clamp01(breathing);
	float bb = bass * 1.2f;
	if (bb > 1.0f)
		bb = 1.0f;
	const float breath = 1.0f - b + b * bb;
	return clamp01(coverage) * breath * clamp01(lock_mix);
}

int pl_breakdown_index(const struct pl_set *s)
{
	if (!s->breakdown[0])
		return -1;
	for (int i = 0; i < s->n_palettes; i++) {
		if (strcmp(s->palettes[i].name, s->breakdown) == 0)
			return i;
	}
	return -1;
}

void pl_palette_default(struct pl_palette *p, const char *name)
{
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", name);
	p->mode = PL_MODE_GRADIENT;
	p->n_colours = 4;
	p->colours[0] = pl_colour_from_hex("#000000");
	p->colours[1] = pl_colour_from_hex("#444444");
	p->colours[2] = pl_colour_from_hex("#BBBBBB");
	p->colours[3] = pl_colour_from_hex("#FFFFFF");
}

void pl_set_default(struct pl_set *s)
{
	pl_set_init(s);
	snprintf(s->name, sizeof(s->name), "Default Set");
	s->n_palettes = 1;
	pl_palette_default(&s->palettes[0], "Palette 1");
}

/* ------------------------------------------------------------------------- */

bool pl_name_unique(const struct pl_set *s, const char *name, int except)
{
	for (int i = 0; i < s->n_palettes; i++) {
		if (i == except)
			continue;
		if (strcmp(s->palettes[i].name, name) == 0)
			return false;
	}
	return true;
}

static void pl_unique_name(const struct pl_set *s, char out[PL_NAME_MAX], const char *base)
{
	if (pl_name_unique(s, base, -1)) {
		snprintf(out, PL_NAME_MAX, "%s", base);
		return;
	}
	for (int n = 2; n < 100; n++) {
		char cand[PL_NAME_MAX];
		snprintf(cand, sizeof(cand), "%s (%d)", base, n);
		if (pl_name_unique(s, cand, -1)) {
			snprintf(out, PL_NAME_MAX, "%s", cand);
			return;
		}
	}
	snprintf(out, PL_NAME_MAX, "%s", base);
}

bool pl_add_palette(struct pl_set *s, const char *name, int *out_index)
{
	if (s->n_palettes >= PL_MAX_PALETTES)
		return false;

	int idx = s->n_palettes;
	char base[PL_NAME_MAX];
	snprintf(base, sizeof(base), "%s", name && name[0] ? name : "Palette");
	char uname[PL_NAME_MAX];
	pl_unique_name(s, uname, base);

	pl_palette_default(&s->palettes[idx], uname);
	s->n_palettes++;

	if (out_index)
		*out_index = idx;
	return true;
}

bool pl_duplicate_palette(struct pl_set *s, int index, int *live, int *edit, int *out_index)
{
	if (index < 0 || index >= s->n_palettes || s->n_palettes >= PL_MAX_PALETTES)
		return false;

	char base[PL_NAME_MAX];
	snprintf(base, sizeof(base), "%s copy", s->palettes[index].name);

	struct pl_palette copy = s->palettes[index];
	snprintf(copy.name, sizeof(copy.name), "%s", base);

	/* shift palettes after index up one, then insert the copy at index+1 */
	memmove(&s->palettes[index + 2], &s->palettes[index + 1],
		(size_t)(s->n_palettes - index - 1) * sizeof(struct pl_palette));
	s->palettes[index + 1] = copy;
	s->n_palettes++;

	/* live/edit follow their palette, not its position (mirror delete) */
	if (live && *live > index)
		(*live)++;
	if (edit && *edit > index)
		(*edit)++;

	/* ensure unique name after insertion */
	char uname[PL_NAME_MAX];
	pl_unique_name(s, uname, base);
	snprintf(s->palettes[index + 1].name, sizeof(s->palettes[index + 1].name), "%s", uname);

	if (out_index)
		*out_index = index + 1;
	return true;
}

bool pl_rename_palette(struct pl_set *s, int index, const char *name)
{
	if (index < 0 || index >= s->n_palettes)
		return false;
	if (!name || !name[0])
		return false;
	if (!pl_name_unique(s, name, index))
		return false;
	/* the breakdown reference points at the palette, so it follows a rename */
	const bool was_bd = s->breakdown[0] && strcmp(s->breakdown, s->palettes[index].name) == 0;
	snprintf(s->palettes[index].name, sizeof(s->palettes[index].name), "%s", name);
	if (was_bd)
		snprintf(s->breakdown, sizeof(s->breakdown), "%s", name);
	return true;
}

bool pl_delete_palette(struct pl_set *s, int index, int *live, int *edit)
{
	if (s->n_palettes <= 1)
		return false;
	if (index < 0 || index >= s->n_palettes)
		return false;

	/* deleting the breakdown palette sets breakdowns to None (design lifecycle rules) */
	if (s->breakdown[0] && strcmp(s->breakdown, s->palettes[index].name) == 0)
		s->breakdown[0] = '\0';

	memmove(&s->palettes[index], &s->palettes[index + 1],
		(size_t)(s->n_palettes - index - 1) * sizeof(struct pl_palette));
	s->n_palettes--;

	if (live) {
		if (*live == index)
			*live = (index >= s->n_palettes) ? s->n_palettes - 1 : index;
		else if (*live > index)
			(*live)--;
	}
	if (edit) {
		if (*edit > index)
			(*edit)--;
		else if (*edit >= s->n_palettes)
			*edit = s->n_palettes - 1;
	}
	return true;
}

void pl_move_palette(struct pl_set *s, int index, int dir, int *live, int *edit)
{
	int other = index + dir;
	if (index < 0 || index >= s->n_palettes || other < 0 || other >= s->n_palettes)
		return;

	struct pl_palette tmp = s->palettes[index];
	s->palettes[index] = s->palettes[other];
	s->palettes[other] = tmp;

	/* live/edit follow the palette, not the position */
	if (live) {
		if (*live == index)
			*live = other;
		else if (*live == other)
			*live = index;
	}
	if (edit) {
		if (*edit == index)
			*edit = other;
		else if (*edit == other)
			*edit = index;
	}
}

/* ------------------------------------------------------------------------- */

bool pl_add_colour(struct pl_palette *p)
{
	if (p->n_colours >= PL_MAX_COLOURS)
		return false;

	struct pl_colour last = p->colours[p->n_colours - 1];
	struct pl_colour next;
	next.r = (last.r * 0.6f) + 0.4f;
	next.g = (last.g * 0.6f) + 0.4f;
	next.b = (last.b * 0.6f) + 0.4f;
	if (next.r > 1.0f)
		next.r = 1.0f;
	if (next.g > 1.0f)
		next.g = 1.0f;
	if (next.b > 1.0f)
		next.b = 1.0f;

	p->colours[p->n_colours] = next;
	p->n_colours++;
	return true;
}

bool pl_remove_colour(struct pl_palette *p, int index)
{
	if (p->n_colours <= 2)
		return false;
	if (index < 0 || index >= p->n_colours)
		return false;

	memmove(&p->colours[index], &p->colours[index + 1],
		(size_t)(p->n_colours - index - 1) * sizeof(struct pl_colour));
	p->n_colours--;
	return true;
}

void pl_move_colour(struct pl_palette *p, int index, int dir)
{
	int other = index + dir;
	if (index < 0 || index >= p->n_colours || other < 0 || other >= p->n_colours)
		return;

	struct pl_colour tmp = p->colours[index];
	p->colours[index] = p->colours[other];
	p->colours[other] = tmp;
}

void pl_sort_colours(struct pl_palette *p)
{
	/* insertion sort by luma, ascending (dark -> bright) */
	for (int i = 1; i < p->n_colours; i++) {
		struct pl_colour v = p->colours[i];
		int j = i - 1;
		while (j >= 0 && pl_luma(p->colours[j]) > pl_luma(v)) {
			p->colours[j + 1] = p->colours[j];
			j--;
		}
		p->colours[j + 1] = v;
	}
}

void pl_reverse_colours(struct pl_palette *p)
{
	for (int i = 0; i < p->n_colours / 2; i++) {
		struct pl_colour tmp = p->colours[i];
		p->colours[i] = p->colours[p->n_colours - 1 - i];
		p->colours[p->n_colours - 1 - i] = tmp;
	}
}

void pl_rotate_colours(struct pl_palette *p, int offset)
{
	int n = p->n_colours;
	if (n <= 1)
		return;

	offset %= n;
	if (offset < 0)
		offset += n;
	if (offset == 0)
		return;

	struct pl_colour tmp[PL_MAX_COLOURS];
	for (int i = 0; i < n; i++)
		tmp[(i + offset) % n] = p->colours[i];
	for (int i = 0; i < n; i++)
		p->colours[i] = tmp[i];
}

/* ------------------------------------------------------------------------- */

float pl_luma(struct pl_colour c)
{
	return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

struct pl_colour pl_colour_from_obs(unsigned obs)
{
	/* OBS stores colours as 0xAABBGGRR: red in the low byte, blue in bits 16..23
	 * (shared/properties-view/properties-view.cpp color_from_int). */
	struct pl_colour c;
	c.r = (float)(obs & 0xFF) / 255.0f;
	c.g = (float)((obs >> 8) & 0xFF) / 255.0f;
	c.b = (float)((obs >> 16) & 0xFF) / 255.0f;
	return c;
}

unsigned pl_colour_to_obs(struct pl_colour c)
{
	unsigned r = (unsigned)(c.r * 255.0f + 0.5f);
	unsigned g = (unsigned)(c.g * 255.0f + 0.5f);
	unsigned b = (unsigned)(c.b * 255.0f + 0.5f);
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	return 0xFF000000u | (b << 16) | (g << 8) | r;
}

struct pl_colour pl_colour_from_hex(const char *hex)
{
	struct pl_colour c = {0.0f, 0.0f, 0.0f};
	if (!hex)
		return c;
	if (hex[0] == '#')
		hex++;
	unsigned r, g, b;
	if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) != 3)
		return c;
	c.r = (float)r / 255.0f;
	c.g = (float)g / 255.0f;
	c.b = (float)b / 255.0f;
	return c;
}

void pl_colour_to_hex(struct pl_colour c, char out[PL_HEX_LEN])
{
	unsigned r = (unsigned)(c.r * 255.0f + 0.5f);
	unsigned g = (unsigned)(c.g * 255.0f + 0.5f);
	unsigned b = (unsigned)(c.b * 255.0f + 0.5f);
	if (r > 255)
		r = 255;
	if (g > 255)
		g = 255;
	if (b > 255)
		b = 255;
	snprintf(out, PL_HEX_LEN, "#%02X%02X%02X", r, g, b);
}
