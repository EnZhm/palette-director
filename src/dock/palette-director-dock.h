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

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#include <obs-module.h>

void palette_director_dock_load(void);
/* show + raise the dock and select `filter` in its instance picker (UI thread) */
void palette_director_dock_show(obs_source_t *filter);
/* false when the plugin was built without the frontend API / Qt */
bool palette_director_dock_available(void);

#ifdef __cplusplus
}
#endif
