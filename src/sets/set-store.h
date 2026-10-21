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
with this program. If not, see <https://www.gnu.org/licenses/>
*/

/* Set store: one schema-2 JSON file per set in Documents\PaletteDirector\sets\ (Windows)
 * or $XDG_CONFIG_HOME/PaletteDirector/sets/ (Linux dev), spec 5.1. UI thread only. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "palette/palette.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PD_SETS_MAX 64

struct pd_set_entry {
	char name[PL_NAME_MAX];
	char path[512];
};

/* sets directory (created on demand); false if it can't be determined/created */
bool pd_sets_dir(char *out, size_t out_len);
/* path for a set name: <dir>/<slug>.json */
bool pd_sets_path_for(const char *name, char *out, size_t out_len);
/* all readable sets in the store, sorted by name; returns the count */
int pd_sets_list(struct pd_set_entry *out, int max);
bool pd_sets_load_file(const char *path, struct pl_set *out, char *err, size_t err_len);
bool pd_sets_load_name(const char *name, struct pl_set *out, char *err, size_t err_len);
/* write the set to its store path (byte-exact, no BOM); path_out optional */
bool pd_sets_save(const struct pl_set *s, char *path_out, size_t path_len);
bool pd_sets_delete(const char *name);
/* true if the set differs from its file in the store (or has no file yet) */
bool pd_sets_dirty(const struct pl_set *s);

#ifdef __cplusplus
}
#endif
