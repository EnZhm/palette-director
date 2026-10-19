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

#include <obs-module.h>

#include "palette-lock-filter.h"

static const char *palette_lock_get_name(void *unused)
{
	(void)unused;
	return obs_module_text("PaletteLock");
}

static void *palette_lock_create(obs_data_t *settings, obs_source_t *filter)
{
	(void)settings;
	return filter;
}

static void palette_lock_destroy(void *data)
{
	(void)data;
}

static void palette_lock_render(void *data, gs_effect_t *effect)
{
	obs_source_t *filter = data;

	if (!obs_source_process_filter_begin(filter, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	obs_source_process_filter_end(filter, effect, 0, 0);
}

static struct obs_source_info palette_lock_filter_info = {
	.id = "palette_lock_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = palette_lock_get_name,
	.create = palette_lock_create,
	.destroy = palette_lock_destroy,
	.video_render = palette_lock_render,
};

void palette_lock_filter_load(void)
{
	obs_register_source(&palette_lock_filter_info);
}
