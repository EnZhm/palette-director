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

/*
Palette runtime model for the Palette Lock filter (P2).

This is the filter-side data model: one performance set holding 1..16 named
palettes, each with a recolour mode and 2..8 colours (dark -> bright).  It is
deliberately free of any libobs/Qt dependency so the set lifecycle can be unit
tested and later bridged to the pure-C++ Director (P4) and set files (P6).

Lifecycle rules (design doc "Filter properties"):
  - A set always holds at least one palette; each palette 2..8 colours.
  - Palette names are unique within the set.
  - Deleting the live palette hands the live marker to the next palette
    (the palette that slides into its position), with no hard cut (the
    filter fades through pfade).
  - Reordering keeps the live marker on its palette, not its position.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PL_MAX_PALETTES 8
#define PL_MAX_COLOURS 8
#define PL_NAME_MAX 64
#define PL_HEX_LEN 8 /* "#RRGGBB" including NUL */

enum pl_mode {
	PL_MODE_GRADIENT = 0, /* gradient map */
	PL_MODE_DUOTONE = 1,  /* duotone (first + last colour) */
};

struct pl_colour {
	float r;
	float g;
	float b;
};

struct pl_palette {
	char name[PL_NAME_MAX];
	int mode; /* enum pl_mode */
	int n_colours;
	struct pl_colour colours[PL_MAX_COLOURS];
};

enum pl_trigger {
	PL_TRIGGER_DROP = 0,    /* every drop */
	PL_TRIGGER_PHRASES = 1, /* every N phrases */
	PL_TRIGGER_RANDOM = 2,  /* random, per phrase */
	PL_TRIGGER_MANUAL = 3,  /* manual only */
};

/* where the gradient-map position comes from (v1 amendments) */
enum pl_brightness_source {
	PL_BSRC_PERCEIVED = 0, /* Rec.709 luma (default) */
	PL_BSRC_MAX = 1,       /* max(r, g, b): saturated colours count as bright */
	PL_BSRC_AVERAGE = 2,   /* (r + g + b) / 3 */
};

enum pl_order {
	PL_ORDER_SEQUENCE = 0, /* in list order */
	PL_ORDER_SHUFFLE = 1,  /* shuffle (no repeats) */
};

struct pl_set {
	char name[PL_NAME_MAX];
	char synesthesia_playlist[PL_NAME_MAX];
	struct pl_palette palettes[PL_MAX_PALETTES];
	int n_palettes;
	float strength;        /* 0..1, shown as "Coverage" 0..100% */
	float contrast;        /* 0.5..2 */
	float preserve;        /* 0..1 (keep brightness) */
	float input_black;     /* 0..0.5: source brightness mapped to the palette's first colour */
	float input_white;     /* 0.5..1: source brightness mapped to the palette's last colour */
	int brightness_source; /* enum pl_brightness_source */
	float crossfade_beats; /* 0 = cut, else beats */

	/* Palette changes (P4; defaults = mock DEF_DIR) */
	int trigger;       /* enum pl_trigger */
	int every_phrases; /* 1..16 */
	float change_chance;
	int order;                   /* enum pl_order */
	char breakdown[PL_NAME_MAX]; /* breakdown palette BY NAME, "" = none */

	/* Director + lock behaviours (P4; defaults = mock DEF_DIR) */
	bool dir_enabled;
	int phrase_beats;      /* 16 | 32 */
	float lock_fade_beats; /* 0.25..8 */
	float fallback_bpm;    /* 60..200 */
	bool random_off;
	float random_chance;
	int bars_min;
	int bars_max;
	int cooldown_phrases;
	bool off_on_breakdown;
	bool off_on_drop;
	int drop_bars;
	float breathing;
	bool rotation;
	unsigned seed; /* 0..99999 */
};

/* Shader blend amount (v1 amendments): coverage x breathing x lock_mix, mock fade model.
 * Breathing (1 - b + b*min(1, bass*1.2)) only ever dips below coverage, and is not applied
 * while the feed is silent (there is nothing to breathe with): coverage 1 + lock on then
 * means pure palette output. */
float pl_lock_amount(float coverage, float breathing, float bass, float lock_mix, bool silent);

/* index of the breakdown palette (resolved by name), -1 = none */
int pl_breakdown_index(const struct pl_set *s);

void pl_set_init(struct pl_set *s);
void pl_set_default(struct pl_set *s);
void pl_palette_default(struct pl_palette *p, const char *name);

/* palette lifecycle (return false when the operation is a no-op / rejected) */
bool pl_name_unique(const struct pl_set *s, const char *name, int except);
bool pl_add_palette(struct pl_set *s, const char *name, int *out_index);
bool pl_duplicate_palette(struct pl_set *s, int index, int *live, int *edit, int *out_index);
bool pl_rename_palette(struct pl_set *s, int index, const char *name);
bool pl_delete_palette(struct pl_set *s, int index, int *live, int *edit);
void pl_move_palette(struct pl_set *s, int index, int dir, int *live, int *edit);

/* colour lifecycle */
bool pl_add_colour(struct pl_palette *p);
bool pl_remove_colour(struct pl_palette *p, int index);
void pl_move_colour(struct pl_palette *p, int index, int dir);
void pl_sort_colours(struct pl_palette *p);
void pl_reverse_colours(struct pl_palette *p);

/* colour helpers */
struct pl_colour pl_colour_from_obs(unsigned obs); /* OBS 0xAABBGGRR */
unsigned pl_colour_to_obs(struct pl_colour c);
struct pl_colour pl_colour_from_hex(const char *hex); /* "#RRGGBB" */
void pl_colour_to_hex(struct pl_colour c, char out[PL_HEX_LEN]);
float pl_luma(struct pl_colour c);

/* CPU-side colour rotation: upload palettes pre-rotated, never in the shader */
void pl_rotate_colours(struct pl_palette *p, int offset);

#ifdef __cplusplus
}
#endif
