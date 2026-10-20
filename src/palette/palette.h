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

This is the filter-side data model: one performance set holding 1..8 named
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

struct pl_set {
	char name[PL_NAME_MAX];
	char synesthesia_playlist[PL_NAME_MAX];
	struct pl_palette palettes[PL_MAX_PALETTES];
	int n_palettes;
	float strength;        /* 0..1 */
	float contrast;        /* 0.5..2 */
	float preserve;        /* 0..1 (keep brightness) */
	float crossfade_beats; /* 0 = cut, else beats */
};

void pl_set_init(struct pl_set *s);
void pl_set_default(struct pl_set *s);
void pl_palette_default(struct pl_palette *p, const char *name);

/* palette lifecycle (return false when the operation is a no-op / rejected) */
bool pl_name_unique(const struct pl_set *s, const char *name, int except);
bool pl_add_palette(struct pl_set *s, const char *name, int *out_index);
bool pl_duplicate_palette(struct pl_set *s, int index, int *out_index);
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
struct pl_colour pl_colour_from_argb(unsigned argb); /* 0xAARRGGBB */
unsigned pl_colour_to_argb(struct pl_colour c);
struct pl_colour pl_colour_from_hex(const char *hex); /* "#RRGGBB" */
void pl_colour_to_hex(struct pl_colour c, char out[PL_HEX_LEN]);
float pl_luma(struct pl_colour c);

/* CPU-side colour rotation: upload palettes pre-rotated, never in the shader */
void pl_rotate_colours(struct pl_palette *p, int offset);

#ifdef __cplusplus
}
#endif
