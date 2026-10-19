#pragma once
#include <cstdint>

#include "wav.h"

namespace pdtest {

// Synthetic fixtures — deterministic, no copyrighted audio (spec 4.3).

// 4-on-the-floor kick: a decaying 55 Hz cosine burst + click transient on every beat.
WavMono gen_kick(double bpm, int bars, uint32_t sample_rate = 44100);

// groove (kicks) -> breakdown (quiet mid-band pad, no bass) -> drop (kicks return on a
// downbeat). Section boundaries are exact: last groove kick at beat (groove_bars*4-1),
// drop resumes at beat ((groove_bars+breakdown_bars)*4).
WavMono gen_breakdown(double bpm, int groove_bars, int breakdown_bars, int drop_bars, uint32_t sample_rate = 44100);

// Pure digital silence.
WavMono gen_silence(double seconds, uint32_t sample_rate = 44100);

} // namespace pdtest
