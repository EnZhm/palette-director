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

/*
Performance set files, schema 2 (spec 5.1 / design "Performance set file").

libobs-free (pure C++17 behind a C API) so the offline test suite exercises exactly
the code the filter uses. The writer emits one canonical layout (2-space indent, one
array item per line, reals in shortest round-trip form), which is the layout of the
bundled data/sets/techno-apocalypse.json: loading it and saving it again reproduces
the file byte for byte. The breakdown palette is stored and resolved BY NAME.
*/

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "palette/palette.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parse a schema-2 set. Returns false (with a message in err) on any error; out is
 * only written on success. */
bool pd_set_parse(const char *json, struct pl_set *out, char *err, size_t err_len);

/* Serialise a set; returns a malloc'd NUL-terminated string (free with pd_set_free). */
char *pd_set_write(const struct pl_set *s);
void pd_set_free(char *p);

/* File name stem for a set name (mock setPath): drop chars other than [A-Za-z0-9_- ],
 * then runs of whitespace -> '-'. Falls back to "set" when empty. */
void pd_set_slug(const char *name, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
