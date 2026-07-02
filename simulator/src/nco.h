#ifndef NCO_H
#define NCO_H

#include "sim_types.h"

#include <stddef.h>
#include <stdint.h>

typedef struct {
    double phase_rad;
    double phase_step_rad;
} nco_t;

void nco_init(nco_t *nco, double frequency_hz, double sample_rate_hz);
void nco_mix_ci16(const nco_t *initial, const iq_ci16_t *in, iq_ci16_t *out, size_t count, double gain);
int16_t sim_clip_i16(double value);

#endif
