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
Per-instance runtime for the Palette Lock filter (spec 3.5 / 4.1 palette-lock-host).

  audio thread   : capture callback -> mono downmix -> lock-free SPSC float ring
  worker thread  : ring -> pd::Director, drains commands, runs the lock-mix fade,
                   publishes snapshots, logs director decisions
  render thread  : pd_host_render_state() reads a lock-free triple buffer
  hotkey/UI      : pd_host_command() -> SPSC command queues drained by the worker

The worker owns attaching/detaching the audio capture callback (weak source ref),
so neither the render thread nor the audio thread ever takes a lock of ours.
*/

#pragma once

#include <obs-module.h>

#include "palette/palette.h"

#ifdef __cplusplus
extern "C" {
#endif

enum pd_cmd {
	PD_CMD_FORCE_ON = 0, /* toggle */
	PD_CMD_FORCE_OFF,    /* hold: pressed */
	PD_CMD_RELEASE_FORCE,
	PD_CMD_PAUSE,
	PD_CMD_RESUME,
	PD_CMD_NEXT,
	PD_CMD_PREV,
	PD_CMD_GO_LIVE, /* idx = palette */
	PD_CMD_ROTATE_NOW,
	PD_CMD_PANIC,
	PD_CMD_TOGGLE_PAUSE, /* host-level: Pause or Resume from the current state */
};

/* where a command came from (selects the producer queue + log tag) */
enum pd_cmd_origin {
	PD_ORIGIN_HOTKEY = 0, /* obs hotkey thread (callbacks are serialised by libobs) */
	PD_ORIGIN_UI = 1,     /* dock / properties (UI or websocket threads) */
};

struct pd_render_state {
	unsigned long long gen; /* palette-layout generation the indices refer to */
	int live;
	int prev; /* -1 = none */
	float pfade;
	int rotation;
	float mix;  /* faded lock mix 0..1 */
	float bass; /* 0..1 bass envelope (breathing) */
	int silent; /* feed silent / no audio source: breathing suspended */
};

struct pd_host;

struct pd_host *pd_host_create(obs_source_t *filter);
void pd_host_destroy(struct pd_host *h);

/* Push new settings (create, then the video thread from update). Returns the palette-layout
 * generation, which only changes when palette count/names/order change.
 * force_live >= 0 sets the Director's live palette. */
unsigned long long pd_host_update(struct pd_host *h, const struct pl_set *set, int force_live,
				  const char *audio_source);

/* filter_add / filter_remove: detach the audio callback while the filter is removed */
void pd_host_set_attached(struct pd_host *h, bool attached);

void pd_host_command(struct pd_host *h, int cmd, int idx, int origin);

/* render thread: lock-free. false until the worker has published once. */
bool pd_host_render_state(struct pd_host *h, struct pd_render_state *out);

/* UI thread: current live palette per the Director (-1 if unknown) */
int pd_host_live_palette(struct pd_host *h);

#ifdef __cplusplus
}
#endif
