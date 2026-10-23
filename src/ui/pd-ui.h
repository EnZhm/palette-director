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

/* Small modal helpers for the filter's properties buttons (set lifecycle: prompts,
 * confirms, file picker). Called from properties callbacks, i.e. on the UI thread.
 * Without Qt they fall back to non-interactive defaults (confirm = yes, prompt = default). */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pd_ui_confirm(const char *title, const char *text);
/* returns a bstrdup'd string (bfree it) or NULL if cancelled */
char *pd_ui_prompt(const char *title, const char *label, const char *def);
/* returns a bstrdup'd path (bfree it) or NULL if cancelled */
char *pd_ui_open_json(const char *title, const char *start_dir);
void pd_ui_message(const char *title, const char *text);

#ifdef __cplusplus
}
#endif
