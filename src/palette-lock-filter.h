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

#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

void palette_lock_filter_load(void);

/* ---- Palette Director dock API (v1 amendments, phase C) --------------------------------
 * UI thread only. Edits go through the filter's settings + obs_source_update, exactly the
 * path the properties window uses; the filter then pushes them into the Director. All calls
 * return false / do nothing when `src` is not a Palette Lock filter. */
struct pl_set;
/* current set as stored in the filter settings, its edit palette and the Director's live palette */
bool palette_lock_read_set(obs_source_t *src, struct pl_set *out, int *edit, int *live);
/* apply a modified set; force_live >= 0 re-syncs the Director's live palette (indices changed) */
bool palette_lock_commit(obs_source_t *src, const struct pl_set *ns, int force_live, const char *why);
/* audio source by name ("" = none / fixed BPM) */
bool palette_lock_set_audio(obs_source_t *src, const char *name);
const char *palette_lock_audio(obs_source_t *src); /* bfree() the result */
/* set lifecycle: "set.new", "set.dup", "set.ren", "set.del", "set.save", "set.load" */
bool palette_lock_set_action(obs_source_t *src, const char *action);
/* switch to a stored set by name (asks first when the current set has unsaved changes) */
bool palette_lock_select_set(obs_source_t *src, const char *name);

#ifdef __cplusplus
}
#endif
