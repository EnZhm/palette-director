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

/* C++-only view of the filter hosts for the Palette Director dock (P5).
 * Every call takes the plugin-global registry mutex, so a host can't be destroyed
 * while the dock reads it. Never called from the render or audio threads. */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "director/director.h"
#include "palette/palette.h"

namespace pdhost {

struct InstanceInfo {
	uint64_t id = 0;
	std::string filter_name;
	std::string parent_name;
};

struct LogLine {
	uint64_t seq = 0;
	uint64_t phrase = 0;
	std::string text;
};

struct TimelineSample {
	double beat = 0.0;
	float mix = 1.0f;
	int section = 0; // pd::Section
	float bass = 0.0f;
	float r = 0.0f, g = 0.0f, b = 0.0f; // live palette colour (strip)
};

struct DockView {
	pd::DirectorState st;
	float mix = 1.0f;
	double beats = 0.0;
	struct pl_set set;
	std::string audio_name;
	bool audio_attached = false;
	std::vector<LogLine> log; // only lines with seq > the caller's cursor, oldest first
	std::vector<TimelineSample> timeline;
};

std::vector<InstanceInfo> list_instances();
// fills `out`; false if the instance no longer exists
bool read_view(uint64_t id, DockView &out, uint64_t log_since);
bool command(uint64_t id, int cmd, int idx);

} // namespace pdhost
