#ifndef NCO_H
#define NCO_H

#include "sim_types.h"

#include <stddef.h>
#include <stdint.h>

int16_t sim_clip_i16(double value);

/* Deterministic, drift-free frequency-shift phase derived from absolute time.
 *
 * The phase step (cycles per output sample) is stored as an unsigned Q0.64 fraction of a
 * turn. Accumulating it for an absolute sample index is a wrapping 64-bit multiply, which is
 * exact modulo one turn no matter how large the index grows -- unlike offset_hz*index in
 * double, which loses precision after a few hours of uptime at high sample rates. Because the
 * phase is a pure function of the absolute sample index, it is continuous across block
 * boundaries and identical across instances. */
uint64_t nco_phase_step_q64(double frequency_hz, double sample_rate_hz);
double nco_phase_rad_at(uint64_t phase_step_q64, uint64_t sample_index);

#endif
